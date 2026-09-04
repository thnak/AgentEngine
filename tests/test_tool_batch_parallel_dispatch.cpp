// Implements decisions/ADR-160-parallel-tool-batch-scheduler.md §5. `test_tool_batch_partition.cpp`
// already proves `partition_batch()`'s pure logic in isolation; THIS file is the real, executed proof
// that `AgentSession::dispatch_tool_calls()` actually runs an eligible batch CONCURRENTLY, end to end,
// through a real `AgentSession` round -- not merely that it compiles or that the sequential fallback
// is unaffected (the full existing suite already re-proves that). Three claims, each demonstrated by
// direct observation rather than inference:
//
//   1. Two independent `Parallelizable` calls in one batch genuinely execute on different OS threads
//      at the same time (a rendezvous: each blocks until it observes the OTHER has started -- only
//      possible if both are actually running concurrently, not one after the other).
//   2. Two `ExclusivityGroup<Name>` calls sharing one name NEVER run concurrently with each other
//      (§5 MUST-FIX 5's "one job per group, not per member" -- verified by a shared, instrumented
//      concurrent-count high-water mark, never observed above 1).
//   3. `history_`'s appended `ToolResult` order is always the model's EMITTED order, regardless of
//      which call PHYSICALLY completed first -- proven by making the first-emitted call block on a
//      rendezvous signal the second-emitted call sets after it finishes (same `wait_up_to()` idiom
//      as claim 1), so real completion is DETERMINISTICALLY out of emitted order, and directly
//      observing the appended history is nonetheless in emitted order every time (006 §5 / G4's own
//      claim). An earlier revision raced two `sleep_for()` calls (15ms vs 1ms) instead -- that
//      assumption broke under real CI load: GitHub-hosted Windows runners' ~15.6ms default timer
//      granularity means `sleep_for(1ms)` can legitimately take nearly as long as `sleep_for(15ms)`,
//      which is exactly what happened (2026-09-04): this test failed in the fast Release legs (where
//      nothing else pads out an iteration) but passed in both ASan legs (slow enough to swamp the
//      jitter), the signature of a margin eaten by scheduler granularity, not a real concurrency
//      defect. The rendezvous is bounded (2s timeout, same ceiling claim 1 uses), so a genuinely
//      broken (secretly-sequential) dispatch path still fails loudly instead of hanging.
//
// NOT the literal "10^4 randomized completions" G4 asks for (ADR-160 §6: named as a still-open
// question whether that scale is CI-practical) -- 64 iterations here, real `AgentSession` round-trips
// each time.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::StartRun;
using agentengine::task;

using agentengine::Capabilities;
using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::ContextContribution;
using agentengine::EffectContext;
using agentengine::EffectClass;
using agentengine::Message;
using agentengine::Parallelizable;
using agentengine::Principal;
using agentengine::SessionContext;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::ToolResult;
using agentengine::ToolTable;
using agentengine::TurnView;
using agentengine::Usage;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::effect_class;
using agentengine::role;

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

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message text_response(std::string text) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

// One assistant message carrying MULTIPLE tool calls -- a real "batch" in one turn.
Message multi_tool_call_response(
        std::vector<std::tuple<std::string, std::string, std::string>> calls) {
    Message m;
    m.role = role::assistant;
    for (auto& [call_id, tool_name, args] : calls) {
        ContentItem item;
        item.origin = content_origin::assistant;
        ToolCall call;
        call.call_id       = std::move(call_id);
        call.tool_name      = std::move(tool_name);
        call.arguments_json = std::move(args);
        call.provenance     = call_provenance::vendor_structured;
        item.value = call;
        m.content.push_back(std::move(item));
    }
    return m;
}

// Returns the call_ids of every ToolResult in `history`, in the order they appear in the FIRST
// role::tool message found -- i.e. the order dispatch_tool_calls() actually appended them.
std::vector<std::string> tool_result_call_id_order(std::vector<Message> const& history) {
    for (Message const& m : history) {
        if (m.role != role::tool) continue;
        std::vector<std::string> order;
        for (ContentItem const& item : m.content) {
            if (auto const* r = std::get_if<ToolResult>(&item.value)) order.push_back(r->call_id);
        }
        return order;
    }
    return {};
}

struct ScriptedOutcome {
    Message message;
    Usage usage;
};

class RecordingChatClient {
public:
    RecordingChatClient() : state_(std::make_shared<State>()) {}
    struct State {
        std::vector<ScriptedOutcome> script;
        std::size_t call_count = 0;
    };
    void set_script(std::vector<ScriptedOutcome> script) { state_->script = std::move(script); }
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        std::size_t const idx = state_->call_count < state_->script.size()
                                     ? state_->call_count
                                     : state_->script.size() - 1;
        ScriptedOutcome const& o = state_->script[idx];
        ++state_->call_count;
        co_return ChatResponse{o.message, o.usage};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<RecordingChatClient>);

template <class... Tools>
class ToolsHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& ctx,
                                                                              EffectContext&) {
        ContextContribution c;
        c.messages.assign(ctx.history.begin(), ctx.history.end());
        c.tools = ToolTable::from_tools<Tools...>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};

struct NoArgs { bool ignored = false; };
AE_JSON_SCHEMA(NoArgs, ignored)
struct TagReply { std::string tag; };
AE_JSON_SCHEMA(TagReply, tag)

// -- Claim 1 fixture: rendezvous -- each blocks until it observes the OTHER has started -----------
std::atomic<bool> g_a_started{false};
std::atomic<bool> g_b_started{false};
std::atomic<bool> g_a_saw_b{false};
std::atomic<bool> g_b_saw_a{false};

bool wait_up_to(std::atomic<bool> const& flag, std::chrono::milliseconds timeout) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load(std::memory_order_acquire);
}

struct RendezvousToolA : agentengine::Tool<RendezvousToolA, Capabilities<>,
                                             EffectClass<effect_class::pure>, Parallelizable> {
    static constexpr std::string_view name = "rendezvous_a";
    static constexpr std::string_view description = "Blocks until rendezvous_b has also started.";
    using Args = NoArgs;
    using Reply = TagReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        g_a_started.store(true, std::memory_order_release);
        if (wait_up_to(g_b_started, std::chrono::milliseconds(2000))) {
            g_a_saw_b.store(true, std::memory_order_release);
        }
        return Reply{"a"};
    }
};
struct RendezvousToolB : agentengine::Tool<RendezvousToolB, Capabilities<>,
                                             EffectClass<effect_class::pure>, Parallelizable> {
    static constexpr std::string_view name = "rendezvous_b";
    static constexpr std::string_view description = "Blocks until rendezvous_a has also started.";
    using Args = NoArgs;
    using Reply = TagReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        g_b_started.store(true, std::memory_order_release);
        if (wait_up_to(g_a_started, std::chrono::milliseconds(2000))) {
            g_b_saw_a.store(true, std::memory_order_release);
        }
        return Reply{"b"};
    }
};

// -- Claim 2 fixture: two members of the SAME ExclusivityGroup<Name> must never overlap -----------
std::atomic<int> g_group_concurrent{0};
std::atomic<int> g_group_max_concurrent{0};

struct GroupToolX : agentengine::Tool<GroupToolX, Capabilities<>, EffectClass<effect_class::pure>,
                                        agentengine::ExclusivityGroup<"db-write">> {
    static constexpr std::string_view name = "group_x";
    static constexpr std::string_view description = "ExclusivityGroup<db-write> member 1.";
    using Args = NoArgs;
    using Reply = TagReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        int const now = g_group_concurrent.fetch_add(1, std::memory_order_acq_rel) + 1;
        int prev = g_group_max_concurrent.load(std::memory_order_relaxed);
        while (now > prev &&
               !g_group_max_concurrent.compare_exchange_weak(prev, now, std::memory_order_acq_rel)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        g_group_concurrent.fetch_sub(1, std::memory_order_acq_rel);
        return Reply{"x"};
    }
};
struct GroupToolY : agentengine::Tool<GroupToolY, Capabilities<>, EffectClass<effect_class::pure>,
                                        agentengine::ExclusivityGroup<"db-write">> {
    static constexpr std::string_view name = "group_y";
    static constexpr std::string_view description = "ExclusivityGroup<db-write> member 2.";
    using Args = NoArgs;
    using Reply = TagReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        int const now = g_group_concurrent.fetch_add(1, std::memory_order_acq_rel) + 1;
        int prev = g_group_max_concurrent.load(std::memory_order_relaxed);
        while (now > prev &&
               !g_group_max_concurrent.compare_exchange_weak(prev, now, std::memory_order_acq_rel)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        g_group_concurrent.fetch_sub(1, std::memory_order_acq_rel);
        return Reply{"y"};
    }
};

// -- Claim 3 fixture: append order vs. completion order -- delay_a (first-emitted) blocks on a
// rendezvous signal delay_b (second-emitted) sets after IT finishes, so real completion order is
// DETERMINISTICALLY [b, a] regardless of OS scheduler timing jitter (see file banner). Reset before
// each iteration below, same as `g_completion_order`.
std::atomic<bool> g_delay_b_done{false};

std::mutex g_completion_mutex;
std::vector<std::string> g_completion_order;  // cleared before each iteration below

struct DelayToolA : agentengine::Tool<DelayToolA, Capabilities<>, EffectClass<effect_class::pure>,
                                        Parallelizable> {
    static constexpr std::string_view name = "delay_a";
    static constexpr std::string_view description =
            "Waits for delay_b to signal completion, then records its own completion.";
    using Args = NoArgs;
    using Reply = TagReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        wait_up_to(g_delay_b_done, std::chrono::milliseconds(2000));
        std::lock_guard<std::mutex> lock(g_completion_mutex);
        g_completion_order.push_back("a");
        return Reply{"a"};
    }
};
struct DelayToolB : agentengine::Tool<DelayToolB, Capabilities<>, EffectClass<effect_class::pure>,
                                        Parallelizable> {
    static constexpr std::string_view name = "delay_b";
    static constexpr std::string_view description = "Records completion, then signals delay_a.";
    using Args = NoArgs;
    using Reply = TagReply;
    static agentengine::result<Reply> invoke(Args, EffectContext&) {
        {
            std::lock_guard<std::mutex> lock(g_completion_mutex);
            g_completion_order.push_back("b");
        }
        g_delay_b_done.store(true, std::memory_order_release);
        return Reply{"b"};
    }
};

}  // namespace

int main() {
    Principal const owner = agentengine::make_embedded_principal("p1", "t1");

    // === Claim 1: real concurrency between two independent Parallelizable calls =================
    {
        using Session = AgentSession<RecordingChatClient, NoSessionState,
                                       ToolsHistoryProvider<RendezvousToolA, RendezvousToolB>>;
        Session session;
        session.initialize("rendezvous", owner);
        session.emplace_chat_client().set_script({
            ScriptedOutcome{multi_tool_call_response(
                                {{"c1", "rendezvous_a", "{\"ignored\":false}"}, {"c2", "rendezvous_b", "{\"ignored\":false}"}}),
                            Usage{}},
            ScriptedOutcome{text_response("done"), Usage{}},
        });

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(), "CLAIM 1: a batch of two Parallelizable calls completes the run");
        check(g_a_saw_b.load() && g_b_saw_a.load(),
              "CLAIM 1: rendezvous_a and rendezvous_b each observed the OTHER had already started "
              "-- only possible if both genuinely ran concurrently on different threads, not one "
              "after the other");
        check(tool_result_call_id_order(session.history()) == std::vector<std::string>{"c1", "c2"},
              "CLAIM 1: history still appends results in the model's emitted order (c1 then c2)");
    }

    // === Claim 2: ExclusivityGroup<Name> members never overlap (MUST-FIX 5) ======================
    {
        using Session = AgentSession<RecordingChatClient, NoSessionState,
                                       ToolsHistoryProvider<GroupToolX, GroupToolY>>;
        Session session;
        session.initialize("group", owner);
        session.emplace_chat_client().set_script({
            ScriptedOutcome{
                multi_tool_call_response({{"c1", "group_x", "{\"ignored\":false}"}, {"c2", "group_y", "{\"ignored\":false}"}}),
                Usage{}},
            ScriptedOutcome{text_response("done"), Usage{}},
        });

        auto outcome = drive(session.start_run(StartRun{user_message("go")}));
        check(outcome.has_value(), "CLAIM 2: a batch of two same-group calls completes the run");
        check(g_group_max_concurrent.load() == 1,
              "CLAIM 2: group_x and group_y (same ExclusivityGroup<\"db-write\">) were NEVER "
              "observed running at the same time -- the high-water mark of concurrent members "
              "never exceeded 1, proving the whole group ran as one job's internal sequential loop");
        check(tool_result_call_id_order(session.history()) == std::vector<std::string>{"c1", "c2"},
              "CLAIM 2: history still appends results in the model's emitted order (c1 then c2)");
    }

    // === Claim 3: append order survives out-of-emitted-order PHYSICAL completion ================
    {
        constexpr int kIterations = 64;
        int out_of_order_completions_observed = 0;
        for (int i = 0; i < kIterations; ++i) {
            g_completion_order.clear();
            g_delay_b_done.store(false, std::memory_order_release);
            using Session = AgentSession<RecordingChatClient, NoSessionState,
                                           ToolsHistoryProvider<DelayToolA, DelayToolB>>;
            Session session;
            session.initialize("delay-" + std::to_string(i), owner);
            // delay_a (emitted FIRST, call c1) blocks until delay_b (emitted SECOND, call c2) signals
            // its own completion -- real completion order is DETERMINISTICALLY [b, a] every iteration
            // (see file banner); if dispatch were secretly still sequential, delay_a would run to
            // completion before delay_b's tool function ever got a chance to run, so delay_a's
            // rendezvous wait would time out (bounded, not a hang) and this iteration's completion
            // order would stay empty/wrong, failing the check below instead of hanging.
            session.emplace_chat_client().set_script({
                ScriptedOutcome{multi_tool_call_response({{"c1", "delay_a", "{\"ignored\":false}"},
                                                            {"c2", "delay_b", "{\"ignored\":false}"}}),
                                Usage{}},
                ScriptedOutcome{text_response("done"), Usage{}},
            });

            auto outcome = drive(session.start_run(StartRun{user_message("go")}));
            check(outcome.has_value(), "CLAIM 3: iteration completes the run");
            check(tool_result_call_id_order(session.history()) ==
                      std::vector<std::string>{"c1", "c2"},
                  "CLAIM 3: history order is ALWAYS emitted order (c1, c2), regardless of which "
                  "call physically finished first this iteration");
            if (g_completion_order == std::vector<std::string>{"b", "a"}) {
                ++out_of_order_completions_observed;
            }
        }
        check(out_of_order_completions_observed == kIterations,
              "CLAIM 3: every single iteration's PHYSICAL completion order was [b, a] -- the "
              "opposite of emitted order -- positively confirming real concurrent execution (not "
              "merely that the final history happens to look right, which a sequential "
              "implementation would also produce trivially)");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "ADR-160 parallel batch dispatch: ALL CHECKS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "ADR-160 parallel batch dispatch: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
