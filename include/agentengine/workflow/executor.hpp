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
#include <string>
#include <utility>

#include "quark/core/actor.hpp"

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

// A function-kind executor node (014 §1). `Sequential` for the same reason the supervisor is (the
// breakdown's decision 4): a node must be passivatable when its Project pauses (030 §4), and
// `ActorRef<A>::passivate()` is a COMPILE error on anything else.
class FunctionExecutor : public quark::Actor<FunctionExecutor, quark::Sequential> {
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

}  // namespace agentengine::workflow
