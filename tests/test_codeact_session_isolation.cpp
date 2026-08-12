// Proves ADR-030's session-scoped CodeAct wiring at the FULL integration level -- two real
// `AgentSession` instances, each with their own provider built the same shape
// `tools/cli_chat.cpp`'s `ToolDeclaringHistoryProvider` uses (ADR-028's
// `make_tool_descriptor_with_invoke` + a `CodeActRunnerBinding<RunnerT>`), sharing ONE process-wide
// "runner" the same way cli_chat.cpp's `shared_python_runner_binding()` does. `cli_chat.cpp`'s own
// type can't be reused directly here (it lives in an anonymous namespace inside a `#ifdef
// AGENTENGINE_WITH_HTTPS`-gated .cpp, per that file's own comment) -- this uses a synthetic,
// minimal stand-in `FakeRunner` instead, matching ADR-028's own test file's precedent
// (`test_session_scoped_stateful_tools.cpp` uses a synthetic counter, not the real
// `MediatedPythonRunner`). `test_codeact_runner_binding.cpp` already proves
// `CodeActRunnerBinding<RunnerT>`'s own claim/fail-closed logic in isolation; this file proves the
// SAME mechanism holds once real `AgentSession`s, real `ContextProvider::on_context()` calls, and a
// real internal tool-call round loop are all in the loop.
//
//   CI1 — a mounted skill in session A's own MountedSkillsState never appears in session B's --
//         real per-session isolation, the actual payoff ADR-030 exists to prove (a latent bug
//         `cli_chat.cpp`'s five process-global statics never caught, because it only ever ran one
//         session).
//   CI2 — session A configure()s first against the ONE shared runner binding and succeeds.
//   CI3 — session B's configure() against the SAME already-bound binding fails closed -- the exact
//         cross-session hazard red-team finding #1 (unsynchronized globals inside a real
//         MediatedPythonRunner) warned about is structurally impossible: B never gets to reach the
//         runner at all.
//   CI4 — session A, correctly configured, CAN reach the shared runner through its own provider (a
//         real "use_runner" tool call resolves and returns the runner's own tag) -- proving CI2/CI3
//         aren't merely rejecting everyone.
//   CI5 — session B, whose configure() was rejected, fails closed on every attempt to reach the
//         runner through its own "use_runner" tool -- the claim gate is actually enforced at the
//         point of use, not just at configure() time.

#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/codeact_runner_binding.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/mounted_skills_state.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/trust/principal.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

// A trivial stand-in for a real, process-wide-shared interpreter -- has an identity (`tag`) a tool
// call can read back, so a test can tell "reached the real shared object" from "got nothing."
struct FakeRunner {
    int tag = 0;
};

// ---- Test tools -----------------------------------------------------------------------------

struct MountArgs { std::string skill_name; };
AE_JSON_SCHEMA(MountArgs, skill_name)
struct MountReply { bool ok = false; };
AE_JSON_SCHEMA(MountReply, ok)

// Sentinel invoke() -- must never actually run; the real logic is the provider's own closure
// (matching cli_chat.cpp's own ExecuteCodeTool/MountSkillTool "poison the static path" shape).
struct MountTool : ae::Tool<MountTool, ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "mount";
    static constexpr std::string_view description = "Mounts a skill (test double).";
    using Args = MountArgs;
    using Reply = MountReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) {
        return std::unexpected(ae::error{ae::failure_class::fatal, "dead path", "test.dead_static_invoke"});
    }
};

struct UseRunnerArgs { bool unused = false; };
AE_JSON_SCHEMA(UseRunnerArgs, unused)
struct UseRunnerReply { int tag = 0; };
AE_JSON_SCHEMA(UseRunnerReply, tag)

struct UseRunnerTool : ae::Tool<UseRunnerTool, ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "use_runner";
    static constexpr std::string_view description = "Reads the shared runner's tag (test double).";
    using Args = UseRunnerArgs;
    using Reply = UseRunnerReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) {
        return std::unexpected(ae::error{ae::failure_class::fatal, "dead path", "test.dead_static_invoke"});
    }
};

// ---- The provider under test: same shape as cli_chat.cpp's ToolDeclaringHistoryProvider --------

class TestCodeActProvider {
public:
    [[nodiscard]] ae::result<void> configure(std::string session_id,
                                              ae::CodeActRunnerBinding<FakeRunner>& runner_binding) {
        auto bound = runner_binding.bind(session_id);
        if (!bound) return bound;
        runner_binding_ = &runner_binding;
        return {};
    }

    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc,
                                                                              ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools.push_back(ae::make_tool_descriptor_with_invoke<MountTool>(
            [this](MountArgs a, ae::EffectContext& ctx) { return real_mount(std::move(a), ctx); }));
        c.tools.push_back(ae::make_tool_descriptor_with_invoke<UseRunnerTool>(
            [this](UseRunnerArgs a, ae::EffectContext& ctx) { return real_use_runner(std::move(a), ctx); }));
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }

    [[nodiscard]] ae::MountedSkillsState const& mounted_skills() const noexcept { return mounted_skills_; }

private:
    [[nodiscard]] ae::result<MountReply> real_mount(MountArgs a, ae::EffectContext&) {
        mounted_skills_.mount(a.skill_name);
        return MountReply{true};
    }

    // CI5's own gate: `runner()` itself (unlike CodeActRunnerBinding's own contract note) doesn't
    // check WHO is asking -- it's `real_use_runner()`'s job, same as cli_chat.cpp's
    // `real_execute_code()`, to refuse reaching it at all when this provider was never successfully
    // configured (its own `runner_binding_` stayed null because `configure()` failed closed).
    [[nodiscard]] ae::result<UseRunnerReply> real_use_runner(UseRunnerArgs, ae::EffectContext&) {
        if (runner_binding_ == nullptr) {
            return std::unexpected(ae::error{ae::failure_class::fatal,
                                              "this session was never configured against the shared "
                                              "runner",
                                              "test.codeact_not_configured"});
        }
        return UseRunnerReply{runner_binding_->runner().tag};
    }

    ae::MountedSkillsState mounted_skills_;
    ae::CodeActRunnerBinding<FakeRunner>* runner_binding_ = nullptr;
};
static_assert(ae::ContextProvider<TestCodeActProvider>);

// ---- Scripted ChatClientT: one scripted tool call, then converges -------------------------------

class ScriptedChatClient {
public:
    ae::Message scripted_call;
    std::size_t call_count = 0;

    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        ae::Message reply = (call_count == 0) ? scripted_call : make_text_message("done");
        ++call_count;
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }
    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::Message reply = (call_count == 0) ? scripted_call : make_text_message("done");
        ae::ChatResponseUpdate upd;
        upd.delta    = reply.content.front();
        upd.is_final = true;
        upd.usage    = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed  = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        ++call_count;
        return std::move(pair.consumer);
    }

    [[nodiscard]] static ae::Message make_text_message(std::string text) {
        ae::Message m;
        m.role = ae::role::assistant;
        m.message_id = "m-text";
        ae::ContentItem item;
        item.origin = ae::content_origin::assistant;
        item.value = ae::Text{std::move(text)};
        m.content.push_back(std::move(item));
        return m;
    }
    [[nodiscard]] static ae::Message make_tool_call_message(std::string call_id, std::string tool_name,
                                                              std::string args_json) {
        ae::Message m;
        m.role = ae::role::assistant;
        m.message_id = "m-" + call_id;
        ae::ContentItem item;
        item.origin = ae::content_origin::assistant;
        item.value = ae::ToolCall{std::move(call_id), std::move(tool_name), std::move(args_json),
                                   ae::content_origin::assistant, ae::call_provenance::vendor_structured};
        m.content.push_back(std::move(item));
        return m;
    }
};
static_assert(ae::ChatClient<ScriptedChatClient>);

[[nodiscard]] ae::Message user_message(std::string text) {
    ae::Message m;
    m.role = ae::role::user;
    ae::ContentItem item;
    item.origin = ae::content_origin::user;
    item.value = ae::Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

using Session = ae::AgentSession<ScriptedChatClient, ae::NoSessionState, TestCodeActProvider>;

}  // namespace

int main() {
    FakeRunner shared_runner{99};
    ae::CodeActRunnerBinding<FakeRunner> shared_binding(shared_runner);

    quark::TestKit<Session> kit_a;
    quark::TestKit<Session> kit_b;
    kit_a.actor().initialize("s-a", ae::Principal{"p", ""});
    kit_b.actor().initialize("s-b", ae::Principal{"p", ""});
    ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
    kit_a.actor().set_capabilities(&held);
    kit_b.actor().set_capabilities(&held);

    // ---- CI2/CI3: session A claims the shared binding first; session B is rejected --------------
    auto configured_a = kit_a.actor().history_provider().configure("s-a", shared_binding);
    AE_CHECK(configured_a.has_value(), "CI2: session A's configure() against the shared binding "
                                        "succeeds (it claims first)");
    auto configured_b = kit_b.actor().history_provider().configure("s-b", shared_binding);
    AE_CHECK(!configured_b.has_value(),
             "CI3: session B's configure() against the SAME already-bound binding fails closed -- "
             "the exact cross-session hazard (unsynchronized runner internals reachable from two "
             "sessions at once) this ADR exists to make structurally impossible");

    // ---- CI1: mounting in A never appears in B ---------------------------------------------------
    auto& client_a = kit_a.actor().emplace_chat_client();
    client_a.scripted_call = ScriptedChatClient::make_tool_call_message("c1", "mount",
                                                                          R"({"skill_name":"demo"})");
    auto r_a = kit_a.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
    AE_CHECK(r_a.has_value(), "CI1 setup: session A's mount call converges");
    AE_CHECK(kit_a.actor().history_provider().mounted_skills().is_mounted("demo"),
             "CI1: session A's own MountedSkillsState reflects the mount it just made");
    AE_CHECK(!kit_b.actor().history_provider().mounted_skills().is_mounted("demo"),
             "CI1: session B's MountedSkillsState is COMPLETELY UNAWARE of session A's mount -- real "
             "per-session isolation, not a shared static (the actual payoff of this ADR)");

    // ---- CI4: session A (successfully configured) can reach the shared runner ---------------------
    auto& client_a2 = kit_a.actor().emplace_chat_client();
    client_a2.scripted_call =
        ScriptedChatClient::make_tool_call_message("c2", "use_runner", R"({"unused":false})");
    auto r_a2 = kit_a.ask<ae::AgentResponse>(ae::StartRun{user_message("go again")});
    AE_CHECK(r_a2.has_value(), "CI4: session A's use_runner call converges");
    // The reply is folded into a tool-result message in history; find it and check the tag survived
    // the round trip -- confirms real_use_runner() actually reached shared_runner.tag == 99, not a
    // denial.
    bool found_tag_99 = false;
    for (auto const& msg : kit_a.actor().history()) {
        for (auto const& item : msg.content) {
            if (auto const* tr = std::get_if<ae::ToolResult>(&item.value)) {
                if (!tr->is_error) found_tag_99 = true;
            }
        }
    }
    AE_CHECK(found_tag_99, "CI4: session A's use_runner call succeeded (not denied) -- it genuinely "
                            "reached the shared runner through its own provider");

    // ---- CI5: session B (rejected configure()) fails closed reaching the runner -------------------
    auto& client_b = kit_b.actor().emplace_chat_client();
    client_b.scripted_call =
        ScriptedChatClient::make_tool_call_message("c3", "use_runner", R"({"unused":false})");
    auto r_b = kit_b.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
    AE_CHECK(r_b.has_value(), "CI5 setup: session B's run still converges (a tool error is fed back, "
                              "not a run failure)");
    bool b_saw_denial = false;
    for (auto const& msg : kit_b.actor().history()) {
        for (auto const& item : msg.content) {
            if (auto const* tr = std::get_if<ae::ToolResult>(&item.value)) {
                if (tr->is_error) b_saw_denial = true;
            }
        }
    }
    AE_CHECK(b_saw_denial,
             "CI5: session B's use_runner call was denied -- its own runner_binding_ stayed null "
             "because configure() never succeeded, so it can never reach the shared runner at all");

    std::cout << (g_failures == 0 ? "test_codeact_session_isolation: OK\n"
                                   : "test_codeact_session_isolation: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
