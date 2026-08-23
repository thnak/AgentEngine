// Proves `ComposedContextProvider<Ms...>` (core/composed_context_provider.hpp), the generic
// N-provider generalization of history_and_skills_provider.hpp's hand-written, fixed two-provider
// `HistoryAndSkillsProvider<H,S>`. Two things are under test:
//
//   1. Direct unit coverage against a THREE-provider composition (deliberately N > 2 -- the whole
//      point of genericity is proven false by a fixture that only ever exercises the same two-
//      provider shape `HistoryAndSkillsProvider` already covers): declared order is preserved on
//      the wire, each contributor's own budget is enforced independently (2026-08-23: exceeding it
//      fails the whole composite closed, per Finding E / ADR-075 -- see the fixed-closed case below,
//      not a silent trim), a provider-contributed tool survives composition, and `on_turn_end` fans
//      out to every wrapped provider.
//
//   2. `rt::AgentSession<ChatClientT, StateT, ComposedContextProvider<...>>` actually compiles and
//      runs a real turn with the composite occupying `AgentSession`'s single `HistoryProviderT`
//      template slot -- this is the literal "wired into AgentSession's provider slot" claim: no
//      change to agent_session.hpp itself, the composite is just a `ContextProvider` conformer like
//      any other, but a caller can now get N real contributors into one session through it.

#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "support/run_task_sync.hpp"

using ae::rt::AgentSession;
using ae::rt::NoSessionState;
using ae::rt::StartRun;

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

// Drives an ae::task<T> to completion -- same idiom every rt::AgentSession test file uses (e.g.
// test_rt_agent_session.cpp): the ChatClientT fixture below never genuinely suspends on anything
// external, so a plain "resume until done" loop is safe.
template <class T>
T drive(ae::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

ae::Message make_msg(std::string text, std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::move(text)};
    item.origin = ae::content_origin::user;
    ae::Message m{};
    m.role       = ae::role::user;
    m.message_id = std::move(message_id);
    m.content.push_back(item);
    return m;
}

// Named distinctly from core/tool_call_extraction.hpp's own `text_of` (pulled in transitively via
// rt/agent_session.hpp) to avoid an ambiguous unqualified call -- same free-function name, same
// namespace, genuinely different overload otherwise.
[[nodiscard]] std::string composed_text_of(ae::Message const& m) {
    return std::get<ae::Text>(m.content.front().value).text;
}

// A minimal, distinct ContextProvider conformer -- contributes a fixed set of messages plus a
// tracked on_turn_end call count, so composition order/budgeting/turn-end fan-out can all be told
// apart from HistoryProvider's own behavior.
struct FixedMessagesProvider {
    static constexpr std::string_view name = "fixed-messages";  // ADR-066 §3

    std::vector<ae::Message>    to_return;
    std::shared_ptr<std::size_t> turn_end_calls = std::make_shared<std::size_t>(0);

    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext&,
                                                                             ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages = to_return;
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) {
        ++*turn_end_calls;
        co_return std::monostate{};
    }
};
static_assert(ae::ContextProvider<FixedMessagesProvider>,
              "FixedMessagesProvider must satisfy ContextProvider (005 §5)");

// Contributes exactly one tool descriptor, nothing else -- proves ContextContribution.tools survives
// N-way composition (006 §1's declaration shape, reused verbatim by 005 §5 providers).
struct ToolContributingProvider {
    static constexpr std::string_view name = "tool-contributing";  // ADR-066 §3

    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext&,
                                                                             ae::EffectContext&) {
        ae::ContextContribution c;
        ae::ToolDescriptor      td;
        td.name = "provider_tool";
        c.tools.push_back(td);
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};
static_assert(ae::ContextProvider<ToolContributingProvider>,
              "ToolContributingProvider must satisfy ContextProvider (005 §5)");

using ThreeWayProvider =
    ae::ComposedContextProvider<ae::HistoryProvider<ae::Window<0>>, FixedMessagesProvider,
                                  ToolContributingProvider>;
static_assert(ae::ContextProvider<ThreeWayProvider>,
              "ComposedContextProvider<...> must itself satisfy ContextProvider (005 §5)");
static_assert(std::is_default_constructible_v<ThreeWayProvider>,
              "must be default-constructible to occupy AgentSession's plain value-member provider slot");

// -- Part 3 fixtures: a NON-default-constructible provider proves ComposedContextProvider stays
// default-constructible (unengaged) even when its own Ms isn't -- ThreeWayProvider above, whose Ms
// ARE all default-constructible, still needs an explicit engage()/tuple-constructor call too; the
// default ctor never auto-engages regardless of Ms (see composed_context_provider.hpp's own comment
// on why an earlier draft's "auto-engage when possible" design was tried and reverted) -- plus proof
// that ComposedContextProvider is move-only (2026-08-22 tracker Finding B;
// decisions/ADR-074-composed-context-provider-consolidation.md).
struct RequiredArgProvider {
    static constexpr std::string_view name = "required-arg";  // ADR-066 §3

    RequiredArgProvider(std::string text, std::shared_ptr<std::size_t> turn_end_calls)
        : text_(std::move(text)), turn_end_calls_(std::move(turn_end_calls)) {}

    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext&,
                                                                             ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.push_back(make_msg(text_, "required-arg-msg"));
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) {
        ++*turn_end_calls_;
        co_return std::monostate{};
    }

private:
    std::string text_;
    std::shared_ptr<std::size_t> turn_end_calls_;
};
static_assert(ae::ContextProvider<RequiredArgProvider>);
static_assert(!std::is_default_constructible_v<RequiredArgProvider>,
              "the whole point: this Ms is deliberately NOT default-constructible");

using LazyProvider = ae::ComposedContextProvider<RequiredArgProvider>;
static_assert(std::is_default_constructible_v<LazyProvider>,
              "ComposedContextProvider stays default-constructible (starts unengaged) even when its "
              "own Ms is not -- the reason LazyComposedContextProvider used to have to exist as a "
              "separate type; this type now closes that gap directly");
static_assert(!std::is_copy_constructible_v<LazyProvider> && !std::is_copy_assignable_v<LazyProvider>,
              "move-only -- Finding B closed at the source: fork_from()'s plain copy-assignment can "
              "no longer silently alias two sessions' underlying providers, it fails to compile");
static_assert(std::is_move_constructible_v<LazyProvider> && std::is_move_assignable_v<LazyProvider>,
              "still move-constructible/assignable -- AgentSession's own internal moves (e.g. "
              "clear_in_process_state()'s history_provider_ = HistoryProviderT{}) must keep working");

// -- Part 2 fixtures: a real AgentSession driven with ThreeWayProvider in its provider slot --------

class CapturingChatClient {
public:
    CapturingChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<ae::ChatRequest> requests;
    };

    [[nodiscard]] std::vector<ae::ChatRequest> const& requests() const { return state_->requests; }

    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest request, ae::EffectContext&) {
        state_->requests.push_back(request);
        ae::Message reply;
        reply.role = ae::role::assistant;
        ae::ContentItem item;
        item.origin = ae::content_origin::assistant;
        item.value  = ae::Text{"ack"};
        reply.content.push_back(item);
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest, ae::EffectContext&) {
        return {};
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(ae::ChatClient<CapturingChatClient>);

using ComposedSession = AgentSession<CapturingChatClient, NoSessionState, ThreeWayProvider>;
static_assert(std::is_default_constructible_v<ComposedSession>,
              "AgentSession<..., ComposedContextProvider<...>> must be constructible -- the actual "
              "'wired into AgentSession's provider slot' claim");

}  // namespace

int main() {
    ae::Principal principal{"p-composed", ""};
    ae::EffectContext ctx{};

    // --- Part 1a: direct unit coverage, N == 3, every budget unbounded -------------------------
    // Proves the composition mechanics (declared order, tool survival, on_turn_end fan-out) with
    // budgets out of the way entirely -- Part 1b below proves the budget-exceeded case separately,
    // since it's no longer just "some messages get dropped" but "the whole composite fails closed"
    // (2026-08-23, Finding E / ADR-075), a materially different assertion shape.
    {
        std::vector<ae::Message> history{make_msg("aaaaaaaa", "h-1"), make_msg("bbbbbbbb", "h-2"),
                                           make_msg("cccccccc", "h-3")};
        FixedMessagesProvider fixed{{make_msg("fixed-msg", "f-1")}};
        auto fixed_turn_end_calls = fixed.turn_end_calls;

        ThreeWayProvider provider{
            std::tuple{ae::HistoryProvider<ae::Window<0>>{}, fixed, ToolContributingProvider{}},
            std::array<ae::ContextBudget, 3>{ae::ContextBudget{0}, ae::ContextBudget{0},
                                               ae::ContextBudget{0}}};

        ae::SessionContext session_ctx{"s-composed", principal, history};
        auto out = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            provider.on_context(session_ctx, ctx));

        check(out.has_value(), "P1a: on_context succeeds across 3 composed providers, all unbounded");
        check(out.has_value() && out->messages.size() == 4,
              "P1a: 3 real history messages + the fixed provider's 1 message == 4, nothing dropped");
        check(out.has_value() && out->messages.size() == 4 &&
                  composed_text_of(out->messages[0]) == "aaaaaaaa" &&
                  composed_text_of(out->messages[3]) == "fixed-msg",
              "P1a: declared order preserved (HistoryProvider first, FixedMessagesProvider second)");
        check(out.has_value() && out->tools.size() == 1 && out->tools[0].name == "provider_tool",
              "P1a: the THIRD provider's contributed tool reaches the combined contribution");

        ae::test_support::run_task_sync<std::monostate>(provider.on_turn_end(
            ae::TurnView{std::span<ae::Message const>{history.data(), history.size()}}, ctx));
        check(*fixed_turn_end_calls == 1,
              "P1a: on_turn_end fans out to every wrapped provider, not just the first");
    }

    // --- Part 1b: a wrapped contributor exceeding its own declared budget fails the WHOLE composite
    // closed, not a silent per-contributor trim (2026-08-23, Finding E / ADR-075) -----------------
    {
        std::vector<ae::Message> history{make_msg("aaaaaaaa", "h-1"), make_msg("bbbbbbbb", "h-2"),
                                           make_msg("cccccccc", "h-3")};
        // approx_token_count("aaaaaaaa") == (8+3)/4 == 2 tokens/message; total history == 6 tokens,
        // exceeding a declared HistoryProvider budget of 3.
        ThreeWayProvider provider{
            std::tuple{ae::HistoryProvider<ae::Window<0>>{}, FixedMessagesProvider{{make_msg("fixed-msg", "f-1")}},
                       ToolContributingProvider{}},
            std::array<ae::ContextBudget, 3>{ae::ContextBudget{3}, ae::ContextBudget{0},
                                               ae::ContextBudget{0}}};

        ae::SessionContext session_ctx{"s-composed-over-budget", principal, history};
        auto out = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            provider.on_context(session_ctx, ctx));

        check(!out.has_value(),
              "P1b: on_context fails closed when a wrapped contributor (HistoryProvider) exceeds "
              "its own declared budget -- ComposedContextProvider propagates assemble_context()'s "
              "own failure, never a partial/trimmed contribution");
        check(!out.has_value() && out.error().klass == ae::failure_class::resource,
              "P1b: the propagated failure is failure_class::resource");
        check(!out.has_value() && out.error().code == "context_assembly.contributor_budget_exceeded",
              "P1b: ComposedContextProvider re-throws assemble_context()'s own stable code verbatim, "
              "not re-coded into a composed_context.* error");
    }

    // --- Part 2: ComposedContextProvider actually driving a real AgentSession turn -------------
    {
        ComposedSession session;
        session.initialize("s-session", principal);
        // Default-constructed session.history_provider() starts UNENGAGED unconditionally now (2026-
        // 08-23: the default ctor no longer auto-engages even when every Ms is default-constructible
        // -- see composed_context_provider.hpp's own comment on why that changed), so an explicit
        // engage() is required before start_run() can succeed.
        auto engaged = session.history_provider().engage(
            std::tuple{ae::HistoryProvider<ae::Window<0>>{}, FixedMessagesProvider{{make_msg("fixed-msg-p2", "f-2")}},
                       ToolContributingProvider{}});
        check(engaged.has_value(), "P2 setup: engage() succeeds");
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        CapturingChatClient& client = session.emplace_chat_client();

        ae::Message user;
        user.role = ae::role::user;
        ae::ContentItem item;
        item.origin = ae::content_origin::user;
        item.value  = ae::Text{"go"};
        user.content.push_back(item);

        auto r = drive(session.start_run(StartRun{user}));
        check(r.has_value(), "P2: a run through ComposedContextProvider's provider slot converges");
        check(client.requests().size() == 1, "P2: exactly one model call happened");
        check(client.requests().size() == 1 && !client.requests()[0].messages.empty(),
              "P2: the outbound request carries at least the turn's own user message");
        check(client.requests().size() == 1 && client.requests()[0].tools.size() == 1 &&
                  client.requests()[0].tools[0].name == "provider_tool",
              "P2: the composite's THIRD (tool-contributing) provider reached the real outbound "
              "ChatRequest through AgentSession's provider slot -- not just through direct "
              "on_context() calls in Part 1");
    }

    // --- Part 3: the lazy engage() path, and move-only == Finding B actually closed --------------
    {
        ae::SessionContext session_ctx{"s-lazy", principal, {}};

        // P3a: default-constructed (unengaged, since RequiredArgProvider is not default-constructible)
        // fails closed on on_context() before engage().
        LazyProvider lazy;
        auto before_engage = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            lazy.on_context(session_ctx, ctx));
        check(!before_engage.has_value() && before_engage.error().code == "composed_context.not_engaged",
              "P3a: on_context() before engage() fails closed with 'not_engaged'");

        auto counter = std::make_shared<std::size_t>(0);
        auto engaged = lazy.engage(std::make_tuple(RequiredArgProvider{"lazy-content", counter}));
        check(engaged.has_value(), "P3a: engage() with the real, non-default-constructible provider succeeds");

        auto after_engage = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            lazy.on_context(session_ctx, ctx));
        check(after_engage.has_value() && after_engage->messages.size() == 1 &&
                  composed_text_of(after_engage->messages[0]) == "lazy-content",
              "P3a: on_context() after engage() carries the real provider's content");

        auto second_engage = lazy.engage(std::make_tuple(RequiredArgProvider{"duplicate", counter}));
        check(!second_engage.has_value() && second_engage.error().code == "composed_context.already_engaged",
              "P3a: a second engage() on the same instance fails closed instead of duplicating/replacing");

        // P3b: move-only, Finding B's actual proof -- move a REAL, engaged instance into a fresh one;
        // the moved-from instance must genuinely revert to not_engaged (not silently succeed empty),
        // and must be engage()-able again (a real recovery path, not permanently bricked).
        LazyProvider target;
        target = std::move(lazy);

        auto moved_from = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            lazy.on_context(session_ctx, ctx));
        check(!moved_from.has_value() && moved_from.error().code == "composed_context.not_engaged",
              "P3b: the MOVED-FROM instance is genuinely not_engaged, not aliasing the moved-to "
              "instance's state");

        auto moved_to = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            target.on_context(session_ctx, ctx));
        check(moved_to.has_value() && moved_to->messages.size() == 1 &&
                  composed_text_of(moved_to->messages[0]) == "lazy-content",
              "P3b: the MOVED-TO instance carries the real, originally-engaged content");

        auto re_engage = lazy.engage(std::make_tuple(RequiredArgProvider{"recovered", counter}));
        check(re_engage.has_value(),
              "P3b: the moved-from instance can be engage()d again -- a real recovery path");

        // Self-move-assignment must be a safe no-op (the `if (this != &other)` guard).
        target = std::move(target);
        auto self_moved = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            target.on_context(session_ctx, ctx));
        check(self_moved.has_value() && self_moved->messages.size() == 1 &&
                  composed_text_of(self_moved->messages[0]) == "lazy-content",
              "P3b: self-move-assignment leaves content intact, not self-cleared");
    }

    std::fprintf(stderr, g_failures == 0 ? "test_composed_context_provider: OK\n"
                                           : "test_composed_context_provider: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
