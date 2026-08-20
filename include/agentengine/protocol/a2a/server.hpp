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
#include <string>
#include <unordered_map>
#include <utility>

#include "agentengine/core/error.hpp"
#include "agentengine/protocol/a2a/mapping.hpp"
#include "agentengine/protocol/a2a/types.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/secure_random.hpp"

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

// ADR-061 §7 R3: was a `std::random_device`-seeded `std::mt19937_64`, which is not a CSPRNG and is
// state-recoverable from its own output. Now a real system CSPRNG (trust/secure_random.hpp), which
// is also where `protocol/mcp/server.hpp`'s own generator now goes -- one implementation, not the
// two independently-written copies that previously shared only a comment.
//
// Fails closed: a handle that cannot be generated securely is not generated at all. The `result`
// propagates to `send_message()`'s caller rather than falling back to a weaker source.
[[nodiscard]] inline result<std::string> generate_task_id() {
    return trust::secure_random_hex(16);
}

}  // namespace server_detail

class A2aServer {
public:
    A2aServer(RunStarter starter, std::string context_id)
        : starter_(std::move(starter)), context_id_(std::move(context_id)) {}

    // §A.2 `SendMessage`. Blocking mode only (see file-top comment) -- there is no `returnImmediately`
    // parameter to honour differently, since this dispatcher has exactly one dispatch shape today.
    //
    // ADR-061 §7 R1/R2: `caller` is now REQUIRED, not optional. Two defects made it so:
    //   - R2: `StartRun::caller` defaults to `std::nullopt`, and `AgentSession::handle()` SKIPS the
    //     018 §2 admission check entirely when it is unset (agent_session.hpp's own comment at the
    //     `has_value()` branch). This dispatcher previously built `StartRun{std::move(input)}` with
    //     `caller` unset, so every inbound A2A message ran with admission bypassed, on any session,
    //     cross-tenant included. The `nullopt`-skips default is a deliberate back-compat affordance
    //     for the ~44 pre-existing in-process `StartRun{message}` test call sites (`StartRun`'s own
    //     comment) and stays legitimate for them; what was never legitimate is a PROTOCOL surface
    //     relying on it. A remote caller is exactly the case 018 §2 exists for, so the parameter is
    //     mandatory here and there is no defaulted overload to fall back into.
    //   - R1: the task store is now keyed per-principal (see `owner_` on `StoredTask`), which needs
    //     the establishing principal at creation time.
    //
    // ADR-061 §35/§37.3: `authority` is a NEW, trailing, DEFAULTED parameter -- unlike `caller` above,
    // its absence does not fail open. `AgentSession::start_run()`'s own admission (§20.4,
    // agent_session.hpp's `if (require_authority_)` branch) consults `authority` ONLY when the
    // underlying session was put into Tier-3 mode (`set_require_authority(true)`); an unset `authority`
    // against such a session is denied outright (`run.authority_required`), never silently admitted
    // through the `caller` branch instead -- fails closed by the session's own construction, not by
    // this dispatcher remembering to check anything. A non-Tier-3 session (the common, embedded/
    // single-tenant case, 018 §1) never reads `authority` at all, so every existing 2-argument call
    // site (`tests/test_a2a_server.cpp`, `tests/test_task_principal_binding.cpp`) is unaffected.
    [[nodiscard]] result<Task> send_message(
            Message const& inbound, agentengine::rt::SessionCaller const& caller,
            std::optional<agentengine::rt::RequestAuthority> authority = std::nullopt) {
        agentengine::Message input = from_a2a_message(inbound);
        agentengine::rt::StartRun start{std::move(input)};
        start.caller    = caller;
        start.authority = std::move(authority);
        result<RunOutcome> outcome = starter_(std::move(start));

        std::lock_guard<std::mutex> lock(mutex_);
        if (!outcome) {
            // No run was ever minted (see file-top comment) -- this dispatcher mints its OWN id for
            // this one case; `Task.id` is therefore NOT a `run_id` here, the honest exception to §1's
            // usual identity, not a silently invented one.
            result<std::string> minted = server_detail::generate_task_id();
            if (!minted) return std::unexpected(minted.error());
            Task t;
            t.id             = *minted;
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
            tasks_.emplace(t.id, StoredTask{t, caller});
            return t;
        }

        Task t;
        t.id           = outcome->run_id;
        t.context_id   = context_id_;
        t.status.state = task_state::completed;
        t.status.message = to_a2a_message(outcome->response.message, t.id, context_id_);
        t.history.push_back(inbound_with_task_id(inbound, t.id, context_id_));
        t.history.push_back(*t.status.message);
        tasks_.emplace(t.id, StoredTask{t, caller});
        return t;
    }

    // §A.2 `GetTask`.
    //
    // ADR-061 §7 R1. This method previously took a bare `task_id` and did a plain `tasks_.find()`
    // with no principal at all, and its own comment waved §4's rule away on the grounds that "there
    // is no principal/authorization boundary in this transport-agnostic dispatcher yet." That
    // reasoning does not hold: `Task::history` carries both the caller's inbound message and the
    // agent's full response, and `Task.id` is `run_id` (§1) which `AgentSession` mints as
    // `session_id + ":run:" + counter` (agent_session.hpp) -- a STRUCTURED, enumerable value. The
    // combination was an unauthenticated cross-principal read of entire conversations: 011 §8a's
    // MUST ("we MUST NOT treat possession of a server-minted handle (or a task id) as
    // authenticating anyone... bound server-side as `<user_id>:<handle>`") and 018 §7 G4's
    // release-blocking cross-tenant-leak class, both directly.
    //
    // Now: every stored task carries the principal that created it, and a caller who is not that
    // principal gets the BYTE-IDENTICAL error an entirely unknown id produces -- §4's own "never
    // distinguish not-found from not-authorized" rule, which does apply here and always did. The
    // error text deliberately does NOT echo `task_id` back (it previously did): under a host-owned
    // transport a credential can ride a path or an id (ADR-061 §7 R15), and an error string is a
    // logging/telemetry surface 018 §4 forbids credentials from reaching.
    //
    // Residual, named not silently carried: `Task.id`'s enumerability itself is NOT fixed here.
    // 012 §1 and §5 state the `task_id`-IS-`run_id` identity at spec level ("`task_id` already **is**
    // `run_id` (§1)", 012 §5), and `run_id` must stay deterministic per 001 §7/I5, so decoupling
    // them is a spec change requiring an ADR -- ADR-061's own decision, not a drive-by edit here.
    // With principal binding in place, enumerability is defense-in-depth rather than the control.
    [[nodiscard]] result<Task> get_task(std::string const& task_id,
                                        agentengine::rt::SessionCaller const& caller) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end() || !owned_by(it->second, caller)) {
            return std::unexpected(unknown_task_error());
        }
        return it->second.task;
    }

    // §A.2 `CancelTask`. §2.3: "further messages to a terminal task are rejected... the task is
    // immutable." Every task this dispatcher can produce is ALREADY terminal by the time it is
    // observable (file-top comment) -- so this always rejects, faithfully proving "terminal is
    // terminal" rather than fabricating a CANCELED transition this implementation cannot really do.
    //
    // ADR-061 §7 R1, same fix and same reasoning as `get_task` above. The ordering matters and is
    // deliberate: the ownership check runs BEFORE the terminal-state rejection, so a non-owner
    // cannot distinguish "this task exists but is terminal" (`a2a.unsupported_operation`) from
    // "no such task" (`a2a.unknown_task`) -- returning the terminal error to a stranger would leak
    // existence, which is exactly what §4's rule forbids.
    [[nodiscard]] result<Task> cancel_task(std::string const& task_id,
                                            agentengine::rt::SessionCaller const& caller) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end() || !owned_by(it->second, caller)) {
            return std::unexpected(unknown_task_error());
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

    // ADR-061 §7 R1: 011 §8a's "bound server-side as `<user_id>:<handle>`". Represented as an owner
    // field rather than by mangling the id, so the wire-visible `Task.id` stays exactly what §1 says
    // it is while the binding is enforced on lookup. Storing the owner also keeps the binding
    // durable across whatever future store replaces this in-process map (ADR-061 §7 R18).
    struct StoredTask {
        Task          task;
        agentengine::rt::SessionCaller owner;
    };

    // 018 §6: identity is `{id, tenant_id}`, and a cross-tenant id collision is NOT ownership --
    // the same rule `principal_admitted_for` (trust/principal.hpp) enforces for sessions, applied
    // here rather than re-derived. Deliberately an exact match: `SessionCaller` cannot express
    // `on_behalf_of` by construction (agent_session.hpp's own comment), so there is no delegation
    // case to widen for, and inventing one here would be strictly more permissive than the session
    // admission rule this mirrors.
    [[nodiscard]] static bool owned_by(StoredTask const& stored, agentengine::rt::SessionCaller const& caller) {
        return stored.owner.id == caller.id && stored.owner.tenant_id == caller.tenant_id;
    }

    // One error value for both "no such task" and "not yours", constructed identically so the two
    // are indistinguishable to a caller (§4). Deliberately echoes nothing back from the request --
    // see `get_task`'s own comment on why the previous `"unknown taskId: " + task_id` was itself a
    // problem.
    [[nodiscard]] static error unknown_task_error() {
        return error{failure_class::contract, "unknown taskId", "a2a.unknown_task"};
    }

    RunStarter                                        starter_;
    std::string                                       context_id_;
    mutable std::mutex                                mutex_;
    mutable std::unordered_map<std::string, StoredTask> tasks_;
};

}  // namespace agentengine::a2a
