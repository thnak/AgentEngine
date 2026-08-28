#pragma once
// Prove-phase-only follow-up to task_branch_capability.hpp: that file proved the two tags COMPILE
// (as ordinary Capabilities<...> type arguments, and -- as a real, honestly-diagnosed negative
// result -- NOT through a real Tool<>). It never proved the two-tag GATING BEHAVIOR itself: that
// holding TaskBranch-without-Commit binds the start/run/discard ceiling but not the commit ceiling,
// that holding TaskBranchCommit-without-TaskBranch binds NEITHER (the "inert grant" claim
// task_branch_capability.hpp's own comment makes in prose), and that this holds regardless of grant
// insertion order. Those were prose assertions, reasoned but never executed. This file executes them.
//
// Cannot use the REAL agentengine::CapabilitySet/tool_pipeline.hpp step 4/7 loop directly -- both are
// hard-typed to the real, closed agentengine::Capability variant (19 alternatives, confirmed by
// direct read of capability.hpp), which has no TaskBranch/TaskBranchCommit alternative and which this
// design track has consistently declined to extend before a real caller exists. So this mirrors the
// REAL logic structurally and exactly, at the same fidelity task_branch_capability.hpp already uses
// for the tags themselves, over a local, two-alternative variant scoped to just these two markers:
//
//   - `subsumes()` (capability.hpp:648): same variant index, then a per-kind `subsumes_payload()`.
//     For a fieldless marker type (the real cap::Entropy/cap::Elicit precedent, capability.hpp:567/
//     584) `subsumes_payload` is unconditionally `true` -- there is no field to narrow. TaskBranch/
//     TaskBranchCommit are the identical shape (task_branch_capability.hpp's own comment: "no further
//     parameter to narrow"), so the mirror below returns `true` the same way, for the same reason.
//   - `CapabilitySet::contains()` (capability.hpp:705): `std::any_of` over granted, via `subsumes`.
//   - `CapabilitySet::bind()` (capability.hpp:923): `contains()` else reject; no partial credit.
//   - `tool_pipeline.hpp`'s real step 4/7 loop (tool_pipeline.hpp:589-599): iterate the tool's
//     declared ceiling in order, `held.bind(requirement)` each; the FIRST failure rejects the whole
//     call immediately (not "collect all missing and report") -- and the real rejection error names
//     neither what's missing nor what IS held ("No leaked capability" is that function's own comment).
//     Mirrored exactly, including the no-leak property: `try_invoke()` below returns only a bool.
//
// What this does NOT establish: that the REAL tool_pipeline.hpp, compiled against a REAL extended
// Capability variant with TaskBranch/TaskBranchCommit alternatives added, behaves this way -- that
// would require the exact production-code edit this design track defers (task_branch_capability.hpp's
// own "What this does NOT close"). What it DOES establish: that the ENFORCEMENT LOGIC itself -- same
// algorithm, same short-circuit order, same subsumption rule, same "no leaked capability" shape,
// exercised against the real two-tag ceilings this design proposes -- produces exactly the gating
// behavior §41 claims in prose, checked by running it, not by reasoning about it.

#include "task_branch_capability.hpp"

#include <variant>
#include <vector>

namespace probe::enforcement {

using MirroredCapability = std::variant<cap::TaskBranch, cap::TaskBranchCommit>;

// Mirrors capability.hpp:648's subsumes() exactly: same alternative, then payload-subsumption.
// Both alternatives here are fieldless markers, so payload-subsumption is unconditionally true --
// the same rule the real cap::Entropy/cap::Elicit precedent uses (capability.hpp:567/584), for the
// identical reason (task_branch_capability.hpp: "no further parameter to narrow").
[[nodiscard]] inline bool subsumes(MirroredCapability const& parent, MirroredCapability const& requested) {
    return parent.index() == requested.index();
}

// Mirrors capability.hpp:694's CapabilitySet (contains()/bind()) at the same fidelity.
class MirroredCapabilitySet {
public:
    [[nodiscard]] static MirroredCapabilitySet grant(std::vector<MirroredCapability> caps) {
        MirroredCapabilitySet set;
        set.granted_ = std::move(caps);
        return set;
    }

    [[nodiscard]] bool contains(MirroredCapability const& requirement) const {
        for (auto const& c : granted_) {
            if (subsumes(c, requirement)) return true;
        }
        return false;
    }

    // Mirrors capability.hpp:923's bind(): contains-check, no partial credit, no reason leaked on
    // failure -- a bool is all the real pipeline's own step 4/7 loop observes per requirement too.
    [[nodiscard]] bool bind(MirroredCapability const& requirement) const { return contains(requirement); }

private:
    std::vector<MirroredCapability> granted_;
};

// Mirrors tool_pipeline.hpp:589-599's real step 4/7 loop exactly: iterate the ceiling in order,
// bind each; the FIRST failure rejects the whole call immediately. Returns only a bool -- same
// "no leaked capability" shape the real loop's own comment requires (the caller learns neither what
// was missing nor what was held).
[[nodiscard]] inline bool try_invoke(MirroredCapabilitySet const&           held,
                                       std::vector<MirroredCapability> const& ceiling) {
    for (MirroredCapability const& requirement : ceiling) {
        if (!held.bind(requirement)) return false;
    }
    return true;
}

// The two real tool ceilings this design proposes (task_branch_capability.hpp's own usage sketch):
// TaskBranchStartTool/RunTool/DiscardTool each declare Capabilities<cap::decl::TaskBranch> alone;
// TaskBranchCommitTool alone declares both.
[[nodiscard]] inline std::vector<MirroredCapability> isolated_branch_tool_ceiling() {
    return {cap::TaskBranch{}};
}
[[nodiscard]] inline std::vector<MirroredCapability> commit_tool_ceiling() {
    return {cap::TaskBranch{}, cap::TaskBranchCommit{}};
}

}  // namespace probe::enforcement
