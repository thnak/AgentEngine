#pragma once
// Implements ADR-182 live mode (decisions/ADR-182-agent-test-driver-mcp.md §3.3): a driver session
// whose model is a REAL OpenAI-compatible provider (DeepSeek by default), wrapped in
// RecordingChatClient so every call is written to disk as a ChatCallRecording (I5: a confusing live
// run can be replayed later without the network).
//
// Only compiled when AGENTENGINE_WITH_HTTPS is on (the real client needs TLS). The key reaches this
// file from a key file named on the command line, never from a tool argument; it is stored in an
// InMemorySecretStore and resolved at the point of use against the session's cap::Secret grant (I2).
//
// Guardrails (host-side, fixed at startup):
//   - `max_calls` per session: past it every call fails `test.live_call_budget_exhausted` (I8), so a
//     looping agent cannot run up a bill.
//   - the key is registered as a secret canary: any driver reply containing it is withheld.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "agentengine/core/chat_recording.hpp"
#include "agentengine/core/recording_chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/trust/secret.hpp"
#include "test_driver/test_driver.hpp"

namespace agentengine::test_driver {

struct LiveConfig {
    std::string   host = "api.deepseek.com";
    std::uint16_t port = 443;
    std::string   path_prefix = "/v1";
    std::string   model = "deepseek-flash";
    std::string   key;                 // the API key itself (read from the key file by main)
    std::filesystem::path record_dir;  // empty = recordings discarded
    std::uint32_t max_calls = 40;      // per session
};

class LiveBackend final : public ModelBackend {
public:
    using Inner = openai::OpenAIChatClient<InMemorySecretStore>;

    LiveBackend(LiveConfig const& cfg, std::string const& session_id)
        : store_(make_store(cfg.key)),
          max_calls_(cfg.max_calls),
          client_(Inner(cfg.host, cfg.port, cfg.model, SecretRef{std::string(kLiveSecretName)}, capabilities_of(),
                        *store_, cfg.path_prefix),
                  make_sink(cfg.record_dir, session_id)) {}

    [[nodiscard]] ChatClientCapabilities capabilities() const override { return capabilities_of(); }

    [[nodiscard]] task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext& ctx) override {
        if (calls_.fetch_add(1) >= max_calls_) co_return std::unexpected(budget_exhausted());
        co_return co_await client_.chat(request, ctx);
    }

    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest const& request, EffectContext& ctx) override {
        if (calls_.fetch_add(1) >= max_calls_) {
            auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
            pair.producer.fail(budget_exhausted());
            return std::move(pair.consumer);
        }
        return client_.chat_stream(request, ctx);
    }

private:
    [[nodiscard]] static ChatClientCapabilities capabilities_of() {
        ChatClientCapabilities caps;
        caps.streaming = true;
        caps.tool_calling = true;
        caps.max_output_tokens = 2048;
        return caps;
    }
    [[nodiscard]] static std::shared_ptr<InMemorySecretStore> make_store(std::string const& key) {
        auto store = std::make_shared<InMemorySecretStore>();
        store->set(std::string(kLiveSecretName), key);
        return store;
    }
    [[nodiscard]] static RecordingChatClient<Inner>::RecordingSink make_sink(std::filesystem::path dir,
                                                                             std::string session_id) {
        if (dir.empty()) return [](ChatCallRecording) {};
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto counter = std::make_shared<std::atomic<std::uint32_t>>(0);
        return [dir = std::move(dir), session_id = std::move(session_id), counter](ChatCallRecording rec) {
            std::uint32_t const n = counter->fetch_add(1) + 1;
            (void)write_chat_call_recording(dir / (session_id + "-call-" + std::to_string(n) + ".json"), rec);
        };
    }
    [[nodiscard]] static error budget_exhausted() {
        return error{failure_class::policy, "this live session used its model-call budget",
                     "test.live_call_budget_exhausted"};
    }

    std::shared_ptr<InMemorySecretStore> store_;  // outlives client_ (declared first)
    std::uint32_t                        max_calls_;
    std::atomic<std::uint32_t>           calls_{0};
    RecordingChatClient<Inner>           client_;
};

}  // namespace agentengine::test_driver
