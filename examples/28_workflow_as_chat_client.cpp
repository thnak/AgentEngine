// AgentEngine "get started" examples, 28 -- a whole Workflow, reused as a `ChatClient` backend
// (GitHub issue #35; design draft: docs/planning/workflow-as-chatclient-adapter-design-draft.md).
//
// Mirrors MAF's samples/03-workflows/agents/sequential_workflow_as_agent.py in spirit --
// `workflow.as_agent()` there, `agentengine::rt::WorkflowChatClient` here
// (rt/workflow_as_chat_client.hpp). Unlike example 21 (`workflow_as_executor_body()`, the OTHER
// direction -- a Workflow embedded as a graph NODE), this wraps a Workflow so it satisfies the
// `ChatClient` concept directly: `capabilities()` + `chat_stream()`, the literal contract a live
// model-backed agent already satisfies.
//
// The wrapped workflow here contains a real `request_port` node -- proving the round-trip this
// design's own ten red-team rounds spent the most effort on: a suspended interaction surfaces as a
// `Custom` ask-signal `ChatResponseUpdate` (never a `ToolCall`, which would let an outer `AgentSession`
// silently mis-resolve it), and a second `chat_stream()` call carrying a matching response signal in
// the growing history resumes and completes the run.
//
// Fully offline -- no live model needed, matching examples 10/13/14/20/21's own style.
//
// Run: ./agentengine_example_28_workflow_as_chat_client

#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/workflow_as_chat_client.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::WorkflowChatClient;
using agentengine::rt::WorkflowSupervisor;

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

[[nodiscard]] Message text_message(std::string text, role r = role::user) {
    ContentItem item{};
    item.origin = (r == role::user) ? content_origin::user : content_origin::assistant;
    item.value = Text{std::move(text)};
    Message m{};
    m.role = r;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string text_of(ContentItem const& item) {
    if (auto const* t = std::get_if<Text>(&item.value)) return t->text;
    return {};
}

// Drains a chat_stream() call to its terminal, printing each pushed update -- the same polling shape
// rt/agent_session_trust.hpp's own drain_streaming_response() uses.
[[nodiscard]] std::vector<ChatResponseUpdate> drain(stream<ChatResponseUpdate> s, char const* label) {
    std::vector<ChatResponseUpdate> updates;
    while (!s.done()) {
        while (auto upd = s.next()) {
            std::printf("[%s] update (is_final=%s)\n", label, upd->is_final ? "true" : "false");
            updates.push_back(std::move(*upd));
        }
        if (!s.done()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (s.terminal() != stream_terminal::closed) {
        std::printf("[%s] stream ended with an error: %s\n", label, s.fail_error().message.c_str());
    }
    return updates;
}

}  // namespace

int main() {
    // A two-node workflow: "greet" (an ordinary function) fans into "ask" (a request_port node) --
    // the wrapped Workflow's own `start` executor is purpose-built to decode this adapter's history
    // envelope (docs/planning/workflow-as-chatclient-adapter-design-draft.md §4a).
    Workflow wf;
    wf.id = "greet-then-ask";
    wf.executors = {
        Executor{.id = "greet", .kind = executor_kind::function, .input_type = "T", .output_type = "T",
                 .worktree_mode = sharing_mode::branch, .capability_ceiling = {}},
        Executor{.id = "ask", .kind = executor_kind::request_port, .input_type = "T", .output_type = "T",
                 .worktree_mode = sharing_mode::branch, .capability_ceiling = {}},
    };
    wf.edges.push_back(Edge{"greet", "ask", edge_kind::direct, {}});
    wf.start = "greet";
    wf.output_selection.push_back("ask");
    wf.bound.max_rounds = 8;

    std::vector<ExecutorBody> bodies = {
        // "greet" decodes the history envelope this adapter builds from ChatRequest.messages --
        // proving the message-flattening design round 4/9 settled on, not a stub.
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            if (in.content.size() != 1) {
                return std::unexpected(error{failure_class::contract, "expected one envelope item",
                                              "example.bad_envelope"});
            }
            auto const* custom = std::get_if<Custom>(&in.content[0].value);
            if (custom == nullptr || custom->type_id != "agentengine.workflow_chat_client_history") {
                return std::unexpected(error{failure_class::contract, "expected the history envelope",
                                              "example.bad_envelope_type"});
            }
            auto parsed = json::parse(custom->payload_json);
            std::size_t const count = (parsed && parsed->is_array()) ? parsed->as_array().size() : 0;
            std::printf("[greet] decoded %zu message(s) from the caller's own history\n", count);
            return ExecutorOutcome{text_message("What's your favorite color?", role::assistant)};
        },
        // "ask" itself is never invoked -- reaching a request_port node IS the event.
        [](Message const&, EffectContext&) -> result<ExecutorOutcome> {
            return std::unexpected(error{failure_class::fatal, "a request_port node must never be invoked",
                                          "example.port_body_invoked"});
        },
    };

    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(wf, bodies);
    WorkflowChatClient client(inner);

    ChatClientCapabilities const caps = client.capabilities();
    std::printf("capabilities(): streaming=%s tool_calling=%s\n", caps.streaming ? "true" : "false",
                caps.tool_calling ? "true" : "false");

    // --- Call 1: open the conversation, reach the request_port node, suspend. ---
    ChatRequest req1;
    req1.messages = {text_message("Hi, I need help picking a color.")};
    EffectContext ctx{};

    std::vector<ChatResponseUpdate> first = drain(client.chat_stream(req1, ctx), "call 1");
    check(first.size() == 1, "reaching the request_port pushes exactly one ask-signal update");
    ContentItem ask_item{};
    std::string interaction_id;
    if (!first.empty()) {
        ask_item = first[0].delta;
        auto const* custom = std::get_if<Custom>(&ask_item.value);
        check(custom != nullptr && custom->type_id == "agentengine.workflow_request_port",
              "the ask is a Custom item, never a ToolCall -- see the design draft's own round-1 finding");
        if (custom != nullptr) {
            auto parsed = json::parse(custom->payload_json);
            if (parsed) {
                if (auto const* iid = parsed->find("interaction_id"); iid != nullptr && iid->is_string()) {
                    interaction_id = iid->as_string();
                }
            }
        }
        check(!interaction_id.empty(), "the ask carries a real interaction_id");
        std::printf("[call 1] paused, waiting for an answer to interaction '%s'\n", interaction_id.c_str());
    }

    // --- Call 2: answer, with the growing history (including the echoed ask) carried forward. ---
    std::vector<std::pair<std::string, json::Value>> resp_obj;
    resp_obj.emplace_back("interaction_id", json::Value::make_string(interaction_id));
    resp_obj.emplace_back("response", rt::message_to_json(text_message("Blue.")));
    std::string const resp_payload = json::dump(json::Value::make_object(std::move(resp_obj)));

    ChatRequest req2;
    req2.messages = req1.messages;
    Message ask_echo{};
    ask_echo.role = role::assistant;
    ask_echo.content.push_back(ask_item);
    req2.messages.push_back(ask_echo);
    Message answer{};
    answer.role = role::user;
    ContentItem answer_item{};
    answer_item.origin = content_origin::user;
    answer_item.value = Custom{"agentengine.workflow_request_port_response", resp_payload};
    answer.content.push_back(answer_item);
    req2.messages.push_back(answer);

    std::vector<ChatResponseUpdate> second = drain(client.chat_stream(req2, ctx), "call 2");
    check(second.size() == 1, "the completed run pushes exactly one update");
    if (!second.empty()) {
        std::printf("[call 2] final answer: %s\n", text_of(second[0].delta).c_str());
        check(text_of(second[0].delta) == "Blue.",
              "the completed output is the resolved port's own response, round-tripped end to end");
        check(second[0].is_final, "the last (only) push has is_final=true");
        // ADR-163: usage is now a real, tracked value (never nullopt) -- an honest zero here, since
        // this example's graph has no agent-kind node to incur any real, metered cost.
        check(second[0].usage.has_value(),
              "usage is a real, tracked value, never nullopt (ADR-163)");
        check(second[0].usage.has_value() && second[0].usage->input_tokens == 0 &&
                  second[0].usage->output_tokens == 0,
              "an honest zero here -- this graph has no agent-kind node to incur real cost");
    }

    std::fprintf(stderr, g_failures == 0 ? "example_28_workflow_as_chat_client: OK\n"
                                          : "example_28_workflow_as_chat_client: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
