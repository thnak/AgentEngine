#pragma once
// Implements 002-Agent-Model-and-Authoring.md §5's `Middleware<Ms...>` -- an ordered chain wrapping
// the model call, one of the four declared interception points ("run", "turn", "model call", "tool
// call"). This file moves the MODEL-CALL point from "declared CRTP policy tag with no
// chain-composition logic or consumer" (§5's own prior text) to real, tested code. ADR-033
// (decisions/ADR-033-middleware-model-call-chain.md).
//
// SCOPE, narrower than "implement Middleware<Ms...>" sounds, deliberately: only the model-call point
// is wired to a real consumer. This file's own machinery below (`ModelCallContext`, `run_before`/
// `run_after`, `enforce_backend_tool_call_provenance`, `MiddlewareTraceHook`) is that real consumer's
// shared implementation -- originally reached only through `MiddlewareChatClient<Inner, Ms...>` (a
// `ChatClient`-conforming template decorator, wrapping `Inner::chat()`), which was REMOVED 2026-08-12
// along with `FailoverChatClient`/`ResilientChatClient` (this repo had shipped nowhere, so there was
// no deprecation-then-migration cost to justify keeping a `chat()`-only wrapper once ADR-036 gave
// `AgentSession` a real, streaming-capable path). `MiddlewareModelCallGateway<Inner, Ms...>`
// (core/model_call_gateway.hpp, ADR-036) is the current real consumer -- it reuses every type/function
// below VERBATIM, unchanged by the removal; only `MiddlewareChatClient` itself (further down this
// file) is gone. The run/turn/tool-call interception points are NOT wired into `AgentSession`'s turn
// loop -- that file is large, mature, and heavily tested; threading a middleware chain through its
// template parameters and every call site is separately-scoped, larger work, matching this project's
// own established "prove the mechanism against one real consumer, name the rest" precedent
// (decisions/ADR-028-session-scoped-stateful-tools.md).
//
// I2 ENFORCEMENT, stated once here because it shapes every type below: `ModelCallContext` carries
// NEITHER `EffectContext&` nor any capability-related type. A hook structurally cannot widen (or
// even read) a capability, because it is never handed one -- this is enforced by omission, not by a
// runtime check, matching this project's "no ambient authority" posture applied to a new call site.
//
// THE FATAL FINDING THIS FILE CLOSES (found by an independent red-team pass before any of this was
// implemented -- see ADR-033 §3): capability-widening is not the only way middleware could reach
// unauthorized effect. `after_model` (or a `before_model` short-circuit) can freely rewrite
// `ChatResponse.message.content`, which may contain `ToolCall` items. A `ToolCall` defaults to
// `call_provenance::vendor_structured` (content.hpp) -- the FULLY TRUSTED provenance class, which
// `invoke_tool`'s step 5 checks only against the target tool's own `approval_mode` (may be
// `never_require`). A middleware that fabricates or mutates a `ToolCall` therefore launders an
// untrusted decision through the same channel a genuine, vendor-returned tool call travels --
// exactly the confused-deputy shape `decisions/ADR-023...` (007 §4 amendment) already forced closed
// once for raw model TEXT, relocated here to a middleware CONTENT REWRITE. Closed by
// `middleware_detail::enforce_backend_tool_call_provenance`: any `ToolCall` in the final response
// that is not byte-identical (ignoring provenance/origin) to one the REAL backend call actually
// returned is forced to `call_provenance::text_derived` before this wrapper ever returns it --
// routing it through ADR-023's strict `is_auto_declassifiable_text_derived_call` gate, which
// overrides even a tool's own `never_require` for anything with a real capability ceiling or a
// non-pure effect class. A `before_model` short-circuit (the real backend is never called at all)
// downgrades EVERY `ToolCall` in its fabricated response -- there is no "raw" response to compare
// against, so nothing in it can claim vendor trust.

#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/task.hpp"

namespace agentengine {

// The model-call interception point's context (002 §5's `ModelCallContext`, forward-declared there
// as a name only). `request` is a mutable working copy a `before_model` hook may rewrite before the
// real call. `response`/`failure` are the SAME two fields used both for "a before_model hook
// short-circuits" and "an after_model hook reviews the real outcome" -- one context object, no
// separate short-circuit-vs-review pair to keep in sync. `settled()` is checked by the chain runner
// after every before-hook; the FIRST hook that settles it stops the before-phase (no further
// before_model runs, the real backend is never called).
// ae-naming-lint: allow ModelCallContext — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ModelCallContext {
    ChatRequest                 request;
    std::optional<ChatResponse> response;
    std::optional<error>        failure;

    [[nodiscard]] bool settled() const noexcept { return response.has_value() || failure.has_value(); }
};

// 002 §5: "its effects are attributed to it by name in the trace." No trace sink exists anywhere in
// this codebase yet (confirmed by every prior ADR that has needed one and named the gap instead of
// building one -- ADR-031 §3, ADR-006's own precedent) -- so this is a caller-injected callback,
// nullptr by default, mirroring `workflow/supervisor.hpp`'s `WorkflowSupervisor::CheckpointHook`
// idiom exactly (an explicitly injected hook, never an ambient sink this file would have to own or
// template over). A host that wants real durability/OTel export closes over its own sink in the hook.
// ae-naming-lint: allow MiddlewareTraceEvent — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct MiddlewareTraceEvent {
    std::string_view middleware_name;
    std::string_view hook;       // "before_model" | "after_model"
    bool             settled_here = false;  // this hook's call is what settled ctx (short-circuit/deny)
    bool             threw = false;         // this hook's call ended in a caught exception
};
// ae-naming-lint: allow MiddlewareTraceHook — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
using MiddlewareTraceHook = std::function<void(MiddlewareTraceEvent const&)>;

namespace middleware_detail {

template <class M>
concept HasBeforeModel = requires(M& m, ModelCallContext& c) {
    { m.before_model(c) } -> std::same_as<task<std::monostate>>;
};

template <class M>
concept HasAfterModel = requires(M& m, ModelCallContext& c) {
    { m.after_model(c) } -> std::same_as<task<std::monostate>>;
};

// A `Middleware` type must declare its own name at compile time -- this codebase does not use
// typeid/RTTI to fabricate one (agent.hpp's own top comment: "CONVENTIONS.md -- no RTTI"), and 002
// §5's "attributed to it by name" constraint needs a real name to attribute to. Checked only when a
// hook actually exists to be named (below), so a hookless middleware (legal -- "any SUBSET of
// hooks", including the empty subset) never has to declare one.
template <class M>
concept HasMiddlewareName = requires {
    { M::name } -> std::convertible_to<std::string_view>;
};

template <class M>
[[nodiscard]] constexpr std::string_view middleware_name() noexcept {
    static_assert(HasMiddlewareName<M>,
                  "a Middleware type with at least one hook must declare "
                  "`static constexpr std::string_view name` (002 Section 5's \"attributed to it by "
                  "name in the trace\" constraint) -- this codebase does not use typeid/RTTI to "
                  "fabricate one");
    return M::name;
}

// If a hook set BOTH response and failure (a buggy hook, or two hooks disagreeing across the same
// settlement point), fail closed: a denial always wins over a response, never the reverse.
inline void reconcile_settlement(ModelCallContext& ctx) noexcept {
    if (ctx.failure.has_value() && ctx.response.has_value()) ctx.response.reset();
}

// -- The before-phase: forward over Ms..., first settlement stops it -----------------------------
template <std::size_t I, class Tuple>
task<std::monostate> run_before(Tuple& mws, ModelCallContext& ctx, std::size_t& stopped_at,
                                 MiddlewareTraceHook const& trace_hook) {
    if constexpr (I < std::tuple_size_v<Tuple>) {
        // Index I "gets a turn" the moment the loop reaches it, regardless of whether it defines
        // before_model -- this is what makes the after-phase's range correct for a middleware that
        // defines ONLY after_model (still gets called on the ordinary, non-short-circuited path).
        stopped_at = I;
        using M = std::tuple_element_t<I, Tuple>;
        if constexpr (HasBeforeModel<M>) {
            bool threw = false;
            try {
                std::monostate const ignored = co_await std::get<I>(mws).before_model(ctx);
                (void)ignored;
            } catch (std::exception const& e) {
                threw = true;
                ctx.response.reset();
                ctx.failure = error{failure_class::fatal,
                                     "middleware '" + std::string(middleware_name<M>()) +
                                         "' threw in before_model: " + e.what(),
                                     "middleware.hook_threw"};
            } catch (...) {
                threw = true;
                ctx.response.reset();
                ctx.failure = error{failure_class::fatal,
                                     "middleware '" + std::string(middleware_name<M>()) +
                                         "' threw a non-std::exception value in before_model",
                                     "middleware.hook_threw"};
            }
            reconcile_settlement(ctx);
            if (trace_hook) {
                trace_hook(MiddlewareTraceEvent{middleware_name<M>(), "before_model", ctx.settled(), threw});
            }
        }
        if (!ctx.settled()) {
            std::monostate const ignored = co_await run_before<I + 1>(mws, ctx, stopped_at, trace_hook);
            (void)ignored;
        }
    }
    co_return std::monostate{};
}

// -- The after-phase: compile-time unrolled to the deepest index FIRST, hooks fire on the way back
// out (I <= stopped_at only) -- an onion unwind, symmetric with a real nested-decorator chain: the
// middleware that itself caused a short-circuit still gets its own after-phase turn, exactly as a
// real `M::chat() { before(); if (!short_circuited) inner->chat(); after(); }` method would run its
// own `after()` regardless of which branch it took.
template <std::size_t I, class Tuple>
task<std::monostate> run_after(Tuple& mws, ModelCallContext& ctx, std::size_t stopped_at,
                                MiddlewareTraceHook const& trace_hook) {
    if constexpr (I < std::tuple_size_v<Tuple>) {
        {
            std::monostate const ignored = co_await run_after<I + 1>(mws, ctx, stopped_at, trace_hook);
            (void)ignored;
        }
        if (I <= stopped_at) {
            using M = std::tuple_element_t<I, Tuple>;
            if constexpr (HasAfterModel<M>) {
                bool threw = false;
                try {
                    std::monostate const ignored = co_await std::get<I>(mws).after_model(ctx);
                    (void)ignored;
                } catch (std::exception const& e) {
                    threw = true;
                    ctx.response.reset();
                    ctx.failure = error{failure_class::fatal,
                                         "middleware '" + std::string(middleware_name<M>()) +
                                             "' threw in after_model: " + e.what(),
                                         "middleware.hook_threw"};
                } catch (...) {
                    threw = true;
                    ctx.response.reset();
                    ctx.failure = error{failure_class::fatal,
                                         "middleware '" + std::string(middleware_name<M>()) +
                                             "' threw a non-std::exception value in after_model",
                                         "middleware.hook_threw"};
                }
                reconcile_settlement(ctx);
                if (trace_hook) {
                    trace_hook(
                        MiddlewareTraceEvent{middleware_name<M>(), "after_model", ctx.settled(), threw});
                }
            }
        }
    }
    co_return std::monostate{};
}

[[nodiscard]] inline bool same_tool_call_ignoring_provenance(ToolCall const& a, ToolCall const& b) noexcept {
    return a.call_id == b.call_id && a.tool_name == b.tool_name && a.arguments_json == b.arguments_json;
}

// The fatal-finding fix -- see this file's top comment for the full rationale. Called exactly once,
// on the FINAL message this wrapper is about to return, after both phases have run.
inline void enforce_backend_tool_call_provenance(Message& final_message,
                                                  std::optional<ChatResponse> const& raw_backend_response) {
    for (auto& item : final_message.content) {
        if (!std::holds_alternative<ToolCall>(item.value)) continue;
        auto& call = std::get<ToolCall>(item.value);
        bool trusted = false;
        if (raw_backend_response.has_value()) {
            for (auto const& raw_item : raw_backend_response->message.content) {
                if (!std::holds_alternative<ToolCall>(raw_item.value)) continue;
                if (same_tool_call_ignoring_provenance(call, std::get<ToolCall>(raw_item.value))) {
                    trusted = true;
                    break;
                }
            }
        }
        if (!trusted) call.provenance = call_provenance::text_derived;
    }
}

}  // namespace middleware_detail

}  // namespace agentengine
