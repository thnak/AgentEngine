// Proof for ADR-037 Phase 2: porting the real behavioral claims from three OLD, Quark-actor-based
// test files onto agentengine::rt::AgentSession (include/agentengine/rt/agent_session.hpp) directly
// -- deterministic, offline, no live model, no network, no quark::TestKit<A>/quark::Ask plumbing.
// Every mechanism these three files exercise (tool_pipeline.hpp's make_tool_descriptor_with_invoke/
// captures_session_state/background_task, codeact_runner_binding.hpp's CodeActRunnerBinding<RunnerT>,
// mounted_skills_state.hpp's MountedSkillsState, trust/principal.hpp's derive_on_behalf_of) lives
// under agentengine::core/agentengine::trust, UNCHANGED by this migration -- only AgentSession itself
// moved. Confirmed directly against rt/agent_session.hpp before porting, not assumed:
//   - fork_from() copies history_provider_ (line ~580: `history_provider_ = source.history_provider_;`)
//     and clear_in_process_state() resets it (`history_provider_ = HistoryProviderT{};`) -- S5 below
//     needed both to still hold.
//   - start_run()'s very first steps set `effect_context_.principal = principal_` unconditionally,
//     before any model call -- the delegation claims below still hold verbatim.
//   - start_background_task() (Slice 3) wires through tool_pipeline.hpp's OWN background_task() free
//     function unchanged, so S3/S4 test that free function directly, exactly like the old file did,
//     rather than needing any rt::AgentSession involvement at all.
// No claim from any of the three old files was dropped -- every one of them ported cleanly.
//
// ---- From test_session_scoped_stateful_tools.cpp (ADR-028's session-scoped-stateful-tools mechanism)
//   S1 -- a stateful tool's state persists correctly across multiple rounds within one run.
//   S2 -- two independent AgentSession instances (two independent provider instances) never see each
//         other's state -- proving per-session isolation, not asserting it.
//   S3 -- the must-fix #1 regression: a descriptor with both `captures_session_state` and
//         `backgroundable` set is refused by `background_task()`, structurally, before step 8.
//   S4 -- `make_tool_descriptor<ToolT>()` (the existing, unmodified path) is completely unaffected --
//         `captures_session_state` defaults false, ordinary stateless tools are still backgroundable
//         exactly as before.
//   S5 -- fork_from() copies provider-owned state (not just history text); clear_in_process_state()
//         wipes it with no residue.
//
// ---- From test_codeact_session_isolation.cpp (ADR-030's session-scoped CodeAct wiring, full
//      integration level: two real AgentSession instances sharing one process-wide "runner" via a
//      synthetic FakeRunner stand-in, matching that file's own precedent for not needing the real
//      MediatedPythonRunner)
//   CI1 -- a mounted skill in session A's own MountedSkillsState never appears in session B's.
//   CI2 -- session A configure()s first against the ONE shared runner binding and succeeds.
//   CI3 -- session B's configure() against the SAME already-bound binding fails closed.
//   CI4 -- session A, correctly configured, CAN reach the shared runner through its own provider.
//   CI5 -- session B, whose configure() was rejected, fails closed on every attempt to reach the
//          runner through its own "use_runner" tool.
//
// ---- From test_agent_session_delegation.cpp (018 §1's "no token passthrough" / on_behalf_of)
//   D1 -- a normally-initialized (non-delegated) session's outbound ChatClient::chat() call carries
//         EXACTLY its own owning principal, never a default-constructed empty Principal{}.
//   D2 -- a session initialized with a principal produced by derive_on_behalf_of() carries that SAME
//         derived identity (kind=agent, on_behalf_of set, tenant preserved, delegation_depth=1)
//         through to the outbound call.

#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/codeact_runner_binding.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/mounted_skills_state.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::StartRun;
using agentengine::task;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses -- every ChatClientT fixture below co_returns immediately.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::CapabilitySet;
using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::ContextContribution;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Principal;
using agentengine::SessionContext;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::ToolResult;
using agentengine::TurnView;
using agentengine::Usage;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::role;

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message text_message(std::string text) {
    Message m;
    m.role       = role::assistant;
    m.message_id = "m-text";
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message tool_call_message(std::string call_id, std::string tool_name, std::string args_json) {
    Message m;
    m.role       = role::assistant;
    m.message_id = "m-" + call_id;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value  = ToolCall{std::move(call_id), std::move(tool_name), std::move(args_json),
                            content_origin::assistant, call_provenance::vendor_structured};
    m.content.push_back(item);
    return m;
}

// ================================================================================================
// S1/S2/S3/S4/S5 fixtures (test_session_scoped_stateful_tools.cpp)
// ================================================================================================

struct CounterArgs { int delta = 0; };
AE_JSON_SCHEMA(CounterArgs, delta)
struct CounterReply { int total = 0; };
AE_JSON_SCHEMA(CounterReply, total)

// CounterTool's OWN static invoke() is deliberately wrong (a sentinel no test path should ever see)
// -- proving make_tool_descriptor_with_invoke genuinely runs the CALLER's callable instead of
// CounterTool::invoke, not just in addition to it.
struct CounterTool : agentengine::Tool<CounterTool, agentengine::Capabilities<>,
                                        agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name        = "counter_tool";
    static constexpr std::string_view description = "Adds delta to a session-scoped counter.";
    using Args  = CounterArgs;
    using Reply = CounterReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        return Reply{-999};  // sentinel: must never appear in any test's observed result
    }
};

// A second tool, declared Backgroundable, for S3 -- gets its descriptor built via
// make_tool_descriptor_with_invoke instead, to prove that combination is refused regardless of the
// tool's own declared capabilities.
struct BgArgs { bool unused = false; };
AE_JSON_SCHEMA(BgArgs, unused)
struct BgReply { bool unused = false; };
AE_JSON_SCHEMA(BgReply, unused)

struct BackgroundableStatefulTool
    : agentengine::Tool<BackgroundableStatefulTool, agentengine::Capabilities<>,
                         agentengine::Backgroundable,
                         agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name        = "bg_stateful_tool";
    static constexpr std::string_view description = "Declared Backgroundable, for S3.";
    using Args  = BgArgs;
    using Reply = BgReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) { return Reply{}; }
};

// The state-owning ContextProvider: counter_ is an ORDINARY MEMBER (no core-seam change, no StateT
// threading) -- exactly the pattern the ADR expects a provider to own its own session-scoped data.
class StatefulCounterProvider {
public:
    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc,
                                                                              EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools.push_back(agentengine::make_tool_descriptor_with_invoke<CounterTool>(
            [this](CounterArgs a, EffectContext&) -> agentengine::result<CounterReply> {
                counter_ += a.delta;
                return CounterReply{counter_};
            }));
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    int counter_ = 0;  // session-scoped state -- one instance per AgentSession, per-instance member
};
static_assert(agentengine::ContextProvider<StatefulCounterProvider>);

// Scripted ChatClientT: a queue of pre-built tool-call responses, consumed in order.
class ScriptedChatClient {
public:
    ScriptedChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<Message> script;
        std::size_t          call_count = 0;
    };

    void set_script(std::vector<Message> script) { state_->script = std::move(script); }
    [[nodiscard]] std::size_t call_count() const { return state_->call_count; }
    // S5's post-clear scenario reuses the SAME client instance against a fresh script -- matching
    // the old Quark-based test's own `fork_client.call_count = 0;` (there, a public field; here, an
    // explicit reset since call_count lives behind a shared_ptr<State>).
    void reset_call_count() { state_->call_count = 0; }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        Message reply = (state_->call_count < state_->script.size())
                            ? state_->script[state_->call_count]
                            : text_message("done");
        ++state_->call_count;
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // unused -- stream_model_calls_ stays false throughout this file
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

// Extracts CounterTool's `total` reply from a role::tool history message -- the black-box way to
// observe the provider's own internal counter_ after the fact.
[[nodiscard]] std::optional<int> counter_total_of(Message const& m) {
    for (ContentItem const& item : m.content) {
        auto const* tr = std::get_if<ToolResult>(&item.value);
        if (!tr || tr->is_error || tr->content.empty()) continue;
        auto const* d = std::get_if<agentengine::Data>(&tr->content[0].value);
        if (!d) continue;
        auto parsed = agentengine::json::parse(d->json);
        if (!parsed) continue;
        auto const* t = parsed->find("total");
        if (t && t->is_number()) return static_cast<int>(t->as_number());
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<int> all_counter_totals(std::vector<Message> const& history) {
    std::vector<int> totals;
    for (auto const& m : history) {
        if (m.role != role::tool) continue;
        if (auto t = counter_total_of(m)) totals.push_back(*t);
    }
    return totals;
}

using StatefulSession = AgentSession<ScriptedChatClient, NoSessionState, StatefulCounterProvider>;

// ================================================================================================
// CI1-CI5 fixtures (test_codeact_session_isolation.cpp)
// ================================================================================================

// A trivial stand-in for a real, process-wide-shared interpreter -- has an identity (tag) a tool
// call can read back, so a test can tell "reached the real shared object" from "got nothing."
struct FakeRunner {
    int tag = 0;
};

struct MountArgs { std::string skill_name; };
AE_JSON_SCHEMA(MountArgs, skill_name)
struct MountReply { bool ok = false; };
AE_JSON_SCHEMA(MountReply, ok)

// Sentinel invoke() -- must never actually run; the real logic is the provider's own closure.
struct MountTool : agentengine::Tool<MountTool, agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name        = "mount";
    static constexpr std::string_view description = "Mounts a skill (test double).";
    using Args  = MountArgs;
    using Reply = MountReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(
            agentengine::error{agentengine::failure_class::fatal, "dead path", "test.dead_static_invoke"});
    }
};

struct UseRunnerArgs { bool unused = false; };
AE_JSON_SCHEMA(UseRunnerArgs, unused)
struct UseRunnerReply { int tag = 0; };
AE_JSON_SCHEMA(UseRunnerReply, tag)

struct UseRunnerTool
    : agentengine::Tool<UseRunnerTool, agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name        = "use_runner";
    static constexpr std::string_view description = "Reads the shared runner's tag (test double).";
    using Args  = UseRunnerArgs;
    using Reply = UseRunnerReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(
            agentengine::error{agentengine::failure_class::fatal, "dead path", "test.dead_static_invoke"});
    }
};

// The provider under test: same shape as tools/cli_chat.cpp's ToolDeclaringHistoryProvider.
class TestCodeActProvider {
public:
    [[nodiscard]] agentengine::result<void> configure(
        std::string session_id, agentengine::CodeActRunnerBinding<FakeRunner>& runner_binding) {
        auto bound = runner_binding.bind(session_id);
        if (!bound) return bound;
        runner_binding_ = &runner_binding;
        return {};
    }

    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc,
                                                                              EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools.push_back(agentengine::make_tool_descriptor_with_invoke<MountTool>(
            [this](MountArgs a, EffectContext& ctx) { return real_mount(std::move(a), ctx); }));
        c.tools.push_back(agentengine::make_tool_descriptor_with_invoke<UseRunnerTool>(
            [this](UseRunnerArgs a, EffectContext& ctx) { return real_use_runner(std::move(a), ctx); }));
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

    [[nodiscard]] agentengine::MountedSkillsState const& mounted_skills() const noexcept {
        return mounted_skills_;
    }

private:
    [[nodiscard]] agentengine::result<MountReply> real_mount(MountArgs a, EffectContext&) {
        mounted_skills_.mount(a.skill_name);
        return MountReply{true};
    }

    // CI5's own gate: runner() itself doesn't check WHO is asking -- it's real_use_runner()'s job to
    // refuse reaching it at all when this provider was never successfully configured (its own
    // runner_binding_ stayed null because configure() failed closed).
    [[nodiscard]] agentengine::result<UseRunnerReply> real_use_runner(UseRunnerArgs, EffectContext&) {
        if (runner_binding_ == nullptr) {
            return std::unexpected(
                agentengine::error{agentengine::failure_class::fatal,
                                    "this session was never configured against the shared runner",
                                    "test.codeact_not_configured"});
        }
        return UseRunnerReply{runner_binding_->runner().tag};
    }

    agentengine::MountedSkillsState                  mounted_skills_;
    agentengine::CodeActRunnerBinding<FakeRunner>*   runner_binding_ = nullptr;
};
static_assert(agentengine::ContextProvider<TestCodeActProvider>);

// Scripted ChatClientT: one scripted tool call, then converges.
class CodeActScriptedChatClient {
public:
    CodeActScriptedChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        Message     scripted_call;
        std::size_t call_count = 0;
    };

    void set_scripted_call(Message m) { state_->scripted_call = std::move(m); }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        Message reply = (state_->call_count == 0) ? state_->scripted_call : text_message("done");
        ++state_->call_count;
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<CodeActScriptedChatClient>);

using CodeActSession = AgentSession<CodeActScriptedChatClient, NoSessionState, TestCodeActProvider>;

// ================================================================================================
// D1/D2 fixtures (test_agent_session_delegation.cpp)
// ================================================================================================

// AgentSession default-constructs its ChatClientT member with no external wiring hook -- so the only
// way for a test to observe what EffectContext a call actually received is a static capture slot,
// reset at the top of each scenario below. Tests in this file run strictly sequentially (a single
// main(), no concurrency), so this is safe.
class CapturingChatClient {
public:
    inline static std::optional<Principal> last_seen_principal;

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext& ctx) {
        last_seen_principal = ctx.principal;
        co_return ChatResponse{text_message("reply"), Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }
};
static_assert(agentengine::ChatClient<CapturingChatClient>,
              "CapturingChatClient must satisfy the ChatClient concept (004 §1)");

}  // namespace

int main() {
    // ============================================================================================
    // S1 -- state persists correctly across multiple rounds within one run
    // ============================================================================================
    {
        StatefulSession session;
        session.initialize("s-s1", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({
            tool_call_message("c1", "counter_tool", R"({"delta":3})"),
            tool_call_message("c2", "counter_tool", R"({"delta":4})"),
            // 3rd call falls through to plain text -- run converges at 3 + 4 = 7
        });
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);

        auto r = drive(session.start_run(StartRun{user_message("go")}));
        check(r.has_value(), "S1: the run converges");
        check(client.call_count() == 3, "S1: 3 model calls happened (2 tool rounds + convergence)");

        auto const totals = all_counter_totals(session.history());
        check(totals.size() == 2, "S1: both tool rounds' results landed in history");
        if (totals.size() == 2) {
            check(totals[0] == 3, "S1: round 1's total reflects only its own delta (3)");
            check(totals[1] == 7,
                  "S1: round 2's total is 7 (3+4), not 4 -- the SAME provider instance's own counter_ "
                  "genuinely accumulated across rounds within this one run, proving "
                  "make_tool_descriptor_with_invoke's closure captured real, persistent state");
        }
    }

    // ============================================================================================
    // S2 -- two independent sessions never see each other's state
    // ============================================================================================
    {
        StatefulSession session_a;
        session_a.initialize("s-s2-a", Principal{"p", ""});
        ScriptedChatClient& client_a = session_a.emplace_chat_client();
        client_a.set_script({tool_call_message("c1", "counter_tool", R"({"delta":10})")});
        CapabilitySet const held_a = CapabilitySet::grant_root({});
        session_a.set_capabilities(&held_a);

        StatefulSession session_b;
        session_b.initialize("s-s2-b", Principal{"p", ""});
        ScriptedChatClient& client_b = session_b.emplace_chat_client();
        client_b.set_script({tool_call_message("c1", "counter_tool", R"({"delta":1})")});
        CapabilitySet const held_b = CapabilitySet::grant_root({});
        session_b.set_capabilities(&held_b);

        // Two entirely independent AgentSession instances -- each default-constructs its OWN
        // StatefulCounterProvider member. If counter_ were accidentally shared (a process-global,
        // the exact bug this mechanism exists to avoid), session B's total below would be 11
        // (10+1), not 1.
        auto ra = drive(session_a.start_run(StartRun{user_message("go")}));
        auto rb = drive(session_b.start_run(StartRun{user_message("go")}));
        check(ra.has_value() && rb.has_value(), "S2: both independent sessions converge");

        auto const totals_a = all_counter_totals(session_a.history());
        auto const totals_b = all_counter_totals(session_b.history());
        check(totals_a.size() == 1 && totals_a[0] == 10,
              "S2: session A's counter reflects ONLY its own call (10)");
        check(totals_b.size() == 1 && totals_b[0] == 1,
              "S2: session B's counter reflects ONLY its own call (1), not session A's (would be 11 "
              "if state were accidentally shared) -- per-instance state, never shared, proving "
              "session isolation rather than asserting it");
    }

    // ============================================================================================
    // S5 -- fork_from() copies provider-owned state; clear_in_process_state() wipes it
    // ============================================================================================
    {
        StatefulSession session;
        session.initialize("s-s5-source", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.set_script({tool_call_message("c1", "counter_tool", R"({"delta":5})")});
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        auto r = drive(session.start_run(StartRun{user_message("go")}));
        check(r.has_value(), "S5: source session converges");
        auto const totals_source = all_counter_totals(session.history());
        check(totals_source.size() == 1 && totals_source[0] == 5, "S5: source's counter is 5");

        StatefulSession fork;
        ScriptedChatClient& fork_client = fork.emplace_chat_client();
        fork.fork_from(session, "s-s5-forked");
        fork.set_capabilities(&held);
        fork_client.set_script({tool_call_message("c2", "counter_tool", R"({"delta":2})")});
        auto r2 = drive(fork.start_run(StartRun{user_message("continue")}));
        check(r2.has_value(), "S5: forked session converges");
        auto const totals_forked = all_counter_totals(fork.history());
        check(!totals_forked.empty() && totals_forked.back() == 7,
              "S5: fork_from() copied the source provider's own counter_ (5) -- the new call's total "
              "is 7 (5+2), not 2 -- proving PROVIDER STATE, not just history text, was copied (a "
              "stale/reset provider would have answered 2)");

        fork.clear_in_process_state();
        check(fork.history().empty(), "S5: clear_in_process_state() resets history");
        fork.initialize("s-s5-after-clear", Principal{"p", ""});
        // capabilities_/chat_client_ wiring deliberately survives clear_in_process_state() (both stay
        // "configuration," never per-session data) -- reuse the SAME client, fresh script.
        fork_client.reset_call_count();
        fork_client.set_script({tool_call_message("c3", "counter_tool", R"({"delta":1})")});
        auto r3 = drive(fork.start_run(StartRun{user_message("go")}));
        check(r3.has_value(), "S5: post-clear session converges");
        auto const totals_after_clear = all_counter_totals(fork.history());
        check(!totals_after_clear.empty() && totals_after_clear.back() == 1,
              "S5: after clear_in_process_state(), counter_tool starts fresh at 1, not 8 (which would "
              "mean the deleted session's provider state, counter_=7, survived) -- "
              "clear_in_process_state() genuinely reset provider-owned state, no residue (005 §6)");
    }

    // ============================================================================================
    // S3 -- captures_session_state + Backgroundable is refused, structurally
    // ============================================================================================
    {
        auto desc = agentengine::make_tool_descriptor_with_invoke<BackgroundableStatefulTool>(
            [](BgArgs, EffectContext&) -> agentengine::result<BgReply> { return BgReply{}; });
        check(desc.captures_session_state, "S3: the descriptor is correctly marked state-capturing");
        check(desc.backgroundable,
              "S3: BackgroundableStatefulTool's OWN Backgroundable declaration still comes through "
              "make_tool_descriptor_with_invoke unchanged (it is not silently cleared)");

        agentengine::ToolTable const table = agentengine::ToolTable::from_descriptors({desc});
        CapabilitySet const held = CapabilitySet::grant_root({});
        agentengine::ToolCallRequest const req{"c-bg", "bg_stateful_tool",
                                                agentengine::json::Value::make_object({}),
                                                /*arguments_tainted=*/false, 0};
        EffectContext ctx;
        bool completion_called = false;

        auto submitted = agentengine::background_task(
            table, held, req, ctx, /*approve=*/{}, /*current_background_count=*/0,
            [&completion_called](ToolResult, agentengine::ToolInvocationAudit) { completion_called = true; });

        check(!submitted.has_value(),
              "S3: background_task() refuses a captures_session_state descriptor even though it "
              "declares Backgroundable -- the fatal dangling-reference hazard closed structurally");
        check(submitted.has_value() ||
                  submitted.error().code == "tool.state_capturing_not_backgroundable",
              "S3: the refusal carries the specific new error code, not a generic denial");
        check(!completion_called, "S3: no thread was ever detached -- the completion callback never fired");
    }

    // ============================================================================================
    // S4 -- make_tool_descriptor<ToolT>() (unmodified path) is unaffected
    // ============================================================================================
    {
        auto stateless_desc = agentengine::make_tool_descriptor<BackgroundableStatefulTool>();
        check(!stateless_desc.captures_session_state,
              "S4: the ordinary make_tool_descriptor<ToolT>() path defaults captures_session_state to "
              "false, exactly as before this change");
        check(stateless_desc.backgroundable,
              "S4: the tool's own Backgroundable declaration is still correctly extracted");

        agentengine::ToolTable const table = agentengine::ToolTable::from_descriptors({stateless_desc});
        CapabilitySet const held =
            CapabilitySet::grant_root({agentengine::Capability{agentengine::cap::Background{4}}});
        agentengine::ToolCallRequest const req{"c-ok", "bg_stateful_tool",
                                                agentengine::json::Value::make_object({}),
                                                /*arguments_tainted=*/false, 0};
        EffectContext ctx;
        bool completion_called = false;

        auto submitted = agentengine::background_task(
            table, held, req, ctx, /*approve=*/{}, /*current_background_count=*/0,
            [&completion_called](ToolResult, agentengine::ToolInvocationAudit) { completion_called = true; });

        check(submitted.has_value(),
              "S4: an ordinary stateless Backgroundable tool still backgrounds successfully -- no "
              "regression from S3's new guard");
        // Detached worker thread completes asynchronously -- give it a moment before checking.
        for (int i = 0; i < 200 && !completion_called; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(completion_called, "S4: the background thread actually ran and completed");
    }

    // ============================================================================================
    // CI2/CI3 -- session A claims the shared binding first; session B is rejected
    // ============================================================================================
    FakeRunner shared_runner{99};
    agentengine::CodeActRunnerBinding<FakeRunner> shared_binding(shared_runner);

    CodeActSession kit_a;
    CodeActSession kit_b;
    kit_a.initialize("s-a", Principal{"p", ""});
    kit_b.initialize("s-b", Principal{"p", ""});
    CapabilitySet const held_ab = CapabilitySet::grant_root({});
    kit_a.set_capabilities(&held_ab);
    kit_b.set_capabilities(&held_ab);

    auto configured_a = kit_a.history_provider().configure("s-a", shared_binding);
    check(configured_a.has_value(),
          "CI2: session A's configure() against the shared binding succeeds (it claims first)");
    auto configured_b = kit_b.history_provider().configure("s-b", shared_binding);
    check(!configured_b.has_value(),
          "CI3: session B's configure() against the SAME already-bound binding fails closed -- the "
          "exact cross-session hazard (unsynchronized runner internals reachable from two sessions at "
          "once) this ADR exists to make structurally impossible");

    // ============================================================================================
    // CI1 -- mounting in A never appears in B
    // ============================================================================================
    CodeActScriptedChatClient& client_a = kit_a.emplace_chat_client();
    client_a.set_scripted_call(tool_call_message("c1", "mount", R"({"skill_name":"demo"})"));
    auto r_a = drive(kit_a.start_run(StartRun{user_message("go")}));
    check(r_a.has_value(), "CI1 setup: session A's mount call converges");
    check(kit_a.history_provider().mounted_skills().is_mounted("demo"),
          "CI1: session A's own MountedSkillsState reflects the mount it just made");
    check(!kit_b.history_provider().mounted_skills().is_mounted("demo"),
          "CI1: session B's MountedSkillsState is COMPLETELY UNAWARE of session A's mount -- real "
          "per-session isolation, not a shared static (the actual payoff of this ADR)");

    // ============================================================================================
    // CI4 -- session A (successfully configured) can reach the shared runner
    // ============================================================================================
    CodeActScriptedChatClient& client_a2 = kit_a.emplace_chat_client();
    client_a2.set_scripted_call(tool_call_message("c2", "use_runner", R"({"unused":false})"));
    auto r_a2 = drive(kit_a.start_run(StartRun{user_message("go again")}));
    check(r_a2.has_value(), "CI4: session A's use_runner call converges");
    bool found_tag_99 = false;
    for (auto const& msg : kit_a.history()) {
        for (auto const& item : msg.content) {
            if (auto const* tr = std::get_if<ToolResult>(&item.value)) {
                if (!tr->is_error) found_tag_99 = true;
            }
        }
    }
    check(found_tag_99,
          "CI4: session A's use_runner call succeeded (not denied) -- it genuinely reached the shared "
          "runner through its own provider");

    // ============================================================================================
    // CI5 -- session B (rejected configure()) fails closed reaching the runner
    // ============================================================================================
    CodeActScriptedChatClient& client_b = kit_b.emplace_chat_client();
    client_b.set_scripted_call(tool_call_message("c3", "use_runner", R"({"unused":false})"));
    auto r_b = drive(kit_b.start_run(StartRun{user_message("go")}));
    check(r_b.has_value(),
          "CI5 setup: session B's run still converges (a tool error is fed back, not a run failure)");
    bool b_saw_denial = false;
    for (auto const& msg : kit_b.history()) {
        for (auto const& item : msg.content) {
            if (auto const* tr = std::get_if<ToolResult>(&item.value)) {
                if (tr->is_error) b_saw_denial = true;
            }
        }
    }
    check(b_saw_denial,
          "CI5: session B's use_runner call was denied -- its own runner_binding_ stayed null because "
          "configure() never succeeded, so it can never reach the shared runner at all");

    // ============================================================================================
    // D1 -- a root (non-delegated) session's outbound call carries exactly its own principal
    // ============================================================================================
    {
        CapturingChatClient::last_seen_principal.reset();

        using DelegationSession = AgentSession<CapturingChatClient>;
        DelegationSession session;
        Principal const owner = agentengine::make_embedded_principal("p-owner", "tenant-a");
        session.initialize("s-root", owner);
        session.emplace_chat_client();

        auto r = drive(session.start_run(StartRun{user_message("hello")}));
        check(r.has_value(), "D1-R1: the run itself succeeds");
        check(CapturingChatClient::last_seen_principal.has_value(),
              "D1-R2: chat() was reached and captured an EffectContext::principal");
        check(*CapturingChatClient::last_seen_principal == owner,
              "D1-R3: the outbound call's EffectContext::principal is EXACTLY the session's own "
              "owning principal -- previously (before H4) this was always a default-constructed, "
              "empty Principal{} regardless of who owned the session");
    }

    // ============================================================================================
    // D2 -- a session initialized with a derived on_behalf_of principal threads it through
    // ============================================================================================
    {
        CapturingChatClient::last_seen_principal.reset();

        Principal const parent = agentengine::make_embedded_principal("p-parent", "tenant-a");
        auto derived = agentengine::derive_on_behalf_of(parent, "sub-agent-1");
        check(derived.has_value(), "D2-R4: deriving the delegated principal succeeds");

        using DelegationSession = AgentSession<CapturingChatClient>;
        DelegationSession session;
        session.initialize("s-delegated", *derived);
        session.emplace_chat_client();

        auto r = drive(session.start_run(StartRun{user_message("hello")}));
        check(r.has_value(), "D2-R5: the delegated session's run succeeds");
        check(CapturingChatClient::last_seen_principal.has_value(),
              "D2-R6: chat() was reached for the delegated session too");

        Principal const& seen = *CapturingChatClient::last_seen_principal;
        check(seen.id == "sub-agent-1",
              "D2-R7: the outbound call carries the DERIVED id, not the parent's id -- the sub-agent "
              "runs as its own principal, never as the host (007 §2)");
        check(seen.on_behalf_of == "p-parent",
              "D2-R8: the outbound call names exactly who this delegated call is acting for -- the "
              "real on_behalf_of expression 018 §1 asks for instead of token passthrough");
        check(seen.tenant_id == "tenant-a",
              "D2-R9: tenant is preserved unchanged across delegation -- never elevated (018 §6)");
        check(seen.kind == agentengine::principal_kind::agent,
              "D2-R10: a delegated principal's kind is always agent, never re-labeled as the more-"
              "trusted human/service the parent might have been");
        check(seen.delegation_depth == 1, "D2-R11: delegation_depth reflects exactly one hop from the parent");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_tooling_and_delegation: ALL PASS\n");
    return 0;
}
