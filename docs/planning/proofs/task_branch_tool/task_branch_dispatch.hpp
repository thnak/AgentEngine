#pragma once
// PROVE-PHASE PROBE HARNESS (A10 dispatch follow-up, 2026-08-28): closes the gap between
// `task_branch_capability_enforcement.hpp`'s already-proven two-tag GATING LOGIC (12 checks, pure
// boolean, `try_invoke()` never calls anything real) and a genuine end-to-end proof that a call the
// gating logic rejects never reaches the real, mutating `TaskBranchSandbox` verb AT ALL.
//
// STILL NOT THE REAL `agentengine::Tool<>`. `task_branch_capability.hpp`'s own header comment records
// a real, honestly-diagnosed NEGATIVE RESULT, found by actually trying it: driving
// `cap::decl::TaskBranch`/`cap::decl::TaskBranchCommit` through the real `Tool<>`'s own
// `declared_capabilities()` does not compile today -- an ADL/namespace gap (self-inflicted, fixable in
// prove-phase code, deliberately left as-is to mirror the real system's exact structure) and, the
// genuinely unavoidable one, the real `agentengine::Capability` variant being closed with 19
// alternatives and no `TaskBranch`/`TaskBranchCommit` member. THAT NEGATIVE RESULT IS UNCHANGED BY THIS
// FILE and is not re-attempted here. Everything below is built over the LOCAL, mirrored
// `MirroredCapability`/`MirroredCapabilitySet` system `task_branch_capability_enforcement.hpp` already
// established -- never the real `agentengine::Capabilities<>`/`Tool<>`.
//
// What IS new here, one level more end-to-end than the enforcement file alone: four tool-SHAPED types
// (`TaskBranchStartTool`/`RunTool`/`DiscardTool`/`CommitTool`), matching `task_branch_capability.hpp`'s
// own "usage sketch" comment for what a real `Tool<>` binding would eventually look like once promoted
// -- each carrying a declared ceiling built from `task_branch_capability_enforcement.hpp`'s own
// `isolated_branch_tool_ceiling()`/`commit_tool_ceiling()` -- and a `dispatch()` function mirroring the
// REAL `tool_pipeline.hpp`'s step 4/7 shape (read directly, lines ~589-599: iterate the tool's declared
// ceiling in order, `held.bind()` each, the FIRST failure rejects the WHOLE call immediately, "no leaked
// capability" -- the error names neither what's missing nor what IS held). Authorize-then-invoke is now
// checked against a REAL backing object: `ToolT::call_sandbox()` -- the only path into the real, mutating
// `TaskBranchSandbox<Surface>` verbs -- is reached ONLY on the success branch of that loop, never on
// rejection. This NARROWS the "real Tool<> wiring" gap; it does not close it -- there is still no real
// `Tool<>`, no real `ToolDescriptor`, no real JSON-arguments boundary, and no real approval/audit step
// (`tool_pipeline.hpp`'s steps 1-3 and 5+ are out of scope for this file, which mirrors ONLY step 4/7).

#include "task_branch_capability_enforcement.hpp"
#include "task_branch_sandbox.hpp"

#include "agentengine/rt/task.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace probe::dispatch {

using enforcement::MirroredCapability;
using enforcement::MirroredCapabilitySet;

// Four separate tool-shaped types, matching task_branch_capability.hpp's own usage-sketch comment
// ("four separate tools ... the commit tool alone carrying BOTH tags") -- not one multi-verb tool.
// Each carries: a `name` (mirroring ToolDescriptor::name), the real Args/Reply types
// task_branch_sandbox.hpp already declares (reused verbatim, never redefined), a `capability_ceiling()`
// mirroring ToolDescriptor::capability_ceiling (a fixed, tool-declared vector of required
// capabilities), and `call_sandbox()` -- the ONLY function in this whole file that ever calls into a
// real TaskBranchSandbox verb. `call_sandbox()` is deliberately NOT called `invoke()`: this design
// track's own real-Tool<> precedent (RunShellTool::invoke(), RunCommandTool::invoke()) reserves that
// name for an unreachable stub that fires only if something bypasses the real dispatch descriptor --
// there is no such stub/bypass distinction to make here, since `dispatch()` below IS the only entry
// point these tool types have.

struct TaskBranchStartTool {
    static constexpr std::string_view name = "task_branch_start";
    using Args = TaskBranchStartArgs;
    using Reply = TaskBranchStartReply;

    [[nodiscard]] static std::vector<MirroredCapability> capability_ceiling() {
        return enforcement::isolated_branch_tool_ceiling();
    }

    template <ExecutionSurface Surface>
    [[nodiscard]] static agentengine::rt::task<result<Reply>> call_sandbox(
        TaskBranchSandbox<Surface>& sandbox, Args args) {
        co_return co_await sandbox.start_task_branch(std::move(args));
    }
};

struct TaskBranchRunTool {
    static constexpr std::string_view name = "task_branch_run";
    using Args = TaskBranchRunArgs;
    using Reply = TaskBranchRunReply;

    [[nodiscard]] static std::vector<MirroredCapability> capability_ceiling() {
        return enforcement::isolated_branch_tool_ceiling();
    }

    template <ExecutionSurface Surface>
    [[nodiscard]] static agentengine::rt::task<result<Reply>> call_sandbox(
        TaskBranchSandbox<Surface>& sandbox, Args args) {
        co_return co_await sandbox.run_in_task_branch(std::move(args));
    }
};

struct TaskBranchDiscardTool {
    static constexpr std::string_view name = "task_branch_discard";
    using Args = TaskBranchDiscardArgs;
    using Reply = TaskBranchDiscardReply;

    [[nodiscard]] static std::vector<MirroredCapability> capability_ceiling() {
        return enforcement::isolated_branch_tool_ceiling();
    }

    template <ExecutionSurface Surface>
    [[nodiscard]] static agentengine::rt::task<result<Reply>> call_sandbox(
        TaskBranchSandbox<Surface>& sandbox, Args args) {
        co_return co_await sandbox.discard_task_branch(std::move(args));
    }
};

struct TaskBranchCommitTool {
    static constexpr std::string_view name = "task_branch_commit";
    using Args = TaskBranchCommitArgs;
    using Reply = TaskBranchCommitReply;

    // The one tool of the four whose declared ceiling names BOTH tags -- task_branch_capability.hpp's
    // own usage sketch and PRECISION comment: commit merges real work into main, a meaningfully more
    // consequential authority than start/run/discard, which stay isolated on a child branch.
    [[nodiscard]] static std::vector<MirroredCapability> capability_ceiling() {
        return enforcement::commit_tool_ceiling();
    }

    template <ExecutionSurface Surface>
    [[nodiscard]] static agentengine::rt::task<result<Reply>> call_sandbox(
        TaskBranchSandbox<Surface>& sandbox, Args args) {
        co_return co_await sandbox.commit_task_branch(std::move(args));
    }
};

// dispatch_tool_call(): mirrors the REAL tool_pipeline.hpp's step 4/7 loop (lines ~589-599, read
// directly, not assumed) at the same fidelity task_branch_capability_enforcement.hpp's own
// `try_invoke()` already established for the pure-boolean case -- iterate `ToolT::capability_ceiling()`
// in the tool's own declared order, `held.bind()` each requirement, and the FIRST failure rejects the
// WHOLE call immediately with a generic error that names neither what's missing nor what IS held (the
// real pipeline's own "no leaked capability" comment, reproduced verbatim in spirit here). The property
// `try_invoke()` alone could never establish: `ToolT::call_sandbox()` -- the only path to a REAL,
// mutating TaskBranchSandbox verb -- is textually reachable ONLY from the success path below. A
// rejected call returns before that line is ever reached; there is no other call site anywhere in this
// file that could reach it. (This is a structural argument, not merely an observed one -- but the
// probe using this function additionally verifies REAL observable state -- Ledger head digest, real
// Docker container count, TaskBranchSandbox::active_count()/has_active_handle() -- is unchanged after a
// rejection, so the claim is checked both ways: by the code's own shape and by its real effects.)
//
// Named `dispatch_tool_call`, not `dispatch`, to avoid an ordinary, boring name collision with this
// file's own enclosing `namespace probe::dispatch` -- a caller writing `using namespace probe::dispatch;`
// alongside `using namespace probe;` would otherwise find the unqualified name `dispatch` ambiguous
// between the namespace itself and this function (confirmed by an actual failed compile attempt while
// building this file's own probe).
//
// Scope, stated explicitly: this mirrors ONLY tool_pipeline.hpp's step 4/7 (authorize + bind). Steps
// 1-3 (resolve/validate/taint) and step 5+ (approve, execute-with-audit) are out of scope -- there is
// no real ToolDescriptor, no real JSON-arguments boundary, and no real approval/audit wiring here.
template <class ToolT, ExecutionSurface Surface>
[[nodiscard]] agentengine::rt::task<result<typename ToolT::Reply>> dispatch_tool_call(
    MirroredCapabilitySet const& held, TaskBranchSandbox<Surface>& sandbox, typename ToolT::Args args) {
    for (MirroredCapability const& requirement : ToolT::capability_ceiling()) {
        if (!held.bind(requirement)) {
            co_return std::unexpected(error{
                "required capability not held", "task_branch.dispatch_capability_not_held"});
        }
    }
    co_return co_await ToolT::call_sandbox(sandbox, std::move(args));
}

}  // namespace probe::dispatch
