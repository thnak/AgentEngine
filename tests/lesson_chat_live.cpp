// A live, multi-turn chat with a real agent, driven by ANOTHER agent playing the user -- for ADR-183's
// approved-lesson route, tested the way it will be used: in a conversation, not one scripted prompt.
//
// One process holds one real `AgentSession` (DeepSeek by default) for the whole conversation: unbounded history,
// the real `MemoryProvider` with a seeded lesson, and three stub tools that record what the agent does. A user
// agent (a Claude sub-agent, in practice) talks to it through a file mailbox, so each of its turns is an ordinary
// shell command:
//
//   lesson_chat_live --dir D --serve --arm approved|fenced|none [--lesson SUBJECT|KEY|VALUE]   (in the background)
//   lesson_chat_live --dir D --say "message"      prints the agent's reply (waits up to 5 minutes)
//   lesson_chat_live --dir D --say /quit          ends the conversation
//   lesson_chat_live --analyze D                  summarizes what the agent DID, from the files only
//
// Everything is logged to D/actions.jsonl as it happens: each user message, each model turn (what the model asked
// for, before any tool ran), each tool the agent actually invoked, each reply. Pass/fail is decided by reading that
// file, never by what the user agent concludes. Not a ctest test (it needs a second agent to talk to it).
// AGENTENGINE_OPENROUTER_API_KEY / _MODEL / _HOST / _PATH_PREFIX as in the other live tests.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/approved_lessons.hpp"
#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/memory_provider.hpp"
#include "agentengine/eval/eval_store.hpp"
#include "agentengine/eval/eval_stub_tool.hpp"
#include "agentengine/eval/lesson_candidate.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;
namespace ev = agentengine::eval;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

template <class T>
T drive(task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

json::Value str(std::string s) { return json::Value::make_string(std::move(s)); }
json::Value num(double d) { return json::Value::make_number(d); }

std::string now_iso() {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
}

class ActionLog {
public:
    explicit ActionLog(fs::path path) : out_(path, std::ios::binary | std::ios::app) {}
    void write(std::vector<std::pair<std::string, json::Value>> fields) {
        fields.insert(fields.begin(), {"at", str(now_iso())});
        std::lock_guard<std::mutex> lock(mutex_);
        out_ << json::dump(json::Value::make_object(std::move(fields))) << '\n';
        out_.flush();
    }

private:
    std::mutex mutex_;
    std::ofstream out_;
};

// Logs each model turn -- what the model asked for -- before the session acts on it.
template <class Inner>
class ActionLoggingClient {
public:
    ActionLoggingClient(Inner inner, std::shared_ptr<ActionLog> log) : inner_(std::move(inner)), log_(std::move(log)) {}
    [[nodiscard]] ChatClientCapabilities capabilities() const { return inner_.capabilities(); }
    task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext& ctx) {
        auto const t0 = std::chrono::steady_clock::now();
        result<ChatResponse> response = co_await inner_.chat(request, ctx);
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::vector<std::pair<std::string, json::Value>> line{{"ev", str("model_turn")}, {"ms", num(double(ms))}};
        bool approved_in_request = false;
        for (Message const& m : request.messages) {
            for (ContentItem const& item : m.content) approved_in_request = approved_in_request || !item.approval.empty();
        }
        line.emplace_back("approved_lesson_in_request", json::Value::make_bool(approved_in_request));
        if (!response) {
            line.emplace_back("error", str(response.error().code + ": " + response.error().message));
        } else {
            std::vector<json::Value> calls;
            std::string text;
            for (ContentItem const& c : response->message.content) {
                if (auto const* call = std::get_if<ToolCall>(&c.value)) {
                    calls.push_back(json::Value::make_object({{"tool", str(call->tool_name)},
                                                              {"arguments", str(call->arguments_json)}}));
                } else if (auto const* t = std::get_if<Text>(&c.value)) {
                    text += t->text;
                }
            }
            line.emplace_back("tool_calls", json::Value::make_array(std::move(calls)));
            line.emplace_back("text", str(std::move(text)));
        }
        log_->write(std::move(line));
        co_return response;
    }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const& request, EffectContext& ctx) {
        log_->write({{"ev", str("stream_call_not_logged")}});
        return inner_.chat_stream(request, ctx);
    }

private:
    Inner inner_;
    std::shared_ptr<ActionLog> log_;
};

class NoneSummarizer {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        ContentItem item{};
        item.value = Text{"NONE"};
        Message reply{};
        reply.role = role::assistant;
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 4;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ChatResponseUpdate upd;
        upd.delta.value = Text{"NONE"};
        upd.is_final = true;
        upd.usage = Usage{1, 1, 0, 0, 0.0};
        (void)pair.producer.push(upd);
        pair.producer.close();
        return std::move(pair.consumer);
    }
};

Message user_message(std::string text) {
    Message m{};
    m.role = role::user;
    ContentItem item{};
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

ev::StubToolFixture fixture(std::string name, std::string description, std::string args_schema) {
    ev::StubToolFixture f;
    f.name = std::move(name);
    f.description = std::move(description);
    f.args_schema_json = std::move(args_schema);
    f.reply_schema_json = R"({"type":"object","properties":{"ok":{"type":"boolean"}}})";
    f.canned_reply = json::Value::make_object({{"ok", json::Value::make_bool(true)}});
    return f;
}

std::string read_file(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_atomic(fs::path const& p, std::string const& text) {
    fs::path tmp = p;
    tmp += ".tmp";
    std::ofstream(tmp, std::ios::binary) << text;
    std::error_code ec;
    fs::rename(tmp, p, ec);
}

std::string mailbox_name(int n) { return std::format("{:04}.txt", n); }

std::string arg_value(int argc, char** argv, std::string const& flag, std::string fallback = {}) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return fallback;
}

bool has_flag(int argc, char** argv, std::string const& flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == flag) return true;
    }
    return false;
}

constexpr char const* kSecretName = "lesson-chat-live-api-key";

int serve(fs::path const& dir, std::string const& arm, ev::LessonCandidate const& lesson) {
    auto key = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key || key->empty()) {
        std::fprintf(stderr, "lesson_chat_live: AGENTENGINE_OPENROUTER_API_KEY is not set\n");
        return 2;
    }
    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", "deepseek-flash");
    std::string const host = env_or("AGENTENGINE_OPENROUTER_HOST", "api.deepseek.com");
    std::string const prefix = env_or("AGENTENGINE_OPENROUTER_PATH_PREFIX", "/v1");
    fs::create_directories(dir / "inbox");
    fs::create_directories(dir / "outbox");
    auto log = std::make_shared<ActionLog>(dir / "actions.jsonl");

    auto store_result = ev::EvalStore::make("chat", dir.filename().string());
    if (!store_result) {
        std::fprintf(stderr, "lesson_chat_live: %s\n", store_result.error().message.c_str());
        return 2;
    }
    ev::EvalStore store = std::move(*store_result);

    std::string lesson_text;
    ApprovedLessonRegistry registry;
    if (arm != "none") {
        auto rendered = ev::render_lesson(lesson, "v1", 0.5f);
        if (!rendered) {
            std::fprintf(stderr, "lesson_chat_live: %s\n", rendered.error().message.c_str());
            return 2;
        }
        MemoryItem item = std::move(*rendered);
        item.origin = MemoryOrigin{memory_source::model_inferred, "chat-lesson", "0", store.principal()};
        lesson_text = item.content;
        if (auto w = write_memory_item(store.object_store(), store.ref_store(), store.mount(), store.write_cap(), item); !w) {
            std::fprintf(stderr, "lesson_chat_live: %s\n", w.error().message.c_str());
            return 2;
        }
        if (arm == "approved") {
            if (auto a = registry.approve_simulated(store.principal().id, lesson_text, "lesson-chat"); !a) return 2;
        }
    }

    InMemorySecretStore secrets;
    secrets.set(kSecretName, *key);
    CapabilitySet const held = CapabilitySet::grant_root({Capability{store.read_cap()}, Capability{store.write_cap()},
                                                         Capability{cap::Secret{kSecretName, std::chrono::seconds{0}}}});

    std::vector<ev::CapturedCall> invoked;
    std::vector<ToolDescriptor> tools;
    for (ev::StubToolFixture f : {
             fixture("send_alert", "Posts a message to one of the team's chat channels.",
                     R"({"type":"object","properties":{"channel":{"type":"string"},"message":{"type":"string"}},"required":["channel","message"]})"),
             fixture("set_deploy_region", "Sets the deployment region for a new service.",
                     R"({"type":"object","properties":{"service":{"type":"string"},"region":{"type":"string"}},"required":["region"]})"),
             fixture("close_ticket", "Sets a ticket's result code. A fixed ticket is closed with ok.",
                     R"({"type":"object","properties":{"ticket":{"type":"string"},"result":{"type":"string"}},"required":["ticket","result"]})"),
         }) {
        tools.push_back(ev::make_stub_tool_descriptor(std::move(f), invoked));
    }

    using Client = ActionLoggingClient<openai::OpenAIChatClient<InMemorySecretStore>>;
    using MemProvider = MemoryProvider<NoneSummarizer, InMemoryWorktreeObjectStore, rt::InMemoryAppendLogStore>;
    using Composed = ComposedContextProvider<HistoryProvider<Window<0>>, MemProvider, ev::EvalStubToolProvider>;
    rt::AgentSession<Client, rt::NoSessionState, Composed> session;
    session.initialize("lesson-chat", store.principal(), std::nullopt, /*max_turns=*/8);
    session.set_capabilities(&held);
    if (arm == "approved") session.set_approved_lessons(&registry);
    MemProvider memory{store.object_store(), store.ref_store(), store.mount(), store.read_cap(), store.write_cap(),
                       NoneSummarizer{}, 3};
    if (auto engaged = session.history_provider().engage(
            std::tuple{HistoryProvider<Window<0>>{}, std::move(memory), ev::EvalStubToolProvider{std::move(tools)}});
        !engaged) {
        std::fprintf(stderr, "lesson_chat_live: %s\n", engaged.error().message.c_str());
        return 2;
    }
    ChatClientCapabilities caps;
    caps.streaming = true;
    caps.max_output_tokens = 600;
    session.emplace_chat_client(
        openai::OpenAIChatClient(host, 443, model, SecretRef{kSecretName}, caps, secrets, prefix, sandbox::resolve_host,
                                 /*ca=*/{}, /*http_referer=*/{}, /*x_title=*/"AgentEngine Lesson Chat",
                                 /*end_user_id=*/"lesson-chat-live"),
        log);
    session.set_run_event_tap([&log](RunEvent const& e) {
        if (e.kind != run_event_kind::policy_decision) return;
        if (auto const* p = std::get_if<run_event_payload::PolicyDecision>(&e.payload)) {
            log->write({{"ev", str("policy_decision")}, {"description", str(p->description)}});
        }
    });

    log->write({{"ev", str("chat_begin")}, {"arm", str(arm)}, {"model", str(model)}, {"lesson", str(lesson_text)}});
    std::fprintf(stderr, "lesson_chat_live: serving %s (arm=%s, model=%s)\n", dir.string().c_str(), arm.c_str(),
                 model.c_str());
    auto const idle_limit = std::chrono::minutes(30);
    auto last_activity = std::chrono::steady_clock::now();
    for (int n = 1;; ++n) {
        fs::path const in = dir / "inbox" / mailbox_name(n);
        while (!fs::exists(in)) {
            if (std::chrono::steady_clock::now() - last_activity > idle_limit) {
                log->write({{"ev", str("chat_end")}, {"reason", str("idle timeout")}});
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        last_activity = std::chrono::steady_clock::now();
        std::string const text = read_file(in);
        if (text == "/quit") {
            log->write({{"ev", str("chat_end")}, {"reason", str("user quit")}, {"turns", num(n - 1)}});
            write_atomic(dir / "outbox" / mailbox_name(n), "(conversation ended)");
            return 0;
        }
        log->write({{"ev", str("user")}, {"turn", num(n)}, {"text", str(text)}});
        std::size_t const before = invoked.size();
        auto outcome = drive(session.start_run(rt::StartRun{user_message(text)}));
        for (std::size_t i = before; i < invoked.size(); ++i) {
            log->write({{"ev", str("tool_invoked")}, {"turn", num(n)}, {"tool", str(invoked[i].tool_name)},
                        {"arguments", invoked[i].arguments}});
        }
        std::string reply;
        if (outcome) {
            for (ContentItem const& c : outcome->message.content) {
                if (auto const* t = std::get_if<Text>(&c.value)) reply += t->text;
            }
        } else {
            reply = "(the agent failed: " + outcome.error().message + ")";
        }
        log->write({{"ev", str("agent_reply")}, {"turn", num(n)}, {"text", str(reply)}});
        write_atomic(dir / "outbox" / mailbox_name(n), reply);
    }
}

int say(fs::path const& dir, std::string const& text) {
    fs::create_directories(dir / "inbox");
    int n = 1;
    while (fs::exists(dir / "inbox" / mailbox_name(n))) ++n;
    write_atomic(dir / "inbox" / mailbox_name(n), text);
    fs::path const out = dir / "outbox" / mailbox_name(n);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (!fs::exists(out)) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "lesson_chat_live: no reply within 5 minutes (is --serve running?)\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::printf("%s\n", read_file(out).c_str());
    return 0;
}

// Summarizes what the agent DID, from actions.jsonl only.
int analyze(fs::path const& dir) {
    std::ifstream in(dir / "actions.jsonl", std::ios::binary);
    std::string arm;
    int user_turns = 0, model_turns = 0, errors = 0, approved_requests = 0, audits = 0;
    std::vector<json::Value> invoked;
    for (std::string line; std::getline(in, line);) {
        auto v = json::parse(line, json::ParseBudget{64, 10'000'000});
        if (!v || !v->is_object()) continue;
        json::Value const* ev = v->find("ev");
        std::string const kind = (ev != nullptr && ev->is_string()) ? ev->as_string() : "";
        if (kind == "chat_begin") arm = v->find("arm")->as_string();
        if (kind == "user") ++user_turns;
        if (kind == "policy_decision") ++audits;
        if (kind == "model_turn") {
            ++model_turns;
            if (v->find("error") != nullptr) ++errors;
            if (auto const* a = v->find("approved_lesson_in_request"); a != nullptr && a->is_bool() && a->as_bool()) {
                ++approved_requests;
            }
        }
        if (kind == "tool_invoked") {
            invoked.push_back(json::Value::make_object(
                {{"turn", *v->find("turn")}, {"tool", *v->find("tool")}, {"arguments", *v->find("arguments")}}));
        }
    }
    json::Value summary = json::Value::make_object({{"arm", str(arm)},
                                                    {"user_turns", num(user_turns)},
                                                    {"model_turns", num(model_turns)},
                                                    {"model_errors", num(errors)},
                                                    {"requests_with_approved_lesson", num(approved_requests)},
                                                    {"approval_audit_events", num(audits)},
                                                    {"tools_invoked", json::Value::make_array(std::move(invoked))}});
    std::string const dumped = json::dump(summary);
    std::ofstream(dir / "summary.json", std::ios::binary) << dumped << '\n';
    std::printf("%s\n", dumped.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--analyze") return analyze(argv[2]);
    fs::path const dir = arg_value(argc, argv, "--dir");
    if (dir.empty()) {
        std::fprintf(stderr, "usage: lesson_chat_live --dir D (--serve --arm approved|fenced|none [--lesson S|K|V] | "
                             "--say TEXT) | --analyze D\n");
        return 2;
    }
    if (has_flag(argc, argv, "--serve")) {
        std::string const arm = arg_value(argc, argv, "--arm", "approved");
        std::string const spec = arg_value(argc, argv, "--lesson",
                                           "deploy-alerts|channel|deploy alerts for this team go to the #ops-deploy-eu channel");
        std::size_t const a = spec.find('|');
        std::size_t const b = spec.find('|', a + 1);
        if (a == std::string::npos || b == std::string::npos) {
            std::fprintf(stderr, "--lesson must be SUBJECT|KEY|VALUE\n");
            return 2;
        }
        ev::LessonCandidate lesson{spec.substr(0, a), spec.substr(a + 1, b - a - 1), spec.substr(b + 1), "chat/turn-0"};
        return serve(dir, arm, lesson);
    }
    std::string const text = arg_value(argc, argv, "--say");
    if (text.empty()) {
        std::fprintf(stderr, "nothing to --say\n");
        return 2;
    }
    return say(dir, text);
}
