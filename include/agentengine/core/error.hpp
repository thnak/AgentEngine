#pragma once
// Implements 001-Execution-Model.md §6 (failure classification) and CONVENTIONS.md's error model:
// ae::result<T> = std::expected<T, ae::error>. No exceptions for control flow (hot paths noexcept).

#include <expected>
#include <string>

namespace agentengine {

// 001 §6 — the only classes a failure can be; policy (retry, escalate, fail) is keyed off this,
// never off ad hoc string matching.
enum class failure_class {
    transient,  // retryable within the caller's remaining deadline (004 §4)
    policy,     // denied by capability or approval policy (007, 006 §4) — never from a model (I3)
    contract,   // caller violated a declared contract: unknown name, schema mismatch
    resource,   // budget, quota, or limit exceeded (023)
    fatal,      // unrecoverable; the run ends
};

// An actionable, host-owned error. `message` is what 026 §3 requires: what would work, never a
// stack trace, never an internal identifier the model has no use for.
struct error {
    failure_class klass;
    std::string   message;
    std::string   code;  // stable, machine-readable; safe to match on in tests and policy
};

template <class T>
using result = std::expected<T, error>;

} // namespace agentengine

namespace ae = agentengine;
