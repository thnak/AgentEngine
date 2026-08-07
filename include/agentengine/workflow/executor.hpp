#pragma once
// Implements 014-Workflow-and-Orchestration.md §1's "Executors are Quark actors" and its
// `Executor = an agent | a function | a sub-workflow | a request port` node kinds.
// Milestone 6 Phase B (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// Phase B builds the FUNCTION kind only, and says so rather than shipping empty branches for the
// other three: the agent kind needs an `AgentSession` bound per node (Phase C), and the request-port
// kind needs 014 §4's suspend/resume (Phase E). What this file must get right now is the thing all
// four kinds share -- being a real Quark actor the supervisor talks to over a real `Ask`, so that
// §1's "concurrency, ordering, and failure isolation come from the runtime, not from a bespoke
// executor pool" is true from the first line rather than retrofitted.
//
// ONE actor TYPE, many instances. Each node in a graph is a separate `FunctionExecutor` activation
// under its own key, carrying its own body. The alternative -- a distinct C++ actor type per node --
// would make the graph's shape a compile-time property of the host program, which is exactly what
// 014 §7 ("the graph is data") and 015's declarative form both rule out.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "quark/core/actor.hpp"
#include "quark/core/supervision.hpp"

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"

namespace agentengine::workflow {

// 014 §1: "Messages between executors are the content model (003), so an agent node and a function
// node are interchangeable at an edge." So the payload IS `Message` -- not a workflow-private
// envelope that would have to be translated at every agent node.
//
// Size matters here in a way it does not elsewhere in this codebase: Quark's message pool has a
// fixed 192-byte cell (`quark::detail::MessagePool::kMaxPayload`) and `ActorRef::ask` static_asserts
// against it. Only the SHALLOW size counts -- `Message`'s content vector and strings are heap-owned
// -- but that is why this struct carries an executor INDEX rather than an executor id string, and
// why nothing else is added to it casually.
struct ExecuteRequest {
    Message       payload;
    std::uint32_t round = 0;  // 014 §2's whole-workflow clock, for attribution and tracing
};

struct ExecuteReply {
    Message payload;
    // Milestone 6 Phase C: the case labels this executor selected, for 014 §1's `switch_case` and
    // `multi_selection` edge kinds -- the Handoff and Router patterns (§3).
    //
    // I3 BOUNDARY, and the reason this is a list of LABELS rather than a target id. A label can only
    // ever select among edges THE GRAPH ALREADY DECLARES. An executor -- including one whose output
    // came from a model -- cannot name a node the author did not wire, cannot create an edge, and
    // cannot reach a target that is not already type-compatible (Phase A validated every edge before
    // the run started). A label matching nothing routes nowhere. Routing is therefore a choice among
    // pre-authorized options, never an authority the executor holds, which is what keeps "switch/case
    // on a classifier's typed output" (§3's Router row) inside I3 rather than in tension with it.
    std::vector<std::string> routes;
    // A failed executor still replies -- 014 §6 classifies the failure and lets the EDGE's declared
    // policy decide (propagate/retry/fallback/fail). Dropping the reply instead would strand the
    // supervisor's `co_await` and turn every executor error into a hang.
    bool          ok = true;
    failure_class klass = failure_class::fatal;
};

// What a function-kind executor produces: a payload, plus (for a routing node) the case labels it
// selected. The converting constructor from `Message` is deliberate -- the overwhelming majority of
// nodes route nowhere, and forcing every one of them to spell out an empty route list would make the
// common case pay for the rare one.
struct ExecutorOutcome {
    Message                  payload;
    std::vector<std::string> routes;

    ExecutorOutcome() = default;
    ExecutorOutcome(Message m) : payload(std::move(m)) {}  // NOLINT(google-explicit-constructor)
    ExecutorOutcome(Message m, std::vector<std::string> r)
        : payload(std::move(m)), routes(std::move(r)) {}
};

// The body of a function-kind executor. Takes an `EffectContext&` from the first version rather than
// being retrofitted with one later: running a node IS an attributable effect (I4), and this
// codebase has already paid once for appending a parameter to a widely-called signature after the
// fact (003 §6's `Usage::cache_write_tokens` amendment).
using ExecutorBody = std::function<result<ExecutorOutcome>(Message const&, EffectContext&)>;

// The failure marker delivered along a `propagate` or `fallback` edge (014 §6). Milestone 6 Phase D.
//
// It carries the executor id and the failure class and NOTHING ELSE -- deliberately. The obvious
// addition is the failed executor's error text, and for an agent-kind node that text can be
// model-derived; splicing it unlabelled into a downstream node's input is exactly the move I3 exists
// to prevent. `ExecuteReply` does not carry a message today, so Phase D does not have to decide the
// taint question in passing -- when a phase needs the text, it adds the field AND its `Tainted<>`
// treatment together. What is here is engine-generated, structured, and attributable (I4): a reader
// can always say which executor failed and how it was classified (001 §6).
[[nodiscard]] inline Message failure_marker(std::string const& executor_id, failure_class klass) {
    auto const* name = "fatal";
    switch (klass) {
        case failure_class::transient: name = "transient"; break;
        case failure_class::policy:    name = "policy"; break;
        case failure_class::contract:  name = "contract"; break;
        case failure_class::resource:  name = "resource"; break;
        case failure_class::fatal:     name = "fatal"; break;
    }
    ContentItem item{};
    item.origin  = content_origin::system;  // the ENGINE wrote this, not a model and not a tool
    item.tainted = false;
    item.value   = Error{"executor '" + executor_id + "' failed (" + name + ")"};
    Message m{};
    m.role = role::system;
    m.content.push_back(std::move(item));
    return m;
}

// A function-kind executor node (014 §1). `Sequential` for the same reason the supervisor is (the
// breakdown's decision 4): a node must be passivatable when its Project pauses (030 §4), and
// `ActorRef<A>::passivate()` is a COMPILE error on anything else.
//
// 014 §6's SECOND bullet, and it is a different failure channel from the first. A body that RETURNS
// an error is a reported failure -- the actor is healthy, and the edge's declared policy decides
// (`workflow/graph.hpp`). A body that THROWS is an actor failure: Quark 007 runs this policy, and
// `OnFailure<Restart, MaxRestarts<3, Within<1000>>>` is §6's "restarted or stopped without taking
// the workflow down, subject to a bounded escalation" spelled in Quark's vocabulary. The budget is
// bounded rather than Quark's unbounded default because an executor that faults every time would
// otherwise restart forever inside one round.
//
// The property that makes this safe to rely on: Quark dead-letters a faulting message's ask BEFORE
// restarting (`OnRestartAsk`'s default `Fail`, supervision.hpp), so the supervisor's `co_await`
// completes with an error instead of hanging. A throwing executor is therefore a failure the
// workflow can HANDLE, not a stall -- which is what makes "without taking the workflow down"
// achievable at all.
class FunctionExecutor
    : public quark::Actor<FunctionExecutor, quark::Sequential,
                          quark::OnFailure<quark::Restart, quark::MaxRestarts<3, quark::Within<1000>>>> {
public:
    using protocol = quark::Protocol<quark::Ask<ExecuteRequest, ExecuteReply>>;

    // Engine-hosted actors are default-constructed (`Engine::spawn<A>()` hands back only an
    // `ActorId`), so identity and body arrive through `initialize()` before registration -- the
    // exact pattern `AgentSession` already establishes for the same reason.
    void initialize(std::string id, ExecutorBody body, EffectContext ctx) {
        id_   = std::move(id);
        body_ = std::move(body);
        ctx_  = std::move(ctx);
    }

    [[nodiscard]] std::string const& id() const noexcept { return id_; }
    [[nodiscard]] std::uint32_t invocations() const noexcept { return invocations_; }
    [[nodiscard]] std::uint32_t last_round() const noexcept { return last_round_; }

    void handle(quark::Ask<ExecuteRequest, ExecuteReply> const& m) {
        ++invocations_;
        last_round_ = m.query.round;

        if (!body_) {
            // Fail closed and REPLY. A node with no body is an assembly error, but swallowing the
            // reply would hang the supervisor rather than surface it.
            m.respond(ExecuteReply{Message{}, {}, false, failure_class::contract});
            return;
        }
        auto out = body_(m.query.payload, ctx_);
        if (!out) {
            m.respond(ExecuteReply{Message{}, {}, false, out.error().klass});
            return;
        }
        m.respond(ExecuteReply{std::move(out->payload), std::move(out->routes), true,
                               failure_class::fatal});
    }

private:
    std::string   id_;
    ExecutorBody  body_;
    EffectContext ctx_{};
    std::uint32_t invocations_ = 0;
    std::uint32_t last_round_  = 0;
};

// Build a workflow actor's `quark::Activation` WITH its declared policies. Milestone 6 Phase D, and
// it exists because of a measurement, not a preference.
//
// `Engine::spawn<A>()` resolves `supervision_of<A>()` and hands it to the Activation constructor.
// `Engine::register_activation()` -- the path this milestone uses, and the one M4 Phase E2
// established, because `spawn<A>()` returns only an `ActorId` and an actor here needs its own
// `initialize()` called -- does NOT. It takes an already-constructed Activation and never touches
// supervision. So a host that writes the obvious
//
//     Activation(node.get(), FunctionExecutor::dispatch_table(), pool.sink())
//
// gets Quark's DEFAULT policy: Restart, unbounded. The `OnFailure<Restart, MaxRestarts<3, …>>`
// declared above is silently inert, and every test still passes -- 014 §6's "subject to a bounded
// escalation" is simply not in force. Measured, not reasoned: Phase D's D7 drove a throwing executor
// past the declared budget of 3 and counted **6** handler entries.
//
// This is decision 5b's shape exactly (the naive host wiring is the broken one), so the fix goes in
// the same place: the workflow layer, in the file that DECLARES the policy, where a host cannot omit
// it by writing less code. `Activation`'s own comment already says the policy is "set at
// registration by the engine/harness" -- this is the harness half, written once.
template <class A, class Reclaim>
[[nodiscard]] inline std::unique_ptr<quark::Activation> make_workflow_activation(A& actor,
                                                                                 Reclaim reclaim) {
    return std::make_unique<quark::Activation>(&actor, A::dispatch_table(), reclaim,
                                               quark::max_concurrency_of<A>(),
                                               quark::supervision_of<A>());
}

}  // namespace agentengine::workflow
