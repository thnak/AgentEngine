#pragma once
// Implements 012-A2A-Conformance.md §2.3 -- Task management (server role): `SendMessage`, `GetTask`,
// `CancelTask` over a REAL `AgentSession` run. Milestone 7 Phase D3
// (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// Transport-agnostic, like `protocol/mcp/server.hpp`: no JSON-RPC/REST envelope here (that is D4+'s
// own job, matching MCP's own "envelope first" split, C1 before C2), and no actor-messaging plumbing
// either -- `A2aServer` is handed a `RunStarter` (a plain callable that starts one run and returns
// once it settles) rather than an `AgentSession` reference itself, exactly the same "no real
// transport yet, take what a transport would supply as given" layering `McpServer` already
// established for `held`/`approve`. This phase's OWN test wires a `RunStarter` against a REAL
// `agentengine::rt::AgentSession`, the same "same process, two roles, real machinery underneath"
// shape every prior Phase C/D test already used.
//
// §1: "Task ← Run... contextId groups related tasks and maps onto our session." A `Task.id` IS the
// `run_id` `AgentSession::last_run_id()` reports once a run settles (never a separately invented id)
// -- EXCEPT when a run is never minted at all (`StartRun`'s admission check runs before `run_counter_`
// increments, agent_session.hpp's own `handle()` comment), in which case there is no `run_id` to
// borrow; `send_message()` mints its own task id for exactly that one case, named explicitly below.
//
// ADR-037: ported onto `agentengine::rt::AgentSession` (rt/agent_session.hpp) -- `RunStarter`/
// `RunOutcome` now name `rt::StartRun`/`rt::AgentResponse` instead of the old Quark-actor-backed
// `agentengine::StartRun`/`agentengine::AgentResponse` (core/agent_session.hpp). This dispatcher was
// ALREADY transport-agnostic before this port (it only ever named `StartRun`/`AgentResponse` as
// types passed through a caller-supplied `RunStarter` callable, never touched `quark::` itself), so
// the only real change is which header those two type names resolve from -- everything else in this
// file (task-id minting, task-state mapping, the honesty notes below) is unchanged. `A2aClient`
// (client.hpp) needed NO changes at all: it was already fully transport-agnostic (`RemoteAgentTransport`
// is three plain `Message`/`Task`-typed callables with no `AgentSession` dependency of any kind).
//
// Task lifecycle honesty: `AgentSession`'s current turn loop (still M1-era, rt/agent_session.hpp's
// own comments throughout) makes `start_run(StartRun) -> task<result<AgentResponse>>` fully
// SYNCHRONOUS/blocking end to end from a caller's point of view -- it either resolves with a real
// `AgentResponse` or never resolves at all (fail-closed). There is no tool-call loop, no real
// approval gate, and `open_interaction()`/`resolve_interaction()` are host-callable but NOT wired
// into the turn loop (rt/agent_session.hpp's own Phase E1-equivalent comment: "NOT wired into the
// synchronous turn loop... What IS real here: minting, tracking, and resolving Interaction
// records... the vocabulary and lifecycle, proven standalone"). Consequently:
//   - `TASK_STATE_SUBMITTED`/`TASK_STATE_WORKING` are never independently OBSERVABLE from outside --
//     `send_message()` itself blocks until the run settles, so a caller never sees an intermediate
//     state (the same "no `returnImmediately: true` async dispatch built yet" gap named below).
//   - `TASK_STATE_INPUT_REQUIRED`/`TASK_STATE_AUTH_REQUIRED` have no real producer -- the turn loop
//     never opens an `Interaction` on its own. Named, not fabricated.
//   - `TASK_STATE_COMPLETED` is the real success terminal; `TASK_STATE_FAILED` is the real generic
//     failure terminal (the `RunStarter` returning an error -- admission denial, context-provider
//     failure, chat-client failure, and token-budget-exceeded all collapse to this ONE generic
//     failure shape in `AgentSession` today, none distinguishable from outside it, so this dispatcher
//     does not invent a false distinction between e.g. `FAILED` and `REJECTED` that the underlying
//     session cannot actually report).
//   - `TASK_STATE_CANCELED` likewise has no real in-flight producer: by the time `send_message()`
//     returns, the task it produced is ALREADY terminal (fully synchronous dispatch, above) -- so
//     `cancel_task()` can only ever observe an already-terminal task and correctly rejects it (§2.3's
//     own "terminal is terminal" rule), never actually interrupts live work. A genuinely cancellable
//     in-flight task needs async dispatch (`returnImmediately: true`, a backgrounded run) -- D4+ scope.

#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

#include "agentengine/core/error.hpp"
#include "agentengine/protocol/a2a/mapping.hpp"
#include "agentengine/protocol/a2a/types.hpp"
#include "agentengine/rt/agent_session.hpp"

namespace agentengine::a2a {

// What starting one real run and waiting for it to settle actually returns: `AgentResponse` alone
// (rt/agent_session.hpp) carries no `run_id` in-band, so a caller that wants to correlate a
// `Task.id` with the real run must ALSO read `AgentSession::last_run_id()` right after the call
// resolves -- the same thing `AgentSessionRecord`'s own checkpoint-content comment already
// documents as the established way to learn a run's id after the fact. `RunOutcome` names that
// pairing explicitly rather than asking every caller to remember to fetch it separately.
struct RunOutcome {
    std::string                    run_id;
    agentengine::rt::AgentResponse response;
};

// A BLOCKING call from the caller's point of view: returns only once the underlying
// `rt::AgentSession::start_run()` call has settled (or failed to). See file-top comment for exactly
// what that means for task-state observability.
using RunStarter = std::function<result<RunOutcome>(agentengine::rt::StartRun)>;

namespace server_detail {

// The same "sufficient entropy, not a predictable counter" idiom `protocol/mcp/server.hpp`'s own
// `generate_task_id()` establishes for the identical reason (§12's own bearer-token-shaped hazard,
// cited there for MCP task ids -- A2A task ids carry the same risk, reproduced rather than shared
// through an unrelated MCP-specific header for one function).
[[nodiscard]] inline std::string generate_task_id() {
    std::random_device rd;
    std::mt19937_64     gen(rd());
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(gen) << dist(gen);
    return oss.str();
}

}  // namespace server_detail

class A2aServer {
public:
    A2aServer(RunStarter starter, std::string context_id)
        : starter_(std::move(starter)), context_id_(std::move(context_id)) {}

    // §A.2 `SendMessage`. Blocking mode only (see file-top comment) -- there is no `returnImmediately`
    // parameter to honour differently, since this dispatcher has exactly one dispatch shape today.
    [[nodiscard]] result<Task> send_message(Message const& inbound) {
        agentengine::Message input = from_a2a_message(inbound);
        result<RunOutcome> outcome = starter_(agentengine::rt::StartRun{std::move(input)});

        std::lock_guard<std::mutex> lock(mutex_);
        if (!outcome) {
            // No run was ever minted (see file-top comment) -- this dispatcher mints its OWN id for
            // this one case; `Task.id` is therefore NOT a `run_id` here, the honest exception to §1's
            // usual identity, not a silently invented one.
            Task t;
            t.id             = server_detail::generate_task_id();
            t.context_id     = context_id_;
            t.status.state   = task_state::failed;
            Message failure_msg;
            failure_msg.message_id = t.id + ":failure";
            failure_msg.task_id     = t.id;
            failure_msg.context_id  = context_id_;
            failure_msg.role        = a2a_role::agent;
            Part p;
            p.value = TextPart{outcome.error().message};
            failure_msg.parts.push_back(std::move(p));
            t.status.message = failure_msg;
            tasks_.emplace(t.id, t);
            return t;
        }

        Task t;
        t.id           = outcome->run_id;
        t.context_id   = context_id_;
        t.status.state = task_state::completed;
        t.status.message = to_a2a_message(outcome->response.message, t.id, context_id_);
        t.history.push_back(inbound_with_task_id(inbound, t.id, context_id_));
        t.history.push_back(*t.status.message);
        tasks_.emplace(t.id, t);
        return t;
    }

    // §A.2 `GetTask`. §4's own "never distinguish not-found from not-authorized" rule does not apply
    // here -- there is no principal/authorization boundary in this transport-agnostic dispatcher yet
    // (the same "authorization is transport work" layering `McpServer`/`McpClient` already name), so
    // an unknown taskId is simply not-found.
    [[nodiscard]] result<Task> get_task(std::string const& task_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) {
            return std::unexpected(
                error{failure_class::contract, "unknown taskId: " + task_id, "a2a.unknown_task"});
        }
        return it->second;
    }

    // §A.2 `CancelTask`. §2.3: "further messages to a terminal task are rejected... the task is
    // immutable." Every task this dispatcher can produce is ALREADY terminal by the time it is
    // observable (file-top comment) -- so this always rejects, faithfully proving "terminal is
    // terminal" rather than fabricating a CANCELED transition this implementation cannot really do.
    [[nodiscard]] result<Task> cancel_task(std::string const& task_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) {
            return std::unexpected(
                error{failure_class::contract, "unknown taskId: " + task_id, "a2a.unknown_task"});
        }
        return std::unexpected(error{failure_class::policy,
                                      "task is already terminal; CancelTask on a terminal task is "
                                      "rejected (012 §2.3: \"terminal is terminal\")",
                                      "a2a.unsupported_operation"});
    }

private:
    [[nodiscard]] static Message inbound_with_task_id(Message m, std::string const& task_id,
                                                        std::string const& context_id) {
        m.task_id    = task_id;
        m.context_id = context_id;
        return m;
    }

    RunStarter                                  starter_;
    std::string                                 context_id_;
    mutable std::mutex                          mutex_;
    mutable std::unordered_map<std::string, Task> tasks_;
};

}  // namespace agentengine::a2a
