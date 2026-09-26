// Proof for ADR-149 (GitHub issue #28 item 3): the typed plan-signoff marshal helpers
// (include/agentengine/workflow/magentic.hpp) -- make_plan_signoff_request()/
// make_plan_signoff_response()/parse_plan_signoff_response().
//   P1 -- make_plan_signoff_request() wraps a MagenticPlanSignoffRequest in a Custom ContentItem
//         under the expected type_id, and its JSON payload round-trips through schema::to_json/
//         schema::from_json directly (the AE_JSON_SCHEMA-generated codec itself is correct).
//   P2 -- parse_plan_signoff_response() correctly reads a MagenticPlanSignoffResponse back out of a
//         Message built by make_plan_signoff_response().
//   P3 -- parse_plan_signoff_response() returns a contract-class error, not a crash or a
//         default-valued response, when no matching Custom item is present.
//   P4 -- END TO END: a require_plan_signoff() port, built by MagenticWorkflowBuilder, suspends the
//         run, and a typed response resumed through the real rt::WorkflowSupervisor::
//         resume_workflow() path is what the manager's NEXT invocation actually receives -- proving
//         the "rides the already-proven request_port/resume_workflow path, no new engine mechanism"
//         claim (ADR-149 §3 finding 3) end to end, not just as marshal-helper unit tests.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/magentic.hpp"

using agentengine::result;
using agentengine::workflow::MagenticGraph;
using agentengine::workflow::MagenticPlanSignoffRequest;
using agentengine::workflow::MagenticPlanSignoffResponse;
using agentengine::workflow::MagenticWorkflowBuilder;
using agentengine::workflow::TypedExecutor;
using agentengine::workflow::make_plan_signoff_request;
using agentengine::workflow::make_plan_signoff_response;
using agentengine::workflow::parse_plan_signoff_response;

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

}  // namespace

// Named and string-declared IDENTICALLY across every file that needs a Magentic test message pair
// -- see test_workflow_magentic_builder.cpp's identical block for the full ODR reasoning (an
// anonymous-namespace fix does not compile for an `AE_WORKFLOW_MESSAGE`-specialized type; identical
// definitions across TUs, unlike differing ones, are not an ODR violation, matching this codebase's
// own pre-existing `Question`/`Draft`/`Verdict` precedent).
struct TaskMsg {};
struct ReportMsg {};
AE_WORKFLOW_MESSAGE(TaskMsg, "AgentEngine.Magentic.TaskMsg");
AE_WORKFLOW_MESSAGE(ReportMsg, "AgentEngine.Magentic.ReportMsg");

int main() {
    // ---- P1: request wrapping + direct schema round-trip ------------------------------------------
    {
        MagenticPlanSignoffRequest req{"Step 1: gather facts. Step 2: write summary."};
        Message m = make_plan_signoff_request(req);
        check(m.content.size() == 1, "P1: exactly one content item");
        auto const* custom = m.content.empty() ? nullptr : std::get_if<agentengine::Custom>(&m.content[0].value);
        check(custom != nullptr, "P1: the item is a Custom content item");
        if (custom != nullptr) {
            check(custom->type_id == "agentengine.workflow.magentic.plan_signoff/1",
                  "P1: tagged with the expected type_id");
        }

        result<agentengine::json::Value> parsed = agentengine::json::parse(custom->payload_json);
        check(parsed.has_value(), "P1: the payload is valid JSON");
        if (parsed) {
            result<MagenticPlanSignoffRequest> decoded =
                agentengine::schema::from_json<MagenticPlanSignoffRequest>(*parsed);
            check(decoded.has_value() && decoded->plan == req.plan,
                  "P1: the AE_JSON_SCHEMA-generated codec round-trips the request's own field");
        }
    }

    // ---- P2: response wrapping + parse_plan_signoff_response() -------------------------------------
    {
        MagenticPlanSignoffResponse resp{true, "Looks good, proceed."};
        Message m = make_plan_signoff_response(resp);
        result<MagenticPlanSignoffResponse> parsed = parse_plan_signoff_response(m);
        check(parsed.has_value(), "P2: parse_plan_signoff_response() succeeds on a matching Message");
        if (parsed) {
            check(parsed->approved == true, "P2: approved round-trips");
            check(parsed->feedback == "Looks good, proceed.", "P2: feedback round-trips");
        }
    }

    // ---- P3: no matching Custom item -> a real, named error, not a default-valued success ---------
    {
        result<MagenticPlanSignoffResponse> parsed = parse_plan_signoff_response(text_message("not a signoff"));
        check(!parsed.has_value(), "P3: an unrelated Message is rejected, not silently defaulted");
        if (!parsed) {
            check(parsed.error().code == "workflow.magentic.plan_signoff.missing", "P3: the specific error code");
        }
    }

    // ---- P4: end to end through a real request_port suspend/resume cycle --------------------------
    {
        MagenticWorkflowBuilder<TaskMsg, ReportMsg> b("plan-signoff-e2e");
        b.manager(TypedExecutor<ReportMsg, TaskMsg>{.id = "mgr", .capability_ceiling = {}});
        b.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "p1", .capability_ceiling = {}});
        b.require_plan_signoff("plan_review");
        b.max_rounds(20);
        result<MagenticGraph> built = b.build();
        check(built.has_value(), "P4 setup: the builder builds");
        if (!built) {
            if (g_failures == 0) { std::printf("test_workflow_magentic_plan_signoff: ALL PASS\n"); return 0; }
            std::fprintf(stderr, "test_workflow_magentic_plan_signoff: %d failure(s)\n", g_failures);
            return 1;
        }

        // mgr: round 0 routes to plan_review carrying a typed request; round 1 (after resume) reads
        // the typed response back out of what the port routed back to it, and finishes via "done".
        std::vector<ExecutorBody> bodies = {
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                result<MagenticPlanSignoffResponse> resp = parse_plan_signoff_response(in);
                if (resp.has_value()) {
                    // Round 1+: the port's resolved response came back. Finish, carrying whether it
                    // was approved so the test can assert on it via r.output.
                    return ExecutorOutcome{text_message(resp->approved ? "approved" : "rejected"), {"done"}};
                }
                // Round 0: no signoff response yet -- send the plan for review.
                Message req = make_plan_signoff_request(MagenticPlanSignoffRequest{"do the thing"});
                return ExecutorOutcome{req, {"plan_review"}};
            },
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{in};  // p1 is never reached in this scenario
            },
            [](Message const& in, agentengine::EffectContext&) -> agentengine::result<ExecutorOutcome> {
                return ExecutorOutcome{in};  // done sink
            },
            {},  // plan_review is a request_port -- never dispatched to a body
        };

        WorkflowSupervisor sup;
        sup.initialize(built->graph, bodies, {}, built->manager_id);
        WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("start")}));
        check(r1.status == workflow_status::suspended, "P4: the run suspends at the plan_review port");
        check(r1.open_interactions.size() == 1, "P4: exactly one open interaction");

        if (r1.open_interactions.size() == 1) {
            Message resp_msg = make_plan_signoff_response(MagenticPlanSignoffResponse{true, "go ahead"});
            WorkflowResult r2 = drive(sup.resume_workflow(
                ResumeWorkflow{r1.open_interactions[0].interaction_id, resp_msg, {}}));

            check(r2.status == workflow_status::completed,
                  "P4: resuming with a typed response drives the run to completion");
            check(text_of(r2.output) == "approved",
                  "P4: the manager's NEXT invocation actually received the typed response the test "
                  "resumed with -- the real request_port/resume_workflow path, not a mock");
        }
    }

    if (g_failures == 0) {
        std::printf("test_workflow_magentic_plan_signoff: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_magentic_plan_signoff: %d failure(s)\n", g_failures);
    return 1;
}
