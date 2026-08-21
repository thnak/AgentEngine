#pragma once
// Implements 005-Sessions-State-and-Memory.md §3 — deterministic, budgeted assembly across an
// ordered list of `ContextProvider` contributors. Milestone 4 Phase B3
// (docs/planning/milestone-4-sessions-durability-memory-breakdown.md): proven here as its own
// standalone, generically-tested seam. `AgentSession` itself still wires exactly one contributor
// (`HistoryProviderT`, Phase B2's own shape) for now — widening `AgentSession`'s own wiring to this
// N-contributor table is deferred to Phase G, when a second real provider (memory) actually needs
// composing alongside `HistoryProvider` for the first time (the breakdown doc's decision 7: 029
// attaches through this seam once it is real, not before, so building the composition NOW against
// a second provider that doesn't exist yet would be designing against vocabulary that might still
// change shape).
//
// `assemble_context` runs contributors as independent fan-out, not a sequential pipeline (each
// `on_context` call below sees only `session_ctx`/`ctx`, never a prior contributor's
// `ContextContribution`) -- unlike MAF's `AIContextProvider`, which chains providers. This was a
// real design question (OpenQuestions.md OQ-18), designed and red-teamed, not an oversight: a
// generic reactive/pipeline mechanism was rejected (no provenance to know which provider produced
// what without MAF's message-source-stamping this seam doesn't have; reopens the same
// cross-contributor coupling the budget rule below already refuses). If you need a later contributor
// to react to an earlier one's output, write a purpose-built composite `ContextProvider` that owns
// its sub-providers directly (see `history_and_skills_provider.hpp`'s `HistoryAndSkillsProvider` for
// the proven shape) instead of extending this function -- read OQ-18 before reopening this.

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"

namespace agentengine {

// No tokenizer is wired in anywhere in this codebase yet (004's real `ChatClient` backends, and
// whatever token-counting they bring, are M5's job) — a byte/4 heuristic is a documented
// approximation for THIS seam's own budget arithmetic, never a claim of tokenizer accuracy. Named,
// not silently asserted precise, the same discipline `tool_pipeline.hpp`'s `ToolInvocationAudit`
// comment already uses for "016's full span shape is out of scope."
[[nodiscard]] inline std::uint64_t approx_token_count(std::string_view text) noexcept {
    return (text.size() + 3) / 4;
}

[[nodiscard]] inline std::uint64_t approx_token_count(Message const& m) noexcept {
    std::uint64_t total = 0;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) total += approx_token_count(t->text);
    }
    return total;
}

// "Every contributor declares a token budget" (005 §3). `max_tokens == 0` means unbounded — a
// contributor that never drops, matching `HistoryProvider<Window<0>>`'s own "0 means unbounded"
// convention (history_provider.hpp) rather than inventing a second sentinel for the same idea.
// ae-naming-lint: allow ContextBudget — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ContextBudget {
    std::uint64_t max_tokens = 0;
};

// decisions/ADR-066-context-provider-attribution-provenance.md §3: a `ContextProvider` type must
// declare its own name at compile time -- the identical requirement, and the identical reasoning,
// as `Middleware`'s own `HasMiddlewareName` (middleware.hpp): this codebase does not use
// typeid/RTTI to fabricate one (CONVENTIONS.md), and provenance stamping (below) needs a real,
// stable name to stamp with. Reused verbatim rather than inventing a second name convention for the
// same idea.
template <class T>
concept HasContextProviderName = requires {
    { T::name } -> std::convertible_to<std::string_view>;
};

template <class T>
[[nodiscard]] constexpr std::string_view context_provider_name() noexcept {
    static_assert(HasContextProviderName<T>,
                  "a ContextProvider type must declare `static constexpr std::string_view name` "
                  "(decisions/ADR-066-context-provider-attribution-provenance.md §3) -- this "
                  "codebase does not use typeid/RTTI to fabricate one");
    return T::name;
}

// One recorded drop — "drops are recorded in the trace" (005 §3). A minimal in-memory record
// (016's full span/telemetry shape is out of scope here, matching `ToolInvocationAudit`'s own
// precedent for the same "not yet built" reason) rather than a real trace-emission call.
// ae-naming-lint: allow ContextDrop — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ContextDrop {
    std::size_t contributor_index = 0;
    std::string contributor_message_id;  // the dropped Message's own id; empty if it had none
    std::string reason;
};

// A type-erased `ContextProvider` instance plus its declared per-contributor budget — mirrors
// `core/tool_pipeline.hpp`'s `ToolDescriptor`/`ToolTable` shape exactly (name, erasure via
// `std::function`, a `make_*_descriptor<T>()` factory): 006 §6 already established this project's
// answer to "N compile-time-declared conformers, one runtime table"; this is that same shape
// applied to `ContextProvider` instead of `Tool`.
// ae-naming-lint: allow ContextProviderDescriptor — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ContextProviderDescriptor {
    // Milestone 5 Phase B4: task<T>-returning, matching the ContextProvider concept's own now-real
    // signature (context_provider.hpp) -- forced by the same cascade that made HistoryProvider and
    // MemoryProvider real coroutines, not an independent design choice here.
    using OnContextFn = std::function<task<result<ContextContribution>>(SessionContext&, EffectContext&)>;
    // `task<std::monostate>`, not `task<>` -- historical: `quark::task<void>` wasn't awaitable at
    // all before ADR-037 (see context_provider.hpp's own comment on `ContextProvider::on_turn_end`
    // for the full history; `task<void>` is genuinely awaitable now, this is kept by convention, not
    // necessity); nothing here calls
    // `co_await` on the stored closure's result today (`assemble_context` never invokes
    // `on_turn_end`), but the type must still match what every real conformer's method returns.
    using OnTurnEndFn = std::function<task<std::monostate>(TurnView, EffectContext&)>;

    // decisions/ADR-066-...: this contributor's declared `contributor_type` -- set from
    // `ProviderT::name` by `make_context_provider_descriptor<ProviderT>()` below, never by a caller
    // hand-building a descriptor (there is no public setter; a hand-built `ContextProviderDescriptor`
    // that bypasses the factory gets an empty name, same fail-... shape `ToolDescriptor::
    // effect_class`'s own comment documents for a hand-built descriptor bypassing ITS factory).
    std::string   name;
    ContextBudget budget;
    OnContextFn   on_context;
    OnTurnEndFn   on_turn_end;
};

// `provider` is moved into a `shared_ptr` so the SAME instance backs both closures — a stateful
// provider's `on_turn_end` (Phase G3's memory-extraction hook) must see what its own `on_context`
// produced this turn, and must persist that state across turns, not be reconstructed per call.
// Neither closure is itself a coroutine -- calling `shared->on_context(...)` just creates and
// returns the (lazy, not-yet-run) task<T> object, which the closure forwards by value; the actual
// body only runs once something later `co_await`s it (rt/task.hpp's own lazy-start idiom, historical:
// quark/core/task.hpp's before ADR-037 removed Quark).
template <class ProviderT>
    requires ContextProvider<ProviderT> && HasContextProviderName<ProviderT>
[[nodiscard]] ContextProviderDescriptor make_context_provider_descriptor(ProviderT provider,
                                                                           ContextBudget budget) {
    auto shared = std::make_shared<ProviderT>(std::move(provider));
    ContextProviderDescriptor d;
    d.name        = std::string(context_provider_name<ProviderT>());
    d.budget      = budget;
    d.on_context  = [shared](SessionContext& sc, EffectContext& ec) { return shared->on_context(sc, ec); };
    d.on_turn_end = [shared](TurnView tv, EffectContext& ec) { return shared->on_turn_end(tv, ec); };
    return d;
}

// ae-naming-lint: allow ContextAssemblyResult — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ContextAssemblyResult {
    ContextContribution      combined;
    std::vector<ContextDrop> drops;
};

// 005 §3's assembly rules, generalized to N ordered contributors: every contributor runs, in
// declared order (`contributors[0]` first), and if ITS OWN contribution exceeds ITS OWN declared
// budget, the OLDEST messages within that one contribution are dropped first — oldest-first,
// matching `Window<N>`'s own "keep the last N verbatim" direction — until it fits. Never a
// cross-contributor competition for one shared pool: that would make drop order depend on
// contributor iteration order AND every other contributor's own size in a way that isn't
// predictable from the declared budgets alone, breaking "drop order is declared, not incidental."
// Deterministic and replayable given `{contributors, session_ctx, ctx}` (005 §3's own purity
// rule) — no wall-clock read, no randomness anywhere in this function. Milestone 5 Phase B4: a
// coroutine, `co_await`ing each contributor's `on_context` IN DECLARED ORDER, one at a time (never
// concurrently) -- 005 §3's own ordering rule ("contributors[0] first") is about drop-order
// determinism, not about serializing unrelated I/O, but running them one at a time is what "declared
// order" naturally means here and what every contributor's own `co_await` chain already assumes
// (no contributor's `chat()` call is written to tolerate running concurrently with another's).
[[nodiscard]] inline task<ContextAssemblyResult> assemble_context(
    std::vector<ContextProviderDescriptor>& contributors, SessionContext& session_ctx,
    EffectContext& ctx) {
    ContextAssemblyResult out;

    for (std::size_t i = 0; i < contributors.size(); ++i) {
        ContextProviderDescriptor& contributor = contributors[i];
        result<ContextContribution> contribution = co_await contributor.on_context(session_ctx, ctx);
        if (!contribution) {
            // 005 has no "one bad contributor aborts the whole run" rule — a failed contributor
            // simply contributes nothing this turn, the same fail-open-for-THIS-source shape
            // `assemble_context`'s own caller (AgentSession) uses fail-CLOSED for, at a different
            // layer (a provider failing is not the same as the model call itself failing).
            continue;
        }

        // Concatenating two ALREADY-explicitly-trusted TaintedText values and rewrapping the result
        // is not a new trust decision (neither capability-granting nor policy-deciding, `tainted.hpp`'s
        // own carve-out) -- each contributor already made its own declassification choice at
        // construction; this only merges data multiple contributors independently vetted.
        if (contribution->instructions.has_value()) {
            std::string const& piece = contribution->instructions->unsafe_view();
            out.combined.instructions = out.combined.instructions.has_value()
                                             ? TaintedText{out.combined.instructions->unsafe_view() + piece}
                                             : TaintedText{piece};
        }

        std::vector<Message>& msgs = contribution->messages;
        std::uint64_t const max_tokens = contributor.budget.max_tokens;
        if (max_tokens != 0) {
            std::uint64_t total = 0;
            for (auto const& m : msgs) total += approx_token_count(m);

            std::size_t drop_from = 0;
            while (total > max_tokens && drop_from < msgs.size()) {
                total -= approx_token_count(msgs[drop_from]);
                out.drops.push_back(ContextDrop{i, msgs[drop_from].message_id, "over budget"});
                ++drop_from;
            }
            msgs.erase(msgs.begin(), msgs.begin() + static_cast<std::ptrdiff_t>(drop_from));
        }

        // decisions/ADR-066-context-provider-attribution-provenance.md §2/§4 -- stamped here,
        // structurally, at the ONE seam every contribution already flows through unconditionally,
        // rather than trusted from each contributor's own discipline (MAF's shape; rejected, see the
        // ADR's §2). Two independent steps per message, in this order:
        //
        // 1. `content_origin::user` is checked, not merely stamped over: a message this
        //    contributor's own `on_context()` just constructed is genuinely `content_origin::user`
        //    ONLY if it is byte-identical to something already present in `session_ctx.history` --
        //    i.e. a real historical replay (`HistoryProvider<Window<N>>`'s whole job), never a
        //    synthesized one (a compromised/hostile plugin conformer, 009 §2, forging
        //    `content_origin::user` on invented text to claim it is literal human input, the
        //    concrete attack this closes). `content_origin::system`/`::assistant`/`::tool` are left
        //    untouched by this check -- unlike `::user`, those are legitimately claimed by
        //    C++-authored, host-controlled contributor code today (`SkillsProvider`'s own
        //    host-declared advertisement message deliberately sets `content_origin::system`,
        //    `skill_provider.hpp:136`; I3 constrains MODEL output, not engine/host code, so
        //    blanket-clamping every non-replayed origin to `::external` would have been a real
        //    regression against that already-shipped, already-tested case, not a fix). This also
        //    closes a real, previously-unstamped bug this same pass found by re-grounding against
        //    shipped code: `HistoryProvider<Summarize<N,SummarizerT>>`'s synthesized summary message
        //    (history_provider.hpp) inherits whatever `content_origin` the summarizer's raw response
        //    carried (typically `::assistant`, ContentItem's own default) with nothing to mark it as
        //    a SUMMARY rather than a real assistant turn -- not verbatim-present in
        //    `session_ctx.history` (it's newly synthesized text), so this check now catches it too
        //    (as `::assistant`, unaffected by the `::user`-only clamp -- named as a real residual,
        //    not silently claimed fixed by this ADR; see its own §7/evidence).
        // 2. `attribution{contributor_index, contributor_type}` is stamped unconditionally,
        //    including on a verbatim-replayed message -- provenance ("who surfaced this content
        //    THIS turn") and content_origin ("what kind of turn content is this, semantically") are
        //    orthogonal; a real user message replayed by `HistoryProvider` is still genuinely
        //    `content_origin::user` (step 1 keeps it that way) AND genuinely attributable to the
        //    `history` contributor for this turn (step 2 stamps that).
        //
        // The `session_ctx.history` scan is checked BEFORE this contributor's own messages are
        // stamped with `attribution` (an unstamped provider-constructed message, e.g. a fresh
        // `HistoryProvider<Window<N>>` copy, equality-compares identically to its `session_ctx.
        // history` source, which is likewise never itself stamped -- see AgentSession::run_rounds(),
        // which appends real turn input/output straight into `history_` without ever routing it
        // through `assemble_context()`). O(this contributor's message count x history size) --
        // acceptable here for the same reason this whole function's own top comment already accepts
        // non-hot-path cost (005 §3's determinism requirement, not perf, is what's load-bearing).
        for (Message& m : msgs) {
            bool const is_verbatim_history_replay =
                std::ranges::find(session_ctx.history, m) != session_ctx.history.end();
            if (!is_verbatim_history_replay) {
                for (ContentItem& item : m.content) {
                    if (item.origin == content_origin::user) item.origin = content_origin::external;
                }
            }
            m.attribution = ContributorProvenance{i, contributor.name};
        }
        for (ToolDescriptor& t : contribution->tools) {
            t.attribution = ContributorProvenance{i, contributor.name};
        }

        out.combined.messages.insert(out.combined.messages.end(), msgs.begin(), msgs.end());
        out.combined.tools.insert(out.combined.tools.end(), contribution->tools.begin(),
                                   contribution->tools.end());
    }

    co_return out;
}

} // namespace agentengine
