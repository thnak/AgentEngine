// Proves ADR-028's general mechanism for session-scoped stateful tools:
// `make_tool_descriptor_with_invoke<ToolT>()` (tool_pipeline.hpp) lets a `ContextProvider` build a
// tool descriptor whose `invoke` closure captures a reference into the PROVIDER's OWN
// session-scoped state (an ordinary member -- no core-seam change, `HistoryProviderT` is already a
// per-`AgentSession`-instance member) instead of `ToolT`'s static `invoke`. Deterministic, offline
// (no live model, no network) -- a scripted `ChatClientT` test double drives every scenario.
//
// Covers:
//   S1 -- a stateful tool's state persists correctly across multiple rounds within one run.
//   S2 -- two independent AgentSession instances (two independent provider instances) never see
//         each other's state -- proving per-session isolation, not asserting it.
//   S3 -- the must-fix #1 regression: a descriptor with both `captures_session_state` and
//         `backgroundable` set is refused by `background_task()`, structurally, before step 8.
//   S4 -- `make_tool_descriptor<ToolT>()` (the existing, unmodified path) is completely
//         unaffected -- `captures_session_state` defaults false, ordinary stateless tools are
//         still backgroundable exactly as before.

#include <chrono>
#include <iostream>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
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

// ---- CounterTool: declared as a real Tool<> purely for its compile-time-checked declarations
// (Capabilities/EffectClass) -- its OWN static invoke() is deliberately wrong (returns a sentinel
// no test path should ever see), proving `make_tool_descriptor_with_invoke` genuinely runs the
// CALLER's callable instead of `CounterTool::invoke`, not just in addition to it. --------------

struct CounterArgs { int delta = 0; };
AE_JSON_SCHEMA(CounterArgs, delta)
struct CounterReply { int total = 0; };
AE_JSON_SCHEMA(CounterReply, total)

struct CounterTool : ae::Tool<CounterTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "counter_tool";
    static constexpr std::string_view description = "Adds delta to a session-scoped counter.";
    using Args = CounterArgs;
    using Reply = CounterReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) {
        return Reply{-999};  // sentinel: must never appear in any test's observed result
    }
};

// A second tool, declared Backgroundable, for S3 -- an ordinary stateless tool would legitimately
// background; this one gets its descriptor built via make_tool_descriptor_with_invoke instead, to
// prove that combination is refused regardless of the tool's own declared capabilities.
struct BgArgs { bool unused = false; };
AE_JSON_SCHEMA(BgArgs, unused)
struct BgReply { bool unused = false; };
AE_JSON_SCHEMA(BgReply, unused)

struct BackgroundableStatefulTool
    : ae::Tool<BackgroundableStatefulTool, ae::Capabilities<>, ae::Backgroundable,
               ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "bg_stateful_tool";
    static constexpr std::string_view description = "Declared Backgroundable, for S3.";
    using Args = BgArgs;
    using Reply = BgReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) { return Reply{}; }
};

// ---- The state-owning ContextProvider: `counter_` is an ORDINARY MEMBER (no core-seam change,
// no StateT threading) -- exactly the pattern SkillsProvider/HistoryAndSkillsProvider already
// establish for provider-owned, cross-turn-persistent state. ------------------------------------

class StatefulCounterProvider {
public:
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc,
                                                                             ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools.push_back(ae::make_tool_descriptor_with_invoke<CounterTool>(
            [this](CounterArgs a, ae::EffectContext&) -> ae::result<CounterReply> {
                counter_ += a.delta;
                return CounterReply{counter_};
            }));
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }

private:
    int counter_ = 0;  // session-scoped state -- one instance per AgentSession, per-instance member
};
static_assert(ae::ContextProvider<StatefulCounterProvider>);

// ---- Scripted ChatClientT: a queue of pre-built tool-call responses, consumed in order ---------

class ScriptedChatClient {
public:
    std::vector<ae::Message> scripted_responses;
    std::size_t call_count = 0;

    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        ae::Message reply;
        if (call_count < scripted_responses.size()) {
            reply = scripted_responses[call_count];
        } else {
            reply = make_text_message("done");
        }
        ++call_count;
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;  // generous enough that a small scripted response never blocks on credit
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::Message reply;
        if (call_count < scripted_responses.size()) {
            reply = scripted_responses[call_count];
        } else {
            reply = make_text_message("done");
        }
        ++call_count;
        ae::ChatResponseUpdate upd;
        if (!reply.content.empty()) upd.delta = reply.content.front();
        upd.is_final = true;
        upd.usage    = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
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

// Extracts CounterTool's `total` reply from a `role::tool` history message -- the black-box way to
// observe the provider's own internal `counter_` after the fact, without needing any new
// introspection accessor on `AgentSession` (out of this ADR's own scope).
[[nodiscard]] std::optional<int> counter_total_of(ae::Message const& m) {
    for (ae::ContentItem const& item : m.content) {
        auto const* tr = std::get_if<ae::ToolResult>(&item.value);
        if (!tr || tr->is_error || tr->content.empty()) continue;
        auto const* d = std::get_if<ae::Data>(&tr->content[0].value);
        if (!d) continue;
        auto parsed = ae::json::parse(d->json);
        if (!parsed) continue;
        auto const* t = parsed->find("total");
        if (t && t->is_number()) return static_cast<int>(t->as_number());
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<int> all_counter_totals(std::vector<ae::Message> const& history) {
    std::vector<int> totals;
    for (auto const& m : history) {
        if (m.role != ae::role::tool) continue;
        if (auto t = counter_total_of(m)) totals.push_back(*t);
    }
    return totals;
}

using Session = ae::AgentSession<ScriptedChatClient, ae::NoSessionState, StatefulCounterProvider>;

}  // namespace

int main() {
    // ---- S1: state persists correctly across multiple rounds within one run -------------------
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c1", "counter_tool", R"({"delta":3})"),
            ScriptedChatClient::make_tool_call_message("c2", "counter_tool", R"({"delta":4})"),
            // 3rd call falls through to plain text -- run converges at 3 + 4 = 7
        };
        kit.actor().initialize("s-s1", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(r.has_value(), "S1: the run converges");
        AE_CHECK(client.call_count == 3, "S1: 3 model calls happened (2 tool rounds + convergence)");

        auto const totals = all_counter_totals(kit.actor().history());
        AE_CHECK(totals.size() == 2, "S1: both tool rounds' results landed in history");
        if (totals.size() == 2) {
            AE_CHECK(totals[0] == 3, "S1: round 1's total reflects only its own delta (3)");
            AE_CHECK(totals[1] == 7,
                     "S1: round 2's total is 7 (3+4), not 4 -- the SAME provider instance's own "
                     "counter_ genuinely accumulated across rounds within this one run, proving "
                     "make_tool_descriptor_with_invoke's closure captured real, persistent state");
        }
    }

    // ---- S2: two independent sessions never see each other's state ----------------------------
    {
        quark::TestKit<Session> kit_a;
        auto& client_a = kit_a.actor().emplace_chat_client();
        client_a.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c1", "counter_tool", R"({"delta":10})")};
        kit_a.actor().initialize("s-s2-a", ae::Principal{"p", ""});
        ae::CapabilitySet const held_a = ae::CapabilitySet::grant_root({});
        kit_a.actor().set_capabilities(&held_a);

        quark::TestKit<Session> kit_b;
        auto& client_b = kit_b.actor().emplace_chat_client();
        client_b.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c1", "counter_tool", R"({"delta":1})")};
        kit_b.actor().initialize("s-s2-b", ae::Principal{"p", ""});
        ae::CapabilitySet const held_b = ae::CapabilitySet::grant_root({});
        kit_b.actor().set_capabilities(&held_b);

        // Two entirely independent AgentSession instances -- each default-constructs its OWN
        // StatefulCounterProvider member (the same shape a real multi-session production host's
        // per-session AgentSession instances would have). If `counter_` were accidentally shared
        // (a process-global, the exact bug this mechanism exists to avoid), session B's total
        // below would be 11 (10+1), not 1.
        auto ra = kit_a.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        auto rb = kit_b.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(ra.has_value() && rb.has_value(), "S2: both independent sessions converge");

        auto const totals_a = all_counter_totals(kit_a.actor().history());
        auto const totals_b = all_counter_totals(kit_b.actor().history());
        AE_CHECK(totals_a.size() == 1 && totals_a[0] == 10,
                 "S2: session A's counter reflects ONLY its own call (10)");
        AE_CHECK(totals_b.size() == 1 && totals_b[0] == 1,
                 "S2: session B's counter reflects ONLY its own call (1), not session A's (would be "
                 "11 if state were accidentally shared) -- per-instance state, never shared, "
                 "proving session isolation rather than asserting it");
    }

    // ---- S5: fork_from() copies provider-owned state; clear_in_process_state() wipes it ---------
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c1", "counter_tool", R"({"delta":5})")};
        kit.actor().initialize("s-s5-source", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(r.has_value(), "S5: source session converges");
        auto const totals_source = all_counter_totals(kit.actor().history());
        AE_CHECK(totals_source.size() == 1 && totals_source[0] == 5, "S5: source's counter is 5");

        quark::TestKit<Session> fork_kit;
        auto& fork_client = fork_kit.actor().emplace_chat_client();
        fork_kit.actor().fork_from(kit.actor(), "s-s5-forked");
        fork_kit.actor().set_capabilities(&held);
        fork_client.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c2", "counter_tool", R"({"delta":2})")};
        auto r2 = fork_kit.ask<ae::AgentResponse>(ae::StartRun{user_message("continue")});
        AE_CHECK(r2.has_value(), "S5: forked session converges");
        auto const totals_forked = all_counter_totals(fork_kit.actor().history());
        AE_CHECK(!totals_forked.empty() && totals_forked.back() == 7,
                 "S5: fork_from() copied the source provider's own counter_ (5) -- the new call's "
                 "total is 7 (5+2), not 2 -- proving PROVIDER STATE, not just history text, was "
                 "copied (a stale/reset provider would have answered 2)");

        fork_kit.actor().clear_in_process_state();
        AE_CHECK(fork_kit.actor().history().empty(), "S5: clear_in_process_state() resets history");
        fork_kit.actor().initialize("s-s5-after-clear", ae::Principal{"p", ""});
        // capabilities_/chat_client_ wiring deliberately survives clear_in_process_state() (both
        // stay "configuration," never per-session data) -- reuse the SAME client, fresh script.
        fork_client.call_count = 0;
        fork_client.scripted_responses = {
            ScriptedChatClient::make_tool_call_message("c3", "counter_tool", R"({"delta":1})")};
        auto r3 = fork_kit.ask<ae::AgentResponse>(ae::StartRun{user_message("go")});
        AE_CHECK(r3.has_value(), "S5: post-clear session converges");
        auto const totals_after_clear = all_counter_totals(fork_kit.actor().history());
        AE_CHECK(!totals_after_clear.empty() && totals_after_clear.back() == 1,
                 "S5: after clear_in_process_state(), counter_tool starts fresh at 1, not 8 (which "
                 "would mean the deleted session's provider state, counter_=7, survived) -- "
                 "clear_in_process_state() genuinely reset provider-owned state, no residue (005 §6)");
    }

    // ---- S3: captures_session_state + Backgroundable is refused, structurally -----------------
    {
        auto desc = ae::make_tool_descriptor_with_invoke<BackgroundableStatefulTool>(
            [](BgArgs, ae::EffectContext&) -> ae::result<BgReply> { return BgReply{}; });
        AE_CHECK(desc.captures_session_state, "S3: the descriptor is correctly marked state-capturing");
        AE_CHECK(desc.backgroundable,
                 "S3: BackgroundableStatefulTool's OWN Backgroundable declaration still comes "
                 "through make_tool_descriptor_with_invoke unchanged (it is not silently cleared)");

        ae::ToolTable const table = ae::ToolTable::from_descriptors({desc});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        ae::ToolCallRequest const req{"c-bg", "bg_stateful_tool", ae::json::Value::make_object({}),
                                       /*arguments_tainted=*/false, 0};
        ae::EffectContext ctx;
        bool completion_called = false;

        auto submitted = ae::background_task(
            table, held, req, ctx, /*approve=*/{}, /*current_background_count=*/0,
            [&completion_called](ae::ToolResult, ae::ToolInvocationAudit) { completion_called = true; });

        AE_CHECK(!submitted.has_value(),
                 "S3: background_task() refuses a captures_session_state descriptor even though it "
                 "declares Backgroundable -- the fatal dangling-reference hazard closed structurally");
        AE_CHECK(submitted.has_value() || submitted.error().code == "tool.state_capturing_not_backgroundable",
                 "S3: the refusal carries the specific new error code, not a generic denial");
        AE_CHECK(!completion_called,
                 "S3: no thread was ever detached -- the completion callback never fired");
    }

    // ---- S4: make_tool_descriptor<ToolT>() (unmodified path) is unaffected --------------------
    {
        auto stateless_desc = ae::make_tool_descriptor<BackgroundableStatefulTool>();
        AE_CHECK(!stateless_desc.captures_session_state,
                 "S4: the ordinary make_tool_descriptor<ToolT>() path defaults captures_session_state "
                 "to false, exactly as before this change");
        AE_CHECK(stateless_desc.backgroundable,
                 "S4: the tool's own Backgroundable declaration is still correctly extracted");

        ae::ToolTable const table = ae::ToolTable::from_descriptors({stateless_desc});
        ae::CapabilitySet const held =
            ae::CapabilitySet::grant_root({ae::Capability{ae::cap::Background{4}}});
        ae::ToolCallRequest const req{"c-ok", "bg_stateful_tool", ae::json::Value::make_object({}),
                                       /*arguments_tainted=*/false, 0};
        ae::EffectContext ctx;
        bool completion_called = false;

        auto submitted = ae::background_task(
            table, held, req, ctx, /*approve=*/{}, /*current_background_count=*/0,
            [&completion_called](ae::ToolResult, ae::ToolInvocationAudit) { completion_called = true; });

        AE_CHECK(submitted.has_value(),
                 "S4: an ordinary stateless Backgroundable tool still backgrounds successfully -- "
                 "no regression from S3's new guard");
        // Detached worker thread completes asynchronously -- give it a moment before checking.
        for (int i = 0; i < 200 && !completion_called; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        AE_CHECK(completion_called, "S4: the background thread actually ran and completed");
    }

    std::cout << (g_failures == 0 ? "test_session_scoped_stateful_tools: OK\n"
                                   : "test_session_scoped_stateful_tools: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
