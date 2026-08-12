// Proof for ADR-037 Phase 3, Slice 1: request-port behavior against agentengine::rt::WorkflowSupervisor
// (include/agentengine/rt/workflow_supervisor.hpp) -- the Quark-actor-free replacement for
// agentengine::workflow::WorkflowSupervisor. This file supersedes two OLD, Quark-actor-based files
// that proved real behavioral claims about 014-Workflow-and-Orchestration.md §4 (human-in-the-loop /
// the request port) with no equivalent against `rt::` yet:
//   - test_workflow_request_port.cpp (E0-E5; E0 and the activation-passivation half of E1/E4 are OUT
//     OF SCOPE here -- E0 is shared, unchanged workflow/graph.hpp validator code, and there are no
//     Quark activations in `rt::` for passivation to apply to).
//   - test_workflow_request_port_independent_qa.cpp (IQ1-IQ6, written independently straight from
//     spec prose rather than by reading the implementation).
//
// test_rt_workflow_supervisor.cpp already proves C1-C5/P1-P5 (C5 is a basic request-port suspend/
// resume round-trip; P4 restores a suspended-at-a-port run onto a fresh instance and drives a real
// resume). This file does NOT re-prove that basic shape -- it proves the claims C5/P4 leave open:
//   E1 -- reaching a port suspends with exactly one Interaction, the port's own ExecutorBody is never
//         invoked, and a SIBLING branch that did NOT reach a port still completes in the same round,
//         landing in WorkflowResult::partial.
//   E2 -- resume_workflow() naming an unknown OR already-resolved interaction_id fails closed.
//   E3 -- OQ-4's flagship case: two ports open concurrently in different fan-out branches of the SAME
//         round; resolving only one leaves the run suspended on exactly the other; resolving both
//         routes each answer to its own downstream target via an ordinary fan_in merge.
//   E5 -- a port's ResumeWorkflow::routes selects a switch_case edge exactly like model output would;
//         an invented/unrecognized label produces routing_failed (I3: a human's answer gets no more
//         authority than a model's).
//   IQ1 -- graph.bound.deadline_ms accounts for RUNNING time only; real wall-clock time spent
//          suspended at a port does not count against the deadline.
//   IQ2 -- a request_port node inside a CYCLE opens twice across two rounds with DISTINCT, round-
//          qualified interaction_ids; the stale first id fails closed after the second opens.
//   IQ3 -- a routing_failed discovered at RESUME time still preserves WorkflowResult::partial from
//          everything that completed before the port was reached.
//   IQ4 -- a port reached in the SAME round as a sibling `fail`-policy failure never gets its
//          Interaction minted at all; the port's executor id lands in WorkflowResult::unopened_ports
//          instead of vanishing silently. (Grepped the existing suite first: no other test exercises
//          `unopened_ports` -- this is the first live check of that field.)
//   IQ5 -- ResumeWorkflow::routes is ignored on a port whose outgoing edge is plain `direct`, not
//          switch_case/multi_selection (I3: routes only mean something where the graph declared they
//          could).
//   IQ6 -- WorkflowSupervisor::open_interactions() (the live accessor) and
//          WorkflowResult::open_interactions (the reply field) stay in agreement through a partial
//          resume (E3's shape) and a rejected double-resolve (E2's shape).
//
// MACHINE SAFETY (CLAUDE.md): bounded round counts throughout; the one real sleep (IQ1) is 200ms,
// tens-of-ms scale like the rest of this migration's timing checks, not unbounded.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"

using agentengine::rt::ContinueWorkflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}
void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

// Same rationale as test_rt_workflow_supervisor.cpp's own drive<T>(): every entry point's only
// suspension points are run_mutex_'s uncontended fast path and a nested co_await execute(), whose own
// body never co_awaits anything -- fan-out concurrency happens through std::future::get(), an ordinary
// blocking call. One resume() call resolves the whole chain in this single-caller test file.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ContentItem;
using agentengine::Message;
using agentengine::Text;
using agentengine::content_origin;
using agentengine::role;
using agentengine::workflow::Edge;
using agentengine::workflow::EdgeFailurePolicy;
using agentengine::workflow::Executor;
using agentengine::workflow::Workflow;
using agentengine::workflow::edge_failure_policy;
using agentengine::workflow::edge_kind;
using agentengine::workflow::executor_kind;
using agentengine::workflow::validate_workflow;

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string text_of(Message const& m) {
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) return t->text;
    }
    return {};
}

// Unlike text_of() (first Text item only), this joins EVERY Text content item with "+" -- needed for
// E3/IQ6's fan_in merge, whose merged delivery carries one ContentItem per resolved port response.
[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += "+";
            out += t->text;
        }
    }
    return out;
}

[[nodiscard]] Executor node_desc(char const* id) {
    return Executor{id, executor_kind::function, "T", "T"};
}
[[nodiscard]] Executor port_desc(char const* id) {
    return Executor{id, executor_kind::request_port, "T", "T"};
}

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in,
                                    agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

[[nodiscard]] ExecutorBody counting_appender(std::string name, std::shared_ptr<int> calls) {
    return [name = std::move(name), calls](
               Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        ++*calls;
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

[[nodiscard]] ExecutorBody failing_body(agentengine::failure_class klass) {
    return [klass](Message const&, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
        return std::unexpected(agentengine::error{klass, "injected failure", "test.injected"});
    };
}

}  // namespace

int main() {
    // ---- E1: reaching a port suspends with one Interaction; the port body is never invoked; a ----
    // ---- sibling in the SAME round that did NOT reach a port still completes into `partial`.   ----
    //
    //            /-> port (request_port)
    //   start --<
    //            \-> sib (function)
    {
        Workflow wf;
        wf.id        = "e1-sibling-completes";
        wf.executors = {node_desc("start"), port_desc("port"), node_desc("sib")};
        wf.edges.push_back(Edge{"start", "port", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"start", "sib", edge_kind::fan_out, {}});
        wf.start = "start";
        wf.output_selection.push_back("sib");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "E1: the fan-out-to-port graph validates");

        auto port_calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            appender("start"),
            // The port's own ExecutorBody, with an invocation counter -- proving "the port node is
            // NEVER invoked" empirically rather than relying only on the structural guarantee that
            // port_deliveries never reach run_executor_job.
            [port_calls](Message const& in,
                         agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                ++*port_calls;
                return ExecutorOutcome{in};
            },
            appender("sib"),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::suspended, "E1: reaching the port suspends the run");
        check(r.open_interactions.size() == 1, "E1: exactly one Interaction is open for the one port");
        check(*port_calls == 0,
              "E1: the port node's ExecutorBody was never invoked -- reaching a port IS the event, not "
              "a call");

        bool found_sib = false, found_start = false;
        for (auto const& out : r.partial) {
            if (out.executor_id == "sib") {
                found_sib = true;
                check(text_of(out.payload) == "in>start>sib",
                      "E1: the sibling branch that did NOT reach a port still completed, and its "
                      "output landed in WorkflowResult::partial");
            }
            if (out.executor_id == "start") found_start = true;
        }
        check(found_sib, "E1: 'sib' has a partial entry at all despite the run suspending this round");
        check(found_start, "E1: 'start' (the round before) also still has its partial entry");
        check(text_of(r.output) == "in>start>sib",
              "E1: the sibling's output is visible as the run's (suspended) selected output too");
    }

    // ---- E2: an unknown interaction_id, and a re-used already-resolved one, both fail closed. ----
    {
        Workflow wf;
        wf.id        = "e2-fail-closed";
        wf.executors = {node_desc("start"), port_desc("ask"), node_desc("finish")};
        wf.edges.push_back(Edge{"start", "ask", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"ask", "finish", edge_kind::direct, {}});
        wf.start = "start";
        wf.output_selection.push_back("finish");
        wf.bound.max_rounds = 8;

        std::vector<ExecutorBody> bodies = {appender("start"), {}, appender("finish")};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r1.status == workflow_status::suspended, "E2 setup: the run suspends at the port");

        WorkflowResult bogus = drive(sup.resume_workflow(ResumeWorkflow{"nope:port:ask:0", text_message("x"), {}}));
        check(bogus.status == workflow_status::invalid,
              "E2: resuming an interaction_id the run never opened fails closed");
        check(sup.open_interactions().size() == 1,
              "E2: the bogus resume advanced nothing -- the real port is still open");

        std::string const id = r1.open_interactions.empty() ? std::string{} : r1.open_interactions.front().interaction_id;
        WorkflowResult r2 = drive(sup.resume_workflow(ResumeWorkflow{id, text_message("yes"), {}}));
        check(r2.status == workflow_status::completed, "E2: the real id resumes the run to completion");

        WorkflowResult replay = drive(sup.resume_workflow(ResumeWorkflow{id, text_message("yes-again"), {}}));
        check(replay.status == workflow_status::invalid,
              "E2: answering the SAME interaction_id a second time fails closed -- a resolved port is "
              "gone, so a duplicated response cannot re-run the branch behind it");
    }

    // ---- E3 (OQ-4 flagship): two ports open concurrently in different fan-out branches of the ----
    // ---- SAME round; resolving one leaves the other open; resolving both merges via fan_in.    ----
    //
    //          /-> portA -\
    //   fan --              >-> merge
    //          \-> portB -/
    {
        Workflow wf;
        wf.id        = "e3-two-concurrent-ports";
        wf.executors = {node_desc("fan"), port_desc("portA"), port_desc("portB"), node_desc("merge")};
        wf.edges.push_back(Edge{"fan", "portA", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"fan", "portB", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"portA", "merge", edge_kind::fan_in, {}});
        wf.edges.push_back(Edge{"portB", "merge", edge_kind::fan_in, {}});
        wf.start = "fan";
        wf.output_selection.push_back("merge");
        wf.bound.max_rounds = 8;
        check(validate_workflow(wf).has_value(), "E3: the two-port fan-out/fan-in graph validates");

        std::vector<ExecutorBody> bodies = {appender("fan"), {}, {}, appender("merge")};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r1.status == workflow_status::suspended,
              "E3: both ports are reached in the same round and the run suspends once");
        check(r1.open_interactions.size() == 2,
              "E3 (OQ-4): TWO ports open concurrently in different branches produce TWO concurrent "
              "Interaction records on the same run -- interaction_id is a set, not a singleton");

        std::string id_a, id_b;
        for (auto const& i : r1.open_interactions) {
            if (i.interaction_id.find(":portA:") != std::string::npos) id_a = i.interaction_id;
            if (i.interaction_id.find(":portB:") != std::string::npos) id_b = i.interaction_id;
        }
        check(!id_a.empty() && !id_b.empty() && id_a != id_b, "E3: both ids are present and distinct");

        WorkflowResult r2 = drive(sup.resume_workflow(ResumeWorkflow{id_a, text_message("A"), {}}));
        check(r2.status == workflow_status::suspended,
              "E3: resolving ONLY portA leaves the run suspended -- it does not leave Suspended until "
              "every open Interaction is resolved");
        check(r2.open_interactions.size() == 1 && r2.open_interactions.front().interaction_id == id_b,
              "E3: exactly portB is still open after resolving portA");

        WorkflowResult r3 = drive(sup.resume_workflow(ResumeWorkflow{id_b, text_message("B"), {}}));
        check(r3.status == workflow_status::completed, "E3: resolving the last port completes the run");
        check(all_text_of(r3.output) == "A+B>merge",
              "E3: BOTH answers routed to their own downstream target and merged via fan_in exactly "
              "like any other fan_in delivery -- a request port is an ordinary node at an edge");
    }

    // ---- E5: a port's routes select a switch_case edge like model output would; an invented ----
    // ---- label produces routing_failed (I3: a human's answer is no more trusted than a model's). ----
    {
        auto build = [] {
            Workflow wf;
            wf.id        = "e5-approve-or-reject";
            wf.executors = {node_desc("draft"), port_desc("review"), node_desc("publish"), node_desc("discard")};
            wf.edges.push_back(Edge{"draft", "review", edge_kind::direct, {}});
            wf.edges.push_back(Edge{"review", "publish", edge_kind::switch_case, "approve"});
            wf.edges.push_back(Edge{"review", "discard", edge_kind::switch_case, "reject"});
            wf.start = "draft";
            wf.output_selection = {"publish", "discard"};
            wf.bound.max_rounds  = 8;
            return wf;
        };
        check(validate_workflow(build()).has_value(), "E5: the switch_case-routed port graph validates");

        for (std::string const answer : {"approve", "reject"}) {
            auto publish_calls = std::make_shared<int>(0);
            auto discard_calls = std::make_shared<int>(0);
            std::vector<ExecutorBody> bodies = {
                appender("draft"), {}, counting_appender("publish", publish_calls),
                counting_appender("discard", discard_calls),
            };
            WorkflowSupervisor sup;
            sup.initialize(build(), bodies);
            WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
            std::string const id = r1.open_interactions.empty() ? std::string{} : r1.open_interactions.front().interaction_id;
            WorkflowResult r2 = drive(sup.resume_workflow(ResumeWorkflow{id, text_message(answer), {answer}}));

            bool const approved = answer == "approve";
            check(r2.status == workflow_status::completed, "E5: the routed answer completes the run");
            check((approved ? *publish_calls == 1 && *discard_calls == 0
                            : *discard_calls == 1 && *publish_calls == 0),
                  "E5: the human's answer selected exactly one declared switch_case branch, the other "
                  "stayed cold");
        }

        // ---- I3 boundary, negative control: an invented label reaches nothing. ------------------
        {
            auto publish_calls = std::make_shared<int>(0);
            auto discard_calls = std::make_shared<int>(0);
            std::vector<ExecutorBody> bodies = {
                appender("draft"), {}, counting_appender("publish", publish_calls),
                counting_appender("discard", discard_calls),
            };
            WorkflowSupervisor sup;
            sup.initialize(build(), bodies);
            WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
            std::string const id = r1.open_interactions.empty() ? std::string{} : r1.open_interactions.front().interaction_id;
            WorkflowResult r2 = drive(
                sup.resume_workflow(ResumeWorkflow{id, text_message("x"), {"admin_override"}}));

            check(r2.status == workflow_status::routing_failed && r2.failed_executor == "review",
                  "E5 (I3): a label the graph never declared as a case selects no edge -- the run "
                  "reports routing_failed, naming the port as the executor whose routing contract "
                  "failed, rather than completing with an empty result");
            check(*publish_calls == 0 && *discard_calls == 0,
                  "E5 (I3): neither branch ran -- an invented label has no more authority than an "
                  "invented model route would");
        }
    }

    // ---- IQ1: graph.bound.deadline_ms accounts for RUNNING time only -- real wall-clock time ----
    // ---- spent SUSPENDED at a port does not count against the deadline.                       ----
    {
        Workflow wf;
        wf.id        = "iq1-deadline-suspend";
        wf.executors = {node_desc("start"), port_desc("port"), node_desc("sink")};
        wf.edges.push_back(Edge{"start", "port", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"port", "sink", edge_kind::direct, {}});
        wf.start             = "start";
        wf.output_selection  = {"sink"};
        // Short: real running work here is microseconds, generous for the real supersteps, but would
        // be blown many times over by 200ms of counted suspended time.
        wf.bound.deadline_ms = 50;

        std::vector<ExecutorBody> bodies = {appender("start"), {}, appender("sink")};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r1.status == workflow_status::suspended,
              "IQ1 setup: the run reaches the port and suspends before the deadline");
        std::string const id = r1.open_interactions.empty() ? std::string{} : r1.open_interactions.front().interaction_id;

        // The one deliberate real sleep in this file -- there is nothing observable to poll while the
        // run is suspended, so this is the only way to let real wall-clock time pass.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        WorkflowResult r2 = drive(sup.resume_workflow(ResumeWorkflow{id, text_message("resp"), {}}));
        check(r2.status == workflow_status::completed,
              "IQ1: resuming after 200ms of real suspended time (4x the 50ms deadline) still "
              "COMPLETES -- the deadline did not accumulate suspended wall-clock time");
        check(text_of(r2.output) == "resp>sink",
              "IQ1: the human's response routed through to the downstream node normally");
    }

    // ---- IQ2: a request_port inside a CYCLE opens twice across two rounds with DISTINCT,      ----
    // ---- round-qualified ids; the stale first id fails closed once the second has opened.     ----
    //
    //   port <-> after   (cycle: port -> after -> port)
    {
        Workflow wf;
        wf.id        = "iq2-cycle";
        wf.executors = {port_desc("port"), node_desc("after")};
        wf.edges.push_back(Edge{"port", "after", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"after", "port", edge_kind::direct, {}});
        wf.start             = "port";
        wf.bound.max_rounds  = 6;

        std::vector<ExecutorBody> bodies = {{}, appender("after")};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("seed")}));
        check(r1.status == workflow_status::suspended, "IQ2: the run opens the port on its first reach");
        check(r1.open_interactions.size() == 1, "IQ2: exactly one Interaction open at the first opening");
        std::string const first_id = r1.open_interactions.front().interaction_id;
        check(first_id == sup.run_id() + ":port:port:0",
              "IQ2: the first opening's derived id matches <run>:port:<executor>:<round> for round 0");

        WorkflowResult r2 = drive(sup.resume_workflow(ResumeWorkflow{first_id, text_message("answer-1"), {}}));
        check(r2.status == workflow_status::suspended,
              "IQ2: after the cycle runs 'after' and routes back to 'port', the SAME port opens a "
              "SECOND time and the run suspends again");
        check(r2.open_interactions.size() == 1,
              "IQ2: exactly one Interaction open at the second opening -- the first is fully resolved "
              "and gone, not merely superseded");
        std::string const second_id = r2.open_interactions.front().interaction_id;

        check(second_id != first_id,
              "IQ2 -- CORE CLAIM: the two openings of the same port produce DIFFERENT interaction ids");
        check(second_id == sup.run_id() + ":port:port:2",
              "IQ2: the second opening's id embeds the round it actually happened in (round 2, not "
              "round 0 again), the mechanism that makes a cycling port's two requests distinguishable");

        WorkflowResult r3 = drive(sup.resume_workflow(ResumeWorkflow{first_id, text_message("replay"), {}}));
        check(r3.status == workflow_status::invalid,
              "IQ2 -- CORE CLAIM: resuming the STALE first interaction_id again fails closed rather "
              "than silently resolving the second, still-open port");
        check(sup.open_interactions().size() == 1 &&
                  sup.open_interactions().front().interaction_id == second_id,
              "IQ2: the second port is still open and untouched after the stale resume attempt");

        WorkflowResult r4 = drive(sup.resume_workflow(ResumeWorkflow{second_id, text_message("answer-2"), {}}));
        check(r4.status != workflow_status::invalid, "IQ2: resuming the CURRENT (second) id is accepted");
    }

    // ---- IQ3: a routing_failed discovered at RESUME time still preserves WorkflowResult::partial ----
    // ---- from everything that completed before the port was reached.                             ----
    {
        Workflow wf;
        wf.id        = "iq3-routing-failed-preserves-partial";
        wf.executors = {node_desc("start"), port_desc("port"), node_desc("approve"), node_desc("reject")};
        wf.edges.push_back(Edge{"start", "port", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"port", "approve", edge_kind::switch_case, "approve"});
        wf.edges.push_back(Edge{"port", "reject", edge_kind::switch_case, "reject"});
        wf.start             = "start";
        wf.output_selection  = {"start"};  // so §6's "last output-selected payload" has something to
                                            // check survives the routing failure untouched
        wf.bound.max_rounds  = 6;

        std::vector<ExecutorBody> bodies = {appender("start"), {}, appender("approve"), appender("reject")};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r1.status == workflow_status::suspended, "IQ3: the run reaches the switch_case port and suspends");
        std::string const id = r1.open_interactions.empty() ? std::string{} : r1.open_interactions.front().interaction_id;

        WorkflowResult r2 = drive(
            sup.resume_workflow(ResumeWorkflow{id, text_message("unrouted"), {"neither_label"}}));
        check(r2.status == workflow_status::routing_failed,
              "IQ3: a routing answer matching no declared case label ends the run routing_failed");
        check(r2.failed_executor == "port", "IQ3: the result names the PORT as the routing contract that failed");

        agentengine::rt::ExecutorOutput const* p_start = nullptr;
        for (auto const& out : r2.partial) {
            if (out.executor_id == "start") p_start = &out;
        }
        check(p_start != nullptr && text_of(p_start->payload) == "in>start",
              "IQ3 -- CORE CLAIM: the executor that completed BEFORE the port (start) still has its "
              "partial result after a routing failure discovered only at resume time");
        check(text_of(r2.output) == "in>start",
              "IQ3: the last output-selected payload survives the routing failure unchanged -- the "
              "partial-results promise holds across a suspend/resume boundary too");
    }

    // ---- IQ4: a port and a FAILING sibling reached in the SAME fan-out round -- the round's ----
    // ---- failure wins, the port's Interaction is never minted, and its id is named in         ----
    // ---- WorkflowResult::unopened_ports rather than vanishing silently.                       ----
    {
        Workflow wf;
        wf.id        = "iq4-port-vs-sibling-failure";
        wf.executors = {node_desc("start"), port_desc("port"), node_desc("failer"), node_desc("sink")};
        wf.edges.push_back(Edge{"start", "port", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"start", "failer", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"port", "sink", edge_kind::direct, {}});
        wf.start             = "start";
        wf.output_selection  = {"start"};
        wf.bound.max_rounds  = 6;

        auto port_calls = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {
            appender("start"),
            [port_calls](Message const& in,
                         agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                ++*port_calls;
                return ExecutorOutcome{in};
            },
            failing_body(agentengine::failure_class::fatal),
            appender("sink"),
        };
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r.status == workflow_status::executor_failed && r.failed_executor == "failer",
              "IQ4: the sibling's `fail`-policy failure ends the run executor_failed");
        check(r.open_interactions.empty(),
              "IQ4: no Interaction is reported for the port reached this same round");
        check(sup.open_interactions().empty(),
              "IQ4: the live accessor agrees -- no Interaction was minted at all, not merely hidden");
        check(*port_calls == 0, "IQ4: the port's ExecutorBody was still never invoked");

        agentengine::rt::ExecutorOutput const* p_start = nullptr;
        for (auto const& out : r.partial) {
            if (out.executor_id == "start") p_start = &out;
        }
        check(p_start != nullptr && text_of(p_start->payload) == "in>start",
              "IQ4: the round BEFORE the failure (start) still preserves its partial result");

        check(r.unopened_ports.size() == 1 && r.unopened_ports[0] == "port",
              "IQ4 -- CORE CLAIM: the port reached this same round is named in "
              "WorkflowResult::unopened_ports, not silently dropped -- the round-ordering rule is "
              "'ports are opened AFTER the round's own failure check', so a sibling failure this round "
              "means the port's Interaction is never minted at all");
    }

    // ---- IQ5: routes on a port whose outgoing edge is plain `direct` (not switch_case/          ----
    // ---- multi_selection) are IGNORED -- I3: routes only mean something where the graph declared ----
    // ---- they could.                                                                              ----
    {
        Workflow wf;
        wf.id        = "iq5-routes-on-direct-edge";
        wf.executors = {port_desc("port"), node_desc("sink")};
        wf.edges.push_back(Edge{"port", "sink", edge_kind::direct, {}});
        wf.start             = "port";
        wf.output_selection  = {"sink"};
        wf.bound.max_rounds  = 6;

        std::vector<ExecutorBody> bodies = {{}, appender("sink")};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r1.status == workflow_status::suspended, "IQ5: the run suspends at the start port");
        std::string const id = r1.open_interactions.empty() ? std::string{} : r1.open_interactions.front().interaction_id;

        // Spurious routes naming labels the graph never declared (there are no case labels here at
        // all -- the one outgoing edge is plain `direct`).
        WorkflowResult r2 = drive(sup.resume_workflow(
            ResumeWorkflow{id, text_message("payload"), {"admin_override", "sink", "port", "not_a_real_label"}}));
        check(r2.status == workflow_status::completed,
              "IQ5 -- CORE CLAIM: a spurious `routes` vector on a port whose only outgoing edge is "
              "`direct` is harmless -- the edge fires exactly as it always would, routes carries no "
              "authority here");
        check(text_of(r2.output) == "payload>sink",
              "IQ5: the response reached 'sink' via the one declared direct edge, unaffected by the "
              "routes content");
    }

    // ---- IQ6: open_interactions() (live accessor) and WorkflowResult::open_interactions (the ----
    // ---- reply field) stay in agreement through a partial resume and a rejected double-resolve. ----
    {
        Workflow wf;
        wf.id        = "iq6-open-interactions-parity";
        wf.executors = {node_desc("start"), port_desc("portA"), port_desc("portB")};
        wf.edges.push_back(Edge{"start", "portA", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"start", "portB", edge_kind::fan_out, {}});
        wf.start             = "start";
        wf.bound.max_rounds  = 6;

        std::vector<ExecutorBody> bodies = {appender("start"), {}, {}};
        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);

        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(r1.status == workflow_status::suspended && r1.open_interactions.size() == 2,
              "IQ6 setup: both ports are reached in the same round and the run suspends once, with 2 "
              "concurrent Interactions (OQ-4's own case)");
        check(sup.open_interactions().size() == 2,
              "IQ6: the live accessor sees the same two, immediately after run_workflow()'s reply");

        std::string id_a, id_b;
        for (auto const& i : r1.open_interactions) {
            if (i.interaction_id.find(":portA:") != std::string::npos) id_a = i.interaction_id;
            if (i.interaction_id.find(":portB:") != std::string::npos) id_b = i.interaction_id;
        }
        check(!id_a.empty() && !id_b.empty() && id_a != id_b, "IQ6: both ids present and distinct");

        WorkflowResult r2 = drive(sup.resume_workflow(ResumeWorkflow{id_a, text_message("A-answer"), {}}));
        check(r2.status == workflow_status::suspended,
              "IQ6: with B still open, the run stays suspended after a PARTIAL resume");
        check(r2.open_interactions.size() == 1 && r2.open_interactions.front().interaction_id == id_b,
              "IQ6: WorkflowResult after the partial resume reports exactly B");
        check(sup.open_interactions().size() == 1 &&
                  sup.open_interactions().front().interaction_id == id_b,
              "IQ6 -- CORE CLAIM: the live accessor agrees with the reply field after a PARTIAL resume "
              "-- both say 'only B is open'");

        WorkflowResult r3 = drive(sup.resume_workflow(ResumeWorkflow{id_a, text_message("A-again"), {}}));
        check(r3.status == workflow_status::invalid,
              "IQ6: resuming an ALREADY-resolved interaction_id a second time fails closed");
        check(sup.open_interactions().size() == 1 &&
                  sup.open_interactions().front().interaction_id == id_b,
              "IQ6: the rejected double-resolve left the live accessor's view of B untouched");

        WorkflowResult r4 = drive(sup.resume_workflow(ResumeWorkflow{id_b, text_message("B-answer"), {}}));
        check(r4.status != workflow_status::suspended,
              "IQ6: once BOTH ports are resolved the run advances past suspended");
        check(r4.open_interactions.empty(), "IQ6: no open interactions remain in the final reply");
        check(sup.open_interactions().empty(), "IQ6: the live accessor agrees -- empty at the end too");
    }

    if (g_failures == 0) {
        std::printf("test_rt_workflow_supervisor_request_port: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_supervisor_request_port: %d failure(s)\n", g_failures);
    return 1;
}
