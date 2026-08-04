#pragma once
// Implements 003-Message-and-Content-Model.md §2 and 007-Capability-and-Trust-Model.md §4 — the
// taint mechanism enforcing I3. Model-originated and externally-retrieved content is `Tainted<T>`,
// transitively; no capability-granting or policy-deciding API may accept a tainted value. This is
// a *type-level* marker, not a bit someone remembers to check: `Tainted<T>` has no implicit
// conversion to `T`/`T const&`, so a tainted value cannot silently flow into an API expecting the
// untainted type. Converting tainted -> trusted requires an explicit, named declassifier (007 §4);
// `unsafe_view()` is that one escape hatch, named `unsafe_` so a caller cannot use it by accident
// (CONVENTIONS.md: no implicit anything on a security-critical path).
//
// `TaintedText` (content.hpp) is `Tainted<std::string>` specialized for the text/bytes case (003
// §2: "not a separate mechanism") — declared here once, aliased there, not reimplemented twice.
//
// Kept per-item, not span-level (007 §10 Q2, decisions/ADR-007-span-level-taint-vs-per-item.md):
// this wrapper taints a whole value, never a sub-range.

#include <type_traits>
#include <utility>

namespace agentengine {

template <class T>
class Tainted {  // ae-naming-lint: allow Tainted — 003 §2 / 007 §4 name this type normatively; 027 has not been updated to list it (same tracked-gap category as the M0 backlog)
public:
    explicit Tainted(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)) {}

    // The one declassification escape hatch (007 §4). Returns the wrapped value without checking
    // anything — an explicit, logged declassifier (schema validation, allowlist match, path check)
    // is the caller's responsibility; this accessor itself does no checking by design, so it must
    // never be reached from a capability-granting or policy-deciding code path directly.
    [[nodiscard]] T const& unsafe_view() const noexcept { return value_; }

    // Deliberately NOT provided: `operator T() const`, `operator T const&() const`, or any
    // conversion to a capability-API parameter type (e.g. `std::string_view` for `T = std::string`).
    // 007 §9 G2 is a compile-fail proof that no such conversion exists — see
    // tests/compile_fail/tainted_no_implicit_conversion.cpp.

private:
    T value_;
};

} // namespace agentengine
