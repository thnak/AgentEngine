#pragma once
// Implements 010-Python-Code-Interpreter.md §1a, §3a — the Runner concept and ExecState. Named
// `Runner`, deliberately not `Executor` (027 §5 — that word already means a workflow graph node).

#include <string>
#include <unordered_map>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/sandbox.hpp"

namespace agentengine {

// {cwd, env}, one instance per session, shared **by reference** across every Runner call (010
// §3a). This is the object that makes "shared context" exact rather than eventually consistent —
// PythonRunner and ShellRunner both take a reference to the same ExecState, never a copy.
struct ExecState {
    std::string                                  cwd;
    std::unordered_map<std::string, std::string> env;
};

// The "held by the sandbox" half of 010 §3a that `ExecState` alone doesn't provide: a concrete
// carrier that hands back the SAME `ExecState&` for a given session on every call (never a fresh
// copy), so "shared by reference" is something a caller can actually rely on rather than trust by
// convention. `PythonRunner`/`ShellRunner` (Phase E2/E3) do not exist yet -- this is the part of
// §3a provable now, ahead of them, matching §3a's own two claims directly:
//   - "the same object, not a synchronized pair": `get_or_create` returns a reference into this
//     registry's own storage, not a copy -- proven by mutating through one reference and observing
//     the mutation through a second `get_or_create` call for the identical session id.
//   - "two sessions never share an ExecState any more than they share a heap": distinct session ids
//     get independent, unrelated `ExecState` instances by construction (`unordered_map` keyed by id).
// `std::unordered_map` is the right storage for this specifically because reference/pointer
// stability across further insertions is a guaranteed property of the container (only iterators may
// be invalidated by growth, never references to an existing element) -- a `vector`-backed store
// would not give `get_or_create` the right to keep handing out the same `ExecState&` forever.
// ae-naming-lint: allow SessionExecStateRegistry — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class SessionExecStateRegistry {
public:
    // Default-constructs a fresh `ExecState` on the FIRST call for `session_id`; every later call
    // for the same id returns a reference to that identical object.
    [[nodiscard]] ExecState& get_or_create(std::string const& session_id) {
        return state_by_session_[session_id];
    }

    // Tears a session's `ExecState` down (010 §3a: "the same per-session lifetime... as everything
    // else in [the sandbox]" -- when a session ends, its ExecState does not linger). A subsequent
    // `get_or_create` for the SAME id constructs a genuinely fresh object, never resurrects the
    // torn-down one -- this is what makes `destroy` a real teardown rather than merely hiding the
    // entry from `has()`.
    void destroy(std::string const& session_id) { state_by_session_.erase(session_id); }

    [[nodiscard]] bool has(std::string const& session_id) const {
        return state_by_session_.contains(session_id);
    }

private:
    std::unordered_map<std::string, ExecState> state_by_session_;
};

// concept, not a base class (010 §1a). PythonRunner and ShellRunner are the concrete kinds;
// neither is declared here — each is a seam backend (CONVENTIONS tier 2) with its own
// implementation, not core vocabulary.
//
// Return type is constrained to `result<ExecOutcome>` (synchronous) rather than `ae::task<T>` — the
// coroutine type CONVENTIONS.md/027 name as the eventual real signature (`core/task.hpp`; no longer
// gated on anything since ADR-037 -- `ae::task<T>` is a plain `agentengine::rt::task<T>` alias, zero
// Quark dependency) — because this header hasn't been migrated to it yet, not because the type is
// unavailable. Once it is, this constraint becomes `-> std::same_as<ae::task<result<ExecOutcome>>>`
// and every conforming `run()` changes with it — tracked here rather than left as a silently
// unconstrained `requires`.
template <class T>
concept Runner = requires(T runner, ExecRequest request, ExecState& state, EffectContext& ctx) {
    { runner.run(request, state, ctx) } -> std::same_as<result<ExecOutcome>>;
};

} // namespace agentengine
