// AgentEngine "get started" examples, 23 -- a mesh-topology handoff, where control transfers
// BETWEEN peer specialists (not just outward from one fixed router).
//
// Mirrors MAF's samples/03-workflows/orchestrations/handoff_simple.py / handoff_autonomous.py:
// "a handoff workflow ... assembles agents in a mesh topology, allowing them to transfer control to
// each other based on the conversation context." 10_conditional_routing.cpp's own top comment already
// notes the Router and Handoff patterns share one graph shape (`edge_kind::switch_case`) -- but 10
// only ever routes OUT of a single fixed triage node, once. This example is the genuinely mesh-shaped
// case that comment leaves unbuilt: billing and tech can EACH hand off to the other when triage
// guessed wrong, and tech can escalate to a human via an ordinary `request_port` node -- still no new
// engine mechanism, just more `switch_case` edges between more nodes, cyclic exactly like
// 13_reflection_loop.cpp already proves cycles are fine.
//
// Four scenarios below prove: (A) triage's first guess resolves with no handoff at all; (B) billing
// hands off to tech when the real issue turns out technical; (D) tech hands off to billing when the
// real issue turns out to be billing -- the REVERSE direction, proving this is a real mesh and not
// two one-way routers glued together; (C) tech escalates to a human via `request_port`, suspending
// and resuming exactly like 05_human_approval.cpp/20_workflow_checkpoint_resume.cpp's own mechanism.
//
// Run: ./agentengine_example_23_handoff_mesh

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
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

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

// A classifier node, same shape as 10_conditional_routing.cpp's triage -- an ordinary function of
// its input text. A model-backed intake agent would fill this same slot.
[[nodiscard]] ExecutorBody triage() {
    return [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const text  = text_of(in);
        std::string const label = text.find("invoice") != std::string::npos ? "billing" : "tech";
        return ExecutorOutcome{text_message(text), {label}};
    };
}

// billing: resolves directly, UNLESS the ticket is actually a technical issue in disguise (triage
// guessed wrong) -- then it hands off to tech with a fresh message that drops the word that would
// bounce it straight back (a real specialist rewriting the ticket for its peer, not forwarding the
// same text verbatim).
[[nodiscard]] ExecutorBody billing() {
    return [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const text = text_of(in);
        if (text.find("crash") != std::string::npos) {
            return ExecutorOutcome{text_message("[billing->tech handoff] please look into this outage"),
                                    {"tech"}};
        }
        return ExecutorOutcome{text_message("[resolved by billing] " + text), {"done"}};
    };
}

// tech: resolves directly, hands off to billing when the ticket is really a billing matter, or
// escalates to a human via the `escalate` request_port when it names one explicitly.
[[nodiscard]] ExecutorBody tech() {
    return [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const text = text_of(in);
        if (text.find("invoice") != std::string::npos || text.find("charge") != std::string::npos) {
            return ExecutorOutcome{text_message("[tech->billing handoff] please check this charge"),
                                    {"billing"}};
        }
        if (text.find("escalate") != std::string::npos || text.find("human") != std::string::npos) {
            return ExecutorOutcome{text_message(text), {"escalate"}};
        }
        return ExecutorOutcome{text_message("[resolved by tech] " + text), {"done"}};
    };
}

[[nodiscard]] Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id = id, .kind = kind, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

[[nodiscard]] Workflow make_graph() {
    Workflow wf;
    wf.id        = "handoff-mesh";
    wf.executors = {node_desc("triage"), node_desc("billing"), node_desc("tech"),
                     node_desc("escalate", executor_kind::request_port), node_desc("done")};
    wf.edges.push_back(Edge{"triage", "billing", edge_kind::switch_case, "billing"});
    wf.edges.push_back(Edge{"triage", "tech", edge_kind::switch_case, "tech"});
    wf.edges.push_back(Edge{"billing", "tech", edge_kind::switch_case, "tech"});
    wf.edges.push_back(Edge{"billing", "done", edge_kind::switch_case, "done"});
    wf.edges.push_back(Edge{"tech", "billing", edge_kind::switch_case, "billing"});
    wf.edges.push_back(Edge{"tech", "escalate", edge_kind::switch_case, "escalate"});
    wf.edges.push_back(Edge{"tech", "done", edge_kind::switch_case, "done"});
    wf.edges.push_back(Edge{"escalate", "done", edge_kind::direct, {}});
    wf.start = "triage";
    wf.output_selection.push_back("done");
    wf.bound.max_rounds = 10;
    return wf;
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] std::vector<ExecutorBody> make_bodies() {
    // Parallel to `wf.executors` by index: triage, billing, tech, escalate (no body -- request_port),
    // done (identity sink).
    return {triage(), billing(), tech(), {},
            [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{in};
            }};
}

}  // namespace

int main() {
    Workflow const wf = make_graph();
    check(validate_workflow(wf).has_value(), "the mesh graph validates");

    // ---- A: triage's first guess resolves with no handoff -----------------------------------------
    {
        WorkflowSupervisor sup;
        sup.initialize(wf, make_bodies());
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("my invoice is wrong")}));
        check(r.status == workflow_status::completed, "A: the run completes");
        std::printf("[A] %s\n", text_of(r.output).c_str());
        check(text_of(r.output).find("[resolved by billing]") != std::string::npos,
              "A: billing resolved it directly -- no handoff needed");
    }

    // ---- B: billing hands off to tech (triage guessed billing, it was really technical) -----------
    {
        WorkflowSupervisor sup;
        sup.initialize(wf, make_bodies());
        WorkflowResult r = drive(sup.run_workflow(
            RunWorkflow{text_message("my invoice needs technical help, the app crashed")}));
        check(r.status == workflow_status::completed, "B: the run completes");
        std::printf("[B] %s\n", text_of(r.output).c_str());
        check(text_of(r.output).find("[resolved by tech] [billing->tech handoff]") != std::string::npos,
              "B: billing genuinely handed off to tech -- both stages ran, in that order");
    }

    // ---- D: tech hands off to billing (triage guessed tech, it was really billing) -- the REVERSE
    //         direction, proving this is a real mesh and not two one-way routers ---------------------
    {
        WorkflowSupervisor sup;
        sup.initialize(wf, make_bodies());
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("why was my card charged twice")}));
        check(r.status == workflow_status::completed, "D: the run completes");
        std::printf("[D] %s\n", text_of(r.output).c_str());
        check(text_of(r.output).find("[resolved by billing] [tech->billing handoff]") != std::string::npos,
              "D: tech genuinely handed off to billing -- the reverse direction from B, same mesh");
    }

    // ---- C: tech escalates to a human via `request_port`, suspending and resuming -----------------
    {
        WorkflowSupervisor sup;
        sup.initialize(wf, make_bodies());
        WorkflowResult r1 = drive(
            sup.run_workflow(RunWorkflow{text_message("please escalate this outage to a human")}));
        check(r1.status == workflow_status::suspended, "C: the run suspends at the escalate port");
        check(r1.open_interactions.size() == 1, "C: exactly one open interaction");

        if (r1.open_interactions.size() == 1) {
            WorkflowResult r2 = drive(sup.resume_workflow(ResumeWorkflow{
                r1.open_interactions[0].interaction_id, text_message("human: restarted the service, fixed"),
                {}}));
            check(r2.status == workflow_status::completed, "C: resuming with the human's answer completes");
            std::printf("[C] %s\n", text_of(r2.output).c_str());
            check(text_of(r2.output) == "human: restarted the service, fixed",
                  "C: the completed run reflects the human's own answer, not anything tech said");
        }
    }

    std::fprintf(stderr, g_failures == 0 ? "example_23_handoff_mesh: OK\n" : "example_23_handoff_mesh: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
