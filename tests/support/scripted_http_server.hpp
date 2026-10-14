#pragma once
// Test-only loopback HTTP/1.1 server: serves connection N with reply N (then `500 {}`), records each raw
// request, and closes every connection after one reply. Lets a provider-plane client (an embedder, a vector
// index, a batch backend) be driven over a real socket offline. Same shape as the scripted server in
// test_openrouter_batch_backend.cpp (ADR-235), lifted here for reuse by #172's RAG backend tests.
//
// `silent_listener()` is its counterpart for cancellation tests: a socket that accepts connections from the
// backlog and never answers, so an exchange against it can only end by being stopped (or by an idle timeout).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/net.hpp"

namespace agentengine::test_support {

inline constexpr std::uint32_t kLoopbackHostOrder = (127u << 24) | 1u;

struct ScriptedReply {
    int         status = 200;
    std::string body;
};

class ScriptedHttpServer {
public:
    explicit ScriptedHttpServer(std::vector<ScriptedReply> replies) : replies_(std::move(replies)) {
        auto listen_r = pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_      = *pal::local_port(listen_fd_);
            thread_    = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~ScriptedHttpServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) pal::close_fd(listen_fd_);
    }
    ScriptedHttpServer(ScriptedHttpServer const&) = delete;
    ScriptedHttpServer& operator=(ScriptedHttpServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] std::vector<std::string> requests() {
        std::lock_guard<std::mutex> l(mu_);
        return requests_;
    }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            std::size_t const n = served_++;
            serve_one(*a, n < replies_.size() ? replies_[n] : ScriptedReply{500, "{}"}, st);
            pal::close_fd(*a);
        }
    }

    static void write_all(pal::fd_t fd, std::string const& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            auto w = pal::send_some(fd, reinterpret_cast<std::byte const*>(data.data() + sent), data.size() - sent);
            if (!w) {
                if (w.error() == pal::would_block()) continue;
                return;
            }
            sent += *w;
        }
    }

    void serve_one(pal::fd_t fd, ScriptedReply const& reply, std::stop_token const& st) {
        std::string buf;
        std::byte   chunk[4096];
        std::size_t want = std::string::npos;
        for (int i = 0; i < 2000 && !st.stop_requested(); ++i) {
            auto r = pal::recv_some(fd, chunk, sizeof(chunk));
            if (!r) {
                if (r.error() == pal::would_block()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                break;
            }
            if (*r == 0) break;
            buf.append(reinterpret_cast<char const*>(chunk), *r);
            std::size_t const head_end = buf.find("\r\n\r\n");
            if (head_end != std::string::npos && want == std::string::npos) {
                std::size_t       len = 0;
                std::size_t const cl  = buf.find("Content-Length:");
                if (cl != std::string::npos && cl < head_end) len = std::stoul(buf.substr(cl + 15));
                want = head_end + 4 + len;
            }
            if (want != std::string::npos && buf.size() >= want) break;
        }
        {
            std::lock_guard<std::mutex> l(mu_);
            requests_.push_back(buf);
        }
        write_all(fd, "HTTP/1.1 " + std::to_string(reply.status) + " X\r\nContent-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(reply.body.size()) + "\r\nConnection: close\r\n\r\n" +
                      reply.body);
    }

    std::vector<ScriptedReply> replies_;
    bool                       ok_ = false;
    pal::fd_t                  listen_fd_{};
    std::uint16_t              port_ = 0;
    std::atomic<std::size_t>   served_{0};
    std::mutex                 mu_;
    std::vector<std::string>   requests_;
    std::jthread               thread_;
};

// A listening socket that is never accepted from: a connect completes from the backlog and no byte comes back.
// The caller closes it with `pal::close_fd`.
struct SilentListener {
    pal::fd_t     fd{};
    std::uint16_t port = 0;
};
[[nodiscard]] inline std::optional<SilentListener> silent_listener() {
    auto l = pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
    if (!l) return std::nullopt;
    return SilentListener{*l, *pal::local_port(*l)};
}

}  // namespace agentengine::test_support
