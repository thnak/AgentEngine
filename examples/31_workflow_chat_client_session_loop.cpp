// AgentEngine "get started" examples, 31 -- driving `agentengine::rt::WorkflowChatClient`
// (rt/workflow_as_chat_client.hpp) the way a real interactive caller actually would: a session loop
// that keeps calling `chat_stream()`, answering each `request_port` ask as it arrives, until the
// wrapped workflow genuinely completes.
//
// Example 28 (workflow_as_chat_client.cpp) proves the mechanism with exactly two hardcoded calls,
// because its demo workflow has only one request_port node. This example's workflow has TWO
// (greet -> ask_color[request_port] -> relay -> ask_size[request_port] -> finish), so the driver
// below cannot get away with a fixed call count -- it has to inspect each response and decide
// "answer again" vs. "done", exactly like tools/cli_chat.cpp's own interactive `while (true)` loop
// decides "keep reading input" vs. "exit". Bounded rather than a literal unbounded `while (true)`,
// matching this project's own documented convention (see the Builder API page's "bounded,
// single-resume() loop" note) -- a caller integrating WorkflowChatClient into a real session should
// never trust an external actor to eventually stop asking.
//
// Fully offline -- no live model needed, matching examples 10/13/14/20/21/28's own style. The
// "human" answers are a canned, deterministic queue rather than real stdin, so this stays a
// reproducible CI example rather than an interactive tool (tools/cli_chat.cpp is that).
//
// Run: ./agentengine_example_31_workflow_chat_client_session_loop

#include <cstdio>
#include <deque>
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
[[nodiscard]] std::string text_of(Message const& m) {
    return m.content.empty() ? std::string{} : text_of(m.content[0]);
}

// Same drain shape as example 28 and rt/agent_session_trust.hpp's own drain_streaming_response().
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

// Non-null only when `item` is the adapter's own ask-signal shape (never a ToolCall -- see the
// design draft's own round-1 finding, already proven by example 28).
[[nodiscard]] bool is_ask_signal(ContentItem const& item, std::string* interaction_id_out) {
    auto const* custom = std::get_if<Custom>(&item.value);
    if (custom == nullptr || custom->type_id != "agentengine.workflow_request_port") return false;
    auto parsed = json::parse(custom->payload_json);
    if (!parsed) return false;
    if (auto const* iid = parsed->find("interaction_id"); iid != nullptr && iid->is_string()) {
        *interaction_id_out = iid->as_string();
    }
    return true;
}

[[nodiscard]] Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id = id, .kind = kind, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

}  // namespace

int main() {
    // greet -> ask_color[request_port] -> relay -> ask_size[request_port] -> finish -- two real
    // suspend points, not one, so the driver loop below has to genuinely loop.
    Workflow wf;
    wf.id = "order-intake";
    wf.executors = {
        node_desc("greet"), node_desc("ask_color", executor_kind::request_port), node_desc("relay"),
        node_desc("ask_size", executor_kind::request_port), node_desc("finish"),
    };
    wf.edges.push_back(Edge{"greet", "ask_color", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"ask_color", "relay", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"relay", "ask_size", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"ask_size", "finish", edge_kind::direct, {}});
    wf.start = "greet";
    wf.output_selection.push_back("finish");
    wf.bound.max_rounds = 16;  // two suspend/resume cycles across five nodes -- generous headroom

    std::vector<ExecutorBody> bodies = {
        // Decodes the history envelope this adapter builds from ChatRequest.messages, exactly like
        // example 28's own "greet" -- only the FIRST chat_stream() call in a run ever reaches this.
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
            return ExecutorOutcome{text_message("What color would you like?", role::assistant)};
        },
        {},  // ask_color is a request_port -- no body
        // The resolved port's own answer message flows here directly as `in` -- same fold example
        // 28's run_worker() proves for a single port, chained one hop further here.
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{
                text_message("Got it, " + text_of(in) + ". Small, medium, or large?", role::assistant)};
        },
        {},  // ask_size is a request_port -- no body
        [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message("Order noted: " + text_of(in) + ". Thanks!", role::assistant)};
        },
    };

    auto inner = std::make_shared<WorkflowSupervisor>();
    inner->initialize(wf, bodies);
    WorkflowChatClient client(inner);

    // The "human" side of this session -- a canned, deterministic queue rather than real stdin, so
    // this example stays reproducible. A real caller (tools/cli_chat.cpp's own while(true) loop, or
    // an outer AgentSession's bound backend) would read this from wherever ITS input actually comes
    // from; nothing about the loop shape below depends on where the answer text originates.
    std::deque<std::string> canned_answers = {"Blue", "Medium"};

    std::vector<Message> history = {text_message("Hi, I'd like to place an order.")};
    EffectContext ctx{};
    std::string final_answer;
    bool completed = false;

    // The session loop itself: keep calling chat_stream() with the growing history, answering
    // whatever ask surfaces, until a plain final answer arrives instead of another ask. Bounded at
    // one iteration per open interaction plus the terminal call, so a bug that kept asking forever
    // fails loudly here rather than hanging the example.
    constexpr int kMaxRounds = 5;
    for (int round = 0; round < kMaxRounds && !completed; ++round) {
        ChatRequest req;
        req.messages = history;
        std::string const label = "round " + std::to_string(round + 1);
        std::vector<ChatResponseUpdate> updates = drain(client.chat_stream(req, ctx), label.c_str());
        check(updates.size() == 1, "each round pushes exactly one update (one open ask, or one final)");
        if (updates.empty()) break;

        ContentItem const& delta = updates[0].delta;
        std::string interaction_id;
        if (is_ask_signal(delta, &interaction_id)) {
            check(!interaction_id.empty(), "an ask-signal update carries a real interaction_id");

            // Echo the ask into history exactly like example 28's call 2 does, then answer with the
            // next canned response -- the SAME shape a real human-in-the-loop turn takes, just
            // driven by a loop instead of copy-pasted per call.
            Message ask_echo{};
            ask_echo.role = role::assistant;
            ask_echo.content.push_back(delta);
            history.push_back(ask_echo);

            bool const has_answer = !canned_answers.empty();
            check(has_answer, "a canned answer is available for this ask");
            std::string const answer_text = has_answer ? canned_answers.front() : std::string{};
            if (has_answer) canned_answers.pop_front();
            std::printf("[session] answering interaction '%s' with '%s'\n", interaction_id.c_str(),
                        answer_text.c_str());

            std::vector<std::pair<std::string, json::Value>> resp_obj;
            resp_obj.emplace_back("interaction_id", json::Value::make_string(interaction_id));
            resp_obj.emplace_back("response", rt::message_to_json(text_message(answer_text)));
            std::string const resp_payload = json::dump(json::Value::make_object(std::move(resp_obj)));

            Message answer{};
            answer.role = role::user;
            ContentItem answer_item{};
            answer_item.origin = content_origin::user;
            answer_item.value = Custom{"agentengine.workflow_request_port_response", resp_payload};
            answer.content.push_back(answer_item);
            history.push_back(answer);
        } else {
            check(updates[0].is_final, "a non-ask update is the run's own final answer");
            final_answer = text_of(delta);
            completed = true;
        }
    }

    check(completed, "the session loop reached completion within the round budget, not the safety cap");
    check(canned_answers.empty(), "every canned answer was actually consumed by an ask");
    std::printf("[session] final answer: %s\n", final_answer.c_str());
    check(final_answer == "Order noted: Medium. Thanks!",
          "the second ask's answer ('Medium') is what reaches finish -- 'Blue' was consumed by the "
          "first ask and folded into relay's own question text, not carried past it");

    std::fprintf(stderr, g_failures == 0 ? "example_31_workflow_chat_client_session_loop: OK\n"
                                          : "example_31_workflow_chat_client_session_loop: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
