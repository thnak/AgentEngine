#pragma once
// ADR-149 (issue #28 item 4): `WorkflowCheckpointManager<StoreT>`, a thin convenience wrapper --
// NOT a new persistence mechanism. `SessionStore` (rt/session_store.hpp, with its
// `InMemorySessionStore`/`FileSessionStore` conformers) and `encode_run_state_record()`/
// `load_workflow_checkpoint()` (rt/workflow_supervisor.hpp) already exist and already work; this
// file only packages the two things every real caller was hand-writing on top of them:
//   (1) auto-persisting a checkpoint every round, instead of the caller writing its own
//       `set_checkpoint_hook()` closure;
//   (2) "resume from store if one exists, else start fresh" as one call, instead of the caller
//       hand-rolling `tests/test_rt_workflow_checkpoint_g2.cpp`'s own "brand-new supervisor,
//       initialize(), restore_from_record()" idiom every time.
//
// FAIL-CLOSED ON THE AGENT-HISTORY GAP (ADR-149 §3 finding 4): docs/planning/agent-as-workflow-
// executor-design-draft.md already documents, as an accepted, tested limitation, that an
// `agent`-kind executor's conversation history does NOT survive checkpoint/resume --
// `restore_from_record()` never touches `bodies_`, and `RunStateRecord` carries no session-history
// field. That's sound for the general case, but Magentic's whole value is a moderator that RETAINS
// context across rounds -- making resume a one-liner here would make it easy to hit that gap by
// accident, for exactly the workflow shape that gap hurts most. `resume_or_start()` therefore
// refuses (a `contract`-class error, not a silent resume) to resume a graph containing any
// `agent`-kind executor unless the caller explicitly passes `acknowledge_agent_history_reset =
// true` -- an explicit host opt-in that fails closed when unset, the same Delegated Decision Seam
// shape ADR-070/ADR-071 already establish elsewhere in this codebase.

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/rt/session_store.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/graph.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow WorkflowCheckpointManager — ADR-149's convenience-API family, mirrors WorkflowSupervisor's naming
template <SessionStore StoreT>
class WorkflowCheckpointManager {
public:
    // `on_save_error`, if set, is called synchronously (from inside the checkpoint hook, itself
    // called synchronously from `WorkflowSupervisor::execute()`'s round loop) whenever a round's
    // auto-persist fails -- e.g. `FileSessionStore`'s own documented "KNOWN DURABILITY LIMITATION".
    // The run itself is NOT aborted on a save failure (a checkpoint failing to persist is not the
    // same as the round itself failing); a host that wants stricter behavior implements that in its
    // own `on_save_error` callback.
    explicit WorkflowCheckpointManager(StoreT& store,
                                        std::function<void(agentengine::error const&)> on_save_error = {})
        : store_(store), on_save_error_(std::move(on_save_error)) {}

    // LIFETIME CONTRACT (matching `agent_session_as_executor_body()`'s own documented by-reference
    // capture contract, rt/agent_workflow_executor.hpp): `this` is captured by the hook installed on
    // `sup`, so this `WorkflowCheckpointManager` must outlive every round `sup` ever executes after
    // `attach()` is called.
    void attach(WorkflowSupervisor& sup) {
        sup.set_checkpoint_hook([this](std::uint32_t /*round*/, RunStateRecord const& rec) {
            result<void> saved = store_.save(rec.run_id, encode_run_state_record(rec));
            if (!saved && on_save_error_) on_save_error_(saved.error());
        });
    }

    // `true` -- resumed from a stored checkpoint; the caller should drive `continue_workflow()`.
    // `false` -- no checkpoint existed for `run_id`; the caller should drive `run_workflow()`.
    // A real error -- the store I/O failed, the stored record was malformed, or (see file banner)
    // the graph has an agent-kind (or sub_workflow-kind) executor and
    // `acknowledge_agent_history_reset` was not set.
    [[nodiscard]] static result<bool> resume_or_start(
        StoreT& store, std::string const& run_id, WorkflowSupervisor& sup,
        agentengine::workflow::Workflow graph, std::vector<ExecutorBody> bodies,
        std::vector<agentengine::EffectContext> contexts = {},
        std::string designated_stall_reporter = {}, bool acknowledge_agent_history_reset = false) {
        sup.initialize(std::move(graph), std::move(bodies), std::move(contexts),
                        std::move(designated_stall_reporter));

        result<std::optional<RunStateRecord>> loaded = load_workflow_checkpoint(store, run_id);
        if (!loaded) return std::unexpected(loaded.error());
        if (!loaded->has_value()) return false;

        // ALSO covers `sub_workflow`-kind, not just `agent`-kind -- a post-implementation audit
        // named this defensively: `sub_workflow` cannot execute at all today
        // (`check_workflow_executable()`, workflow/graph.hpp, refuses it unconditionally), so this
        // branch is currently unreachable for it in practice, but a future `sub_workflow` runtime
        // bridge (issue #33) would carry the SAME "hidden per-executor state lost on resume" hazard
        // agent-kind already has (an embedded sub-workflow's own in-flight state), and this guard's
        // shape should already be broad enough to catch it rather than needing a second fix later.
        bool const has_stateful_kind =
            std::any_of(sup.graph().executors.begin(), sup.graph().executors.end(),
                        [](agentengine::workflow::Executor const& e) {
                            return e.kind == agentengine::workflow::executor_kind::agent ||
                                   e.kind == agentengine::workflow::executor_kind::sub_workflow;
                        });
        if (has_stateful_kind && !acknowledge_agent_history_reset) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "WorkflowCheckpointManager::resume_or_start: resuming a graph with an agent-kind "
                "(or sub_workflow-kind) executor silently discards that executor's own in-flight "
                "state (docs/planning/agent-as-workflow-executor-design-draft.md's own accepted, "
                "tested limitation for agent-kind) -- pass acknowledge_agent_history_reset=true to "
                "proceed anyway",
                "rt.workflow_checkpoint_manager.agent_history_reset_unacknowledged"});
        }

        sup.restore_from_record(**loaded);
        return true;
    }

private:
    StoreT&                                          store_;
    std::function<void(agentengine::error const&)>   on_save_error_;
};

}  // namespace agentengine::rt
