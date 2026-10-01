// Issue #45: review points (014 §4 "Review points", include/agentengine/workflow/review_points.hpp) -- MAF's
// SequentialBuilder/ConcurrentBuilder `.with_request_info()` as a builder-level expansion into ordinary
// request_port nodes and edges.
//
//   G1-G3 -- the builders produce EXACTLY the hand-wired graph (`Workflow ==`), and it validates:
//            G1 sequential, every step, with revision; G2 sequential, one selected step, approve-only;
//            G3 concurrent, selected branches before the fan_in aggregator.
//   G4    -- a step's own failure policy moves onto step -> review; the port's edges get the default.
//   Y1-Y2 -- I6: a YAML document with `review_points` compiles to the same graph the C++ builders build,
//            and a malformed entry is refused.
//   R1-R8 -- every refusal the header documents (unknown step, review of a port, routing source, id taken,
//            ambiguous fallback, revision on a type-changing step / on a fan_in step / on a last step,
//            unknown `only` id).
//   E1    -- end to end through rt::WorkflowSupervisor, sequential: the run suspends at each review point
//            with exactly one interaction; approve passes the step's output on; "revise" re-runs the step
//            and reopens the review under a new id; a route the last (approve-only) point does not declare
//            is refused and the port stays open; an amended answer becomes the run's output.
//   E2    -- end to end, concurrent: one interaction per SELECTED branch, distinct ids; the unselected
//            branch runs without pausing; the aggregator waits for every review and runs exactly once.
//   E3    -- positive control for E1/E2: the same graphs WITHOUT with_request_info() complete without
//            suspending -- the pauses come from the review points and nothing else.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/yaml_value.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/review_points.hpp"
#include "agentengine/workflow/yaml_compiler.hpp"

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

struct Draft {};
struct Brief {};
struct Finding {};
struct Report {};

}  // namespace

AE_WORKFLOW_MESSAGE(Draft, "Test.Draft");
AE_WORKFLOW_MESSAGE(Brief, "Test.Brief");
AE_WORKFLOW_MESSAGE(Finding, "Test.Finding");
AE_WORKFLOW_MESSAGE(Report, "Test.Report");

namespace {

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

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

[[nodiscard]] ExecutorBody counting_appender(std::string name, std::shared_ptr<int> calls) {
    return [name = std::move(name), calls](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
        ++*calls;
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

[[nodiscard]] Executor node(char const* id, char const* in, char const* out) {
    return Executor{.id = id, .kind = executor_kind::function, .input_type = in, .output_type = out,
                    .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}
[[nodiscard]] Executor port(char const* id, char const* type) {
    return Executor{.id = id, .kind = executor_kind::request_port, .input_type = type, .output_type = type,
                    .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

using SeqStep = TypedExecutor<Draft, Draft>;
SeqStep const kOutline{.id = "outline", .capability_ceiling = {}};
SeqStep const kWrite{.id = "write", .capability_ceiling = {}};
SeqStep const kEdit{.id = "edit", .capability_ceiling = {}};

[[nodiscard]] SequentialWorkflowBuilder<Draft> sequential() {
    SequentialWorkflowBuilder<Draft> b("seq");
    b.participant(kOutline).participant(kWrite).participant(kEdit).max_rounds(32);
    return b;
}

TypedExecutor<Brief, Brief> const   kDispatch{.id = "dispatch", .capability_ceiling = {}};
TypedExecutor<Brief, Finding> const kA{.id = "a", .capability_ceiling = {}};
TypedExecutor<Brief, Finding> const kB{.id = "b", .capability_ceiling = {}};
TypedExecutor<Brief, Finding> const kC{.id = "c", .capability_ceiling = {}};
TypedExecutor<Finding, Report> const kMerge{.id = "merge", .capability_ceiling = {}};

[[nodiscard]] ConcurrentWorkflowBuilder<Brief, Finding, Report> concurrent() {
    ConcurrentWorkflowBuilder<Brief, Finding, Report> b("conc");
    b.dispatcher(kDispatch).participant(kA).participant(kB).participant(kC).aggregator(kMerge).max_rounds(16);
    return b;
}

// The hand-wired versions -- written out the way an author would, following the header's documented rule.
[[nodiscard]] Workflow hand_sequential_all_revisable() {
    Workflow wf;
    wf.id        = "seq";
    wf.executors = {node("outline", "Test.Draft", "Test.Draft"), node("write", "Test.Draft", "Test.Draft"),
                    node("edit", "Test.Draft", "Test.Draft"), port("outline.review", "Test.Draft"),
                    port("write.review", "Test.Draft"), port("edit.review", "Test.Draft")};
    wf.edges = {
        Edge{"outline", "outline.review", edge_kind::direct, {}, {}},
        Edge{"outline.review", "write", edge_kind::switch_default, {}, {}},
        Edge{"outline.review", "outline", edge_kind::switch_case, "revise", {}},
        Edge{"write", "write.review", edge_kind::direct, {}, {}},
        Edge{"write.review", "edit", edge_kind::switch_default, {}, {}},
        Edge{"write.review", "write", edge_kind::switch_case, "revise", {}},
        Edge{"edit", "edit.review", edge_kind::direct, {}, {}},
    };
    wf.start            = "outline";
    wf.output_selection = {"edit.review"};
    wf.bound.max_rounds = 32;
    return wf;
}

[[nodiscard]] Workflow hand_concurrent_a_c() {
    Workflow wf;
    wf.id        = "conc";
    wf.executors = {node("dispatch", "Test.Brief", "Test.Brief"), node("a", "Test.Brief", "Test.Finding"),
                    node("b", "Test.Brief", "Test.Finding"),      node("c", "Test.Brief", "Test.Finding"),
                    node("merge", "Test.Finding", "Test.Report"), port("a.review", "Test.Finding"),
                    port("c.review", "Test.Finding")};
    wf.edges = {
        Edge{"dispatch", "a", edge_kind::fan_out, {}, {}},
        Edge{"dispatch", "b", edge_kind::fan_out, {}, {}},
        Edge{"dispatch", "c", edge_kind::fan_out, {}, {}},
        Edge{"a", "a.review", edge_kind::direct, {}, {}},
        Edge{"a.review", "merge", edge_kind::fan_in, {}, {}},
        Edge{"b", "merge", edge_kind::fan_in, {}, {}},
        Edge{"c", "c.review", edge_kind::direct, {}, {}},
        Edge{"c.review", "merge", edge_kind::fan_in, {}, {}},
    };
    wf.start            = "dispatch";
    wf.output_selection = {"merge"};
    wf.bound.max_rounds = 16;
    return wf;
}

[[nodiscard]] std::string id_for(WorkflowResult const& r, std::string const& port_id) {
    std::string const needle = ":port:" + port_id + ":";
    for (auto const& i : r.open_interactions) {
        if (i.interaction_id.find(needle) != std::string::npos) return i.interaction_id;
    }
    return {};
}

[[nodiscard]] Message ask_for(WorkflowSupervisor const& sup, std::string const& interaction_id) {
    for (auto const& a : sup.open_interaction_asks()) {
        if (a.interaction.interaction_id == interaction_id) return a.ask;
    }
    return {};
}

[[nodiscard]] bool refused(result<Workflow> const& r, char const* code) {
    return !r.has_value() && r.error().code == code;
}

}  // namespace

int main() {
    // ---- G1: sequential, every step, with revision == the hand-wired graph ----
    {
        auto built = sequential().with_request_info({.only = {}, .allow_revision = true}).build();
        check(built.has_value(), "G1: the sequential builder with request info builds (validate_workflow passes)");
        Workflow const hand = hand_sequential_all_revisable();
        check(validate_workflow(hand).has_value(), "G1: the hand-wired graph validates on its own");
        check(built.has_value() && *built == hand,
              "G1: the builder's graph is EXACTLY the hand-wired one (executors, edges, start, output, bound)");
    }

    // ---- G2: sequential, only 'write', approve-only == hand-wired ----
    {
        auto built = sequential().with_request_info({.only = {"write"}, .allow_revision = false}).build();
        Workflow hand;
        hand.id        = "seq";
        hand.executors = {node("outline", "Test.Draft", "Test.Draft"), node("write", "Test.Draft", "Test.Draft"),
                          node("edit", "Test.Draft", "Test.Draft"), port("write.review", "Test.Draft")};
        hand.edges = {
            Edge{"outline", "write", edge_kind::chain, {}, {}},
            Edge{"write", "write.review", edge_kind::direct, {}, {}},
            Edge{"write.review", "edit", edge_kind::chain, {}, {}},
        };
        hand.start            = "outline";
        hand.output_selection = {"edit"};
        hand.bound.max_rounds = 32;
        check(built.has_value() && *built == hand && validate_workflow(hand).has_value(),
              "G2: one selected step gets one approve-only review point; the chain edge after it keeps its kind");
    }

    // ---- G3: concurrent, branches a and c reviewed before aggregation == hand-wired ----
    {
        auto built = concurrent().with_request_info({"a", "c"}).build();
        Workflow const hand = hand_concurrent_a_c();
        check(validate_workflow(hand).has_value(), "G3: the hand-wired concurrent graph validates");
        check(built.has_value() && *built == hand,
              "G3: the concurrent builder's graph is EXACTLY the hand-wired one (review ports in front of fan_in)");
    }

    // ---- G4: the step's own failure policy moves onto step -> review ----
    {
        Workflow wf;
        wf.id        = "g4";
        wf.executors = {node("s", "Test.Draft", "Test.Draft"), node("t", "Test.Draft", "Test.Draft")};
        wf.edges     = {Edge{"s", "t", edge_kind::direct, {}, EdgeFailurePolicy{edge_failure_policy::retry, 2, {}}}};
        wf.start     = "s";
        wf.bound.max_rounds = 4;
        auto out = insert_review_points(wf, {ReviewPoint{"s", false}});
        check(out.has_value() && out->edges.size() == 2 && out->edges[0].from == "s" &&
                  out->edges[0].to == "s.review" && out->edges[0].on_failure.kind == edge_failure_policy::retry &&
                  out->edges[0].on_failure.attempts == 2 && out->edges[1].from == "s.review" &&
                  out->edges[1].on_failure == EdgeFailurePolicy{},
              "G4: s -> s.review carries s's retry policy; s.review -> t has the default policy");
        check(out.has_value() && validate_workflow(*out).has_value(), "G4: the result validates");
    }

    // ---- Y1: I6 -- the YAML form expands to the same graphs ----
    {
        std::string const seq_doc = R"YAML(apiVersion: agentengine.dev/v1
kind: Workflow
metadata: { id: seq }
spec:
  start: outline
  executors:
    - { id: outline, input_type: Test.Draft, output_type: Test.Draft }
    - { id: write,   input_type: Test.Draft, output_type: Test.Draft }
    - { id: edit,    input_type: Test.Draft, output_type: Test.Draft }
  edges:
    - { from: outline, to: write }
    - { from: write,   to: edit }
  limits: { max_rounds: 32 }
  output_from: edit
  review_points:
    - { after: outline, revisable: true }
    - { after: write, revisable: true }
    - edit
)YAML";
        auto parsed = yaml::parse(seq_doc);
        check(parsed.has_value(), "Y1: the sequential review document parses");
        if (parsed.has_value()) {
            auto compiled = compile_workflow_document(*parsed);
            // The YAML compiler has no `chain` form (a `to` edge is `direct`), and the revisable points turn
            // both forward edges into switch_default anyway, so this is exactly G1's graph.
            check(compiled.has_value() && *compiled == hand_sequential_all_revisable() &&
                      validate_workflow(*compiled).has_value(),
                  "Y1: YAML review_points expand to EXACTLY the C++ builder's / hand-wired sequential graph");
        }

        std::string const conc_doc = R"YAML(apiVersion: agentengine.dev/v1
kind: Workflow
metadata: { id: conc }
spec:
  start: dispatch
  executors:
    - { id: dispatch, input_type: Test.Brief,   output_type: Test.Brief }
    - { id: a,        input_type: Test.Brief,   output_type: Test.Finding }
    - { id: b,        input_type: Test.Brief,   output_type: Test.Finding }
    - { id: c,        input_type: Test.Brief,   output_type: Test.Finding }
    - { id: merge,    input_type: Test.Finding, output_type: Test.Report }
  edges:
    - { from: dispatch, fan_out_to: [a, b, c] }
    - { from: a, fan_in_to: merge }
    - { from: b, fan_in_to: merge }
    - { from: c, fan_in_to: merge }
  limits: { max_rounds: 16 }
  output_from: merge
  review_points: [a, c]
)YAML";
        auto cparsed = yaml::parse(conc_doc);
        check(cparsed.has_value(), "Y1: the concurrent review document parses");
        if (cparsed.has_value()) {
            auto compiled = compile_workflow_document(*cparsed);
            check(compiled.has_value() && *compiled == hand_concurrent_a_c(),
                  "Y1: YAML review_points expand to EXACTLY the C++ builder's / hand-wired concurrent graph");
        }
    }

    // ---- Y2: a malformed review_points entry is refused, never ignored ----
    {
        std::string const doc = R"YAML(apiVersion: agentengine.dev/v1
kind: Workflow
metadata: { id: y2 }
spec:
  start: s
  executors:
    - { id: s, input_type: T, output_type: T }
  limits: { max_rounds: 4 }
  review_points:
    - { revisable: true }
)YAML";
        auto parsed = yaml::parse(doc);
        auto compiled = parsed.has_value() ? compile_workflow_document(*parsed) : result<Workflow>{};
        check(parsed.has_value() && refused(compiled, "yaml_compiler.bad_review_point"),
              "Y2: a review_points entry with no 'after' is refused (yaml_compiler.bad_review_point)");
    }

    // ---- R1-R8: refusals ----
    {
        Workflow base = hand_concurrent_a_c();
        check(refused(insert_review_points(base, {ReviewPoint{"nope", false}}), "workflow.review_unknown_step"),
              "R1: a review point after an undeclared executor is refused");
        check(refused(insert_review_points(base, {ReviewPoint{"a.review", false}}),
                      "workflow.review_after_request_port"),
              "R2: a review point after a request port is refused");
        check(refused(insert_review_points(base, {ReviewPoint{"a", false}}), "workflow.review_port_id_taken"),
              "R4: a review port id that already exists (the same step selected twice) is refused");

        Workflow routing;
        routing.id        = "r3";
        routing.executors = {node("s", "T", "T"), node("x", "T", "T"), node("y", "T", "T")};
        routing.edges     = {Edge{"s", "x", edge_kind::switch_case, "left", {}},
                             Edge{"s", "y", edge_kind::switch_default, {}, {}}};
        routing.start     = "s";
        routing.bound.max_rounds = 4;
        check(refused(insert_review_points(routing, {ReviewPoint{"s", false}}), "workflow.review_after_routing_source"),
              "R3: a review point after a step whose edges route (switch) is refused");

        Workflow fb;
        fb.id        = "r5";
        fb.executors = {node("s", "T", "T"), node("x", "T", "T"), node("y", "T", "T"), node("f1", "T", "T"),
                        node("f2", "T", "T")};
        fb.edges     = {Edge{"s", "x", edge_kind::fan_out, {}, EdgeFailurePolicy{edge_failure_policy::fallback, 0, "f1"}},
                        Edge{"s", "y", edge_kind::fan_out, {}, EdgeFailurePolicy{edge_failure_policy::fallback, 0, "f2"}}};
        fb.start     = "s";
        fb.bound.max_rounds = 4;
        check(refused(insert_review_points(fb, {ReviewPoint{"s", false}}), "workflow.review_ambiguous_fallback"),
              "R5: two different fallback targets cannot fold onto the one step -> review edge");

        Workflow typed;
        typed.id        = "r6";
        typed.executors = {node("s", "In", "Out"), node("t", "Out", "Out")};
        typed.edges     = {Edge{"s", "t", edge_kind::direct, {}, {}}};
        typed.start     = "s";
        typed.bound.max_rounds = 4;
        check(refused(insert_review_points(typed, {ReviewPoint{"s", true}}), "workflow.review_revision_type_mismatch"),
              "R6: revision on a step whose input and output types differ is refused");

        // A T->T step feeding a fan_in, and a T->T last step: both type-correct, both still refused.
        Workflow fanin;
        fanin.id        = "r7";
        fanin.executors = {node("dispatch", "T", "T"), node("a", "T", "T"), node("merge", "T", "T")};
        fanin.edges     = {Edge{"dispatch", "a", edge_kind::fan_out, {}, {}}, Edge{"a", "merge", edge_kind::fan_in, {}, {}}};
        fanin.start     = "dispatch";
        fanin.bound.max_rounds = 4;
        check(refused(insert_review_points(fanin, {ReviewPoint{"a", true}}),
                      "workflow.review_revision_needs_single_successor"),
              "R7: revision on a step that feeds a fan_in is refused (a fan_in edge cannot be the approve default)");
        check(refused(insert_review_points(fanin, {ReviewPoint{"merge", true}}),
                      "workflow.review_revision_needs_single_successor"),
              "R7: revision on a last step (no successor) is refused");
        check(insert_review_points(fanin, {ReviewPoint{"a", false}, ReviewPoint{"merge", false}}).has_value(),
              "R7: the same two steps accept approve-only review points");

        check(!sequential().with_request_info({.only = {"ghost"}, .allow_revision = false}).build().has_value() &&
                  sequential().with_request_info({.only = {"ghost"}, .allow_revision = false}).build().error().code ==
                      "workflow.review_unknown_step",
              "R8: with_request_info naming a non-participant is refused");
    }

    // ---- E1: sequential, end to end through rt::WorkflowSupervisor ----
    {
        auto built = sequential().with_request_info({.only = {}, .allow_revision = true}).build();
        check(built.has_value(), "E1 setup: builds");
        auto c_outline = std::make_shared<int>(0), c_write = std::make_shared<int>(0), c_edit = std::make_shared<int>(0);
        // Participants in order, then one empty body per review port.
        std::vector<ExecutorBody> bodies = {counting_appender("outline", c_outline), counting_appender("write", c_write),
                                            counting_appender("edit", c_edit), {}, {}, {}};
        WorkflowSupervisor sup;
        sup.initialize(*built, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("topic")}));
        std::string const id1 = id_for(r, "outline.review");
        check(r.status == workflow_status::suspended && r.open_interactions.size() == 1 && !id1.empty(),
              "E1: the run suspends after step 1 with exactly one interaction, at outline.review");
        check(*c_outline == 1 && *c_write == 0, "E1: step 2 has not run while step 1 is under review");
        check(all_text_of(ask_for(sup, id1)) == "topic>outline", "E1: the ask is step 1's output");

        // Approve: answer with the ask, no routes.
        r = drive(sup.resume_workflow(ResumeWorkflow{id1, ask_for(sup, id1), {}}));
        std::string const id2 = id_for(r, "write.review");
        check(r.status == workflow_status::suspended && r.open_interactions.size() == 1 && !id2.empty() && *c_write == 1,
              "E1: approving passes step 1's output on; the run suspends again, once, at write.review");

        // Revise: the reviewer's message goes back to 'write', which runs again; the review reopens.
        r = drive(sup.resume_workflow(ResumeWorkflow{id2, text_message("make it shorter"), {"revise"}}));
        std::string const id2b = id_for(r, "write.review");
        check(r.status == workflow_status::suspended && r.open_interactions.size() == 1 && *c_write == 2 &&
                  *c_edit == 0 && !id2b.empty() && id2b != id2,
              "E1: 'revise' re-runs step 2 (not step 3) and reopens write.review under a NEW interaction id");
        check(all_text_of(ask_for(sup, id2b)) == "make it shorter>write",
              "E1: the revised step saw the reviewer's message as its input");

        r = drive(sup.resume_workflow(ResumeWorkflow{id2b, ask_for(sup, id2b), {}}));
        std::string const id3 = id_for(r, "edit.review");
        check(r.status == workflow_status::suspended && r.open_interactions.size() == 1 && !id3.empty() && *c_edit == 1,
              "E1: approving the revision moves on to step 3, which suspends at edit.review");

        // The last review point is approve-only: "revise" is not declared there and is refused, port kept.
        WorkflowResult bad = drive(sup.resume_workflow(ResumeWorkflow{id3, text_message("x"), {"revise"}}));
        check(bad.status == workflow_status::invalid_routes && sup.open_interactions().size() == 1 &&
                  sup.open_interactions().front().interaction_id == id3,
              "E1: 'revise' on the approve-only last point is refused (invalid_routes) and the port stays open");

        r = drive(sup.resume_workflow(ResumeWorkflow{id3, text_message("FINAL (amended by reviewer)"), {}}));
        check(r.status == workflow_status::completed, "E1: answering the last review completes the run");
        check(all_text_of(r.output) == "FINAL (amended by reviewer)",
              "E1: the reviewed (amended) message is the run's output -- output selection names edit.review");
        check(*c_outline == 1 && *c_write == 2 && *c_edit == 1, "E1: each step ran exactly as often as reviewed");
    }

    // ---- E2: concurrent, end to end ----
    {
        auto built = concurrent().with_request_info({"a", "c"}).build();
        check(built.has_value(), "E2 setup: builds");
        auto cd = std::make_shared<int>(0), ca = std::make_shared<int>(0), cb = std::make_shared<int>(0),
             cc = std::make_shared<int>(0), cm = std::make_shared<int>(0);
        std::vector<ExecutorBody> bodies = {counting_appender("dispatch", cd), counting_appender("a", ca),
                                            counting_appender("b", cb),        counting_appender("c", cc),
                                            counting_appender("merge", cm),    {},
                                            {}};
        WorkflowSupervisor sup;
        sup.initialize(*built, bodies);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("q")}));
        std::string const ida = id_for(r, "a.review");
        std::string const idc = id_for(r, "c.review");
        check(r.status == workflow_status::suspended && r.open_interactions.size() == 2 && !ida.empty() &&
                  !idc.empty() && ida != idc,
              "E2: one interaction per SELECTED branch (a, c), with distinct ids");
        check(id_for(r, "b.review").empty() && *cb == 1, "E2: the unselected branch b ran and did not pause");
        check(*cm == 0, "E2: the aggregator has not run while reviews are open");

        r = drive(sup.resume_workflow(ResumeWorkflow{ida, ask_for(sup, ida), {}}));
        check(r.status == workflow_status::suspended && r.open_interactions.size() == 1 &&
                  r.open_interactions.front().interaction_id == idc && *cm == 0,
              "E2: answering a's review leaves c's open, and the aggregator still waits");

        r = drive(sup.resume_workflow(ResumeWorkflow{idc, text_message("q>dispatch>c (checked)"), {}}));
        check(r.status == workflow_status::completed && *cm == 1,
              "E2: answering the last review completes the run; the aggregator ran exactly once");
        std::string const out = all_text_of(r.output);
        check(out.find("q>dispatch>a") != std::string::npos && out.find("q>dispatch>b") != std::string::npos &&
                  out.find("q>dispatch>c (checked)") != std::string::npos && out.find(">merge") != std::string::npos,
              "E2: the aggregate has all three branches, the reviewed ones as the reviewers answered");
    }

    // ---- E3: positive control -- no review points, no suspension ----
    {
        auto seq = sequential().build();
        auto c1 = std::make_shared<int>(0);
        WorkflowSupervisor s1;
        s1.initialize(*seq, {counting_appender("outline", c1), counting_appender("write", c1),
                             counting_appender("edit", c1)});
        WorkflowResult r1 = drive(s1.run_workflow(RunWorkflow{text_message("topic")}));
        check(r1.status == workflow_status::completed && r1.open_interactions.empty() &&
                  all_text_of(r1.output) == "topic>outline>write>edit",
              "E3: the same sequential graph without with_request_info() completes without suspending");

        auto conc = concurrent().build();
        auto c2   = std::make_shared<int>(0);
        WorkflowSupervisor s2;
        s2.initialize(*conc, {counting_appender("dispatch", c2), counting_appender("a", c2), counting_appender("b", c2),
                              counting_appender("c", c2), counting_appender("merge", c2)});
        WorkflowResult r2 = drive(s2.run_workflow(RunWorkflow{text_message("q")}));
        check(r2.status == workflow_status::completed && r2.open_interactions.empty(),
              "E3: the same concurrent graph without with_request_info() completes without suspending");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
}
