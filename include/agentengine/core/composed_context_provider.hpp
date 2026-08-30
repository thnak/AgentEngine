#pragma once
// The general N-provider `ContextProvider` composite, all composed through the same real,
// already-proven `core/context_assembly.hpp::assemble_context()`. `AgentSession<ChatClientT, StateT,
// ComposedContextProvider<HistoryProvider<Window<0>>, SkillsProvider, MemoryProvider>>` plugs N real
// contributors into `AgentSession`'s single `ContextProvider` template slot without widening
// `AgentSession` itself (that wider change -- `AgentSession` natively taking a provider pack and
// calling `assemble_context` inside its own turn loop -- stays deferred, named in
// context_assembly.hpp/memory_provider.hpp as later, separately-scoped "Phase G" work touching a
// large, mature, heavily tested file).
//
// 2026-08-23 consolidation (docs/planning/2026-08-22-component-role-audit-tracker.md Findings A/B,
// decisions/ADR-074-composed-context-provider-consolidation.md): this type used to be one of THREE
// overlapping composites -- a hand-written, fixed two-provider `HistoryAndSkillsProvider<H,S>`
// (deleted, fully subsumed); this type, eager-construction-only and plain-copyable; and
// `core/session_builder.hpp`'s `detail::LazyComposedContextProvider<Ms...>`, which existed ONLY
// because this type's old default constructor required every `Ms` to be default-constructible --
// untrue for a real `SkillsProvider`/`MemoryProvider`/`VectorRagContextProvider` -- and which had
// ALREADY been fixed (move-only; a strong-exception-guarantee `engage()`) for the exact aliasing bug
// this type still had. This type now absorbs both: always default-constructible (ALWAYS starting
// empty, never auto-engaging even when every `Ms` happens to be default-constructible -- an earlier
// draft of this consolidation tried auto-engaging and found it genuinely irreconcilable with
// `ComposedQuickstartSessionBuilder`'s own later `engage()` call, see the constructor's own comment
// below and the ADR's §4), and move-only. See the ADR for the full before/after and the verified
// blast radius.

#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "agentengine/core/context_assembly.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/error.hpp"
// ADR-116: needed ONLY for the `friend` declaration on `bind_owner()` below (this class has no other
// dependency on `AgentSession` or anything else in `rt/`). `agent_session.hpp` never includes this
// file back, so this is a one-directional edge, not a cycle -- and `core/session_builder.hpp`
// (this exact directory) already includes `agentengine/rt/agent_session.hpp` directly for the same
// kind of reason, so this is a repeated pattern, not a new one. Real, disclosed cost: this is a
// widely-included header (ADR-102 §51's own note), so this pulls the larger `agent_session.hpp` into
// every translation unit that composes providers, even ones that never touch `AgentSession` directly.
#include "agentengine/rt/agent_session.hpp"

namespace agentengine {

// `Ms...` in declared order == `assemble_context`'s own contributor order (`contributors_[0]`
// first) -- 005 §3's drop-order-determinism rule; also the final wire-message order (a caller
// composing e.g. a skills advertisement before history, matching the old `HistoryAndSkillsProvider`'s
// own wire-order rule, does so by declaring `Ms...` in that order directly now -- there is no second,
// implicit reordering the way that hand-written composite used to apply).
template <class... Ms>
    requires (sizeof...(Ms) >= 1) && (ContextProvider<Ms> && ...)
// ae-naming-lint: allow ComposedContextProvider — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class ComposedContextProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3 -- only load-bearing if this
    // composite is itself wrapped in its own descriptor; its own internal `contributors_` (below) are
    // named by each wrapped `Ms`'s own declared name.
    static constexpr std::string_view name = "composed";  // ae-naming-lint: allow name — ADR-033's HasMiddlewareName precedent, reused verbatim per ADR-066 §3

    // Always available now, regardless of whether every `Ms` is default-constructible -- closes the
    // reason `LazyComposedContextProvider` had to exist as a separate type (`AgentSession<...>::
    // history_provider_` is a plain, always-default-constructed value member). ALWAYS starts empty,
    // unconditionally -- NOT auto-engaged even when every `Ms` happens to be default-constructible.
    //
    // This is deliberate, not a missed convenience: an earlier version of this fix auto-engaged with
    // `Ms{}...` whenever every `Ms` was default-constructible, matching the old eager-only
    // `ComposedContextProvider`'s own ergonomics. That silently broke `ComposedQuickstartSessionBuilder`
    // (session_builder.hpp) for the specific case its own `Ms...` happens to be default-constructible:
    // `AgentSession::history_provider_` default-constructs unconditionally as part of `AgentSession`'s
    // own construction, and `build()` always calls `.engage()` on it afterward expecting an EMPTY
    // instance -- an auto-engaged one made that `.engage()` call fail closed with `already_engaged`
    // every time, a real functional bug for any real caller choosing default-constructible providers,
    // not just a test artifact (found live via a component-role-audit-tracker session, 2026-08-23,
    // decisions/ADR-074-composed-context-provider-consolidation.md -- the two requirements are
    // genuinely irreconcilable on one no-argument constructor spelling, since whether `Ms` happens to
    // be default-constructible cannot change what construction-time DOES). A caller that wants the old
    // eager, immediately-usable-when-possible ergonomics uses the explicit tuple constructor below
    // instead (`ComposedContextProvider{std::tuple{Ms{}...}}` if truly needed with defaults), or calls
    // `engage()` explicitly -- both unambiguous about when population happens.
    ComposedContextProvider() = default;

    // `providers` as an explicit `std::tuple<Ms...>`, not a trailing pack, is what lets `budgets`
    // follow it -- the identical reason `core/model_call_gateway.hpp`'s `ModelCallGateway` takes its
    // own `Fallback...` as `std::tuple<Fallback...>` rather than a second pack. `budgets` defaults to
    // `{}` -- every `ContextBudget` default-constructs to `max_tokens == 0` (unbounded).
    explicit ComposedContextProvider(std::tuple<Ms...> providers,
                                      std::array<ContextBudget, sizeof...(Ms)> budgets = {}) {
        contributors_ = build_contributors(providers, budgets, std::index_sequence_for<Ms...>{});
        engaged_ = true;
    }

    // Move-only, NOT copyable -- closes Finding B at the source. `make_context_provider_descriptor()`
    // (context_assembly.hpp) wraps each `Ms` in a `shared_ptr<Ms>` CAPTURED BY VALUE in
    // `contributors_`'s closures, so an implicit memberwise COPY of this type would copy those
    // `shared_ptr`s too -- aliasing the SAME underlying provider instances, not independent ones. The
    // one real place a plain copy used to bite: `AgentSession::fork_from()`'s `history_provider_ =
    // source.history_provider_;` -- a fork of a session using this type would silently start sharing
    // live, mutable provider state (a memory-provider write-back, a per-skill usage counter, a RAG
    // cache) with its source, an I1/I4-adjacent session-isolation gap a caller has no reason to
    // expect. Deleting copy turns that into a COMPILE ERROR at the exact `fork_from()` call site
    // instead -- `fork_from()` is only compiled when actually called, so this has zero effect on any
    // already-passing construction/`engage()`/`on_context()`/`on_turn_end()` path.
    //
    // CLOSED (2026-08-30, ADR-116) -- previously disclosed here as "necessary, not sufficient"
    // (2026-08-28 red-team, ADR-102 §41-48's own follow-on round): the identical hazard shape above
    // was still fully reachable through the public `history_provider()` accessor's plain MOVE-
    // assignment -- `target.history_provider() = std::move(source.history_provider());` compiled
    // cleanly and transferred a live, already-`engage()`d contributor set (e.g. a real
    // `SandboxToolProvider`, whose `unique_ptr<SessionShellSandbox>` is rooted at a host directory
    // named after `source`'s OWN `session_id` digest) into `target`, while `target`'s `session_id_`/
    // `principal_` stayed untouched. Not a new discovery in kind: `core/session_builder.hpp`'s sibling
    // `LazyComposedContextProvider` already named this EXACT bypass route for itself (that file's own
    // finding 9/11) before ADR-074's consolidation folded it into this type without carrying the
    // disclosure forward.
    //
    // Of the two remediations that comment named -- a non-assignable accessor shape (touching every
    // `history_provider()` caller across ~30 files, most of which use an unrelated, non-composed
    // `HistoryProviderT` this hazard never applied to) or an owning-session-identity check inside
    // `operator=` itself -- ADR-116 implements the second, narrower one: `owner_` (below) is an
    // opaque, non-owning identity tag that `AgentSession::history_provider()` stamps with its own
    // `this` on every call (see `bind_owner()`'s own comment). `operator=` refuses -- as a silent,
    // fully-disclosed no-op, not a crash, matching this codebase's own established best-effort-
    // disclosed precedent (ADR-112 §2's per-entry ACL-grant cap) -- only when BOTH sides are tagged
    // with two DIFFERENT sessions' own addresses. A bare, standalone instance (`owner_ == nullptr` on
    // both sides, e.g. every `tests/test_composed_context_provider.cpp` instance and every move-
    // CONSTRUCTION-based check in `tests/test_session_builder.cpp`) and a session assigning to itself
    // (same address, or caught by the identity check just above) are both completely unaffected --
    // this is why `history_provider().engage(...)`, `ComposedQuickstartSessionBuilder::build()`, and
    // every other real caller needed zero changes for this fix.
    //
    // Declared explicitly (not `=default`): a defaulted move would trivially copy the plain `bool
    // engaged_`, leaving it `true` on the moved-from side even though its `contributors_` is now
    // empty -- desyncing the class's own "`engaged_ == true` iff `contributors_` is populated"
    // invariant (this exact bug, live-reproduced, is why the type being consolidated here fixed it
    // this way originally -- see decisions/ADR-074-composed-context-provider-consolidation.md).
    ComposedContextProvider(ComposedContextProvider&& other) noexcept
        : contributors_(std::move(other.contributors_)), engaged_(other.engaged_) {
        other.engaged_ = false;
        other.contributors_.clear();
    }
    ComposedContextProvider& operator=(ComposedContextProvider&& other) noexcept {
        if (this == &other) return *this;
        // ADR-116: refuse a cross-session transfer -- see this class's own top comment and
        // `bind_owner()`'s comment for the full rationale. Both sides untouched by an
        // `AgentSession::history_provider()` accessor (`owner_ == nullptr`, ordinary standalone use)
        // or both tagged with the SAME session are unaffected; only two DIFFERENT, non-null tags
        // trigger the refusal.
        if (owner_ != nullptr && other.owner_ != nullptr && owner_ != other.owner_) {
            return *this;
        }
        contributors_ = std::move(other.contributors_);
        engaged_      = other.engaged_;
        other.engaged_ = false;
        other.contributors_.clear();
        return *this;
    }
    ComposedContextProvider(ComposedContextProvider const&) = delete;
    ComposedContextProvider& operator=(ComposedContextProvider const&) = delete;

    // Host-only, configuration-time call, never derived from model output (I3) -- builds each `Ms`'s
    // real `ContextProviderDescriptor` and populates this instance, in `Ms...`'s declared order. May
    // be called AT MOST ONCE per instance (fails closed instead of silently duplicating every
    // contributor on the wire); a default-constructed, auto-engaged instance (every `Ms` default-
    // constructible) is already engaged, so calling this on one is also rejected, not silently
    // replaced -- an intentional, stricter guarantee than the bare move-assignment `history_provider()`
    // otherwise allows.
    [[nodiscard]] result<void> engage(std::tuple<Ms...> providers,
                                       std::array<ContextBudget, sizeof...(Ms)> budgets = {}) {
        if (engaged_) {
            return std::unexpected(error{
                failure_class::contract,
                "ComposedContextProvider::engage() called on an already-engaged instance -- every "
                "contributor already present would otherwise be duplicated (or silently discarded) "
                "on the wire",
                "composed_context.already_engaged"});
        }
        contributors_ = build_contributors(providers, budgets, std::index_sequence_for<Ms...>{});
        engaged_ = true;
        return {};
    }

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx,
                                                                 EffectContext& ctx) {
        if (!engaged_) {
            // Only reachable via an unengaged, non-all-default-constructible instance whose
            // `engage()` was never called -- e.g. reached in through `AgentSession::
            // history_provider()` before a builder finished configuring it. Fail-closed, matching
            // this project's convention for a used-before-configured contract violation, rather than
            // silently contributing nothing.
            co_return std::unexpected(error{failure_class::contract,
                                             "ComposedContextProvider::on_context() called before engage()",
                                             "composed_context.not_engaged"});
        }
        result<ContextAssemblyResult> assembled = co_await assemble_context(contributors_, session_ctx, ctx);
        if (!assembled) {
            // A wrapped contributor's own declared ContextBudget was exceeded -- assemble_context()
            // itself now fails closed for that (2026-08-23, Finding E / ADR-075), so the composite
            // fails closed too rather than papering over it with a partial/empty contribution.
            co_return std::unexpected(assembled.error());
        }
        co_return assembled->combined;
    }

    // `assemble_context` itself never calls `on_turn_end` on its contributors (context_assembly.hpp's
    // own function only ever invokes `on_context`) -- forwarded manually here to every wrapped
    // provider. A no-op fan-out over an empty `contributors_` (unengaged instance) rather than an
    // error -- `on_turn_end` has no "before engage()" hazard analogous to `on_context()`'s (nothing to
    // silently misrepresent by doing nothing).
    task<std::monostate> on_turn_end(TurnView turn, EffectContext& ctx) {
        for (auto& contributor : contributors_) (void)co_await contributor.on_turn_end(turn, ctx);
        co_return std::monostate{};
    }

private:
    // Shared by both constructors and `engage()`: builds into a LOCAL vector first, returned only
    // once every `Ms`'s descriptor has been constructed without throwing -- a strong exception
    // guarantee. Pushing directly into a member `contributors_` per-`Ms` (this type's own previous
    // shape) would leave `contributors_` partially populated if a later `Ms`'s move constructor (or
    // `make_context_provider_descriptor()`'s own `make_shared`) threw partway through; for `engage()`
    // specifically, where `engaged_` stays `false` on that path and a retry is therefore not blocked
    // by the `already_engaged` guard above, a retry would otherwise APPEND onto the stale entries
    // instead of starting fresh, silently duplicating a contributor on the wire.
    template <std::size_t... I>
    static std::vector<ContextProviderDescriptor> build_contributors(
        std::tuple<Ms...>& providers, std::array<ContextBudget, sizeof...(Ms)> const& budgets,
        std::index_sequence<I...>) {
        std::vector<ContextProviderDescriptor> built;
        built.reserve(sizeof...(Ms));
        (built.push_back(make_context_provider_descriptor(std::move(std::get<I>(providers)), budgets[I])), ...);
        return built;
    }

    // ADR-116: called ONLY from `AgentSession::history_provider()` (friended below), once per
    // accessor call, with that session's own `this` -- idempotent (a session's address is stable for
    // its whole lifetime, per `session_builder.hpp`'s own always-heap-allocated-via-`make_unique`
    // construction), so calling it repeatedly on the same session's provider is a no-op in effect.
    // Deliberately NOT public: this is `AgentSession` stamping identity on its OWN member, not a knob
    // for an arbitrary caller -- a caller able to invoke this directly could simply retag one side to
    // spoof a match and defeat the very check in `operator=` above that reads `owner_`.
    void bind_owner(void const* owner) noexcept { owner_ = owner; }

    // Must repeat `AgentSession`'s own requires-clause verbatim (agent_session.hpp) -- MSVC treats a
    // friend template declaration as a real redeclaration of the entity for constraint-matching
    // purposes, and rejects a friend declaration whose constraints don't match the real one
    // (`error C3864: requires clause is incompatible with the declaration`, empirically confirmed).
    template <class ChatClientT, class StateT, class HistoryProviderT>
        requires (agentengine::ChatClient<ChatClientT> || agentengine::ModelCallGatewayLike<ChatClientT>) &&
                 agentengine::ContextProvider<HistoryProviderT>
    friend class agentengine::rt::AgentSession;

    std::vector<ContextProviderDescriptor> contributors_;
    bool engaged_ = false;
    // Opaque, non-owning identity tag -- see `bind_owner()`'s own comment and `operator=`'s use of it
    // above. `nullptr` for a bare, standalone instance never reached through
    // `AgentSession::history_provider()` (every instance this class's own test file constructs
    // directly). Deliberately NOT touched by the move constructor below (a freshly move-constructed
    // instance is, by construction, not yet embedded in any session's member slot -- it gets tagged
    // the first time ITS OWN eventual owner's `history_provider()` is called, same as any other fresh
    // instance) and NOT reset by `operator=`'s own moved-from logic (`owner_` describes which
    // session's member slot `other` itself IS, not what content it currently holds -- consuming its
    // content via a legitimate same-session move does not change which session `other` lives in).
    void const* owner_ = nullptr;
};

}  // namespace agentengine
