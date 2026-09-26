// HandleRelay (docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md,
// decisions/ADR-085-jailed-python-worker-slice-2-handle-relay.md) -- the two positive/negative
// controls the design draft's own §6 named but the regression sweep against the existing suite does
// not exercise: (1) a real, successful socket relay round trip under a genuinely granted `cap::NetOut`
// -- nothing in the pre-existing suite ever grants NetOut and expects a live connect to succeed, every
// existing socket test is a DENIAL-path check; (2) the design draft §4 item 1's own claim, revised
// during implementation ("the access mask lives entirely host-side... calling WriteFile against a
// handle opened GENERIC_READ fails at the KERNEL level") -- verified here by calling
// `_ae_internal.file_write` DIRECTLY on a read-opened file_id, bypassing `_AeRelayFile`'s own
// Python-level `for_write` check entirely, so the denial observed is the OS's, not this wrapper's.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/pal/net.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"

using namespace agentengine;
using agentengine::native_jail::MediatedPythonConfig;
using agentengine::native_jail::MediatedPythonRunner;
using agentengine::native_jail::NativeJailBackend;

namespace {

// Red-team finding (independent review, this pass): a bare set-then-reset pair around
// `set_test_connect_resolver_override` leaks the override (silently disabling SSRF protection for
// every OTHER worker session sharing this process) on any early return/exception between the two
// calls. RAII closes that window unconditionally, the same discipline this codebase already uses for
// HANDLE ownership elsewhere in this backend (e.g. native_jail_backend.cpp's HandleGuard).
struct TestResolverOverrideGuard {
    explicit TestResolverOverrideGuard(
        std::function<result<sandbox::VerifiedEndpoint>(std::string_view, std::uint16_t)> fn) {
        NativeJailBackend::set_test_connect_resolver_override(std::move(fn));
    }
    ~TestResolverOverrideGuard() { NativeJailBackend::set_test_connect_resolver_override(nullptr); }
    TestResolverOverrideGuard(TestResolverOverrideGuard const&) = delete;
};

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s at %s:%d\n", (label), __FILE__, __LINE__);              \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::printf("  ok: %s\n", (label));                                                    \
        }                                                                                           \
    } while (0)

std::string read_file(std::string const& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A tiny loopback echo server: accepts one connection, reads whatever arrives, echoes it back
// byte-for-byte, then closes -- just enough for a real connect+send+recv+close round trip, the same
// `agentengine::pal` primitives (not a second socket abstraction) `test_https_egress.cpp`'s own local
// test server already uses.
class LoopbackEchoServer {
public:
    LoopbackEchoServer() {
        agentengine::pal::ensure_winsock();
        auto listen_r = agentengine::pal::tcp_listen(0x7F000001ull /* 127.0.0.1 */, 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *agentengine::pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~LoopbackEchoServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) agentengine::pal::close_fd(listen_fd_);
    }
    LoopbackEchoServer(LoopbackEchoServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = agentengine::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            serve_one(*a);
            agentengine::pal::close_fd(*a);
            return;  // one connection is all this test needs
        }
    }

    void serve_one(agentengine::pal::fd_t fd) {
        std::byte buf[4096];
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            auto r = agentengine::pal::recv_some(fd, buf, sizeof(buf));
            if (!r) {
                if (r.error() == agentengine::pal::would_block()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                return;
            }
            if (*r == 0) return;  // peer closed
            std::size_t sent = 0;
            while (sent < *r) {
                auto s = agentengine::pal::send_some(fd, buf + sent, *r - sent);
                if (!s) {
                    if (s.error() == agentengine::pal::would_block()) continue;
                    return;
                }
                sent += *s;
            }
        }
    }

    bool ok_ = false;
    agentengine::pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    std::jthread thread_;
};

}  // namespace

int main() {
    NativeJailBackend backend;

    std::string const scratch =
        ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") + "/ae_handle_relay_mount";
    std::filesystem::create_directories(scratch);

    // ============================================================================================
    // Socket relay: a real successful connect_authorize + connect_send + connect_recv + connect_close
    // round trip under a genuinely granted cap::NetOut, against a real local TCP listener.
    // ============================================================================================
    {
        LoopbackEchoServer server;
        AE_CHECK(server.ok(), "setup: loopback echo server listens on an ephemeral port");

        // sandbox::resolve_and_validate categorically blocks loopback/private/link-local/CGNAT
        // addresses BY DESIGN (008/ADR-011's SSRF posture, deliberately reused here per the design
        // draft's own §2 item 1) -- there is no hermetic same-machine target that check would ever
        // pass, so this test overrides ONLY the resolver (a test-only seam, never used from production
        // code, mirroring sandbox::HostEgressProxy::resolver's own established precedent exactly) to
        // answer "127.0.0.1 resolves to itself," proving the REAL post-resolution composition --
        // pal::tcp_connect, and the real connect_send/connect_recv byte relay -- against a real local
        // peer, without weakening the production resolver at all.
        TestResolverOverrideGuard resolver_guard(
            [](std::string_view, std::uint16_t port) -> result<sandbox::VerifiedEndpoint> {
                return sandbox::VerifiedEndpoint{0x7F000001u, port};
            });

        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "setup: runner initializes cleanly");

        ExecState state{};
        CapabilitySet caps = CapabilitySet::grant_root(
            {Capability{cap::NetOut{{"127.0.0.1:" + std::to_string(server.port()) + ":tcp"}, std::nullopt, {}}}});
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        std::string const script = "import socket\n"
                                    "s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)\n"
                                    "s.connect(('127.0.0.1', " +
                                    std::to_string(server.port()) +
                                    "))\n"
                                    "s.sendall(b'hello relay')\n"
                                    "data = b''\n"
                                    "while len(data) < len(b'hello relay'):\n"
                                    "    chunk = s.recv(1024)\n"
                                    "    if not chunk:\n"
                                    "        break\n"
                                    "    data += chunk\n"
                                    "s.close()\n"
                                    "print('GOT:', data)";
        ExecRequest req{"python", script};
        auto out = runner.run(req, state, ctx);
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok,
                 "SOCK-1: a script connecting to a real granted NetOut target runs successfully");
        AE_CHECK(out.has_value() && out->stdout_text.find("GOT: b'hello relay'") != std::string::npos,
                 "SOCK-1: connect_authorize+connect_send+connect_recv+connect_close round-trip real "
                 "bytes through the host-owned live socket, echoed back by a real TCP peer");
        // resolver_guard's destructor restores the real resolver here, unconditionally.
    }

    // ============================================================================================
    // Red-team finding 1 (independent review): every socket.socket method OTHER than
    // connect/send/sendall/recv/close must be denied explicitly, not left as the real, unmediated
    // CPython implementation. Proven here for the two most concrete bypass shapes named in the
    // finding: sendto() (a UDP path that never touches _ae_connect/_ae_send at all) and bind()+
    // listen()+accept() (an inbound server, never gated by cap::NetOut in either direction).
    // ============================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "setup: runner initializes cleanly (SOCK-DENY)");

        ExecState state{};
        EffectContext ctx{};  // no capabilities granted at all -- irrelevant here, since these
                                // methods must be denied unconditionally, not merely without a grant

        ExecRequest req{"python",
                         "import socket\n"
                         "results = []\n"
                         "for name, args in [\n"
                         "    ('sendto', lambda s: s.sendto(b'x', ('127.0.0.1', 9))),\n"
                         "    ('bind', lambda s: s.bind(('127.0.0.1', 0))),\n"
                         "    ('listen', lambda s: s.listen(1)),\n"
                         "    ('accept', lambda s: s.accept()),\n"
                         "    ('makefile', lambda s: s.makefile()),\n"
                         "]:\n"
                         "    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM if name == 'sendto' "
                         "else socket.SOCK_STREAM)\n"
                         "    try:\n"
                         "        args(s)\n"
                         "        results.append(name + ':RAN')\n"
                         "    except PermissionError:\n"
                         "        results.append(name + ':DENIED')\n"
                         "    finally:\n"
                         "        s.close()\n"
                         "print('RESULTS:', results)"};
        auto out = runner.run(req, state, ctx);
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok,
                 "SOCK-DENY: setup -- the unmediated-socket-method probe script runs");
        AE_CHECK(out.has_value() &&
                     out->stdout_text.find(
                         "RESULTS: ['sendto:DENIED', 'bind:DENIED', 'listen:DENIED', "
                         "'accept:DENIED', 'makefile:DENIED']") != std::string::npos,
                 "SOCK-DENY: sendto/bind/listen/accept/makefile are all denied outright, never "
                 "reaching the real, unmediated CPython implementation");
    }

    // ============================================================================================
    // File relay: the access mask is enforced by the KERNEL against the host-held handle, not merely
    // by _AeRelayFile's own Python-level for_write bookkeeping -- verified by calling
    // _ae_internal.file_write DIRECTLY on a file_id opened for READ, bypassing the Python wrapper.
    // ============================================================================================
    {
        std::string const marker_path = scratch + "/readonly_marker.txt";
        {
            std::ofstream f(marker_path, std::ios::binary);
            f << "untouched";
        }

        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.mount_roots["work"] = std::wstring(scratch.begin(), scratch.end());
        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "setup: runner initializes cleanly with a real 'work' mount");

        ExecState state{};
        CapabilitySet caps =
            CapabilitySet::grant_root({Capability{cap::FsRead{"work", "", std::nullopt}}});
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        // `_ae_internal` is not itself a name in the guest's own __main__ globals (it lives only in
        // the bootstrap's own throwaway globals dict, discarded after run_mediation_bootstrap()
        // returns) -- reached here the same way E4-PY9 (test_mediated_python_runner_hostile_corpus.cpp)
        // already reaches bootstrap-scoped state for a white-box probe: via a bound method's own
        // __globals__, since every method _AeRelayFile defines shares that one module-level scope.
        ExecRequest req{"python",
                         "f = open('/work/readonly_marker.txt', 'r')\n"
                         "internal = f.close.__func__.__globals__['_ae_internal']\n"
                         "try:\n"
                         "    internal.file_write(f._id, b'malicious')\n"
                         "    print('SHOULD NOT REACH')\n"
                         "except OSError as e:\n"
                         "    print('DENIED:', e)\n"
                         "f.close()"};
        auto out = runner.run(req, state, ctx);
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok,
                 "FILE-RO-1: setup -- the direct _ae_internal.file_write probe script runs");
        AE_CHECK(out.has_value() && out->stdout_text.find("DENIED") != std::string::npos,
                 "FILE-RO-1: WriteFile against a GENERIC_READ-only host-held handle is denied by the "
                 "kernel, not merely by _AeRelayFile's own Python-level for_write check (bypassed "
                 "here by calling _ae_internal.file_write directly)");
        AE_CHECK(out.has_value() && out->stdout_text.find("SHOULD NOT REACH") == std::string::npos,
                 "FILE-RO-1: the malicious write never reported success");
        AE_CHECK(read_file(marker_path) == "untouched",
                 "FILE-RO-1: the file's real on-disk content is unchanged");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_native_jail_python_worker_handle_relay: ALL PASS\n");
    return 0;
}
