// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 4a). A `Tool<>` that
// declares neither `Approval<M>` nor `ApprovalChosenByHost` used to run unapproved -- `never_require` by
// omission. If this ever compiles, a tool's approval is again a choice nobody made.

#include "agentengine/core/tool.hpp"

using namespace agentengine;

struct NoApprovalTool : Tool<NoApprovalTool, EffectClass<effect_class::pure>> {};

int main() { return NoApprovalTool::declared_approval().has_value() ? 1 : 0; }
