// Positive control for tool_rejects_missing_approval.cpp and tool_rejects_two_approvals.cpp
// (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 4a): the same tools with exactly one approval
// declaration compile, so the negatives fail for the approval rule and not for some unrelated reason.

#include "agentengine/core/tool.hpp"

using namespace agentengine;

struct StatedTool : Tool<StatedTool, EffectClass<effect_class::pure>, Approval<approval_mode::never_require>> {};
struct HostChosenTool : Tool<HostChosenTool, ApprovalChosenByHost> {};

int main() {
    bool const ok = StatedTool::declared_approval() == approval_mode::never_require &&
                    !HostChosenTool::declared_approval().has_value();
    return ok ? 0 : 1;
}
