#pragma once
// Shared compile-time policy tags used at BOTH Tool (006 §1) and Agent (002 §2) declaration sites
// -- one definition, reused at both, per 006 §1: "Capabilities are declared on the tool, and the
// agent's ceiling must cover them" -- the same vocabulary describes both ceilings. Split out of
// core/agent.hpp (which used to define this alone) so core/tool.hpp can use it too without
// depending on the whole (heavier) Agent surface.

#include "agentengine/trust/capability.hpp"

namespace agentengine {

// `Cs...` are `trust::cap::decl::*` compile-time declaration tags (`FsRead<"mount">`,
// `NetOut<"host">`, ...; decisions/ADR-009-capability-set-enforcement-mechanism.md).
template <class... Cs>
struct Capabilities {};  // ae-naming-lint: allow Capabilities — pre-existing M0 scaffolding, reconcile at owning milestone

}  // namespace agentengine
