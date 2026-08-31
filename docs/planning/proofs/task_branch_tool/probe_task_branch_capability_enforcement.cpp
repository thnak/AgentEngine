// PROVE-PHASE PROBE: executes the two-tag GATING BEHAVIOR §41 claims in prose (see
// task_branch_capability_enforcement.hpp's own header comment for exactly what is mirrored, at what
// fidelity, and what this does/does not establish). Upgrades those claims from "reasoned" to "run."

#include "task_branch_capability_enforcement.hpp"

#include <cstdio>

using namespace probe;
using namespace probe::enforcement;

namespace {
int checks_run   = 0;
int checks_failed = 0;

void expect(bool cond, char const* label) {
    ++checks_run;
    std::printf("[%d] %-70s %s\n", checks_run, label, cond ? "PASS" : "FAIL");
    if (!cond) ++checks_failed;
}
}  // namespace

int main() {
    auto const isolated_ceiling = isolated_branch_tool_ceiling();  // start/run/discard tools
    auto const commit_ceiling   = commit_tool_ceiling();           // commit tool alone

    // [1] TaskBranch-only grant: exactly the "may try things in isolation" host intent.
    {
        auto held = MirroredCapabilitySet::grant({cap::TaskBranch{}});
        expect(try_invoke(held, isolated_ceiling),
               "TaskBranch alone binds start/run/discard ceiling");
        expect(!try_invoke(held, commit_ceiling),
               "TaskBranch alone does NOT bind commit ceiling (missing TaskBranchCommit)");
    }

    // [2] Both tags: exactly the "may try things AND merge into main" host intent.
    {
        auto held = MirroredCapabilitySet::grant({cap::TaskBranch{}, cap::TaskBranchCommit{}});
        expect(try_invoke(held, isolated_ceiling), "TaskBranch+Commit binds start/run/discard ceiling");
        expect(try_invoke(held, commit_ceiling), "TaskBranch+Commit binds commit ceiling");
    }

    // [3] TaskBranchCommit-only: the "inert grant" claim task_branch_capability.hpp's own PRECISION
    // comment makes in prose -- a host that somehow granted Commit without TaskBranch gets no real
    // tool access AT ALL, not even the commit tool (which also requires TaskBranch). Executed, not
    // assumed.
    {
        auto held = MirroredCapabilitySet::grant({cap::TaskBranchCommit{}});
        expect(!try_invoke(held, isolated_ceiling),
               "TaskBranchCommit alone does NOT bind start/run/discard ceiling");
        expect(!try_invoke(held, commit_ceiling),
               "TaskBranchCommit alone does NOT bind commit ceiling either (inert grant, confirmed)");
    }

    // [4] Nothing granted: both ceilings rejected.
    {
        auto held = MirroredCapabilitySet::grant({});
        expect(!try_invoke(held, isolated_ceiling), "empty grant binds nothing (isolated ceiling)");
        expect(!try_invoke(held, commit_ceiling), "empty grant binds nothing (commit ceiling)");
    }

    // [5] Insertion-order independence: the real tool_pipeline.hpp loop iterates the CEILING's own
    // declared order (task_branch_capability.hpp's usage sketch always lists TaskBranch first), never
    // the GRANT's insertion order -- so a grant built in the reverse order must behave identically to
    // [2]. A real ordering bug here would mean the mirror's grant-storage (not the ceiling check)
    // leaked an unintended order-dependency the real CapabilitySet (a flat, unordered any_of scan)
    // does not have.
    {
        auto held = MirroredCapabilitySet::grant({cap::TaskBranchCommit{}, cap::TaskBranch{}});
        expect(try_invoke(held, isolated_ceiling),
               "reverse-order grant still binds start/run/discard ceiling");
        expect(try_invoke(held, commit_ceiling), "reverse-order grant still binds commit ceiling");
    }

    // [6] Duplicate grants (a host granting the same capability twice, e.g. via two separate policy
    // rules that both happened to name TaskBranch): must not change the verdict either direction --
    // any_of-based contains() is idempotent under duplication by construction.
    {
        auto held = MirroredCapabilitySet::grant({cap::TaskBranch{}, cap::TaskBranch{}, cap::TaskBranch{}});
        expect(try_invoke(held, isolated_ceiling), "duplicate TaskBranch grants still bind (idempotent)");
        expect(!try_invoke(held, commit_ceiling),
               "duplicate TaskBranch grants still do NOT bind commit ceiling (Commit still missing)");
    }

    std::printf("\n%d/%d checks passed.\n", checks_run - checks_failed, checks_run);
    if (checks_failed > 0) {
        std::printf("%d CHECK(S) FAILED -- the two-tag gating behavior does NOT match what §41 claims "
                    "in prose. Do not treat the design as proven until this is green.\n",
                    checks_failed);
        return 1;
    }
    std::printf("ALL CHECKS PASSED -- the two-tag gating behavior (isolation-only vs isolation+commit, "
                "the inert-grant claim, order/duplicate independence) is executed, not merely reasoned "
                "about, against a structural mirror of the real CapabilitySet::bind()/tool_pipeline.hpp "
                "step 4/7 loop.\n");
    return 0;
}
