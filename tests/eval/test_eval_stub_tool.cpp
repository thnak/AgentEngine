// Implements decisions/ADR-195-evaluation-harness.md §3.9's stub-tool requirement: a host-authored
// tool descriptor that records arguments and causes no effect, following
// MemoryProvider::make_recall_tool_descriptor's own pattern.

#include <iostream>
#include <string>
#include <vector>

#include "agentengine/eval/eval_stub_tool.hpp"
#include "../support/run_task_sync.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    namespace ev = ae::eval;

    ev::StubToolFixture fixture;
    fixture.name              = "set_deploy_region";
    fixture.description       = "Sets the deployment region.";
    fixture.args_schema_json  = R"({"type":"object","properties":{"region":{"type":"string"}}})";
    fixture.reply_schema_json = R"({"type":"object","properties":{"ok":{"type":"boolean"}}})";
    fixture.canned_reply =
        ae::json::Value::make_object({{"ok", ae::json::Value::make_bool(true)}});

    std::vector<ev::CapturedCall> sink;
    ae::ToolDescriptor descriptor = ev::make_stub_tool_descriptor(fixture, sink);

    AE_CHECK(descriptor.name == "set_deploy_region", "the descriptor carries the fixture's name");
    AE_CHECK(descriptor.capability_ceiling.empty(),
             "a stub tool's capability ceiling is empty -- it needs no capability");

    // ---- invoking the descriptor records the call and returns the fixed reply, regardless of args
    ae::EffectContext ctx{};
    auto args1 = ae::json::Value::make_object({{"region", ae::json::Value::make_string("eu-west-1")}});
    auto reply1 = descriptor.invoke(args1, ctx);
    AE_CHECK(reply1.has_value(), "invoking the stub tool succeeds");
    AE_CHECK(sink.size() == 1, "exactly one call landed in the sink");
    if (!sink.empty()) {
        AE_CHECK(sink[0].tool_name == "set_deploy_region", "the captured call names the right tool");
        AE_CHECK(sink[0].arguments.find("region") != nullptr &&
                     sink[0].arguments.find("region")->as_string() == "eu-west-1",
                 "the captured call's arguments match exactly what was sent");
    }
    if (reply1.has_value()) {
        auto const* ok = reply1->find("ok");
        AE_CHECK(ok != nullptr && ok->as_bool(), "the reply is exactly the fixture's canned reply");
    }

    // ---- a second call with DIFFERENT arguments still returns the SAME canned reply -- the reply
    // is never derived from the arguments (I3) ------------------------------------------------------
    auto args2 = ae::json::Value::make_object({{"region", ae::json::Value::make_string("us-east-2")}});
    auto reply2 = descriptor.invoke(args2, ctx);
    AE_CHECK(reply2.has_value() && reply2->find("ok") != nullptr && reply2->find("ok")->as_bool(),
             "a second call with different arguments returns the identical canned reply");
    AE_CHECK(sink.size() == 2, "two calls in sequence both land in the sink, in order");
    if (sink.size() == 2) {
        AE_CHECK(sink[1].arguments.find("region")->as_string() == "us-east-2",
                 "the second captured call's arguments are the second call's own, not stale");
    }

    // ---- EvalStubToolProvider contributes exactly the descriptors it was constructed with --------
    std::vector<ae::ToolDescriptor> descriptors;
    descriptors.push_back(ev::make_stub_tool_descriptor(fixture, sink));
    ev::EvalStubToolProvider provider(std::move(descriptors));
    ae::Principal const session_principal{"p", ""};
    std::vector<ae::Message> const empty_history;
    ae::SessionContext session_ctx{"s-eval", session_principal, empty_history};
    auto contribution = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
        provider.on_context(session_ctx, ctx));
    AE_CHECK(contribution.has_value() && contribution->tools.size() == 1 &&
                 contribution->tools.front().name == "set_deploy_region",
             "EvalStubToolProvider::on_context contributes exactly the constructed descriptors");

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_eval_stub_tool: all checks passed\n";
    return 0;
}
