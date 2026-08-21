#pragma once
// Implements 002-Agent-Model-and-Authoring.md §5's `turn` interception point AND
// 017-Safety-and-Content-Governance.md §4's `pre_model` filter point -- one mechanism closes both,
// per decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md. Depends on
// decisions/ADR-066-context-provider-attribution-provenance.md (provenance must exist first -- see
// context_assembly.hpp's own `assemble_context()` stamping loop).
//
// SCOPE, same "prove the mechanism against one real consumer, name the rest" precedent
// middleware.hpp's own top comment already states for its OWN run/turn/tool-call points: this file
// wires the turn/pre_model point as its own standalone, tested mechanism. Wiring it into
// `rt::AgentSession::run_rounds()` is deliberately NOT done here -- that file calls
// `history_provider_.on_context(...)` directly, at multiple call sites (the ordinary round path, the
// codeact-ask branch, and others), never `assemble_context()` itself, so there is no single seam to
// hook into without threading a middleware chain through several call sites of a large, mature,
// heavily tested file -- the identical situation middleware.hpp's own top comment already describes
// for the run/tool-call points, not a new problem this file introduces.
//
// CLOSES `pre_model` ONLY, NOT `post_model` -- `TurnContext` (below) carries no `ChatResponse` field
// and this chain runs exactly once, before the round's model call; it is structurally incapable of
// doing anything post-model-shaped (ADR-067 §4 finding #3).
//
// A REAL DESIGN REFINEMENT FOUND DURING IMPLEMENTATION, not anticipated by the design draft: the
// draft's own §2 said the turn/pre_model point "reuses ADR-033's proven composition shape verbatim
// ... before-phase forward, after-phase backward, onion-unwind on short-circuit." That shape
// (`middleware.hpp`'s `run_before`/`run_after`) exists to sandwich a REAL inner action (the backend
// model call) between a middleware's own before/after hooks. The turn/pre_model point has no
// analogous inner action to sandwich -- `assemble_context()` already ran, in full, before this chain
// even starts, and there is no model call between two turn middlewares to wrap. Reusing the full
// before/after onion here would have added a symmetric "after" phase with nothing real on the other
// side of it. What is actually implementable, and matches 017 §4's verdict vocabulary directly, is a
// single FORWARD pass over `Ms...`: each middleware's `on_turn(TurnContext&)` either succeeds
// (having applied whatever `annotate`/`redact` actions it wants via `TurnContext`/`ToolSurfaceView`
// directly, in place) or returns `std::unexpected` (`deny`), which stops the chain -- no later
// middleware runs. `require_approval` (017 §4's fifth verdict) is explicitly NOT modeled here: it
// would mean suspending the run for human approval before the model even sees this context, which
// needs `rt::AgentSession`'s own real suspend/approval machinery
// (`test_rt_agent_session_suspend_approval.cpp`) -- wiring that in is separate, unscoped work, named
// here rather than left implied (ADR-067 §7's own residual-risk discipline).
//
// A SECOND REFINEMENT, on `ToolSurfaceView`'s own guarantee: it is the SANCTIONED, documented way for
// a `turn` middleware to touch the tool surface, and using ONLY its public API there is no path to
// substituting a tool's `invoke` closure (proven in tests/test_turn_middleware.cpp). It is NOT a
// guarantee against a middleware that deliberately bypasses the sanctioned API and reaches into
// `TurnContext::assembled.combined.tools` directly (still a plain, mutable `std::vector<
// ToolDescriptor>&`, reachable through the SAME reference `TurnContext` already needs for message
// compaction) -- C++ offers no way to give one reference read/write access to `.messages` while
// denying it for `.tools` within the same `ContextContribution` without a larger struct-splitting
// refactor this ADR does not attempt. This matches this codebase's own established scoping for
// similar mechanisms -- `Tainted<T>`'s own doc comment: "not a fix for a careless or bad-faith wrap,"
// only for the accidental/disciplined-API case.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/context_assembly.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/middleware.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine {

// decisions/ADR-067 §3, the FATAL finding's fix: structural prevention, not detection. Never exposes
// a mutable `ToolDescriptor&` for anything fan-out already produced -- `redact()`/`reorder()`/
// `annotate_description()` are the ONLY mutations available, and none of them can touch `invoke`,
// `capability_ceiling`, or `approval_mode`. `redact()`/`reorder()` operate on a stable HANDLE (the
// original index into the fan-out-produced vector), applied at `finalize()`, so "the entry a
// middleware looked at" and "the entry that will actually be dispatched to" are provably the same
// object by construction -- there is no reconstruction step anywhere in this class for either
// mutation to hide inside.
class ToolSurfaceView {
public:
    explicit ToolSurfaceView(std::vector<ToolDescriptor>& tools) : tools_(tools) {}

    [[nodiscard]] std::span<ToolDescriptor const> descriptors() const noexcept { return tools_; }

    // Marks `handle` for removal at `finalize()`. Out-of-range handles are silently ignored (a
    // middleware that mis-tracks its own handle across a chain gets a no-op, not a crash -- matching
    // this file's general "fail into inertness, not into undefined behavior" posture for a content-
    // policy hook that must never be able to bring down a turn by itself).
    void redact(std::size_t handle) {
        if (handle < tools_.size()) redacted_.insert(handle);
    }

    // Declares the desired final ORDER, by original handle. Applied at `finalize()`, AFTER redaction
    // -- a handle both reordered and redacted is dropped (redaction wins); a handle omitted from
    // `new_order` entirely is ALSO dropped -- an incomplete reorder silently keeping tools nobody
    // asked to keep would be the wrong default for a security-relevant tool-surface mutation.
    void reorder(std::vector<std::size_t> new_order) { pending_reorder_ = std::move(new_order); }

    // The ONE field a `turn` middleware may edit directly, in place -- description is advisory text
    // shown to the model, never consulted for any trust/authority decision anywhere in this codebase
    // (`tool_pipeline.hpp`'s own `ToolDescriptor` never reads `.description` for approval/capability
    // purposes), so mutating it in place carries none of `invoke`/`capability_ceiling`/
    // `approval_mode`'s risk.
    void annotate_description(std::size_t handle, std::string shortened_text) {
        if (handle < tools_.size()) tools_[handle].description = std::move(shortened_text);
    }

    // Applies every pending `redact()`/`reorder()` against the underlying, fan-out-produced vector.
    // Called exactly once, by `run_turn_middleware_chain()` below, after the whole chain finishes --
    // NEVER by a middleware itself. A middleware calls `redact()`/`reorder()`/`annotate_description()`
    // on the ONE shared instance handed to it via `TurnContext::tool_surface` (below) -- constructing
    // a SEPARATE, local `ToolSurfaceView` over the same vector inside a middleware would silently
    // disconnect that middleware's own `redact()`/`reorder()` calls from the one `finalize()` this
    // chain actually calls, since each `ToolSurfaceView` instance owns its own `redacted_`/
    // `pending_reorder_` bookkeeping, not the vector itself (found the hard way, during this ADR's
    // own prove phase -- test_turn_middleware.cpp's first attempt built a fresh, local view both
    // inside the test middleware AND again after the chain to check the result, and neither ever saw
    // the other's mutations; fixed by giving `TurnContext` one shared `tool_surface` instance instead
    // of leaving construction to each call site).
    void finalize() {
        std::vector<ToolDescriptor> kept;
        if (pending_reorder_.has_value()) {
            kept.reserve(pending_reorder_->size());
            for (std::size_t h : *pending_reorder_) {
                if (h < tools_.size() && !redacted_.contains(h)) kept.push_back(std::move(tools_[h]));
            }
        } else {
            kept.reserve(tools_.size());
            for (std::size_t h = 0; h < tools_.size(); ++h) {
                if (!redacted_.contains(h)) kept.push_back(std::move(tools_[h]));
            }
        }
        tools_ = std::move(kept);
    }

private:
    std::vector<ToolDescriptor>&    tools_;
    std::set<std::size_t>           redacted_;
    std::optional<std::vector<std::size_t>> pending_reorder_;
};

// 002 §5's `turn` interception point context. Deliberately NO `EffectContext&`, no capability-
// related type -- the identical I2-structural argument `middleware.hpp`'s own `ModelCallContext`
// comment already makes for the model-call point: a hook cannot widen or even READ a capability
// because it is never handed one. `tool_surface` is constructed ONCE, wrapping `assembled.combined.
// tools`, and is the ONE instance every middleware in the chain sees and mutates -- see
// `ToolSurfaceView::finalize()`'s own comment for why a second, independently-constructed view over
// the same vector is not equivalent.
struct TurnContext {
    explicit TurnContext(ContextAssemblyResult& a) : assembled(a), tool_surface(a.combined.tools) {}

    ContextAssemblyResult& assembled;
    ToolSurfaceView        tool_surface;
};

// decisions/ADR-067 §5, the taint-boundary fix: the ONLY way a `turn` middleware may produce a
// replacement for an already-declassified `TaintedText` (e.g. `assembled.combined.instructions`).
// Subtractive-only -- removes a byte range and returns the result, still carrying the same
// declassification. There is no code path here that can introduce a byte that wasn't already present
// in `original`, so "read-to-re-emit inherits the original taint" is enforced by this function's own
// signature, not by a rule a middleware author has to remember (the general `TaintedText{}`
// constructor is simply never reachable at this boundary at all -- a middleware wanting to ADD new
// instruction text must do so as its own attributed contribution instead, matching §3's "adding a
// tool" rule).
//
// Fails safe on an invalid range rather than throwing/asserting -- a content-policy hook must never
// be able to crash a turn via a bad offset/length: `offset >= size` is a no-op (nothing to remove);
// `length` is clamped to what's actually left after `offset`. The result is always exactly
// `original`'s bytes with one contiguous range removed -- a genuine subsequence of the input, for
// every `{offset, length}` including adversarial ones (proven by property test,
// tests/test_turn_middleware.cpp).
[[nodiscard]] inline TaintedText redact_subspan(TaintedText const& original, std::size_t offset,
                                                  std::size_t length) {
    std::string const& s = original.unsafe_view();
    if (offset >= s.size()) return original;
    std::size_t const clamped_length = std::min(length, s.size() - offset);
    std::string out = s.substr(0, offset);
    out += s.substr(offset + clamped_length);
    return TaintedText{std::move(out)};
}

namespace turn_middleware_detail {

template <class M>
concept HasOnTurn = requires(M& m, TurnContext& c) {
    { m.on_turn(c) } -> std::same_as<task<result<std::monostate>>>;
};

[[nodiscard]] inline std::optional<std::string> tool_call_id_of(Message const& m) {
    for (auto const& item : m.content) {
        if (auto const* c = std::get_if<ToolCall>(&item.value)) return c->call_id;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::vector<std::string> tool_result_call_ids_of(Message const& m) {
    std::vector<std::string> ids;
    for (auto const& item : m.content) {
        if (auto const* r = std::get_if<ToolResult>(&item.value)) ids.push_back(r->call_id);
    }
    return ids;
}

template <std::size_t I, class Tuple>
task<result<std::monostate>> run_turn_chain(Tuple& mws, TurnContext& ctx,
                                              MiddlewareTraceHook const& trace_hook) {
    if constexpr (I == std::tuple_size_v<Tuple>) {
        co_return result<std::monostate>{};
    } else {
        using M = std::tuple_element_t<I, Tuple>;
        if constexpr (HasOnTurn<M>) {
            result<std::monostate> outcome{};
            bool threw = false;
            try {
                outcome = co_await std::get<I>(mws).on_turn(ctx);
            } catch (std::exception const& e) {
                threw = true;
                outcome = std::unexpected(
                    error{failure_class::fatal,
                          "turn middleware '" + std::string(middleware_detail::middleware_name<M>()) +
                              "' threw in on_turn: " + e.what(),
                          "turn_middleware.hook_threw"});
            } catch (...) {
                threw = true;
                outcome = std::unexpected(error{
                    failure_class::fatal,
                    "turn middleware '" + std::string(middleware_detail::middleware_name<M>()) +
                        "' threw a non-std::exception value in on_turn",
                    "turn_middleware.hook_threw"});
            }
            if (trace_hook) {
                trace_hook(MiddlewareTraceEvent{middleware_detail::middleware_name<M>(), "on_turn",
                                                  !outcome.has_value(), threw});
            }
            if (!outcome) co_return outcome;
        }
        co_return co_await run_turn_chain<I + 1>(mws, ctx, trace_hook);
    }
}

}  // namespace turn_middleware_detail

// The chain entry point. `Ms...` is a declared, compile-time-ordered pack (`std::tuple<Ms...>`,
// matching `ComposedContextProvider`'s own convention) -- position 0 runs first; the FIRST `on_turn`
// that returns `std::unexpected` (017 §4's `deny` verdict) stops the chain, no later middleware runs.
// `allow`/`annotate`/`redact` are all expressed as a middleware calling `ctx.tool_surface`/
// `redact_subspan` itself, in place, before returning success -- there is no separate verdict enum,
// matching `ModelCallContext::settled()`'s own "one outcome object, not a richer result type" shape.
// Calls `ctx.tool_surface.finalize()` exactly once, unconditionally, after the chain settles (denied
// or not) -- the ONE place any pending `redact()`/`reorder()` actually takes effect.
template <class... Ms>
[[nodiscard]] task<result<std::monostate>> run_turn_middleware_chain(
    std::tuple<Ms...>& middlewares, TurnContext& ctx, MiddlewareTraceHook trace_hook = nullptr) {
    result<std::monostate> outcome =
        co_await turn_middleware_detail::run_turn_chain<0>(middlewares, ctx, trace_hook);
    ctx.tool_surface.finalize();
    co_return outcome;
}

// A host-injected callback, matching this codebase's own established idiom (`ApprovalDecider`,
// `MiddlewareTraceHook`, `ContentReplayTrigger`) -- the ONE seam `rt::AgentSession` (agent_session.hpp)
// actually calls, once per round, right before building that round's `ChatRequest`. A host wanting a
// declared `std::tuple<Ms...>` chain builds it externally and wraps `run_turn_middleware_chain()`
// (above) in a closure over that tuple; `AgentSession` itself never needs to know `Ms...` at compile
// time, matching the same reason `MiddlewareTraceHook` is `std::function`-erased rather than adding a
// template parameter to whatever calls it. `nullptr` (the default) means no turn middleware at all --
// every existing `AgentSession<...>` specialization is unaffected until a caller opts in.
using TurnMiddlewareHook = std::function<task<result<std::monostate>>(TurnContext&)>;

// decisions/ADR-067 §4 -- 005 §8 Q3's re-resolution, made real. A `turn`-level compactor that shapes
// what THIS turn's model call sees, never `history[]`: PROVABLE BY THE TYPE SIGNATURE, not merely by
// behavior -- `on_turn` below only ever touches `ctx.assembled`, and `TurnContext` (above) carries no
// reference to any session's durable `history_` at all, so there is no expression by which this type
// COULD touch it, the same "proof by absence" `ContributorProvenance`'s I2 argument already uses.
// Keeps the last `N` messages of the ASSEMBLED VIEW, extending the cut backward (never forward) to
// avoid splitting a `ToolCall`/`ToolResult` pair across the boundary -- the same atomicity invariant
// 005 §4's own `history[]` compaction already requires, applied here to the transient, per-turn view
// instead. A bare compile-time `N`, not `history_provider.hpp`'s `Window<N>` tag type -- this file
// has no dependency on that header, and there is exactly one strategy here today (no second one to
// disambiguate from via a wrapper tag the way `HistoryProvider<Strategy>` needs one).
template <std::size_t N>
struct Compactor {
    static constexpr std::string_view name = "compactor";  // decisions/ADR-067 §3/middleware.hpp's HasMiddlewareName precedent

    [[nodiscard]] task<result<std::monostate>> on_turn(TurnContext& ctx) {
        std::vector<Message>& msgs = ctx.assembled.combined.messages;
        if (msgs.size() > N) {
            std::size_t start = msgs.size() - N;
            bool changed = true;
            while (changed && start > 0) {
                changed = false;
                for (auto const& result_call_id : turn_middleware_detail::tool_result_call_ids_of(msgs[start])) {
                    bool matching_call_before_start = false;
                    for (std::size_t j = 0; j < start; ++j) {
                        if (turn_middleware_detail::tool_call_id_of(msgs[j]) == result_call_id) {
                            matching_call_before_start = true;
                            break;
                        }
                    }
                    if (matching_call_before_start) {
                        --start;
                        changed = true;
                        break;
                    }
                }
            }
            msgs.erase(msgs.begin(), msgs.begin() + static_cast<std::ptrdiff_t>(start));
        }
        co_return result<std::monostate>{};
    }
};

}  // namespace agentengine
