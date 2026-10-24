#pragma once
// Shared compile-time policy tags used at BOTH Tool (006 §1) and Agent (002 §2) declaration sites
// -- one definition, reused at both, per 006 §1: "Capabilities are declared on the tool, and the
// agent's ceiling must cover them" -- the same vocabulary describes both ceilings. Split out of
// core/agent.hpp (which used to define this alone) so core/tool.hpp can use it too without
// depending on the whole (heavier) Agent surface.

#include "agentengine/trust/capability.hpp"

namespace agentengine {

// `Cs...` are `trust::cap::decl::*` compile-time declaration tags (`FsRead<"mount", agentengine::unbounded()>`,
// `NetOut<"host", agentengine::unbounded()>`, ...; decisions/archive/ADR-009-capability-set-enforcement-mechanism.md).
template <class... Cs>
struct Capabilities {};  // ae-naming-lint: allow Capabilities — pre-existing M0 scaffolding, reconcile at owning milestone

// decisions/archive/ADR-237-async-extension-points-and-io-reactor.md §9 D3: the static BOUND of a per-call requirement.
// A tool that declares `PerCallRequirement<Cs...>` also provides `std::vector<Capability> required_capabilities(Args
// const&) const`; whatever that returns must be covered (`subsumes`) by one of `Cs...`, so the argument only ever selects
// WHICH already-held grant must cover the call, inside the kind and mount the declaration names. It binds nothing by
// itself and is not a ceiling: the AND-of-all `Capabilities<...>` ceiling cannot express "this mount, this program".
template <class... Cs>
struct PerCallRequirement {};  // ae-naming-lint: allow PerCallRequirement — ADR-237 D3 policy tag; 027 has not been updated to list it

}  // namespace agentengine
