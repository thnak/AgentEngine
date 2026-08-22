// Proof for the v1 slice of docs/planning/dynamic-multi-agent-fanout-design-draft.md (rounds 1-14) --
// agentengine::rt::multi_agent::{Budget, spawn, spawn_with_retry, parallel}
// (include/agentengine/rt/multi_agent.hpp). This is the "prove" phase real evidence a future ADR will
// cite, per decisions/README.md's requirement that an ADR record executed evidence, not a design read
// as correct.
//
//   S1 -- spawn() fails closed (multi_agent.no_live_capabilities) when the caller's EffectContext has
//         no live capability grant -- the exact hazard round 6/7/9's FATAL findings existed to close.
//   S2 -- spawn() narrows correctly: a child session granted a SUBSET of the caller's live capabilities
//         can invoke a tool requiring exactly that subset.
//   S3 -- spawn() fails closed (capability.attenuation_not_subsumed) when narrower_grant asks for MORE
//         than the caller's own live EffectContext::capabilities holds -- "child exceeds parent" is a
//         runtime-impossible case, not a documentation note.
//   S4 -- spawn() derives the child's principal from the caller's LIVE EffectContext::principal by
//         default (not a bare caller-fabricated value) -- proven by a tool observing the ACTUAL
//         principal id the child ran under.
//   S5 -- spawn() with an explicit delegate_principal validates it via principal_admitted_for() against
//         the caller's own live principal, and rejects an unrelated (not-admitted) delegate.
//   S6 -- spawn() fails closed (multi_agent.factory_returned_null) when SessionFactory returns nullptr.
//   B1 -- Budget::try_reserve admits up to max_spawns, refuses the request that would exceed it, and
//         reserves NOTHING on a refused call (a later smaller request can still succeed).
//   B2 -- Budget::try_reserve's token backstop refuses new admission once debited spend has already
//         crossed max_tokens, even though the spawn-count axis alone would still allow it.
//   B3 -- REAL concurrency: two threads racing try_reserve() for the LAST available slot against a
//         live Budget never both succeed -- the exact TOCTOU class round 9 found in the original
//         unguarded design, checked against actual concurrent execution, not argued from the mutex's
//         presence alone.
//   P1 -- parallel() fans out N thunks and returns every result in its own correct slot, in order.
//   P2 -- parallel(): one thunk's failure resolves to an error in its OWN slot; every other thunk still
//         completes and debits budget normally (round 1's own "error in its slot, caller filters").
//   P3 -- parallel() refuses the WHOLE batch atomically (every slot: multi_agent.fanout_budget_exceeded)
//         when the batch size alone would exceed max_spawns -- no partial admission.
//   P4 -- parallel()'s max_in_flight genuinely bounds concurrently in-flight (dispatched-but-not-yet-
//         debited) thunks -- measured against real overlapping execution, not merely configured.
//   P5 -- a thunk that THROWS (not merely returns an error) is caught, converted to a real,
//         attributable error in its own slot, and does not leak its in-flight slot -- proven by
//         successfully dispatching a further batch against the SAME Budget afterward.
//   R1 -- spawn_with_retry() retries a `resource`-classified failure and succeeds on a later attempt,
//         debiting Budget once per attempt actually made (not once per logical call).
//   R2 -- spawn_with_retry() does NOT retry a `contract`-classified failure -- exactly one attempt.
//
// MACHINE SAFETY (CLAUDE.md): every loop below is bounded; B3/P4's real threads/pool are bounded-size
// and joined/destroyed before the test exits.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/multi_agent.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::StartRun;
using agentengine::rt::multi_agent::Budget;
using agentengine::rt::multi_agent::SessionFactory;
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

// Same driving idiom every other rt::AgentSession test file uses -- every fixture below is fully
// synchronous under the hood (chat()/tool invoke() never suspend on anything external), matching this
// codebase's own "safe here" precondition for a naive resume()-loop driver.
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
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Principal;
using agentengine::Text;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::error;
using agentengine::failure_class;
using agentengine::result;
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

Message text_response(std::string text) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

[[nodiscard]] std::string reply_text(agentengine::rt::AgentResponse const& r) {
    auto const* t = std::get_if<Text>(&r.message.content.front().value);
    return t != nullptr ? t->text : std::string{"<non-text reply>"};
}

// -- A gated tool, same shape as test_rt_agent_session_tier3_authority.cpp's own GatedTool: declares a
// -- REAL capability_ceiling and stamps the observed principal id, so a test can prove BOTH that the
// -- child's capabilities are exactly what spawn() narrowed to, and that its principal is exactly
// -- whatever spawn() derived -- not merely that the run "succeeded."
struct GatedArgs { bool unused = false; };
AE_JSON_SCHEMA(GatedArgs, unused)
struct GatedReply { bool unused = false; };
AE_JSON_SCHEMA(GatedReply, unused)

std::string g_last_observed_principal_id;

struct GatedTool : agentengine::Tool<GatedTool, agentengine::Capabilities<agentengine::cap::decl::Entropy>> {
    static constexpr std::string_view name        = "gated_tool";
    static constexpr std::string_view description = "Requires cap::Entropy -- gating probe.";
    using Args  = GatedArgs;
    using Reply = GatedReply;
    static agentengine::result<Reply> invoke(Args, EffectContext& ctx) {
        g_last_observed_principal_id = ctx.principal.id;
        return Reply{};
    }
};

class GatedToolProvider {
public:
    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools.push_back(agentengine::make_tool_descriptor<GatedTool>());
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) {
        co_return std::monostate{};
    }
};

// -- Scripted backend for the gated-tool fixtures: first call issues the gated_tool call, second call
// -- echoes allowed/denied so the test can assert on the FINAL reply text.
class ScriptedGatedChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext&) {
        if (call_count_ == 0) {
            ++call_count_;
            Message m;
            m.role = role::assistant;
            ContentItem item;
            item.origin = content_origin::assistant;
            agentengine::ToolCall call;
            call.call_id        = "call-1";
            call.tool_name      = "gated_tool";
            call.arguments_json = R"({"unused":false})";
            call.provenance     = agentengine::call_provenance::vendor_structured;
            item.value = call;
            m.content.push_back(item);
            co_return ChatResponse{m, Usage{1, 1, 0, 0, 0.0}};
        }
        ++call_count_;
        std::string outcome = "<no tool result seen>";
        if (!request.messages.empty()) {
            auto const& item = request.messages.back().content.front();
            if (std::holds_alternative<agentengine::ToolResult>(item.value)) {
                outcome = std::get<agentengine::ToolResult>(item.value).is_error ? "denied" : "allowed";
            }
        }
        co_return ChatResponse{text_response(outcome), Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    int call_count_ = 0;
};
static_assert(agentengine::ChatClient<ScriptedGatedChatClient>);

// -- Plain, tool-free canned client: one call, one canned reply carrying a known Usage, for the
// -- Budget/parallel()-focused fixtures that don't care about capability gating.
class CannedChatClient {
public:
    explicit CannedChatClient(Usage usage = Usage{5, 5, 0, 0, 0.0}) : usage_(usage) {}
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        co_return ChatResponse{text_response("reply"), usage_};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    Usage usage_;
};
static_assert(agentengine::ChatClient<CannedChatClient>);

// -- A client that fails N times with a given failure_class, then succeeds -- for spawn_with_retry()'s
// -- R1/R2 fixtures. Failure happens BEFORE any Usage is produced, matching a real "the call itself
// -- never completed" shape (never fabricates partial usage for a failed attempt).
//
// The failure counter is SHARED (a std::shared_ptr<std::atomic<int>>), not a per-instance member --
// spawn_with_retry()'s own contract (fix 3, round 1) mints a genuinely FRESH session, and therefore a
// fresh FlakyChatClient, on EVERY retry attempt, so a per-instance counter would reset to 0 every
// attempt and never observe progress across retries. Real-world flakiness (a rate limit, a transient
// network blip) lives in the VENDOR's own state, external to any one client object -- this mirrors
// that shape directly, rather than the (impossible, given fix 3) alternative of a chat client that
// remembers its own prior attempts across a fresh construction.
class FlakyChatClient {
public:
    FlakyChatClient(std::shared_ptr<std::atomic<int>> shared_attempts, int fail_times, failure_class klass)
        : shared_attempts_(std::move(shared_attempts)), fail_times_(fail_times), klass_(klass) {}
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        int const attempt_index = shared_attempts_->fetch_add(1, std::memory_order_relaxed);
        if (attempt_index < fail_times_) {
            co_return std::unexpected(error{klass_, "scripted flaky failure", "test.flaky"});
        }
        co_return ChatResponse{text_response("reply"), Usage{3, 3, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    std::shared_ptr<std::atomic<int>> shared_attempts_;
    int fail_times_;
    failure_class klass_;
};
static_assert(agentengine::ChatClient<FlakyChatClient>);

}  // namespace

int main() {
    // S1: spawn() fails closed when the caller's EffectContext has no live capabilities.
    {
        EffectContext ctx;  // default-constructed -- capabilities is a null shared_ptr
        SessionFactory<CannedChatClient, agentengine::rt::NoSessionState,
                       agentengine::HistoryProvider<agentengine::Window<0>>>
            factory = [](Principal const& p) {
                auto s = std::make_unique<
                    AgentSession<CannedChatClient, agentengine::rt::NoSessionState,
                                 agentengine::HistoryProvider<agentengine::Window<0>>>>();
                s->initialize("child", p);
                s->emplace_chat_client();
                return s;
            };
        auto res = drive(agentengine::rt::multi_agent::spawn(ctx, factory, StartRun{user_message("hi")},
                                                              std::vector<agentengine::Capability>{}));
        check(!res.has_value(), "S1: spawn() denies a caller with no live capabilities");
        if (!res.has_value()) {
            check(res.error().code == "multi_agent.no_live_capabilities",
                  "S1: denied specifically for the missing live grant");
        }
    }

    // S2/S3/S4/S5: capability narrowing + principal derivation, against a real gated tool.
    {
        using ChildSession =
            AgentSession<ScriptedGatedChatClient, agentengine::rt::NoSessionState, GatedToolProvider>;
        SessionFactory<ScriptedGatedChatClient, agentengine::rt::NoSessionState, GatedToolProvider>
            factory = [](Principal const& p) {
                auto s = std::make_unique<ChildSession>();
                s->initialize("child", p);
                s->emplace_chat_client();
                return s;
            };

        CapabilitySet const parent_caps = CapabilitySet::grant_root({agentengine::cap::Entropy{}});
        EffectContext ctx;
        ctx.capabilities = agentengine::borrow_capabilities(parent_caps);
        ctx.principal     = Principal{"caller", "tenant-a"};

        // S2: a narrower_grant subsumed by the parent's live capabilities lets the gated tool through.
        g_last_observed_principal_id.clear();
        auto ok = drive(agentengine::rt::multi_agent::spawn(
            ctx, factory, StartRun{user_message("hi")},
            std::vector<agentengine::Capability>{agentengine::cap::Entropy{}}));
        check(ok.has_value() && reply_text(*ok) == "allowed",
              "S2: a narrower_grant subsumed by the caller's live capabilities is granted to the child");

        // S4: absent a delegate, the child's principal is the CALLER's live principal.
        check(g_last_observed_principal_id == "caller",
              "S4: the child ran under the caller's own live EffectContext::principal by default");

        // S3: asking for MORE than the caller's own live grant fails closed.
        CapabilitySet const empty_caller_caps = CapabilitySet::grant_root({});
        EffectContext ctx_empty;
        ctx_empty.capabilities = agentengine::borrow_capabilities(empty_caller_caps);
        ctx_empty.principal     = Principal{"caller", "tenant-a"};
        auto denied = drive(agentengine::rt::multi_agent::spawn(
            ctx_empty, factory, StartRun{user_message("hi")},
            std::vector<agentengine::Capability>{agentengine::cap::Entropy{}}));
        check(!denied.has_value(), "S3: narrower_grant exceeding the caller's live grant is refused");
        if (!denied.has_value()) {
            check(denied.error().code == "capability.attenuation_not_subsumed",
                  "S3: refused specifically as an unsubsumed attenuation, not a generic failure");
        }

        // S5: an admitted delegate (on_behalf_of the caller, same tenant) is used as the child's
        // principal instead of the caller's own.
        g_last_observed_principal_id.clear();
        auto delegated = drive(agentengine::rt::multi_agent::spawn(
            ctx, factory, StartRun{user_message("hi")},
            std::vector<agentengine::Capability>{agentengine::cap::Entropy{}},
            Principal{"delegate", "tenant-a", agentengine::principal_kind::agent, "caller"}));
        check(delegated.has_value(), "S5a: an admitted delegate principal is accepted");
        check(g_last_observed_principal_id == "delegate",
              "S5a: the child ran under the DELEGATE's principal, not the caller's");

        // S5b: an unrelated (not admitted) delegate is refused.
        auto not_admitted = drive(agentengine::rt::multi_agent::spawn(
            ctx, factory, StartRun{user_message("hi")},
            std::vector<agentengine::Capability>{agentengine::cap::Entropy{}},
            Principal{"attacker", "tenant-b"}));
        check(!not_admitted.has_value(), "S5b: an unrelated delegate principal is refused");
        if (!not_admitted.has_value()) {
            check(not_admitted.error().code == "multi_agent.delegate_not_admitted",
                  "S5b: refused specifically as a non-admitted delegate");
        }
    }

    // S6: a factory returning nullptr is refused, not dereferenced.
    {
        EffectContext ctx;
        CapabilitySet const caps = CapabilitySet::grant_root({});
        ctx.capabilities = agentengine::borrow_capabilities(caps);
        ctx.principal     = Principal{"caller", "tenant-a"};
        SessionFactory<CannedChatClient, agentengine::rt::NoSessionState,
                       agentengine::HistoryProvider<agentengine::Window<0>>>
            null_factory = [](Principal const&) {
                return std::unique_ptr<AgentSession<CannedChatClient, agentengine::rt::NoSessionState,
                                                     agentengine::HistoryProvider<agentengine::Window<0>>>>{};
            };
        auto res = drive(agentengine::rt::multi_agent::spawn(
            ctx, null_factory, StartRun{user_message("hi")}, std::vector<agentengine::Capability>{}));
        check(!res.has_value(), "S6: a null-returning SessionFactory is refused");
        if (!res.has_value()) {
            check(res.error().code == "multi_agent.factory_returned_null",
                  "S6: refused specifically as a factory contract violation");
        }
    }

    // B1: try_reserve admits up to max_spawns, refuses beyond, reserves nothing on a refused call.
    {
        Budget budget(/*max_spawns=*/2, /*max_tokens=*/1000, /*max_in_flight=*/2);
        check(budget.try_reserve(2), "B1: a request exactly at max_spawns is admitted");
        check(!budget.try_reserve(1), "B1: any further request is refused once max_spawns is reached");
        check(budget.remaining_spawns() == 0,
              "B1: the refused call reserved nothing (remaining_spawns still exactly 0, not negative "
              "or altered)");
    }

    // B2: the token backstop refuses new admission once debited spend has already crossed max_tokens,
    // even though the spawn-count axis alone would still allow it.
    {
        Budget budget(/*max_spawns=*/10, /*max_tokens=*/5, /*max_in_flight=*/10);
        check(budget.try_reserve(1), "B2: first admission succeeds (0 spent so far)");
        budget.debit_tokens(Usage{10, 0, 0, 0, 0.0});  // 10 tokens spent, over the 5-token ceiling
        check(!budget.try_reserve(1),
              "B2: a second admission is refused purely on the token backstop, though spawns_reserved "
              "(1 of 10) alone would still allow it");
    }

    // B3: REAL concurrency -- two threads race try_reserve() for the LAST slot; never both succeed.
    {
        constexpr int kRounds = 200;
        int both_succeeded_count = 0;
        for (int round = 0; round < kRounds; ++round) {
            Budget budget(/*max_spawns=*/1, /*max_tokens=*/1000, /*max_in_flight=*/2);
            std::atomic<int> successes{0};
            auto racer = [&budget, &successes] {
                if (budget.try_reserve(1)) successes.fetch_add(1, std::memory_order_relaxed);
            };
            std::thread t1(racer);
            std::thread t2(racer);
            t1.join();
            t2.join();
            if (successes.load() == 2) ++both_succeeded_count;
            check(successes.load() <= 1,
                  round == 0 ? "B3: two threads racing for the LAST slot never both succeed"
                             : "");
        }
        check(both_succeeded_count == 0,
              "B3: across 200 real-thread races for one slot, both threads succeeding together never "
              "happened once");
    }

    // P1/P2/P3/P4/P5: parallel().
    {
        using agentengine::rt::ThreadPool;
        using agentengine::rt::multi_agent::parallel;

        // P1: N thunks, all succeed, results land in the correct slot in order.
        {
            ThreadPool pool(4);
            Budget budget(/*max_spawns=*/10, /*max_tokens=*/10000, /*max_in_flight=*/10);
            std::vector<std::function<task<result<agentengine::rt::AgentResponse>>()>> thunks;
            for (int i = 0; i < 5; ++i) {
                thunks.push_back([i]() -> task<result<agentengine::rt::AgentResponse>> {
                    co_return agentengine::rt::AgentResponse{text_response("item-" + std::to_string(i)),
                                                              Usage{1, 1, 0, 0, 0.0}, std::nullopt};
                });
            }
            auto results = drive(parallel(pool, budget, std::move(thunks)));
            check(results.size() == 5, "P1: parallel() returns one slot per thunk");
            bool all_ok = true;
            for (int i = 0; i < 5; ++i) {
                if (!results[static_cast<std::size_t>(i)].has_value() ||
                    reply_text(*results[static_cast<std::size_t>(i)]) != "item-" + std::to_string(i)) {
                    all_ok = false;
                }
            }
            check(all_ok, "P1: every slot holds the RIGHT thunk's own result, in order");
            check(budget.remaining_tokens() == 10000 - 10,
                  "P1: Budget debited exactly 2 tokens per successful child (5 x (1+1) = 10)");
        }

        // P2: one thunk fails; every OTHER thunk still completes and debits normally.
        {
            ThreadPool pool(4);
            Budget budget(/*max_spawns=*/10, /*max_tokens=*/10000, /*max_in_flight=*/10);
            std::vector<std::function<task<result<agentengine::rt::AgentResponse>>()>> thunks;
            thunks.push_back([]() -> task<result<agentengine::rt::AgentResponse>> {
                co_return agentengine::rt::AgentResponse{text_response("ok-0"), Usage{2, 0, 0, 0, 0.0},
                                                          std::nullopt};
            });
            thunks.push_back([]() -> task<result<agentengine::rt::AgentResponse>> {
                co_return std::unexpected(error{failure_class::fatal, "boom", "test.boom"});
            });
            thunks.push_back([]() -> task<result<agentengine::rt::AgentResponse>> {
                co_return agentengine::rt::AgentResponse{text_response("ok-2"), Usage{3, 0, 0, 0, 0.0},
                                                          std::nullopt};
            });
            auto results = drive(parallel(pool, budget, std::move(thunks)));
            check(results[0].has_value() && reply_text(*results[0]) == "ok-0",
                  "P2: sibling 0 succeeds independently of sibling 1's failure");
            check(!results[1].has_value() && results[1].error().code == "test.boom",
                  "P2: the failing thunk's OWN error is preserved in its own slot");
            check(results[2].has_value() && reply_text(*results[2]) == "ok-2",
                  "P2: sibling 2 succeeds independently too");
            check(budget.remaining_tokens() == 10000 - 5,
                  "P2: only the two SUCCESSFUL children's usage was debited (2+3=5), not the failed one");
        }

        // P3: the whole batch is refused atomically when its size alone exceeds max_spawns.
        {
            ThreadPool pool(4);
            Budget budget(/*max_spawns=*/2, /*max_tokens=*/10000, /*max_in_flight=*/10);
            std::vector<std::function<task<result<agentengine::rt::AgentResponse>>()>> thunks;
            for (int i = 0; i < 3; ++i) {
                thunks.push_back([]() -> task<result<agentengine::rt::AgentResponse>> {
                    co_return agentengine::rt::AgentResponse{text_response("should-not-run"),
                                                              Usage{1, 1, 0, 0, 0.0}, std::nullopt};
                });
            }
            auto results = drive(parallel(pool, budget, std::move(thunks)));
            bool all_refused = true;
            for (auto const& r : results) {
                if (r.has_value() || r.error().code != "multi_agent.fanout_budget_exceeded") {
                    all_refused = false;
                }
            }
            check(all_refused, "P3: every slot is refused with fanout_budget_exceeded, none dispatched");
            check(budget.remaining_spawns() == 2,
                  "P3: the refused batch reserved NOTHING -- remaining_spawns is untouched");
        }

        // P4: max_in_flight genuinely bounds concurrently in-flight thunks, measured against real
        // overlapping execution.
        {
            ThreadPool pool(8);
            constexpr std::size_t kMaxInFlight = 2;
            Budget budget(/*max_spawns=*/8, /*max_tokens=*/100000, kMaxInFlight);
            std::atomic<int> current_in_flight{0};
            std::atomic<int> observed_max{0};
            std::vector<std::function<task<result<agentengine::rt::AgentResponse>>()>> thunks;
            for (int i = 0; i < 8; ++i) {
                thunks.push_back([&current_in_flight, &observed_max]()
                                      -> task<result<agentengine::rt::AgentResponse>> {
                    int const now_in_flight = current_in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
                    int prev_max = observed_max.load(std::memory_order_relaxed);
                    while (now_in_flight > prev_max &&
                           !observed_max.compare_exchange_weak(prev_max, now_in_flight,
                                                                std::memory_order_relaxed)) {
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    current_in_flight.fetch_sub(1, std::memory_order_acq_rel);
                    co_return agentengine::rt::AgentResponse{text_response("ok"), Usage{1, 0, 0, 0, 0.0},
                                                              std::nullopt};
                });
            }
            auto results = drive(parallel(pool, budget, std::move(thunks)));
            bool all_succeeded = true;
            for (auto const& r : results) all_succeeded = all_succeeded && r.has_value();
            check(all_succeeded, "P4: every thunk still completes successfully under the throttle");
            check(observed_max.load() <= static_cast<int>(kMaxInFlight),
                  "P4: the observed concurrent in-flight count NEVER exceeded max_in_flight");
            check(observed_max.load() >= 2,
                  "P4: real overlap actually happened (not accidentally fully serialized) -- a "
                  "meaningful positive control, not a test that would pass regardless");
        }

        // P5: a thunk that THROWS is caught, converted to an attributable error, and does not leak its
        // in-flight slot -- proven by a further batch against the SAME Budget succeeding afterward.
        {
            ThreadPool pool(4);
            Budget budget(/*max_spawns=*/10, /*max_tokens=*/10000, /*max_in_flight=*/2);
            std::vector<std::function<task<result<agentengine::rt::AgentResponse>>()>> throwing_thunks;
            throwing_thunks.push_back([]() -> task<result<agentengine::rt::AgentResponse>> {
                throw std::runtime_error("thunk-boom");
                co_return agentengine::rt::AgentResponse{};  // unreachable
            });
            auto first = drive(parallel(pool, budget, std::move(throwing_thunks)));
            check(!first[0].has_value() && first[0].error().code == "multi_agent.child_threw",
                  "P5: a throwing thunk resolves to a real, attributable error, not a crash or hang");

            std::vector<std::function<task<result<agentengine::rt::AgentResponse>>()>> follow_up;
            for (int i = 0; i < 2; ++i) {
                follow_up.push_back([]() -> task<result<agentengine::rt::AgentResponse>> {
                    co_return agentengine::rt::AgentResponse{text_response("ok"), Usage{1, 0, 0, 0, 0.0},
                                                              std::nullopt};
                });
            }
            auto second = drive(parallel(pool, budget, std::move(follow_up)));
            check(second[0].has_value() && second[1].has_value(),
                  "P5: a further 2-wide batch against the SAME Budget (max_in_flight=2) still succeeds "
                  "-- the throwing thunk's in-flight slot was NOT leaked");
        }
    }

    // R1/R2: spawn_with_retry().
    {
        using ChildSession = AgentSession<FlakyChatClient, agentengine::rt::NoSessionState,
                                           agentengine::HistoryProvider<agentengine::Window<0>>>;
        EffectContext ctx;
        CapabilitySet const caps = CapabilitySet::grant_root({});
        ctx.capabilities = agentengine::borrow_capabilities(caps);
        ctx.principal     = Principal{"caller", "tenant-a"};

        // R1: fails twice with a RETRYABLE class, succeeds on the third attempt -- Budget debits
        // spawn-count exactly 3 times (once per attempt actually made).
        {
            auto shared_attempts = std::make_shared<std::atomic<int>>(0);
            SessionFactory<FlakyChatClient, agentengine::rt::NoSessionState,
                           agentengine::HistoryProvider<agentengine::Window<0>>>
                factory = [shared_attempts](Principal const& p) {
                    auto s = std::make_unique<ChildSession>();
                    s->initialize("child", p);
                    s->emplace_chat_client(shared_attempts, /*fail_times=*/2, failure_class::resource);
                    return s;
                };
            Budget budget(/*max_spawns=*/5, /*max_tokens=*/10000, /*max_in_flight=*/5);
            agentengine::RetryPolicy policy;
            policy.max_attempts = 3;
            auto res = drive(agentengine::rt::multi_agent::spawn_with_retry(
                ctx, factory, StartRun{user_message("hi")}, std::vector<agentengine::Capability>{},
                policy, budget));
            check(res.has_value(), "R1: succeeds by the 3rd attempt after 2 retryable failures");
            check(budget.remaining_spawns() == 5 - 3,
                  "R1: Budget debited spawn-count once per ATTEMPT actually made (3), not once per "
                  "logical call");
        }

        // R2: a CONTRACT-classified failure is never retried -- exactly one attempt.
        {
            auto shared_attempts = std::make_shared<std::atomic<int>>(0);
            SessionFactory<FlakyChatClient, agentengine::rt::NoSessionState,
                           agentengine::HistoryProvider<agentengine::Window<0>>>
                factory = [shared_attempts](Principal const& p) {
                    auto s = std::make_unique<ChildSession>();
                    s->initialize("child", p);
                    s->emplace_chat_client(shared_attempts, /*fail_times=*/99, failure_class::contract);
                    return s;
                };
            Budget budget(/*max_spawns=*/5, /*max_tokens=*/10000, /*max_in_flight=*/5);
            agentengine::RetryPolicy policy;
            policy.max_attempts = 3;
            auto res = drive(agentengine::rt::multi_agent::spawn_with_retry(
                ctx, factory, StartRun{user_message("hi")}, std::vector<agentengine::Capability>{},
                policy, budget));
            check(!res.has_value(), "R2: a contract failure is not recovered");
            check(budget.remaining_spawns() == 5 - 1,
                  "R2: exactly ONE attempt was made -- a contract failure is never retried");
        }
    }

    if (g_failures == 0) {
        std::printf("test_rt_multi_agent: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_multi_agent: %d failure(s)\n", g_failures);
    return 1;
}
