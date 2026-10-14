#pragma once
// Implements 001-Execution-Model.md §6 (failure classification) and CONVENTIONS.md's error model:
// ae::result<T> = std::expected<T, ae::error>. No exceptions for control flow (hot paths noexcept).
// ADR-237 §9 D6: `failure_class::canceled`, the one string spelling of every class, and the one retry
// decision (`is_retryable`). Every classification site is listed in
// docs/research/2026-10-03-failure-class-canceled-inventory.md.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace agentengine {

// 001 §6 — the only classes a failure can be; policy (retry, escalate, fail) is keyed off this,
// never off ad hoc string matching.
enum class failure_class {
    transient,  // retryable within the caller's remaining deadline (004 §4)
    policy,     // denied by capability or approval policy (007, 006 §4) — never from a model (I3)
    contract,   // caller violated a declared contract: unknown name, schema mismatch
    resource,   // budget, quota, or limit exceeded (023) -- a deadline included: code `*deadline_exceeded` (I8)
    fatal,      // unrecoverable; the run ends
    // ADR-237 D6: stopped on request -- by the host, a parent, or a sibling's failure. Not retryable, not a
    // policy denial, not a fault: never retried, never rerouted to a workflow fallback, never propagated as a
    // failure marker. A run the host canceled ends `canceled` + `run.canceled` (amends ADR-178). A deadline is
    // NOT this class: "ran out of time" is `resource`, so the two are told apart by class, not by code.
    canceled,
};

// ADR-237 D6: the one spelling of each class -- recordings, the test driver's scripts, failure markers.
[[nodiscard]] constexpr std::string_view failure_class_to_string(failure_class k) noexcept {
    switch (k) {
        case failure_class::transient: return "transient";
        case failure_class::policy:    return "policy";
        case failure_class::contract:  return "contract";
        case failure_class::resource:  return "resource";
        case failure_class::fatal:     return "fatal";
        case failure_class::canceled:  return "canceled";
    }
    return "fatal";
}

[[nodiscard]] constexpr std::optional<failure_class> failure_class_from_string(std::string_view s) noexcept {
    for (failure_class k : {failure_class::transient, failure_class::policy, failure_class::contract,
                            failure_class::resource, failure_class::fatal, failure_class::canceled}) {
        if (s == failure_class_to_string(k)) return k;
    }
    return std::nullopt;
}

// ADR-237 D6: which budget a retry runs under -- the only thing a retrying site states. Which CLASSES retry is
// decided once, in `is_retryable` below, never per site.
//   shared: the same call re-sent inside the caller's same deadline and budget (ModelCallGateway: its backoff
//           never sleeps past `ctx.deadline`). 001 §6 / 004 §4: Transient only.
//   fresh:  a whole unit re-invoked with a budget of its own (a spawned child session's own run budget,
//           `multi_agent::spawn_with_retry`; a workflow executor re-invoked by its edge's `retry` policy).
enum class retry_budget : std::uint8_t { shared, fresh };

// ADR-237 D6 -- THE retry decision. `canceled` never retries (someone asked it to stop; a retry would undo that),
// nor do `policy` (retrying a denial is how a loop becomes an attack, 001 §6), `contract` or `fatal`. `resource`
// -- a deadline (`deadline_exceeded`) included -- is never retried under the budget it exhausted, and may be
// retried by a fresh attempt that carries its own budget (the bound on attempts stays the caller's).
[[nodiscard]] constexpr bool is_retryable(failure_class k, retry_budget b) noexcept {
    switch (k) {
        case failure_class::transient: return true;
        case failure_class::resource:  return b == retry_budget::fresh;
        case failure_class::policy:
        case failure_class::contract:
        case failure_class::fatal:
        case failure_class::canceled:  return false;
    }
    return false;
}

// An actionable, host-owned error. `message` is what 026 §3 requires: what would work, never a
// stack trace, never an internal identifier the model has no use for.
struct error {  // ae-naming-lint: allow error — pre-existing M0 scaffolding, reconcile at owning milestone
    failure_class klass;
    std::string   message;
    std::string   code;  // stable, machine-readable; safe to match on in tests and policy
    // Milestone 3 Phase G4 (026-Agent-Facing-Runtime-Surface.md §3, §9 Q2's "sourced from real
    // occurrences, never hand-authored" resolution): an optional platform error code (win32
    // GetLastError()/errno) a guest-facing boundary can use to raise a REAL, correctly-typed,
    // correctly-worded exception (e.g. PyErr_SetFromWindowsErr) instead of re-wording `message`
    // itself. 0 means none -- most `error`s are host-authored contract/policy violations with no OS
    // code behind them, and `message` remains their only text.
    int native_code = 0;
};

template <class T>
using result = std::expected<T, error>;  // ae-naming-lint: allow result — pre-existing M0 scaffolding, reconcile at owning milestone

} // namespace agentengine

namespace ae = agentengine;
