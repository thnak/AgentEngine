#pragma once
// Implements ADR-182 (decisions/ADR-182-agent-test-driver-mcp.md), driver phase 1 (§12's revised
// phase 2): the core of `agentengine_test_driver`, an MCP server over stdio that lets a test agent
// (a Claude Code sub-agent) drive in-process `rt::AgentSession`s with a scripted model.
//
// This header holds everything except the stdio loop, so tests drive it in-process through
// `Driver::handle_line()` (tests/test_agentengine_test_driver.cpp). `agentengine_test_driver.cpp`
// is only the stdin/stdout pump.
//
// What phase 1 deliberately does NOT have (ADR-182 §12):
//   - fixture files. Fixtures are compiled in (`fixtures()`), so no tool argument and no file the
//     tester can write changes a session's tools or capability grant (§12 C-3);
//   - real tools. The only tools are the in-process test doubles below, so an approval the tester
//     gives authorizes nothing outside this process (§12 C-2);
//   - scenario export, assert, fork, live mode (later phases).
//
// Threading (ADR-182 §3.6, §12 R1). The MCP thread never touches an `AgentSession` directly. Each
// session owns one `rt::ThreadPool(1)` and every call into the session is a job on it. The run-event
// tap may be called from the worker or from parallel-batch threads, so `SessionMonitor` is
// mutex-protected, never blocks, and never calls back into the session. While a run is in flight a
// snapshot is built from the monitor alone (`partial: true`).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/message_codec.hpp"
#include "agentengine/rt/thread_pool.hpp"
#include "agentengine/testing/scripted_chat_client.hpp"

namespace agentengine::test_driver {

inline constexpr std::string_view kServerName = "agentengine-test-driver";
inline constexpr std::string_view kServerVersion = "0.1.0";
inline constexpr std::string_view kProtocolVersion = "2026-07-28";

inline constexpr std::size_t kMaxSessions = 8;
inline constexpr std::size_t kEventRingCapacity = 10000;
inline constexpr std::size_t kMaxLineBytes = 4u * 1024u * 1024u;
inline constexpr std::uint64_t kMaxWaitMs = 60000;
inline constexpr std::uint64_t kDefaultWaitMs = 10000;
inline constexpr std::size_t kMaxEventsPerResult = 200;
inline constexpr std::uint64_t kMaxTurnsPerRun = 32;
inline constexpr auto kJobTimeout = std::chrono::seconds(10);

// ---- JSON helpers ----------------------------------------------------------------------------------

using json::Value;
using Members = std::vector<std::pair<std::string, Value>>;

[[nodiscard]] inline Value str(std::string s) { return Value::make_string(std::move(s)); }
[[nodiscard]] inline Value num(double n) { return Value::make_number(n); }
[[nodiscard]] inline Value boolean(bool b) { return Value::make_bool(b); }
[[nodiscard]] inline Value obj(Members m) { return Value::make_object(std::move(m)); }
[[nodiscard]] inline Value arr(std::vector<Value> a) { return Value::make_array(std::move(a)); }

[[nodiscard]] inline std::optional<std::string> get_string(Value const& o, std::string_view key) {
    Value const* v = o.find(key);
    if (v == nullptr || !v->is_string()) return std::nullopt;
    return v->as_string();
}
[[nodiscard]] inline std::optional<std::uint64_t> get_u64(Value const& o, std::string_view key) {
    Value const* v = o.find(key);
    if (v == nullptr) return std::nullopt;
    return json::as_bounded_integer(*v);
}
[[nodiscard]] inline std::optional<bool> get_bool(Value const& o, std::string_view key) {
    Value const* v = o.find(key);
    if (v == nullptr || !v->is_bool()) return std::nullopt;
    return v->as_bool();
}

// ---- Test tools (in-process, no effects outside this process) ---------------------------------------

struct TextArgs { std::string text; };
AE_JSON_SCHEMA(TextArgs, text)
struct TextReply { std::string text; };
AE_JSON_SCHEMA(TextReply, text)

// Returns its input. Never needs approval.
struct EchoTool : Tool<EchoTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "echo";
    static constexpr std::string_view description = "Returns its text argument unchanged.";
    using Args = TextArgs;
    using Reply = TextReply;
    static result<Reply> invoke(Args a, EffectContext&) { return Reply{std::move(a.text)}; }
};

// Returns its input, but always needs approval first -- the tool a test uses to exercise the gate.
struct GatedEchoTool : Tool<GatedEchoTool, Capabilities<>, EffectClass<effect_class::pure>,
                            Approval<approval_mode::always_require>> {
    static constexpr std::string_view name = "gated_echo";
    static constexpr std::string_view description =
        "Returns its text argument unchanged. Needs human approval before every call.";
    using Args = TextArgs;
    using Reply = TextReply;
    static result<Reply> invoke(Args a, EffectContext&) { return Reply{std::move(a.text)}; }
};

// Always fails, so a test can exercise the tool-error path.
struct FailTool : Tool<FailTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "fail";
    static constexpr std::string_view description = "Always fails with the given text as the error message.";
    using Args = TextArgs;
    using Reply = TextReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        return std::unexpected(error{failure_class::fatal, std::move(a.text), "test_driver.fail_tool"});
    }
};

[[nodiscard]] inline std::vector<ToolDescriptor> all_test_tool_descriptors() {
    return ToolTable::from_tools<EchoTool, GatedEchoTool, FailTool>().descriptors();
}

// ---- Fixtures (compiled in; ADR-182 §12 C-3) --------------------------------------------------------

struct Fixture {
    std::string              name;
    std::string              description;
    std::vector<std::string> tools;
    bool                     suspend_for_approval = true;
};

[[nodiscard]] inline std::vector<Fixture> const& fixtures() {
    static std::vector<Fixture> const all{
        {"basic", "echo, gated_echo (needs approval) and fail. Gated calls suspend the run for approval.",
         {"echo", "gated_echo", "fail"}, true},
        {"no_tools", "No tools at all: the model can only answer in text.", {}, true},
    };
    return all;
}

[[nodiscard]] inline Fixture const* find_fixture(std::string_view name) {
    for (Fixture const& f : fixtures())
        if (f.name == name) return &f;
    return nullptr;
}

// ---- The session's context provider: history plus the fixture's tools ------------------------------

class DriverHistoryProvider {
public:
    void set_tools(std::vector<ToolDescriptor> tools) { tools_ = std::move(tools); }

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = tools_;
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    std::vector<ToolDescriptor> tools_;
};

using Session = rt::AgentSession<testing::ScriptedChatClient, rt::NoSessionState, DriverHistoryProvider>;

// ---- Event → JSON ----------------------------------------------------------------------------------

[[nodiscard]] inline std::string_view kind_name(run_event_kind k) noexcept {
    switch (k) {
        case run_event_kind::run_started: return "run_started";
        case run_event_kind::run_finished: return "run_finished";
        case run_event_kind::run_failed: return "run_failed";
        case run_event_kind::run_canceled: return "run_canceled";
        case run_event_kind::turn_started: return "turn_started";
        case run_event_kind::turn_finished: return "turn_finished";
        case run_event_kind::model_call_started: return "model_call_started";
        case run_event_kind::model_delta: return "model_delta";
        case run_event_kind::model_call_finished: return "model_call_finished";
        case run_event_kind::tool_call_started: return "tool_call_started";
        case run_event_kind::tool_call_delta: return "tool_call_delta";
        case run_event_kind::tool_call_finished: return "tool_call_finished";
        case run_event_kind::sandbox_exec_started: return "sandbox_exec_started";
        case run_event_kind::sandbox_exec_finished: return "sandbox_exec_finished";
        case run_event_kind::state_changed: return "state_changed";
        case run_event_kind::artifact_produced: return "artifact_produced";
        case run_event_kind::input_required: return "input_required";
        case run_event_kind::input_resolved: return "input_resolved";
        case run_event_kind::auth_required: return "auth_required";
        case run_event_kind::auth_resolved: return "auth_resolved";
        case run_event_kind::approval_requested: return "approval_requested";
        case run_event_kind::approval_resolved: return "approval_resolved";
        case run_event_kind::codeact_ask_requested: return "codeact_ask_requested";
        case run_event_kind::hook_decision_requested: return "hook_decision_requested";
        case run_event_kind::warning: return "warning";
        case run_event_kind::policy_decision: return "policy_decision";
        case run_event_kind::model_output_discarded: return "model_output_discarded";
    }
    return "unknown";
}

// Text of a content list: Text items verbatim, Data items (a tool's structured reply) as their JSON,
// Error items as "error: <message>".
[[nodiscard]] inline std::string content_text(std::vector<ContentItem> const& items) {
    std::string out;
    for (ContentItem const& c : items) {
        if (auto const* t = std::get_if<Text>(&c.value)) {
            out += t->text;
        } else if (auto const* d = std::get_if<Data>(&c.value)) {
            out += d->json;
        } else if (auto const* e = std::get_if<Error>(&c.value)) {
            out += "error: " + e->message;
        }
    }
    return out;
}

template <class... Fs>
struct overloaded : Fs... { using Fs::operator()...; };
template <class... Fs>
overloaded(Fs...) -> overloaded<Fs...>;

[[nodiscard]] inline Value payload_json(RunEventPayload const& p) {
    namespace rp = run_event_payload;
    return std::visit(
        overloaded{
            [](rp::Empty const&) { return obj({}); },
            [](rp::RunFailed const& x) {
                return obj({{"error_code", str(x.error_code)}, {"message", str(x.message)}});
            },
            [](rp::Turn const& x) { return obj({{"turn_index", num(static_cast<double>(x.turn_index))}}); },
            [](rp::ModelDelta const& x) {
                return std::visit(overloaded{
                                      [](rp::ModelTextDelta const& d) { return obj({{"text", str(d.text)}}); },
                                      [](rp::ModelToolCallArgumentDelta const& d) {
                                          return obj({{"call_id", str(d.call_id)},
                                                      {"tool_name", str(d.tool_name)},
                                                      {"arguments_fragment", str(d.arguments_fragment)}});
                                      },
                                      [](rp::ModelReasoningDelta const& d) {
                                          return obj({{"reasoning", str(d.text)}});
                                      },
                                  },
                                  x.value);
            },
            [](rp::ToolCallStarted const& x) {
                return obj({{"call_id", str(x.call_id)}, {"tool_name", str(x.tool_name)}});
            },
            [](rp::ToolCallDelta const& x) {
                return obj({{"call_id", str(x.call_id)}, {"text", str(content_text({x.content}))}});
            },
            [](rp::ToolCallFinished const& x) {
                return obj({{"call_id", str(x.call_id)},
                            {"is_error", boolean(x.result.is_error)},
                            {"result", str(content_text(x.result.content))}});
            },
            [](rp::SandboxExec const& x) {
                return obj({{"backend", str(x.backend)}, {"stage", str(x.stage)}, {"ok", boolean(x.ok)},
                            {"error_code", str(x.error_code)}});
            },
            [](rp::StateChanged const& x) { return obj({{"description", str(x.description)}}); },
            [](rp::ArtifactProduced const& x) { return obj({{"artifact_id", str(x.artifact_id)}}); },
            [](rp::InteractionRef const& x) { return obj({{"interaction_id", str(x.interaction_id)}}); },
            [](rp::ApprovalRequested const& x) {
                return obj({{"call_id", str(x.call_id)}, {"interaction_id", str(x.interaction_id)},
                            {"tool_name", str(x.tool_name)}, {"arguments", str(x.arguments_json)},
                            {"needs_approval", boolean(x.needs_approval)}});
            },
            [](rp::ApprovalResolved const& x) {
                return obj({{"call_id", str(x.call_id)}, {"approved", boolean(x.approved)},
                            {"interaction_id", str(x.interaction_id)}});
            },
            [](rp::Warning const& x) { return obj({{"message", str(x.message)}}); },
            [](rp::PolicyDecision const& x) { return obj({{"description", str(x.description)}}); },
            [](rp::CodeActAskRequested const& x) {
                return obj({{"call_id", str(x.call_id)}, {"interaction_id", str(x.interaction_id)},
                            {"prompt", str(x.prompt)}});
            },
            [](rp::HookDecisionRequested const& x) {
                return obj({{"call_id", str(x.call_id)}, {"interaction_id", str(x.interaction_id)},
                            {"tool_name", str(x.tool_name)}});
            },
            [](rp::ModelOutputDiscarded const& x) {
                return obj({{"attempt", num(x.attempt)}, {"reason", str(x.reason)}});
            },
        },
        p);
}

// ---- SessionMonitor: event ring, interaction index, run status --------------------------------------

enum class run_state { idle, running, suspended };

[[nodiscard]] inline std::string_view run_state_name(run_state s) noexcept {
    switch (s) {
        case run_state::idle: return "idle";
        case run_state::running: return "running";
        case run_state::suspended: return "suspended";
    }
    return "unknown";
}

struct LoggedEvent {
    std::uint64_t  seq = 0;  // driver-assigned, session-wide (RunEvent::seq restarts per run)
    std::string    run_id;
    run_event_kind kind = run_event_kind::run_started;
    Value          payload;
};

struct PendingCall {
    std::string call_id;
    std::string interaction_id;
    std::string tool_name;
    std::string arguments_json;
    bool        needs_approval = true;
};

struct RunOutcome {
    std::string run_id;
    bool        ok = false;
    std::string error_code;
    std::string error_message;
    std::string text;  // the final assistant text when ok
};

class SessionMonitor {
public:
    // Called by the session's run-event tap, on whatever thread emitted the event.
    void on_event(RunEvent const& ev) {
        {
            std::lock_guard lock(mutex_);
            LoggedEvent le{next_seq_++, ev.run_id, ev.kind, payload_json(ev.payload)};
            if (ev.kind == run_event_kind::approval_requested) {
                if (auto const* p = std::get_if<run_event_payload::ApprovalRequested>(&ev.payload)) {
                    pending_.push_back(
                        PendingCall{p->call_id, p->interaction_id, p->tool_name, p->arguments_json, p->needs_approval});
                }
            } else if (ev.kind == run_event_kind::approval_resolved) {
                if (auto const* p = std::get_if<run_event_payload::ApprovalResolved>(&ev.payload)) {
                    std::erase_if(pending_, [&](PendingCall const& c) {
                        return c.call_id == p->call_id && c.interaction_id == p->interaction_id;
                    });
                }
            }
            events_.push_back(std::move(le));
            if (events_.size() > kEventRingCapacity) {
                events_.pop_front();
                ++dropped_;
            }
        }
        cv_.notify_all();
    }

    void set_state(run_state s) {
        {
            std::lock_guard lock(mutex_);
            state_ = s;
        }
        cv_.notify_all();
    }
    void finish_run(run_state s, RunOutcome outcome) {
        {
            std::lock_guard lock(mutex_);
            state_ = s;
            last_outcome_ = std::move(outcome);
            // Anything still indexed belongs to interactions the run no longer has open.
            if (s == run_state::idle) pending_.clear();
        }
        cv_.notify_all();
    }
    void clear_interaction(std::string const& interaction_id) {
        std::lock_guard lock(mutex_);
        std::erase_if(pending_, [&](PendingCall const& c) { return c.interaction_id == interaction_id; });
    }

    [[nodiscard]] run_state state() const {
        std::lock_guard lock(mutex_);
        return state_;
    }
    [[nodiscard]] std::uint64_t last_seq() const {
        std::lock_guard lock(mutex_);
        return next_seq_ - 1;
    }
    [[nodiscard]] std::vector<PendingCall> pending() const {
        std::lock_guard lock(mutex_);
        return pending_;
    }
    [[nodiscard]] std::optional<RunOutcome> last_outcome() const {
        std::lock_guard lock(mutex_);
        return last_outcome_;
    }
    [[nodiscard]] std::uint64_t dropped() const {
        std::lock_guard lock(mutex_);
        return dropped_;
    }

    // Events with seq > since, oldest first, at most `limit`, optionally filtered by kind name.
    [[nodiscard]] std::vector<LoggedEvent> events_since(std::uint64_t since, std::size_t limit,
                                                        std::vector<std::string> const& kinds = {}) const {
        std::lock_guard lock(mutex_);
        return collect(since, limit, kinds);
    }

    // Blocks until `pred` (evaluated under the lock, with the state and the events after `since`)
    // holds, or the timeout passes. Returns whether it held.
    using Predicate = std::function<bool(run_state, std::vector<LoggedEvent> const&)>;
    [[nodiscard]] bool wait(Predicate const& pred, std::uint64_t since, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return pred(state_, collect(since, kEventRingCapacity, {})); });
    }

private:
    [[nodiscard]] std::vector<LoggedEvent> collect(std::uint64_t since, std::size_t limit,
                                                   std::vector<std::string> const& kinds) const {
        std::vector<LoggedEvent> out;
        for (LoggedEvent const& e : events_) {
            if (e.seq <= since) continue;
            if (!kinds.empty() &&
                std::find(kinds.begin(), kinds.end(), std::string(kind_name(e.kind))) == kinds.end())
                continue;
            out.push_back(e);
            if (out.size() >= limit) break;
        }
        return out;
    }

    mutable std::mutex          mutex_;
    std::condition_variable     cv_;
    std::deque<LoggedEvent>     events_;
    std::uint64_t               next_seq_ = 1;
    std::uint64_t               dropped_ = 0;
    std::vector<PendingCall>    pending_;
    run_state                   state_ = run_state::idle;
    std::optional<RunOutcome>   last_outcome_;
};

// ---- One driven session ------------------------------------------------------------------------------

struct DriverSession {
    std::string                      id;
    Fixture                          fixture;
    std::shared_ptr<SessionMonitor>  monitor = std::make_shared<SessionMonitor>();
    CapabilitySet                    held = CapabilitySet::grant_root({});  // must outlive `session`
    std::unique_ptr<Session>         session = std::make_unique<Session>();
    testing::ScriptedChatClient      client;  // shares state with the session's own copy
    std::uint64_t                    action_mark = 0;  // monitor seq at the last send/resolve/cancel
    std::uint64_t                    next_call_id = 1;
    // Declared LAST so it is destroyed FIRST: its destructor finishes every queued job while the
    // session, monitor and grant are still alive (ADR-182 §12 R10).
    std::unique_ptr<rt::ThreadPool>  pool = std::make_unique<rt::ThreadPool>(1);
};

[[nodiscard]] inline Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] inline bool is_suspension(std::string const& code) {
    return code == Session::kSuspendedForApproval || code == Session::kSuspendedForCodeActAsk ||
           code == Session::kSuspendedForHookDecision;
}

// Records how a start_run()/resolve_interaction() ended and sets the resulting state.
inline void record_run_result(DriverSession& s, result<rt::AgentResponse> const& r) {
    RunOutcome o;
    o.run_id = s.session->last_run_id();
    if (r) {
        o.ok = true;
        o.text = content_text(r->message.content);
        s.monitor->finish_run(run_state::idle, std::move(o));
        return;
    }
    o.error_code = r.error().code;
    o.error_message = r.error().message;
    bool const suspended = is_suspension(o.error_code) && s.session->has_open_interactions();
    s.monitor->finish_run(suspended ? run_state::suspended : run_state::idle, std::move(o));
}

// Jobs. Free functions taking a raw pointer: the pool is destroyed before the session, so the
// pointer outlives every job (a capturing lambda coroutine would dangle).
[[nodiscard]] inline rt::task<void> send_job(DriverSession* s, std::string text) {
    result<rt::AgentResponse> r = co_await s->session->start_run(rt::StartRun{user_message(std::move(text))});
    record_run_result(*s, r);
    co_return;
}

[[nodiscard]] inline rt::task<void> resolve_job(DriverSession* s, std::string interaction_id, bool approve) {
    result<rt::AgentResponse> r =
        co_await s->session->resolve_interaction(rt::ResolveInteraction{interaction_id, approve});
    s->monitor->clear_interaction(interaction_id);
    record_run_result(*s, r);
    co_return;
}

// ADR-182 §12 R2: on a suspended session, cancel = cancel() then deny every open interaction.
[[nodiscard]] inline rt::task<void> cancel_suspended_job(DriverSession* s) {
    s->session->cancel();
    std::vector<std::string> ids;
    for (Interaction const& ix : s->session->open_interactions()) ids.push_back(ix.interaction_id);
    std::optional<result<rt::AgentResponse>> last;
    for (std::string const& id : ids) {
        last = co_await s->session->resolve_interaction(rt::ResolveInteraction{id, false});
        s->monitor->clear_interaction(id);
    }
    if (last) {
        record_run_result(*s, *last);
    } else {
        s->monitor->finish_run(run_state::idle, RunOutcome{s->session->last_run_id(), false, "run.canceled",
                                                           "canceled with nothing open", ""});
    }
    co_return;
}

[[nodiscard]] inline rt::task<void> read_job(std::function<void()> fn) {
    fn();
    co_return;
}

// ---- The driver ------------------------------------------------------------------------------------

struct ToolError {
    std::string code;
    std::string message;
};
using ToolResultJson = std::variant<Value, ToolError>;

class Driver {
public:
    // One JSON-RPC message in, zero or one JSON-RPC message out (notifications get no reply).
    [[nodiscard]] std::optional<std::string> handle_line(std::string_view line) {
        if (line.size() > kMaxLineBytes) {
            return json::dump(rpc_error(Value{}, -32600, "request line exceeds the size limit"));
        }
        auto parsed = json::parse(line);
        if (!parsed || !parsed->is_object()) {
            return json::dump(rpc_error(Value{}, -32700, "parse error"));
        }
        Value const& msg = *parsed;
        Value const* id = msg.find("id");
        auto method = get_string(msg, "method");
        if (!method) {
            if (id == nullptr) return std::nullopt;  // a response to something we never sent
            return json::dump(rpc_error(*id, -32600, "missing method"));
        }
        if (id == nullptr) return std::nullopt;  // notification
        Value const empty = obj({});
        Value const* params = msg.find("params");
        if (params == nullptr || !params->is_object()) params = &empty;

        if (*method == "initialize" || *method == "server/discover") {
            return json::dump(rpc_result(*id, server_info(*params)));
        }
        if (*method == "ping") return json::dump(rpc_result(*id, obj({})));
        if (*method == "tools/list") return json::dump(rpc_result(*id, obj({{"tools", tool_list()}})));
        if (*method == "tools/call") {
            auto name = get_string(*params, "name");
            if (!name) return json::dump(rpc_error(*id, -32602, "tools/call needs a name"));
            Value const* args = params->find("arguments");
            if (args == nullptr || !args->is_object()) args = &empty;
            auto handler = handlers().find(*name);
            if (handler == handlers().end()) {
                return json::dump(rpc_error(*id, -32602, "unknown tool: " + *name));
            }
            ToolResultJson out = (this->*(handler->second))(*args);
            return json::dump(rpc_result(*id, tool_result(std::move(out))));
        }
        return json::dump(rpc_error(*id, -32601, "method not found: " + *method));
    }

    [[nodiscard]] std::size_t session_count() const { return sessions_.size(); }

    // Closes every session (stdin EOF / shutdown).
    void close_all() {
        std::vector<std::string> ids;
        for (auto const& [id, _] : sessions_) ids.push_back(id);
        for (std::string const& id : ids) (void)close_session(id);
    }

private:
    using Handler = ToolResultJson (Driver::*)(Value const&);

    // ---- protocol plumbing ----

    [[nodiscard]] static Value rpc_result(Value const& id, Value result) {
        return obj({{"jsonrpc", str("2.0")}, {"id", id}, {"result", std::move(result)}});
    }
    [[nodiscard]] static Value rpc_error(Value const& id, int code, std::string message) {
        return obj({{"jsonrpc", str("2.0")},
                    {"id", id},
                    {"error", obj({{"code", num(code)}, {"message", str(std::move(message))}})}});
    }
    [[nodiscard]] static Value server_info(Value const& params) {
        std::string version = get_string(params, "protocolVersion").value_or(std::string(kProtocolVersion));
        return obj({{"protocolVersion", str(version)},
                    {"supportedVersions", arr({str(std::string(kProtocolVersion)), str("2025-06-18")})},
                    {"capabilities", obj({{"tools", obj({})}})},
                    {"serverInfo", obj({{"name", str(std::string(kServerName))},
                                        {"version", str(std::string(kServerVersion))}})},
                    {"instructions",
                     str("Drives AgentEngine sessions with a scripted model. Typical loop: session_start -> "
                         "model_script_push -> session_send -> session_wait_for -> interaction_list / "
                         "interaction_resolve -> session_snapshot. The model never answers on its own: "
                         "every model call consumes one scripted turn, and a call with none left fails "
                         "the run with scripted_chat_client.script_exhausted.")}});
    }

    // Text first (Claude Code's handling of structuredContent is undocumented; ADR-182 §3.4).
    [[nodiscard]] static Value tool_result(ToolResultJson out) {
        if (auto* err = std::get_if<ToolError>(&out)) {
            Value body = obj({{"error", obj({{"code", str(err->code)}, {"message", str(err->message)}})}});
            return obj({{"content", arr({obj({{"type", str("text")}, {"text", str(json::dump(body))}})})},
                        {"structuredContent", body},
                        {"isError", boolean(true)}});
        }
        Value& v = std::get<Value>(out);
        std::string text = json::dump(v);
        return obj({{"content", arr({obj({{"type", str("text")}, {"text", str(std::move(text))}})})},
                    {"structuredContent", std::move(v)},
                    {"isError", boolean(false)}});
    }

    [[nodiscard]] static Value schema(Members properties, std::vector<std::string> required) {
        std::vector<Value> req;
        for (std::string& r : required) req.push_back(str(std::move(r)));
        return obj({{"type", str("object")},
                    {"properties", obj(std::move(properties))},
                    {"required", arr(std::move(req))},
                    {"additionalProperties", boolean(false)}});
    }
    [[nodiscard]] static Value prop(std::string type, std::string description) {
        return obj({{"type", str(std::move(type))}, {"description", str(std::move(description))}});
    }
    [[nodiscard]] static Value tool_def(std::string name, std::string description, Value input_schema) {
        return obj({{"name", str(std::move(name))},
                    {"description", str(std::move(description))},
                    {"inputSchema", std::move(input_schema)}});
    }

    // Deterministic order (MCP 2026-07-28 asks tools/list to be stable).
    [[nodiscard]] static Value tool_list() {
        Value const sid = prop("string", "Session handle returned by session_start.");
        Value turn_schema = obj({
            {"type", str("object")},
            {"properties",
             obj({{"text", prop("string", "Assistant text for this turn.")},
                  {"tool_calls",
                   obj({{"type", str("array")},
                        {"items", obj({{"type", str("object")},
                                       {"properties",
                                        obj({{"name", prop("string", "Tool name.")},
                                             {"arguments", obj({{"description",
                                                                 str("Arguments: a JSON object, or a JSON "
                                                                     "string (sent verbatim, may be malformed).")}})},
                                             {"call_id", prop("string", "Optional; generated if absent.")}})},
                                       {"required", arr({str("name")})}})}})},
                  {"error", obj({{"type", str("object")},
                                 {"description", str("Make this model call fail: {code, message, class?}. "
                                                     "class is transient|fatal|contract (default fatal).")}})}})},
        });
        return arr({
            tool_def("fixtures_list", "List the compiled-in session fixtures and their tools.", schema({}, {})),
            tool_def("session_start",
                     "Start a session from a fixture. Returns its session_id. The model is scripted: push "
                     "turns with model_script_push before sending.",
                     schema({{"fixture", prop("string", "Fixture name (see fixtures_list).")}}, {"fixture"})),
            tool_def("model_script_push",
                     "Append scripted model turns. Each model call the engine makes consumes one turn. "
                     "Only while the session is idle or suspended.",
                     schema({{"session_id", sid},
                             {"turns", obj({{"type", str("array")}, {"items", std::move(turn_schema)}})}},
                            {"session_id", "turns"})),
            tool_def("session_send",
                     "Send a user message; starts a run and returns at once. Then call session_wait_for.",
                     schema({{"session_id", sid}, {"text", prop("string", "User message text.")}},
                            {"session_id", "text"})),
            tool_def("session_wait_for",
                     "Wait (<= 60 s) until a condition holds. until: settled (not running) | idle | "
                     "suspended | event (needs kind; optional tool_name/call_id). Considers events after "
                     "since_seq, default: the last send/resolve/cancel.",
                     schema({{"session_id", sid},
                             {"until", prop("string", "settled | idle | suspended | event")},
                             {"kind", prop("string", "Event kind for until=event, e.g. tool_call_started.")},
                             {"tool_name", prop("string", "Optional filter for until=event.")},
                             {"call_id", prop("string", "Optional filter for until=event.")},
                             {"since_seq", prop("integer", "Only events with seq greater than this.")},
                             {"timeout_ms", prop("integer", "Default 10000, max 60000.")}},
                            {"session_id", "until"})),
            tool_def("session_snapshot",
                     "State, last run outcome, pending approvals, script queue, event counters. History "
                     "only when the session is not running.",
                     schema({{"session_id", sid},
                             {"include_history", prop("boolean", "Include the full message history.")}},
                            {"session_id"})),
            tool_def("session_events", "Logged run events with seq > since_seq.",
                     schema({{"session_id", sid},
                             {"since_seq", prop("integer", "Default 0.")},
                             {"limit", prop("integer", "Default and max 200.")},
                             {"kinds", obj({{"type", str("array")}, {"items", obj({{"type", str("string")}})}})}},
                            {"session_id"})),
            tool_def("interaction_list",
                     "Pending approval requests, one entry per tool call, with tool name, arguments and "
                     "needs_approval (false = the call shares the round but was not itself gated).",
                     schema({{"session_id", sid}}, {"session_id"})),
            tool_def("interaction_resolve",
                     "Approve or deny an open interaction. The decision applies to every call in it "
                     "(per-call decisions are not supported yet).",
                     schema({{"session_id", sid},
                             {"interaction_id", prop("string", "From interaction_list.")},
                             {"decision", prop("string", "approve | deny")}},
                            {"session_id", "interaction_id", "decision"})),
            tool_def("session_cancel",
                     "Cancel the current run. On a suspended session this also denies every open "
                     "interaction, so the session returns to idle.",
                     schema({{"session_id", sid}}, {"session_id"})),
            tool_def("session_close", "Cancel anything in flight and discard the session.",
                     schema({{"session_id", sid}}, {"session_id"})),
            tool_def("model_requests",
                     "What the engine sent the model on each call: messages (role + text) and tool names.",
                     schema({{"session_id", sid}, {"since_index", prop("integer", "Default 0.")}},
                            {"session_id"})),
        });
    }

    [[nodiscard]] static std::map<std::string, Handler, std::less<>> const& handlers() {
        static std::map<std::string, Handler, std::less<>> const h{
            {"fixtures_list", &Driver::t_fixtures_list},
            {"session_start", &Driver::t_session_start},
            {"model_script_push", &Driver::t_model_script_push},
            {"session_send", &Driver::t_session_send},
            {"session_wait_for", &Driver::t_session_wait_for},
            {"session_snapshot", &Driver::t_session_snapshot},
            {"session_events", &Driver::t_session_events},
            {"interaction_list", &Driver::t_interaction_list},
            {"interaction_resolve", &Driver::t_interaction_resolve},
            {"session_cancel", &Driver::t_session_cancel},
            {"session_close", &Driver::t_session_close},
            {"model_requests", &Driver::t_model_requests},
        };
        return h;
    }

    // ---- helpers ----

    [[nodiscard]] static ToolError err(std::string code, std::string message) {
        return ToolError{std::move(code), std::move(message)};
    }

    DriverSession* find_session(Value const& args, ToolError& e) {
        auto id = get_string(args, "session_id");
        if (!id) {
            e = err("test.bad_arguments", "session_id is required");
            return nullptr;
        }
        auto it = sessions_.find(*id);
        if (it == sessions_.end()) {
            e = err("test.unknown_session", "no session " + *id);
            return nullptr;
        }
        return it->second.get();
    }

    // Runs `fn` on the session's worker and waits for it. Only ever called when no run is in flight,
    // so the job does not queue behind a long run.
    [[nodiscard]] static bool run_on_worker(DriverSession& s, std::function<void()> fn) {
        std::future<rt::JobOutcome> f = s.pool->submit(read_job(std::move(fn)));
        if (f.wait_for(kJobTimeout) != std::future_status::ready) return false;
        return !f.get().faulted;
    }

    [[nodiscard]] static Value event_json(LoggedEvent const& e) {
        return obj({{"seq", num(static_cast<double>(e.seq))},
                    {"run_id", str(e.run_id)},
                    {"kind", str(std::string(kind_name(e.kind)))},
                    {"payload", e.payload}});
    }
    [[nodiscard]] static Value events_json(std::vector<LoggedEvent> const& events) {
        std::vector<Value> out;
        out.reserve(events.size());
        for (LoggedEvent const& e : events) out.push_back(event_json(e));
        return arr(std::move(out));
    }
    [[nodiscard]] static Value pending_json(std::vector<PendingCall> const& pending) {
        std::vector<Value> out;
        for (PendingCall const& c : pending) {
            out.push_back(obj({{"interaction_id", str(c.interaction_id)},
                               {"call_id", str(c.call_id)},
                               {"tool_name", str(c.tool_name)},
                               {"arguments", str(c.arguments_json)},
                               {"needs_approval", boolean(c.needs_approval)}}));
        }
        return arr(std::move(out));
    }
    [[nodiscard]] static Value outcome_json(std::optional<RunOutcome> const& o) {
        if (!o) return Value{};
        return obj({{"run_id", str(o->run_id)}, {"ok", boolean(o->ok)}, {"error_code", str(o->error_code)},
                    {"error_message", str(o->error_message)}, {"text", str(o->text)}});
    }

    [[nodiscard]] Value snapshot(DriverSession& s, bool include_history) {
        run_state const st = s.monitor->state();
        Members m{
            {"session_id", str(s.id)},
            {"fixture", str(s.fixture.name)},
            {"state", str(std::string(run_state_name(st)))},
            {"last_outcome", outcome_json(s.monitor->last_outcome())},
            {"pending_approvals", pending_json(s.monitor->pending())},
            {"script_pending", num(static_cast<double>(s.client.pending()))},
            {"model_calls", num(static_cast<double>(s.client.call_count()))},
            {"last_seq", num(static_cast<double>(s.monitor->last_seq()))},
            {"events_dropped", num(static_cast<double>(s.monitor->dropped()))},
        };
        if (st == run_state::running) {
            m.emplace_back("partial", boolean(true));
            return obj(std::move(m));
        }
        std::size_t history_len = 0;
        std::vector<Value> history;
        Session* session = s.session.get();
        bool const ran = run_on_worker(s, [&] {
            history_len = session->history().size();
            if (include_history) {
                for (Message const& msg : session->history()) history.push_back(rt::message_to_json(msg));
            }
        });
        m.emplace_back("partial", boolean(!ran));
        m.emplace_back("history_length", num(static_cast<double>(history_len)));
        if (include_history && ran) m.emplace_back("history", arr(std::move(history)));
        return obj(std::move(m));
    }

    // ---- tools ----

    ToolResultJson t_fixtures_list(Value const&) {
        std::vector<Value> out;
        for (Fixture const& f : fixtures()) {
            std::vector<Value> tools;
            for (std::string const& t : f.tools) tools.push_back(str(t));
            out.push_back(obj({{"name", str(f.name)},
                               {"description", str(f.description)},
                               {"tools", arr(std::move(tools))},
                               {"suspend_for_approval", boolean(f.suspend_for_approval)}}));
        }
        return obj({{"fixtures", arr(std::move(out))}});
    }

    ToolResultJson t_session_start(Value const& args) {
        auto name = get_string(args, "fixture");
        if (!name) return err("test.bad_arguments", "fixture is required");
        Fixture const* fixture = find_fixture(*name);
        if (fixture == nullptr) return err("test.unknown_fixture", "no compiled-in fixture named " + *name);
        if (sessions_.size() >= kMaxSessions) {
            return err("test.too_many_sessions", "close a session first (max " + std::to_string(kMaxSessions) + ")");
        }

        auto ds = std::make_unique<DriverSession>();
        ds->id = "s" + std::to_string(next_session_++);
        ds->fixture = *fixture;
        Session& session = *ds->session;
        session.initialize(ds->id, Principal{"test-driver/" + fixture->name, "test"}, std::nullopt,
                           kMaxTurnsPerRun);
        ds->client = session.emplace_chat_client();
        session.set_capabilities(&ds->held);
        session.set_suspend_for_approval(fixture->suspend_for_approval);
        std::vector<ToolDescriptor> tools;
        for (ToolDescriptor const& d : all_test_tool_descriptors()) {
            if (std::find(fixture->tools.begin(), fixture->tools.end(), d.name) != fixture->tools.end())
                tools.push_back(d);
        }
        session.history_provider().set_tools(std::move(tools));
        // Installed once, before any job runs (ADR-182 §12 R1).
        std::shared_ptr<SessionMonitor> monitor = ds->monitor;
        session.set_run_event_tap([monitor](RunEvent const& ev) { monitor->on_event(ev); });

        DriverSession& ref = *ds;
        sessions_.emplace(ds->id, std::move(ds));
        return obj({{"session_id", str(ref.id)}, {"snapshot", snapshot(ref, false)}});
    }

    ToolResultJson t_model_script_push(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        if (s->monitor->state() == run_state::running) {
            return err("test.session_running",
                       "push turns only while the session is idle or suspended (keeps runs deterministic)");
        }
        Value const* turns = args.find("turns");
        if (turns == nullptr || !turns->is_array() || turns->as_array().empty()) {
            return err("test.bad_arguments", "turns must be a non-empty array");
        }
        std::vector<testing::ScriptedTurn> parsed;
        for (Value const& t : turns->as_array()) {
            if (!t.is_object()) return err("test.bad_arguments", "each turn must be an object");
            if (Value const* fe = t.find("error"); fe != nullptr) {
                failure_class klass = failure_class::fatal;
                std::string const k = get_string(*fe, "class").value_or("fatal");
                if (k == "transient") klass = failure_class::transient;
                else if (k == "contract") klass = failure_class::contract;
                else if (k != "fatal") return err("test.bad_arguments", "error.class must be transient|fatal|contract");
                parsed.push_back(testing::failure_turn(
                    error{klass, get_string(*fe, "message").value_or("scripted model failure"),
                          get_string(*fe, "code").value_or("test.scripted_failure")}));
                continue;
            }
            testing::ScriptedTurn turn;
            turn.message.role = role::assistant;
            turn.usage = Usage{1, 1, 0, 0, 0.0};
            if (auto text = get_string(t, "text"); text && !text->empty()) {
                ContentItem item;
                item.origin = content_origin::assistant;
                item.value = Text{*text};
                turn.message.content.push_back(std::move(item));
            }
            if (Value const* calls = t.find("tool_calls"); calls != nullptr) {
                if (!calls->is_array()) return err("test.bad_arguments", "tool_calls must be an array");
                for (Value const& c : calls->as_array()) {
                    auto name = get_string(c, "name");
                    if (!name) return err("test.bad_arguments", "each tool call needs a name");
                    std::string arguments = "{}";
                    if (Value const* a = c.find("arguments"); a != nullptr) {
                        arguments = a->is_string() ? a->as_string() : json::dump(*a);
                    }
                    ToolCall call;
                    call.call_id = get_string(c, "call_id").value_or("call_" + std::to_string(s->next_call_id++));
                    call.tool_name = *name;
                    call.arguments_json = std::move(arguments);
                    call.provenance = call_provenance::vendor_structured;
                    ContentItem item;
                    item.origin = content_origin::assistant;
                    item.value = std::move(call);
                    turn.message.content.push_back(std::move(item));
                }
            }
            if (turn.message.content.empty()) {
                return err("test.bad_arguments", "a turn needs text, tool_calls or error");
            }
            parsed.push_back(std::move(turn));
        }
        if (auto pushed = s->client.push(std::move(parsed)); !pushed) {
            return err(pushed.error().code, pushed.error().message);
        }
        return obj({{"script_pending", num(static_cast<double>(s->client.pending()))}});
    }

    ToolResultJson t_session_send(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        auto text = get_string(args, "text");
        if (!text) return err("test.bad_arguments", "text is required");
        run_state const st = s->monitor->state();
        if (st == run_state::running) return err("test.session_running", "a run is already in flight");
        if (st == run_state::suspended) {
            return err("test.session_suspended",
                       "resolve the open interaction (interaction_resolve) or session_cancel first");
        }
        s->action_mark = s->monitor->last_seq();
        s->monitor->set_state(run_state::running);  // before submit, so a wait never sees a stale idle
        (void)s->pool->submit(send_job(s, *text));
        return obj({{"started", boolean(true)}, {"since_seq", num(static_cast<double>(s->action_mark))}});
    }

    ToolResultJson t_session_wait_for(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        auto until = get_string(args, "until");
        if (!until) return err("test.bad_arguments", "until is required");
        std::uint64_t const since = get_u64(args, "since_seq").value_or(s->action_mark);
        std::uint64_t const timeout = std::min(get_u64(args, "timeout_ms").value_or(kDefaultWaitMs), kMaxWaitMs);

        SessionMonitor::Predicate pred;
        if (*until == "settled") {
            pred = [](run_state st, auto const&) { return st != run_state::running; };
        } else if (*until == "idle") {
            pred = [](run_state st, auto const&) { return st == run_state::idle; };
        } else if (*until == "suspended") {
            pred = [](run_state st, auto const&) { return st == run_state::suspended; };
        } else if (*until == "event") {
            auto kind = get_string(args, "kind");
            if (!kind) return err("test.bad_arguments", "until=event needs kind");
            auto tool_name = get_string(args, "tool_name");
            auto call_id = get_string(args, "call_id");
            pred = [kind = *kind, tool_name, call_id](run_state, std::vector<LoggedEvent> const& evs) {
                for (LoggedEvent const& ev : evs) {
                    if (kind_name(ev.kind) != kind) continue;
                    if (tool_name && get_string(ev.payload, "tool_name") != tool_name) continue;
                    if (call_id && get_string(ev.payload, "call_id") != call_id) continue;
                    return true;
                }
                return false;
            };
        } else {
            return err("test.bad_arguments", "until must be settled | idle | suspended | event");
        }
        bool const matched = s->monitor->wait(pred, since, std::chrono::milliseconds(timeout));
        return obj({{"matched", boolean(matched)},
                    {"timed_out", boolean(!matched)},
                    {"events", events_json(s->monitor->events_since(since, kMaxEventsPerResult))},
                    {"snapshot", snapshot(*s, false)}});
    }

    ToolResultJson t_session_snapshot(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        return snapshot(*s, get_bool(args, "include_history").value_or(false));
    }

    ToolResultJson t_session_events(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        std::uint64_t const since = get_u64(args, "since_seq").value_or(0);
        std::size_t const limit =
            static_cast<std::size_t>(std::min<std::uint64_t>(get_u64(args, "limit").value_or(kMaxEventsPerResult),
                                                             kMaxEventsPerResult));
        std::vector<std::string> kinds;
        if (Value const* k = args.find("kinds"); k != nullptr && k->is_array()) {
            for (Value const& v : k->as_array())
                if (v.is_string()) kinds.push_back(v.as_string());
        }
        return obj({{"events", events_json(s->monitor->events_since(since, limit, kinds))},
                    {"last_seq", num(static_cast<double>(s->monitor->last_seq()))},
                    {"events_dropped", num(static_cast<double>(s->monitor->dropped()))}});
    }

    ToolResultJson t_interaction_list(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        return obj({{"state", str(std::string(run_state_name(s->monitor->state())))},
                    {"pending", pending_json(s->monitor->pending())}});
    }

    ToolResultJson t_interaction_resolve(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        auto interaction_id = get_string(args, "interaction_id");
        auto decision = get_string(args, "decision");
        if (!interaction_id || !decision) return err("test.bad_arguments", "interaction_id and decision are required");
        if (*decision != "approve" && *decision != "deny") {
            return err("test.bad_arguments", "decision must be approve or deny");
        }
        if (args.find("call_ids") != nullptr) {
            return err("test.unsupported",
                       "per-call decisions need BUG-2's fix (ADR-182 §6 P1b); the decision applies to every "
                       "call in the interaction");
        }
        if (s->monitor->state() != run_state::suspended) {
            return err("test.not_suspended", "the session has no open interaction to resolve");
        }
        bool known = false;
        for (PendingCall const& c : s->monitor->pending())
            if (c.interaction_id == *interaction_id) known = true;
        if (!known) return err("test.unknown_interaction", "no open interaction " + *interaction_id);
        s->action_mark = s->monitor->last_seq();
        s->monitor->set_state(run_state::running);
        (void)s->pool->submit(resolve_job(s, *interaction_id, *decision == "approve"));
        return obj({{"resumed", boolean(true)}, {"since_seq", num(static_cast<double>(s->action_mark))}});
    }

    ToolResultJson t_session_cancel(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        run_state const st = s->monitor->state();
        s->action_mark = s->monitor->last_seq();
        if (st == run_state::running) {
            s->session->cancel();  // thread-safe (stop source under cancel_mutex_)
            return obj({{"canceled", boolean(true)}, {"was", str("running")}});
        }
        if (st == run_state::suspended) {
            s->monitor->set_state(run_state::running);
            (void)s->pool->submit(cancel_suspended_job(s));
            return obj({{"canceled", boolean(true)}, {"was", str("suspended")}});
        }
        return obj({{"canceled", boolean(false)}, {"was", str("idle")}});
    }

    ToolError close_session(std::string const& id) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return err("test.unknown_session", "no session " + id);
        DriverSession& s = *it->second;
        run_state const st = s.monitor->state();
        if (st == run_state::running) s.session->cancel();
        if (st == run_state::suspended) {
            s.monitor->set_state(run_state::running);
            (void)s.pool->submit(cancel_suspended_job(&s));
        }
        sessions_.erase(it);  // ~DriverSession: the pool finishes queued jobs first (§12 R10)
        return ToolError{};
    }

    ToolResultJson t_session_close(Value const& args) {
        auto id = get_string(args, "session_id");
        if (!id) return err("test.bad_arguments", "session_id is required");
        ToolError const e = close_session(*id);
        if (!e.code.empty()) return e;
        return obj({{"closed", boolean(true)}});
    }

    ToolResultJson t_model_requests(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        std::size_t const since = static_cast<std::size_t>(get_u64(args, "since_index").value_or(0));
        std::vector<ChatRequest> const reqs = s->client.requests();
        std::vector<Value> out;
        for (std::size_t i = since; i < reqs.size() && out.size() < 50; ++i) {
            std::vector<Value> messages;
            for (Message const& m : reqs[i].messages) {
                std::vector<Value> calls;
                for (ToolCall const& c : tool_calls_of(m)) {
                    calls.push_back(obj({{"call_id", str(c.call_id)}, {"tool_name", str(c.tool_name)},
                                         {"arguments", str(c.arguments_json)}}));
                }
                std::string results;
                for (ContentItem const& c : m.content) {
                    if (auto const* r = std::get_if<ToolResult>(&c.value)) {
                        results += "[" + r->call_id + (r->is_error ? " error] " : "] ") + content_text(r->content);
                    }
                }
                Members mm{{"role", str(std::string(rt::role_to_wire_string(m.role)))},
                           {"text", str(content_text(m.content))}};
                if (!calls.empty()) mm.emplace_back("tool_calls", arr(std::move(calls)));
                if (!results.empty()) mm.emplace_back("tool_results", str(std::move(results)));
                messages.push_back(obj(std::move(mm)));
            }
            std::vector<Value> tools;
            for (ToolDescriptor const& t : reqs[i].tools) tools.push_back(str(std::string(t.name)));
            out.push_back(obj({{"index", num(static_cast<double>(i))},
                               {"messages", arr(std::move(messages))},
                               {"tools", arr(std::move(tools))}}));
        }
        return obj({{"requests", arr(std::move(out))}, {"total", num(static_cast<double>(reqs.size()))}});
    }

    std::map<std::string, std::unique_ptr<DriverSession>, std::less<>> sessions_;
    std::uint64_t next_session_ = 1;
};

}  // namespace agentengine::test_driver
