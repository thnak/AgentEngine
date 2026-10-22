// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 4a). Two approval
// declarations on one tool are refused, not resolved: the fold would keep the last `Approval<M>`, and with
// `ApprovalChosenByHost` it would be unclear whether the host or the declaration decides.

#include "agentengine/core/tool.hpp"

using namespace agentengine;

struct TwoApprovalTool
    : Tool<TwoApprovalTool, Approval<approval_mode::always_require>, ApprovalChosenByHost> {};

int main() { return TwoApprovalTool::declared_approval().has_value() ? 0 : 1; }
