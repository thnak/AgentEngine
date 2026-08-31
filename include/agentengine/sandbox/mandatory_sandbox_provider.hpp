#pragma once
// Implements ADR-102 Phase 4 (identity-native sandbox/worktree design, ADR-099 §1 items 1/3/4, §8's
// own "MandatorySandboxProvider is designed to compose as AgentSession's real HistoryProviderT, but
// has only been proven against FakeAgentSession" residual) -- `MandatorySandboxProvider<Surface>`/
// `RunCommandTool`: the real `ContextProvider` conformer wiring `SandboxRuntime` (Phase 3) into a
// session's BARE `HistoryProviderT` slot, mirroring `tools/cli_chat.cpp`'s own real, shipped
// `ToolDeclaringHistoryProvider` pattern -- deliberately NOT `ComposedContextProvider<Ms...>`, which
// has zero real production consumers anywhere in this codebase.
//
// Ported from docs/planning/proofs/mandatory_sandbox/mandatory_sandbox_provider.hpp (ADR-099's own
// standalone, red-teamed, live-tested prove-phase original -- kept as-is, this is a new file). Real
// changes made during the port, not cosmetic:
//   - `probe::Principal` -> `agentengine::IdentityHandle` throughout (ADR-102 Phase 1's naming
//     decision) -- every `owner`/`requested_by`/`caller` parameter and the `owner_` member.
//   - `IdentityAuthority::bootstrap().adopt(ctx.principal.id, ctx.principal.on_behalf_of)` (the
//     prove-phase original's two-string `adopt()` call) -> `IdentityAuthority::bootstrap().adopt(
//     ctx.principal)` -- Phase 1's real, ported `adopt()` takes `agentengine::Principal const&`
//     directly (a typed bridge, not two bare strings); the two-string overload never existed on the
//     ported type.
//   - `probe::error{message, code}` -> the real `agentengine::error{failure_class, message, code}`.
//     `would_fork_succeed()`'s "source not bound" and `reset_to_turn()`'s identical check pick
//     `failure_class::contract` (a caller-side precondition violation, not a policy refusal or a
//     resource exhaustion); `would_fork_succeed()`'s "BranchCost quota is currently exhausted" picks
//     `failure_class::resource`, matching `AsyncQuota::try_consume()`'s own convention for the same
//     condition. Every error-code prefix renamed from `"mandatory_sandbox.*"` to
//     `"mandatory_sandbox_provider.*"`, matching this file's own module name (Phase 2's `ledger.hpp`
//     precedent for this exact kind of rename, disclosed here rather than silently done).
//   - A REAL FIDELITY FIX, not merely a rename: the prove-phase original's `on_context()` tool closure
//     re-wrapped a FAILED `SandboxRuntime::run()` outcome as `agentengine::error{failure_class::policy,
//     outcome.error().message, outcome.error().code}` -- discarding whatever real `failure_class`
//     `SandboxRuntime::run()`/`Ledger`/`AsyncQuota` actually returned (which could be `resource` for
//     quota exhaustion, `contract` for an unauthorized digest, `fatal` for a Docker CLI failure) and
//     always reporting it as `policy`. This was never a deliberate choice in the original -- the
//     prove-phase `probe::error` had only 2 fields (`message`, `code`), so there was nothing to
//     preserve; `policy` was a required-but-arbitrary placeholder for a field that didn't exist yet.
//     Now that `agentengine::error` genuinely carries a `failure_class`, this port passes `outcome.
//     error()` straight through unchanged instead of reconstructing it -- a caller (e.g. a future
//     policy/retry layer keyed on `failure_class`) sees the REAL classification of what actually
//     failed, not a fixed placeholder.
//
// SCOPE, UPDATED by ADR-119 (2026-08-30): `RunCommandTool` originally (ADR-102 Phase 4) declared NO
// static `Capabilities<...>` ceiling, authorized entirely against THIS design's own `Grant<T>`/
// `IdentityAuthority`/`AsyncQuota<T>` model. That model is UNCHANGED and remains the real, dynamic
// gate bounding how much/how far a call can go: `MandatorySandboxProvider::is_bound()` (checked
// inside `on_context()` before the tool is even contributed to the table) plus `SandboxRuntime::
// run()`'s own `AsyncQuota<RunCost>`/`AsyncQuota<StorageBytes>` gates (Phase 3). ADR-119 layers a
// SECOND, static `Capabilities<cap::decl::RunCommand>` ceiling on top -- a pure membership/audit gate
// ("is this agent even allowed to have run_command tooling at all"), mirroring the exact double-gate
// shape ADR-117 already established for the task-branch tools immediately below. This is a real,
// disclosed BEHAVIOR CHANGE: every real production caller (`tools/cli_chat.cpp`, `tools/
// sandboxed_shell_chat.cpp`, `tools/containerd_shell_chat.cpp`) must now also grant `cap::RunCommand`
// on the session, or the real `invoke_tool()` pipeline rejects every `run_command` call with
// `tool.capability_not_held` even though the identity/quota gate would have allowed it -- see ADR-119
// for the full list of real callers updated in the same change.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/ledger.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/async_quota.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/sandbox/execution_surface.hpp"
#include "agentengine/sandbox/sandbox_runtime.hpp"
#include "agentengine/trust/identity_authority.hpp"

namespace agentengine {

struct RunCommandArgs {
    std::string command;
};
AE_JSON_SCHEMA(RunCommandArgs, command)

struct RunCommandReply {
    bool ok = false;
    int exit_code = -1;
    std::string stdout_text;
    std::string tree_digest;
    std::uint64_t turn_index = 0;
};
AE_JSON_SCHEMA(RunCommandReply, ok, exit_code, stdout_text, tree_digest, turn_index)

// ADR-119: now declares a real `Capabilities<cap::decl::RunCommand>` ceiling -- see this file's own
// top comment for the double-gate shape this creates (the identity/quota model is unchanged and
// still does the real bounding; this ceiling is a separate, static membership/audit gate on top).
// `invoke()`'s body is an unreachable sentinel -- real dispatch is entirely through the
// `make_tool_descriptor_with_invoke()` closure `MandatorySandboxProvider::on_context()` installs below.
struct RunCommandTool
    : agentengine::Tool<RunCommandTool, agentengine::Capabilities<agentengine::cap::decl::RunCommand>> {
    static constexpr std::string_view name = "run_command";
    static constexpr std::string_view description =
        "Run a shell command in this session's own isolated sandbox, backed by a real, independent "
        "Ledger checkpoint branch.";
    using Args = RunCommandArgs;
    using Reply = RunCommandReply;

    [[nodiscard]] static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "RunCommandTool::invoke() must never run directly -- real dispatch goes through the "
            "session-capturing closure make_tool_descriptor_with_invoke() installs.",
            "mandatory_sandbox_provider.invoke_unreachable"});
    }
};

// A10 promotion (identity-native sandbox/worktree design, ADR-099 §7's own real-world-use-case
// finding: every actively-developed coding agent surveyed ships branch/worktree isolation as a
// PRIMARY tool-facing primitive -- "isolate one attempt, evaluate it, merge or discard"). Gives
// `SandboxRuntime::merge_into()` (Phase 3, ADR-111's `MergeCost`-gated form) its first real
// production caller. Ported from `docs/planning/proofs/task_branch_tool/task_branch_sandbox.hpp`
// (ADR-099's own standalone, three-independent-red-team-round, prove-phase original) -- but NOT as a
// separate `TaskBranchSandbox<Surface>` sibling object: this class already owns exactly what that
// design's own header comment required its "main" parameter to be (`runtime_`, a `SandboxRuntime`
// this class exclusively owns) plus every quota its constructor threads through
// (`branch_quota_`/`run_quota_`/`storage_quota_`), so a second object holding its own copies of the
// same pointers would be duplication, not a real seam.
//
// Args/Reply schemas mirror the prove-phase original's own shapes exactly (`TaskBranchStartArgs`'s
// `label` field is agent-supplied free text for logging ONLY -- I3: never used to select which
// branch/authority is operated on; every operation's real authority comes from this class's own
// already-bound state and the calling turn's own `ctx.principal`, never from a tool argument).
struct TaskBranchStartArgs {
    std::string label;
};
AE_JSON_SCHEMA(TaskBranchStartArgs, label)

struct TaskBranchStartReply {
    std::string handle_id;
};
AE_JSON_SCHEMA(TaskBranchStartReply, handle_id)

struct TaskBranchRunArgs {
    std::string handle_id;
    std::string command;
};
AE_JSON_SCHEMA(TaskBranchRunArgs, handle_id, command)

struct TaskBranchRunReply {
    int         exit_code = -1;
    std::string stdout_text;
};
AE_JSON_SCHEMA(TaskBranchRunReply, exit_code, stdout_text)

struct TaskBranchCommitArgs {
    std::string handle_id;
};
AE_JSON_SCHEMA(TaskBranchCommitArgs, handle_id)

struct TaskBranchCommitReply {
    bool          ok = false;
    std::uint64_t turn_index = 0;
};
AE_JSON_SCHEMA(TaskBranchCommitReply, ok, turn_index)

struct TaskBranchDiscardArgs {
    std::string handle_id;
};
AE_JSON_SCHEMA(TaskBranchDiscardArgs, handle_id)

struct TaskBranchDiscardReply {
    bool ok = false;
};
AE_JSON_SCHEMA(TaskBranchDiscardReply, ok)

// CAPABILITY-GATING DECISION for all four tools below, UPDATED by ADR-117 (2026-08-30, same day as
// this file's own ADR-114 promotion): the real, closed `agentengine::Capability` variant now carries
// `cap::TaskBranch`/`cap::TaskBranchCommit` (promoted verbatim from the prove-phase original's
// `task_branch_capability.hpp` design, confirmed with the project owner back when THAT file was
// written), so these four tools now declare a REAL static `Capabilities<...>` ceiling -- no longer
// mirroring `RunCommandTool`'s zero-ceiling precedent. `StartTaskBranchTool`/`RunInTaskBranchTool`/
// `DiscardTaskBranchTool` each declare `Capabilities<cap::decl::TaskBranch>`; `CommitTaskBranchTool`
// alone declares BOTH `cap::decl::TaskBranch` and `cap::decl::TaskBranchCommit` (a commit can only
// ever operate on a handle `start_task_branch` produced, so it needs both tags -- enforced by the
// TOOL's own declared ceiling, per `capability.hpp`'s own comment on the pair, not by any
// relationship between the two capability kinds themselves).
//
// THIS IS NOW A DOUBLE GATE, not a replacement of the old one -- both must hold for a call to reach
// `call_sandbox()`'s real verb through the REAL `session.start_run() -> invoke_tool()` pipeline:
//   1. The EXISTING dynamic gate, unchanged: `MandatorySandboxProvider::is_bound()` AND
//      `bind_task_branch_tools()` having been called (checked together, `merge_quota_ != nullptr`,
//      before any of the four tools is even contributed to `on_context()`'s table) -- see that
//      method's own comment for why this is a second, deliberately separate opt-in from
//      `bind_sandbox()` itself.
//   2. The NEW static gate: `invoke_tool()`'s own step 4/7 (`core/tool_pipeline.hpp`) binds each
//      declared capability against the calling session's OWN granted `CapabilitySet`
//      (`AgentSession::capabilities()`, set via `set_capabilities()` -- `agent_session.hpp` defaults
//      this to a genuinely EMPTY `CapabilitySet::grant_root({})` when never called, never to
//      "unrestricted"). A host that calls `bind_task_branch_tools()` but never separately grants
//      `cap::TaskBranch`/`cap::TaskBranchCommit` on the session gets tools that are CONTRIBUTED (they
//      appear in the tool table, the model can see and attempt to call them) but every real call
//      through `invoke_tool()` is rejected at step 4 with the pipeline's own generic "no leaked
//      capability" error (names neither what's missing nor what IS held) -- fail-closed, matching
//      007 §6's empty-by-default rule, the identical contract the real, shipped `RunShellTool`
//      (`src/backends/native_jail/session_shell_wiring.hpp`, ADR-096) already lives under for
//      `cap::decl::FsRead<"work">`/`cap::decl::FsWrite<"work">`.
//
// REAL, DISCLOSED BEHAVIOR CHANGE from this file's own ADR-114 original: before this widening, a
// session that called `bind_sandbox()` + `bind_task_branch_tools()` could reach all four verbs
// through the real pipeline with NO capability grant at all (gate 1 alone was sufficient, since gate
// 2 didn't exist -- `declared_capabilities()` returned an empty vector, so `invoke_tool()`'s step 4/7
// loop had nothing to bind). Landing gate 2 on the SAME day as ADR-114 itself, before any real host
// outside `tests/test_task_branch_tools.cpp` has adopted the old contract, is deliberately the lowest
// -risk moment to make this change -- see ADR-117 for why this was judged worth doing now rather than
// deferred again. `tests/test_task_branch_tools.cpp`'s own real-pipeline section [6] is updated to
// grant both tags; every other section calls the plain provider methods directly
// (`start_task_branch()` etc.), bypassing `Tool<>`/`invoke_tool()` entirely, so gate 2 has no effect
// on those and needed no change.
struct StartTaskBranchTool
    : agentengine::Tool<StartTaskBranchTool, agentengine::Capabilities<agentengine::cap::decl::TaskBranch>> {
    static constexpr std::string_view name = "start_task_branch";
    static constexpr std::string_view description =
        "Start a new, isolated task branch forked from this session's current sandbox state. Run "
        "commands on it freely via run_in_task_branch, then commit_task_branch to fold the result "
        "into the main branch, or discard_task_branch to throw it away with no lasting effect.";
    using Args = TaskBranchStartArgs;
    using Reply = TaskBranchStartReply;
    [[nodiscard]] static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "StartTaskBranchTool::invoke() must never run directly -- real dispatch goes through the "
            "session-capturing closure MandatorySandboxProvider::on_context() installs.",
            "mandatory_sandbox_provider.invoke_unreachable"});
    }
};

struct RunInTaskBranchTool
    : agentengine::Tool<RunInTaskBranchTool, agentengine::Capabilities<agentengine::cap::decl::TaskBranch>> {
    static constexpr std::string_view name = "run_in_task_branch";
    static constexpr std::string_view description =
        "Run a shell command inside a task branch previously created by start_task_branch. Never "
        "touches the session's main branch.";
    using Args = TaskBranchRunArgs;
    using Reply = TaskBranchRunReply;
    [[nodiscard]] static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "RunInTaskBranchTool::invoke() must never run directly -- real dispatch goes through the "
            "session-capturing closure MandatorySandboxProvider::on_context() installs.",
            "mandatory_sandbox_provider.invoke_unreachable"});
    }
};

struct CommitTaskBranchTool
    : agentengine::Tool<CommitTaskBranchTool,
                         agentengine::Capabilities<agentengine::cap::decl::TaskBranch,
                                                    agentengine::cap::decl::TaskBranchCommit>> {
    static constexpr std::string_view name = "commit_task_branch";
    static constexpr std::string_view description =
        "Fold a task branch's accumulated work into the session's main branch as a new checkpoint. "
        "On a real conflict, the rejection is reported and the same handle stays usable for a retry "
        "or a discard -- the work is never silently lost.";
    using Args = TaskBranchCommitArgs;
    using Reply = TaskBranchCommitReply;
    [[nodiscard]] static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "CommitTaskBranchTool::invoke() must never run directly -- real dispatch goes through the "
            "session-capturing closure MandatorySandboxProvider::on_context() installs.",
            "mandatory_sandbox_provider.invoke_unreachable"});
    }
};

struct DiscardTaskBranchTool
    : agentengine::Tool<DiscardTaskBranchTool, agentengine::Capabilities<agentengine::cap::decl::TaskBranch>> {
    static constexpr std::string_view name = "discard_task_branch";
    static constexpr std::string_view description =
        "Throw away a task branch and everything it did, with no lasting effect on the main branch.";
    using Args = TaskBranchDiscardArgs;
    using Reply = TaskBranchDiscardReply;
    [[nodiscard]] static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "DiscardTaskBranchTool::invoke() must never run directly -- real dispatch goes through "
            "the session-capturing closure MandatorySandboxProvider::on_context() installs.",
            "mandatory_sandbox_provider.invoke_unreachable"});
    }
};

// REAL FINDING an independent red-team pass caught (2026-08-28), EMPIRICALLY PROVED against the real,
// unmodified `AsyncMutex`/`task<T>` (a targeted repro, 5/5 runs reproduced the corruption), not merely
// reasoned about: this file's own FIRST version used a plain, local, naive "while (!t.done()) t.resume();"
// drive loop here -- matching `rt/agent_workflow_executor.hpp`'s own real, production
// `agent_executor_detail::drive()` precedent -- reasoned as safe under the claim "I1 (one session, one
// executor) plus invoke_tool()'s sequential dispatch mean nothing here ever drives two concurrent
// invocations into the SAME MandatorySandboxProvider/SandboxRuntime instance from two different
// threads." That claim is TRUE but insufficient: `bind_sandbox()` stores `AsyncQuota<RunCost>`/
// `AsyncQuota<BranchCost>`/`AsyncQuota<StorageBytes>` as raw pointers, and those quotas are
// LEGITIMATELY SHARED across multiple independent `SandboxRuntime` instances -- an ordinary "one
// budget for a whole family of sibling sessions" pattern this phase's own test itself demonstrates
// (one quota triple bound across six separate sessions). If two sibling sessions' round loops ever run
// on genuinely different OS threads concurrently (the entire reason `AgentSession::session_mutex_`
// exists per-session at all), their two `RunCommandTool` closures' calls into the SAME shared
// `AsyncQuota` genuinely CONTEND on that quota's own internal `AsyncMutex` -- no two
// `MandatorySandboxProvider`/`SandboxRuntime` instances need to be touched concurrently for this to
// fire, only the shared quota. Under that real contention, the naive loop's SECOND `resume()` call on
// an awaiter that has already genuinely suspended does not wait -- it directly runs `await_resume()`,
// handing back a `Guard` as if the lock were acquired when it is not (defeating `AsyncMutex`'s mutual
// exclusion outright), and leaves a STALE handle in the mutex's own waiter queue that a later, real
// `unlock()` resumes against an ALREADY-DESTROYED coroutine frame -- a genuine, reproducible
// use-after-free, the same hazard class the prove-phase original's own `block_on()`
// (docs/planning/proofs/common/block_on.hpp) was built to prevent and this port's first version wrongly
// judged unreachable. FIXED: this file now uses the real, ported `agentengine::rt::block_on<T>()`
// (`rt/block_on.hpp`) everywhere it previously used the naive loop -- see that file's own top comment
// for the full mechanism and the same finding, recorded there too so it is not lost if this file is
// ever read in isolation.
//
// A SECOND, RELATED, disclosed-not-fixed gap the same pass found: `AgentSession::fork_from()`
// (agent_session.hpp) is deliberately NOT guarded by `session_mutex_` (unlike `start_run()`/
// `resolve_interaction()`), so nothing prevents a future caller invoking it concurrently with an
// in-flight `run()` on the SAME source session from a different thread -- the copy-assignment below
// would then contend for `SandboxRuntime::exclusivity_` itself, the identical hazard class `block_on()`
// now protects against for the QUOTA case, but this specific vector was not exercised by this pass's
// own repro. Not live today (`fork_from()` has no real production caller anywhere in this codebase --
// confirmed by search; `agent.spawn`, the only real spawn path, does not use it), but real follow-on
// work before any future caller wires `fork_from()` up to run concurrently with an in-flight tool call.

// ADR-132 -- gained a second template parameter, `Store` (defaulted to
// `agentengine::InMemoryWorktreeObjectStore`, matching `Ledger<Store>`/`SandboxRuntime<Store>`'s own
// default), closing the "hardcoded to `Ledger<>`, not `Store`-generic" gap `ADR-130` §2 found while
// building a real, durable `WorktreeObjectStore` conformer (`FileWorktreeObjectStore`) and explicitly
// left as separate, larger follow-on work rather than folded into that ADR. Every `Ledger<>`/
// `BranchHandle<>`/bare `SandboxRuntime` reference below became `Ledger<Store>`/`BranchHandle<Store>`/
// `SandboxRuntime<Store>` -- purely a widened parameter/member type, no method BODY changed. Every
// existing, already-verified caller (every real production tool, every test in this whole session's own
// task-branch/crash-recovery line) that only ever named `MandatorySandboxProvider<Surface>` with ONE
// template argument continues to compile and behave IDENTICALLY -- `Store` defaults to the same
// `InMemoryWorktreeObjectStore` it was hardcoded to before, and ordinary template default-argument rules
// mean a 1-argument use of a 2-parameter template stays valid with no call-site change anywhere.
template <ExecutionSurface Surface, class Store = agentengine::InMemoryWorktreeObjectStore>
class MandatorySandboxProvider {
public:
    static constexpr std::string_view name = "mandatory_sandbox_provider";

    // The "no sandbox bound yet" state -- required to exist for `AgentSession::clear_in_process_state()`'s
    // real, unmodifiable `history_provider_ = HistoryProviderT{};` statement (agent_session.hpp) to
    // compile at all. Safe by construction: every accessor/`on_context()` checks `runtime_.has_value()`
    // first.
    MandatorySandboxProvider() = default;

    // The REAL binding call -- mirrors `AgentSession::initialize()`'s own established "config-time
    // setter, called once before first use" convention exactly. A host that never calls this gets a
    // session with no execution capability, never a crash and never a session that silently aliases
    // another session's sandbox. HONEST, DISCLOSED GAP inherited unchanged from the prove-phase
    // original: this design's own stronger "a session with no execution capability still owns a
    // branch" requirement (ADR-099 §1 item 2) is NOT established here -- a provider that has never
    // once been bound (in THIS or any prior process) still owns no branch at all, and that remains
    // ~~Not attempted in this phase; a `bind_branch_only()` variant is real follow-on work.~~ **out of
    // this file's own scope** -- it is a session-lifecycle question (should an unbound session even
    // exist), not a durability one, and ADR-128 does not attempt it. What ADR-128 DOES close is the
    // narrower, durability-shaped half of the SAME disclosed gap: `bind_root_branch()` below resolves
    // (reclaim-if-orphaned, create-if-not) `owner`'s own deterministic root branch by identity alone,
    // for the crash-recovery case where a branch already exists (in this same Ledger's durable store)
    // and the caller just needs to reattach to it without hand-computing `Ledger`'s own root-name
    // format or hand-sequencing `reclaim_orphaned_branch()`/`create_root_branch()` itself.
    void bind_sandbox(agentengine::Ledger<Store>& ledger, agentengine::BranchHandle<Store> branch,
                        agentengine::IdentityHandle owner, std::filesystem::path staging_root,
                        agentengine::rt::AsyncQuota<agentengine::BranchCost>& branch_quota,
                        agentengine::rt::AsyncQuota<agentengine::RunCost>& run_quota,
                        agentengine::rt::AsyncQuota<agentengine::StorageBytes>& storage_quota) {
        // MUST run BEFORE any member below is reassigned -- uses THIS object's OWN, OLD
        // branch_quota_/task_branches_ (whatever it was previously bound to, if anything), refunding
        // into the quota that actually granted them. Calling it after `branch_quota_ = &branch_quota`
        // below would refund into the WRONG (new) quota object instead. See the method's own comment
        // for why this matters at all.
        discard_all_active_task_branches_and_refund();
        ledger_ = &ledger;
        owner_.emplace(owner);
        branch_quota_ = &branch_quota;
        run_quota_ = &run_quota;
        storage_quota_ = &storage_quota;
        runtime_.emplace(ledger, std::move(branch), std::move(staging_root));
        surface_.emplace();
        // Defensive reset, not load-bearing for a first-ever bind: a re-bind of an already-used
        // provider must not silently carry forward a PRIOR binding's task-branch state (a stale
        // merge_quota_ pointer describing the OLD runtime_) into the fresh one.
        merge_quota_ = nullptr;
        task_branch_mutex_ = std::make_unique<agentengine::rt::AsyncMutex>();
        // ADR-126 -- closes the "`active_`'s table has no durability of its own" residual (ADR-114
        // §5/§6, inherited unchanged from the prove-phase original's own finding 6): if `ledger`'s own
        // durable Store already remembers child branches of `branch` that were live-but-unresolved at
        // the moment of a prior crash (`Ledger::orphaned_branches()`, populated by `load_durable_
        // state()` at THIS Ledger's own construction), rehydrate them back into `task_branches_` here
        // -- so a resumed session whose own durable history still shows an unresolved `start_task_
        // branch` reply can `commit_task_branch()`/`discard_task_branch()`/`run_in_task_branch()` that
        // SAME `handle_id` again through this tool surface's own normal verbs, instead of permanently
        // needing the lower-level `Ledger::reclaim_orphaned_branch()` API. See that method's own
        // comment for the exact matching rule and its own honestly-disclosed scope.
        recover_orphaned_task_branches();
    }

    // ADR-128 -- closes the "root-branch recovery remains the caller's own, separate, already-
    // disclosed responsibility" residual ADR-126 §5 named (itself tracing back to ADR-102's own
    // `bind_branch_only()` note, see `bind_sandbox()`'s own comment above for the precise scope this
    // does and does not close). Computes `owner`'s own deterministic root-branch name EXACTLY the way
    // `Ledger::create_root_branch()` itself does (`"root-" + owner.id() [+ "-" + disambiguator]`) --
    // duplicated here rather than exposed as a separate `Ledger` accessor, the same "caller recomputes
    // a documented deterministic name" shape `recover_orphaned_task_branches()`'s own child-prefix
    // match already established for this file. Two, and only two, real outcomes:
    //   - the name is currently in `ledger.orphaned_branches()` (this owner's root branch was left
    //     live-but-unresolved by a prior process's crash or clean exit) -- reclaimed via `Ledger::
    //     reclaim_orphaned_branch()`, which independently re-checks `owner`'s own authorization for the
    //     branch's current head tree and fails closed (a real `ledger.reclaim_unauthorized`/`ledger.
    //     not_an_orphan` error, not a crash or a silent no-op) if that check fails;
    //   - it is not -- treated as a genuine first-ever bind for this owner/disambiguator pair and a
    //     fresh root branch is created via `Ledger::create_root_branch()`, identical to what a host
    //     calling that API directly and then `bind_sandbox()` would have done by hand.
    // HONEST, NOT-WIDENED RESIDUAL: this does not, and cannot, distinguish "no branch has ever existed
    // for this owner" from "a branch already exists and is currently LIVE (bound in this same process,
    // or in some other process that has not exited)" -- `Ledger::orphaned_branches()` only ever
    // contains what `load_durable_state()` restored at THIS Ledger's own construction, so a live branch
    // is invisible to it by construction. Calling `bind_root_branch()` a second time for the same
    // owner/disambiguator while a branch from the first call is still live elsewhere reaches the
    // "create a fresh one" path and silently produces a duplicate-named branch that overwrites the
    // live one's `branches_` entry -- the EXACT SAME pre-existing hazard `Ledger::create_root_branch()`
    // itself already documents and leaves as the caller's own responsibility (ADR-102 §43.2's own
    // disambiguator fix narrows, not eliminates, this). `bind_root_branch()` makes calling that API
    // correctly less error-prone for the crash-recovery case; it does not add a new safety property
    // `create_root_branch()` did not already have, and must not be read as one.
    [[nodiscard]] agentengine::result<void> bind_root_branch(
            agentengine::Ledger<Store>& ledger, agentengine::IdentityHandle owner,
            std::filesystem::path staging_root,
            agentengine::rt::AsyncQuota<agentengine::BranchCost>& branch_quota,
            agentengine::rt::AsyncQuota<agentengine::RunCost>& run_quota,
            agentengine::rt::AsyncQuota<agentengine::StorageBytes>& storage_quota,
            std::string disambiguator = {}) {
        std::string root_name = "root-" + std::to_string(owner.id());
        if (!disambiguator.empty()) root_name += "-" + disambiguator;
        bool is_orphan = false;
        for (std::string const& orphan_name : ledger.orphaned_branches()) {
            if (orphan_name == root_name) { is_orphan = true; break; }
        }
        agentengine::result<agentengine::BranchHandle<Store>> resolved =
            is_orphan ? ledger.reclaim_orphaned_branch(root_name, owner)
                      : agentengine::rt::block_on(ledger.create_root_branch(owner, disambiguator));
        if (!resolved.has_value()) return std::unexpected(resolved.error());
        bind_sandbox(ledger, std::move(*resolved), owner, std::move(staging_root), branch_quota,
                     run_quota, storage_quota);
        return agentengine::result<void>{};
    }

    // SHOULD-FIX (independent red-team, 2026-08-30): silently `task_branches_.clear()`-ing at any of
    // this class's three reassignment points (`bind_sandbox()`'s own re-bind, and both paths of
    // `operator=`) would drop every active `SandboxRuntime` without discarding it -- the underlying
    // branch survives as an eventual `reap_pending_abandons()` orphan at the LOWER Ledger level, but
    // the `BranchCost` unit `start_task_branch()` spent for it would be gone with no refund and no
    // error, a silent, permanent quota leak. Not reachable through any real production caller today
    // (every real tool binds exactly once at startup; `fork_from()`, the only caller of the
    // copy-assignment operator, has zero real production callers anywhere in this tree -- confirmed by
    // grep) but a real gap this promotion introduced and must not silently regress once one does.
    // Synchronous (not a coroutine) via the same `agentengine::rt::block_on()` mechanism `operator=`'s
    // own `spawn_child_branch()` call already relies on (this file's own top comment) -- needed because
    // `bind_sandbox()`/`operator=` are deliberately plain functions, not coroutines, and changing that
    // would ripple into every real caller's own signature for a path nothing currently reaches.
    void discard_all_active_task_branches_and_refund() {
        for (auto& [handle_id, runtime] : task_branches_) {
            (void)handle_id;
            (void)agentengine::rt::block_on(std::move(runtime).discard());
            if (branch_quota_ != nullptr) (void)agentengine::rt::block_on(branch_quota_->refund(1));
        }
        task_branches_.clear();
    }

    // ADR-126 -- the durability-recovery half of `bind_sandbox()`'s own new call. Branch names are
    // deterministic (`Ledger::branch_from()`'s own comment: "<parent>/child-<id>-<seq>"), so a child of
    // `runtime_`'s own branch is identified by NAME PREFIX alone, with no separate parent-lineage index
    // needed. Matches ONLY direct children of THIS root branch.
    //
    // MUST-FIX (independent red-team, 2026-08-30): the ORIGINAL version of this method matched on
    // `child_prefix` alone (`orphan_name.compare(0, child_prefix.size(), child_prefix) != 0`) and its
    // own comment claimed a grandchild "would carry a DIFFERENT prefix and is correctly left alone" --
    // that claim was FALSE as stated. `Ledger::branch_from()` names a child as `parent.name() +
    // "/child-" + id + "-" + seq` UNCONDITIONALLY, including when `parent` is itself already a child --
    // so a grandchild's name is literally `"<root>/child-A-B/child-C-D"`, which DOES start with
    // `"<root>/child-"` (a plain prefix match cannot distinguish "direct child" from "any descendant,
    // however deep"). The ORIGINAL comment's safety argument rested entirely on the UNENFORCED, then-
    // undocumented-as-load-bearing observation that nothing in THIS tool surface's own call graph ever
    // invokes `branch_from()`/`spawn_child_branch()` with a task branch (rather than `runtime_`'s own
    // bound branch) as parent -- true today (`start_task_branch()` always calls `runtime_->
    // spawn_child_branch()`, never a `task_branches_` entry's own), but that invariant lives in a
    // DIFFERENT method entirely, is not enforced by this one, and `Ledger::branch_from()` itself places
    // no restriction on chaining -- a future feature, or any other lower-level caller of the same
    // `Ledger`, could produce a genuine grandchild-shaped orphan with NO code change needed here for it
    // to be silently misfiled into `task_branches_` as if it were this root's own direct child (a real
    // I4 attribution/scope defect: a handle recovered under this root's own tool surface would then
    // actually name a branch this root never directly created). The fix: after the prefix match,
    // require NO further `/` in the remainder -- a direct child's own suffix is exactly `<id>-<seq>`,
    // which never contains `/`; a descendant deeper than one hop always does. Empirically proven with a
    // new regression case in `tests/test_task_branch_durability_recovery.cpp` ([5]/[6]): a
    // Ledger-API-constructed grandchild orphan is correctly left an orphan (never enters
    // `task_branches_`, remains reachable only via the lower-level `Ledger::reclaim_orphaned_branch()`
    // API, exactly like any other out-of-scope orphan), while a genuine direct child is still recovered
    // normally -- reverting this guard makes that new case fail (the grandchild gets wrongly recovered).
    //
    // `reclaim_orphaned_child()` re-checks `owner_`'s own authorization for each candidate independently
    // (the same ACL gate every other read in this design goes through) -- failing to reclaim one
    // candidate (a genuine auth mismatch, or a race with some other, unrelated reclaim of the SAME
    // orphan) is not fatal to the others; each is attempted independently and silently skipped on its
    // own failure, matching this method's own best-effort, convenience-only framing -- a host that needs
    // the authoritative list already has it via `Ledger::orphaned_branches()`/`reclaim_orphaned_branch()`
    // directly. Deliberately BEST-EFFORT, not guaranteed: `BranchCost` itself is NOT durable (a fresh
    // `AsyncQuota<BranchCost>` after a restart has no memory of what was spent before the crash) --
    // recovering these entries here does not, and should not, re-charge it a second time; a real, but
    // unchanged and separately-disclosed, part of this whole design's existing "quotas are in-process
    // state, not durable" posture.
    void recover_orphaned_task_branches() {
        std::string const child_prefix = runtime_->branch_name() + "/child-";
        for (std::string const& orphan_name : ledger_->orphaned_branches()) {
            if (orphan_name.compare(0, child_prefix.size(), child_prefix) != 0) continue;
            // Direct child only: the suffix after `child_prefix` must contain no further `/` -- a
            // deeper descendant (a "grandchild" or beyond) carries at least one more "/child-" segment
            // and is correctly left as an orphan, reachable only via the lower-level Ledger API.
            if (orphan_name.find('/', child_prefix.size()) != std::string::npos) continue;
            auto reclaimed = agentengine::rt::block_on(runtime_->reclaim_orphaned_child(
                orphan_name, *owner_, runtime_->staging_root().parent_path()));
            if (!reclaimed.has_value()) continue;
            task_branches_.insert_or_assign(orphan_name, std::move(*reclaimed));
        }
    }

    // Second, deliberately SEPARATE opt-in from `bind_sandbox()` itself -- a host that calls
    // `bind_sandbox()` alone gets `run_command` only, exactly as before this promotion existed; the
    // task-branch/commit tool surface (a strictly more consequential capability than `run_command`,
    // per this file's own capability-gating comment above `StartTaskBranchTool`) is only contributed
    // once a host explicitly calls this too, mirroring ADR-070's Delegated Decision Seam discipline
    // (host opt-in, fails closed/absent when unset). Must be called AFTER `bind_sandbox()` -- calling
    // it first is harmless (the pointer is simply stored) but the tools stay uncontributed until
    // `is_bound()` is also true, since `on_context()` checks both.
    void bind_task_branch_tools(agentengine::rt::AsyncQuota<agentengine::MergeCost>& merge_quota) {
        merge_quota_ = &merge_quota;
    }

    [[nodiscard]] bool is_bound() const noexcept { return runtime_.has_value(); }

    // OPTIONAL, side-effect-free advisory pre-check: does this session currently look like it could
    // fork right now? Never authoritative -- state can change between this call and the actual
    // `fork_from()` -- but lets a caller (e.g. a future `agent.spawn` tool body) reject an obviously-
    // doomed spawn attempt with a real, specific error BEFORE committing to it.
    [[nodiscard]] agentengine::result<void> would_fork_succeed() const {
        if (!runtime_.has_value()) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "cannot fork a session whose own sandbox was never bound",
                "mandatory_sandbox_provider.source_not_bound"});
        }
        if (branch_quota_->remaining() < 1) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::resource, "BranchCost quota is currently exhausted",
                "mandatory_sandbox_provider.branch_quota_exhausted"});
        }
        return agentengine::result<void>{};
    }

    // `fork_from()`'s own copy constructor/assignment -- every copy-assignment is a fully SELF-
    // CONTAINED operation: it performs its OWN real `Ledger::branch_from()` call (via `SandboxRuntime::
    // spawn_child_branch()`, Phase 3), synchronously driven through the real `agentengine::rt::
    // block_on()` (`rt/block_on.hpp`, safe under genuine cross-thread contention -- see this class's
    // own top comment for why the naive alternative is not),
    // and EITHER succeeds (a genuinely fresh, independent child) or fails closed to the exact same safe
    // "no sandbox" state default-construction produces -- never aliases, never leaves inconsistent
    // state, never depends on shared mutable state between calls. This means: (1) ANY number of
    // `fork_from()` calls against the same parent, sequential or interleaved, each independently
    // succeed or fail on their own merits; (2) an incidental copy through `AgentSession::
    // history_provider()`'s mutable reference is completely SAFE (merely wasteful -- it mints a real,
    // unused child branch and pays its BranchCost); (3) there is no cross-call shared state to race on.
    // Self-copy (`this == &other`) is a genuine no-op, not "fork a child off of yourself" -- without
    // this guard, a FAILED self-fork attempt (e.g. BranchCost exhaustion) would fail-closed-reset
    // `other` too, destroying an already-bound, already-working session's real sandbox.
    MandatorySandboxProvider(MandatorySandboxProvider const& other) { *this = other; }
    MandatorySandboxProvider& operator=(MandatorySandboxProvider const& other) {
        if (this == &other) return *this;
        // MUST run BEFORE anything below is reassigned -- uses THIS object's OWN, OLD state (whatever
        // *this* was previously bound to, independent of `other`), refunding into the quota that
        // actually granted them. See that method's own comment.
        discard_all_active_task_branches_and_refund();
        if (!other.runtime_.has_value()) {
            ledger_ = nullptr;
            owner_.reset();
            branch_quota_ = nullptr;
            run_quota_ = nullptr;
            storage_quota_ = nullptr;
            runtime_.reset();
            surface_.reset();
            merge_quota_ = nullptr;
            task_branch_mutex_.reset();
            return *this;
        }
        auto child = agentengine::rt::block_on(other.runtime_->spawn_child_branch(
            *other.owner_, *other.branch_quota_, other.runtime_->staging_root().parent_path()));
        if (!child.has_value()) {
            // Fails closed to the EXACT same state as "source was never bound" above -- a genuine
            // resource-exhaustion failure and a caller-side "nothing to fork from" precondition
            // violation are indistinguishable from the resulting object's own point of view, by design.
            ledger_ = nullptr;
            owner_.reset();
            branch_quota_ = nullptr;
            run_quota_ = nullptr;
            storage_quota_ = nullptr;
            runtime_.reset();
            surface_.reset();
            merge_quota_ = nullptr;
            task_branch_mutex_.reset();
            return *this;
        }
        ledger_ = other.ledger_;
        owner_ = other.owner_;
        branch_quota_ = other.branch_quota_;
        run_quota_ = other.run_quota_;
        storage_quota_ = other.storage_quota_;
        runtime_.emplace(std::move(*child));
        surface_.emplace();
        // `merge_quota_` is a SHARED resource reference, exactly like `branch_quota_`/`run_quota_`/
        // `storage_quota_` above -- carried forward so a forked child retains the same task-branch
        // capability its parent had. `task_branches_`/`task_branch_mutex_` are NOT carried forward:
        // the child starts with zero active task branches of its own (I2 -- a fork shares AUTHORITY,
        // it never inherits another instance's ACTIVE, in-flight state) and its own fresh mutex,
        // matching `SandboxRuntime`'s own "possession, not reference" discipline for `BranchHandle`.
        // (`task_branches_` is already empty here -- the discard-and-refund call above cleared THIS
        // object's own prior entries before any of the reassignment above happened.)
        merge_quota_ = other.merge_quota_;
        task_branch_mutex_ = std::make_unique<agentengine::rt::AsyncMutex>();
        return *this;
    }
    MandatorySandboxProvider(MandatorySandboxProvider&&) = default;
    MandatorySandboxProvider& operator=(MandatorySandboxProvider&&) = default;

    // NOTE, disclosed rather than silently assumed: `spawn_child_branch()` takes `SandboxRuntime`'s own
    // `exclusivity_` lock (the same one `run()` takes for its whole call, Phase 3). Calling this
    // copy-assignment REENTRANTLY, on the same OS thread, from code already running inside an in-flight
    // `run()` call on the SAME session would self-deadlock `block_on()`'s own final busy-wait spin
    // (`BlockOnState::take()`, `rt/block_on.hpp`) on that same, already-held, non-reentrant lock --
    // still a real hang, just via that mechanism's own spin rather than the naive loop's now-fixed
    // use-after-free. Not reachable through `RunCommandTool` as built (its
    // command executes externally, in a container, never recursively back into this C++ call stack) and
    // not this design's own established tool-invocation model (a tool returns one reply; it does not
    // synchronously invoke further operations on the same session mid-body) -- a real, structural
    // constraint worth stating explicitly, not an implicit assumption.
    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext&, agentengine::EffectContext&) {
        agentengine::ContextContribution contribution;
        if (runtime_.has_value()) {
            contribution.tools.push_back(agentengine::make_tool_descriptor_with_invoke<RunCommandTool>(
                [this](RunCommandArgs args, agentengine::EffectContext& ctx)
                    -> agentengine::result<RunCommandReply> {
                    agentengine::IdentityHandle caller =
                        agentengine::IdentityAuthority::bootstrap().adopt(ctx.principal);
                    auto outcome = agentengine::rt::block_on(
                        runtime_->run(*surface_, args.command, caller, *run_quota_, *storage_quota_));
                    if (!outcome.has_value()) return std::unexpected(outcome.error());
                    return RunCommandReply{true, outcome->exec.exit_code, outcome->exec.stdout_text,
                                             outcome->checkpoint.tree, outcome->checkpoint.turn_index};
                }));
        }
        // Second, deliberately separate gate (`bind_task_branch_tools()`'s own comment) -- these four
        // tools are contributed only once BOTH `runtime_` is bound AND a host has opted in with a
        // real `MergeCost` quota. Every closure re-derives its own `caller` from `ctx.principal`, the
        // same "never assume owner_" discipline `run_command`'s own closure already established.
        if (runtime_.has_value() && merge_quota_ != nullptr) {
            contribution.tools.push_back(agentengine::make_tool_descriptor_with_invoke<StartTaskBranchTool>(
                [this](TaskBranchStartArgs, agentengine::EffectContext& ctx)
                    -> agentengine::result<TaskBranchStartReply> {
                    agentengine::IdentityHandle caller =
                        agentengine::IdentityAuthority::bootstrap().adopt(ctx.principal);
                    auto outcome = agentengine::rt::block_on(start_task_branch(caller));
                    if (!outcome.has_value()) return std::unexpected(outcome.error());
                    return *outcome;
                }));
            contribution.tools.push_back(agentengine::make_tool_descriptor_with_invoke<RunInTaskBranchTool>(
                [this](TaskBranchRunArgs args, agentengine::EffectContext& ctx)
                    -> agentengine::result<TaskBranchRunReply> {
                    agentengine::IdentityHandle caller =
                        agentengine::IdentityAuthority::bootstrap().adopt(ctx.principal);
                    auto outcome = agentengine::rt::block_on(
                        run_in_task_branch(std::move(args.handle_id), std::move(args.command), caller));
                    if (!outcome.has_value()) return std::unexpected(outcome.error());
                    return *outcome;
                }));
            contribution.tools.push_back(agentengine::make_tool_descriptor_with_invoke<CommitTaskBranchTool>(
                [this](TaskBranchCommitArgs args, agentengine::EffectContext& ctx)
                    -> agentengine::result<TaskBranchCommitReply> {
                    agentengine::IdentityHandle caller =
                        agentengine::IdentityAuthority::bootstrap().adopt(ctx.principal);
                    auto outcome =
                        agentengine::rt::block_on(commit_task_branch(std::move(args.handle_id), caller));
                    if (!outcome.has_value()) return std::unexpected(outcome.error());
                    return *outcome;
                }));
            contribution.tools.push_back(agentengine::make_tool_descriptor_with_invoke<DiscardTaskBranchTool>(
                [this](TaskBranchDiscardArgs args, agentengine::EffectContext&)
                    -> agentengine::result<TaskBranchDiscardReply> {
                    auto outcome = agentengine::rt::block_on(discard_task_branch(std::move(args.handle_id)));
                    if (!outcome.has_value()) return std::unexpected(outcome.error());
                    return *outcome;
                }));
        }
        co_return contribution;
    }

    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        co_return std::monostate{};
    }

    [[nodiscard]] SandboxRuntime<Store> const* runtime() const noexcept {
        return runtime_.has_value() ? &*runtime_ : nullptr;
    }

    // Exposes `SandboxRuntime::reset_to_turn()` at THIS class's own level -- required because
    // `runtime()` above deliberately only ever hands back a `SandboxRuntime const*` (no mutable
    // reference to the owned runtime ever leaves this class), so a caller reaching in through that
    // accessor could never call a mutating verb on it. `requested_by` is taken explicitly, not
    // defaulted to `owner_`, matching this class's own established pattern for who a call's real
    // principal is (`on_context()`'s tool closure re-derives its `caller` from `ctx.principal` on every
    // call rather than assuming `owner_`). Deliberately NOT tool-facing today (no `Tool<>` wires it) --
    // a direct, host-callable method; a future `ResetSandboxTool` would supply whatever real,
    // ctx-derived principal is calling, the same way `run_command`'s own closure already does.
    [[nodiscard]] agentengine::rt::task<agentengine::result<agentengine::Checkpoint>> reset_to_turn(
        std::uint64_t target_turn_index, agentengine::IdentityHandle requested_by,
        agentengine::rt::AsyncQuota<agentengine::ResetCost>& reset_quota) {
        if (!runtime_.has_value()) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "cannot reset a session whose own sandbox was never bound",
                "mandatory_sandbox_provider.not_bound"});
        }
        co_return co_await runtime_->reset_to_turn(target_turn_index, requested_by, reset_quota);
    }

    // The four task-branch verbs (A10 promotion, this file's own top comment above
    // `StartTaskBranchTool`). Public and host-callable the same way `reset_to_turn()` already is,
    // independent of whether `on_context()` also wires them as tools -- `requested_by` is always
    // taken explicitly, never defaulted to `owner_`, matching every other method on this class.
    //
    // Every one of the four fails closed with the SAME `task_branch_not_enabled` error if either
    // `bind_sandbox()` was never called (no `runtime_`) or `bind_task_branch_tools()` was never
    // called (`merge_quota_ == nullptr`) -- a host that only wants `run_command` sees these four
    // simply refuse to work, never a crash and never partial/inconsistent behavior.
    [[nodiscard]] agentengine::rt::task<agentengine::result<TaskBranchStartReply>> start_task_branch(
        agentengine::IdentityHandle requested_by) {
        if (!runtime_.has_value() || merge_quota_ == nullptr) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "cannot start a task branch: sandbox not bound or task-branch tools not enabled",
                "mandatory_sandbox_provider.task_branch_not_enabled"});
        }
        agentengine::rt::AsyncMutex::Guard guard = co_await task_branch_mutex_->lock();
        auto child = co_await runtime_->spawn_child_branch(requested_by, *branch_quota_,
                                                              runtime_->staging_root().parent_path());
        if (!child.has_value()) co_return std::unexpected(child.error());
        std::string handle_id = child->branch_name();  // unique by construction (Ledger::branch_
                                                          // from()'s own internal sequence counter) --
                                                          // unguessability is not this design's own
                                                          // security boundary, per-instance map
                                                          // scoping is (task_branch_sandbox.hpp's own
                                                          // header comment, carried forward unchanged).
        task_branches_.insert_or_assign(handle_id, std::move(*child));
        co_return TaskBranchStartReply{std::move(handle_id)};
    }

    [[nodiscard]] agentengine::rt::task<agentengine::result<TaskBranchRunReply>> run_in_task_branch(
        std::string handle_id, std::string command, agentengine::IdentityHandle requested_by) {
        if (!runtime_.has_value() || merge_quota_ == nullptr) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "cannot run in a task branch: sandbox not bound or task-branch tools not enabled",
                "mandatory_sandbox_provider.task_branch_not_enabled"});
        }
        agentengine::rt::AsyncMutex::Guard guard = co_await task_branch_mutex_->lock();
        auto it = task_branches_.find(handle_id);
        if (it == task_branches_.end()) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract, "unknown task-branch handle: " + handle_id,
                "mandatory_sandbox_provider.task_branch_unknown_handle"});
        }
        auto outcome = co_await it->second.run(*surface_, std::move(command), requested_by,
                                                  *run_quota_, *storage_quota_);
        if (!outcome.has_value()) co_return std::unexpected(outcome.error());
        co_return TaskBranchRunReply{outcome->exec.exit_code, outcome->exec.stdout_text};
    }

    // Mirrors docs/planning/proofs/task_branch_tool/task_branch_sandbox.hpp's own A10 fix
    // (`commit_task_branch`, header-comment finding 3): `Ledger::merge()` registers the child branch
    // into its own orphan set on EVERY rejection path (not just this call's own reasoning -- already
    // proven at the `Ledger`/`SandboxRuntime::merge_into()` layer), so a REJECTED commit (most
    // commonly a real merge conflict) is reclaimed immediately and re-surfaced under the SAME
    // `handle_id` -- the caller still sees the ORIGINAL rejection error, but the handle keeps working
    // afterward (retry, run more work, or discard) instead of becoming "unknown handle" on the very
    // next call.
    [[nodiscard]] agentengine::rt::task<agentengine::result<TaskBranchCommitReply>> commit_task_branch(
        std::string handle_id, agentengine::IdentityHandle requested_by) {
        if (!runtime_.has_value() || merge_quota_ == nullptr) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "cannot commit a task branch: sandbox not bound or task-branch tools not enabled",
                "mandatory_sandbox_provider.task_branch_not_enabled"});
        }
        agentengine::rt::AsyncMutex::Guard guard = co_await task_branch_mutex_->lock();
        auto it = task_branches_.find(handle_id);
        if (it == task_branches_.end()) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract, "unknown task-branch handle: " + handle_id,
                "mandatory_sandbox_provider.task_branch_unknown_handle"});
        }
        SandboxRuntime<Store> child = std::move(it->second);
        task_branches_.erase(it);
        std::string const branch_name = child.branch_name();  // captured BEFORE merge_into() moves child
        auto cp = co_await std::move(child).merge_into(*runtime_, requested_by, *merge_quota_);
        if (cp.has_value()) co_return TaskBranchCommitReply{true, cp->turn_index};

        auto reclaimed = co_await runtime_->reclaim_orphaned_child(
            branch_name, requested_by, runtime_->staging_root().parent_path());
        if (!reclaimed.has_value()) {
            // A genuine internal inconsistency, not the ordinary conflict case -- merge() registers
            // every rejection as an orphan unconditionally, so a reclaim failure here means something
            // else is wrong. Surfaced distinctly so it is never mistaken for an ordinary conflict.
            co_return std::unexpected(agentengine::error{
                cp.error().klass,
                "commit was rejected (" + cp.error().message +
                    ") and the branch could not be reclaimed for retry (" + reclaimed.error().message +
                    ") -- the work may be reachable only via the lower-level Ledger orphan-reclaim API",
                "mandatory_sandbox_provider.task_branch_commit_rejected_and_reclaim_failed"});
        }

        // MUST-FIX (independent red-team, 2026-08-30): `Ledger::merge()` refunds its own `MergeCost`
        // unit on EVERY rejection (ADR-111's own contract), including a real CONFLICT -- which is only
        // detected AFTER real, expensive work (three tree loads plus a full `merge_trees()` diff,
        // `core/ledger.hpp`'s own `merge()`). Re-inserting the reclaimed branch under the SAME
        // `handle_id` with no further charge let a caller repeat that real, expensive work
        // indefinitely for a NET `MergeCost` cost of zero -- empirically proven: 50/50 rejected retries
        // on one permanently-conflicting handle, 0 net units spent, each call also briefly holding
        // `Ledger::mutex_` (shared by every session on that ledger) -- a real, LIVE I8/availability gap
        // this promotion made reachable for the first time (ADR-111 itself disclosed `merge()` had no
        // real caller yet, so this exact free-loop was never live until this file existed). The
        // underlying `Ledger::merge()` refund-on-rejection contract is intentionally left UNTOUCHED
        // here (other, already-tested callers still get it unchanged) -- instead, THIS tool surface
        // re-charges 1 `MergeCost` unit, to the SAME identity that just attempted the commit,
        // immediately after a successful reclaim: every retry now costs real budget again before the
        // handle becomes usable a second time, bounding the total free work to whatever `MergeCost`
        // remained at the time of the first rejection.
        auto retry_charge = co_await merge_quota_->try_consume(1, requested_by);
        if (!retry_charge.has_value()) {
            // Cannot afford to keep this handle retryable. Rather than leaving `*reclaimed` unresolved
            // (which would silently queue a REAL erasure on the next `reap_pending_abandons()` cycle --
            // the exact implicit-destructor hazard ADR-111's own MUST-FIX already closed for
            // `merge_into()` itself, one layer down), discard it explicitly and refund the `BranchCost`
            // unit `start_task_branch()` spent -- the same, already-established `discard_task_branch()`
            // contract, just triggered here instead of by an explicit caller request. `Ledger::abandon()`
            // has no failure mode (this file's own `discard_task_branch()` comment), so this is safe to
            // fire without checking its result.
            (void)co_await std::move(*reclaimed).discard();
            (void)co_await branch_quota_->refund(1);
            co_return std::unexpected(agentengine::error{
                cp.error().klass,
                "commit was rejected (" + cp.error().message +
                    ") and the retry quota needed to keep this handle usable is exhausted (" +
                    retry_charge.error().message +
                    ") -- the work has been discarded and its BranchCost unit refunded",
                "mandatory_sandbox_provider.task_branch_commit_rejected_and_retry_quota_exhausted"});
        }
        task_branches_.insert_or_assign(std::move(handle_id), std::move(*reclaimed));
        co_return std::unexpected(cp.error());
    }

    [[nodiscard]] agentengine::rt::task<agentengine::result<TaskBranchDiscardReply>> discard_task_branch(
        std::string handle_id) {
        if (!runtime_.has_value() || merge_quota_ == nullptr) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "cannot discard a task branch: sandbox not bound or task-branch tools not enabled",
                "mandatory_sandbox_provider.task_branch_not_enabled"});
        }
        agentengine::rt::AsyncMutex::Guard guard = co_await task_branch_mutex_->lock();
        auto it = task_branches_.find(handle_id);
        if (it == task_branches_.end()) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract, "unknown task-branch handle: " + handle_id,
                "mandatory_sandbox_provider.task_branch_unknown_handle"});
        }
        SandboxRuntime<Store> child = std::move(it->second);
        task_branches_.erase(it);
        auto discarded = co_await std::move(child).discard();
        if (!discarded.has_value()) co_return std::unexpected(discarded.error());
        (void)co_await branch_quota_->refund(1);  // discarded work costs nothing lasting, matching
                                                     // RunCost's own refund-on-"nothing kept" precedent
        co_return TaskBranchDiscardReply{true};
    }

private:
    agentengine::Ledger<Store>* ledger_ = nullptr;
    // `IdentityHandle` deliberately has NO default constructor of its own (Phase 1's own "identity-
    // only, no minting power" design -- construction is friend-gated to `IdentityAuthority`) --
    // `optional<>` is what lets THIS type still be default-constructible (required for
    // `clear_in_process_state()` to compile) without giving `IdentityHandle` one it was never meant to
    // have.
    std::optional<agentengine::IdentityHandle> owner_;
    agentengine::rt::AsyncQuota<agentengine::BranchCost>* branch_quota_ = nullptr;
    agentengine::rt::AsyncQuota<agentengine::RunCost>* run_quota_ = nullptr;
    agentengine::rt::AsyncQuota<agentengine::StorageBytes>* storage_quota_ = nullptr;
    std::optional<SandboxRuntime<Store>> runtime_;
    std::optional<Surface> surface_;

    // A10 promotion state (StartTaskBranchTool's own top comment). `merge_quota_` starts null --
    // `bind_task_branch_tools()` is the only thing that ever sets it, and every task-branch method
    // checks it alongside `runtime_.has_value()` before doing anything.
    agentengine::rt::AsyncQuota<agentengine::MergeCost>* merge_quota_ = nullptr;
    std::map<std::string, SandboxRuntime<Store>> task_branches_;
    std::unique_ptr<agentengine::rt::AsyncMutex> task_branch_mutex_;
};

}  // namespace agentengine
