#pragma once
// Implements ADR-182 (decisions/ADR-182-agent-test-driver-mcp.md), driver phase 1 (§12's revised
// phase 2): the core of `agentengine_test_driver`, an MCP server over stdio that lets a test agent
// (a Claude Code sub-agent) drive in-process `rt::AgentSession`s with a scripted model.
//
// This header holds everything except the stdio loop, so tests drive it in-process through
// `Driver::handle_line()` (tests/testing/test_agentengine_test_driver.cpp). `agentengine_test_driver.cpp`
// is only the stdin/stdout pump.
//
// What phase 1 deliberately does NOT have (ADR-182 §12):
//   - fixture files (P4, added in §21): a fixture file is a 015 Agent document under a host-fixed
//     root, accepted only when the host's trust check passes (git-tracked and unmodified), resolved
//     only against the test tools below, and never able to grant a capability (§12 C-3);
//   - real tools, until ADR-208 (P5): the mediated `run_shell` and `read_sandbox_file`, confined to a
//     per-session scratch directory under a host-fixed root, recorded at the invoke seam, and replayed
//     offline by doubles that serve the recording (§12 C-2, R5);
//   - workflows, until ADR-210: `workflow_*` and `request_port_*` drive one `rt::WorkflowSupervisor` over a
//     compiled fixture graph, each agent step a hidden scripted session with an empty grant;
//   - scenario export, assert, fork, live mode (later phases).
//
// Threading (ADR-182 §3.6, §12 R1). The MCP thread never touches an `AgentSession` directly. Each
// session owns one `rt::ThreadPool(1)` and every call into the session is a job on it. The run-event
// tap may be called from the worker or from parallel-batch threads, so `SessionMonitor` is
// mutex-protected, never blocks, and never calls back into the session. While a run is in flight a
// snapshot is built from the monitor alone (`partial: true`).

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <limits>
#include <future>
#include <random>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/agent_yaml_compiler.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/core/tool_registry.hpp"
#include "agentengine/core/yaml_value.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/agent_workflow_executor.hpp"
#include "agentengine/rt/message_codec.hpp"
#include "agentengine/rt/thread_pool.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/testing/scripted_chat_client.hpp"
#include "agentengine/tools/read_sandbox_file.hpp"
// Only for `RunShellTool`, run_shell's declared shape (ADR-208 §2.3). Nothing here constructs a sandbox:
// that is `DriverConfig::sandbox_factory`'s job, so a binary that never sets it (the scenario runner)
// does not link the mediated shell at all (ADR-208 G6).
#include "backends/native_jail/session_shell_wiring.hpp"

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
inline constexpr std::size_t kMaxForkDepth = 8;
inline constexpr auto kJobTimeout = std::chrono::seconds(10);
// ADR-208 G1/G2: what one driver session may do with its real tools.
inline constexpr std::uint64_t kRealToolReadCapBytes = 1u << 20;   // FsRead size cap
inline constexpr std::uint64_t kRealToolQuotaBytes = 16u << 20;    // FsWrite quota
inline constexpr std::uint32_t kRealToolFileCap = 1024;            // FsWrite file count
inline constexpr std::size_t kMaxRealToolCalls = 64;               // per session
inline constexpr std::size_t kMaxRealToolResultBytes = 64u * 1024u;
inline constexpr std::chrono::milliseconds kRealToolWallClock{2000};  // per run_shell call

// ---- JSON helpers ----------------------------------------------------------------------------------

using json::Value;
using Members = std::vector<std::pair<std::string, Value>>;

[[nodiscard]] inline Value str(std::string s) { return Value::make_string(std::move(s)); }
[[nodiscard]] inline Value num(double n) { return Value::make_number(n); }
[[nodiscard]] inline Value boolean(bool b) { return Value::make_bool(b); }
[[nodiscard]] inline Value obj(Members m) { return Value::make_object(std::move(m)); }
[[nodiscard]] inline Value arr(std::vector<Value> a) { return Value::make_array(std::move(a)); }
// `o` (an object) with one more member appended.
[[nodiscard]] inline Value with_field(Value const& o, std::string key, Value v) {
    Members m = o.as_object();
    m.emplace_back(std::move(key), std::move(v));
    return obj(std::move(m));
}
[[nodiscard]] inline Value without_field(Value const& o, std::string_view key) {
    Members m;
    for (auto const& [k, v] : o.as_object())
        if (k != key) m.emplace_back(k, v);
    return obj(std::move(m));
}

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
    // A live fixture's model is a real provider (ADR-182 §3.3). Refused unless the driver was
    // started with --allow-live. Live runs are exploratory and never gating (022 §1).
    bool                     live = false;
    // File fixtures only (§21): the agent's instructions (sent as the session's static instructions)
    // and its declared limits. max_turns is capped at kMaxTurnsPerRun.
    std::string                  instructions{};
    std::optional<std::uint64_t> max_turns{};
    std::optional<std::uint64_t> token_budget{};
    std::string                  source = "compiled";  // "compiled" | "file"
};

[[nodiscard]] inline std::vector<Fixture> const& fixtures() {
    static std::vector<Fixture> const all{
        {"basic", "echo, gated_echo (needs approval) and fail. Gated calls suspend the run for approval.",
         {"echo", "gated_echo", "fail"}, true},
        {"no_tools", "No tools at all: the model can only answer in text.", {}, true},
        {"basic_live",
         "Like basic, but a REAL model answers (live mode; needs --allow-live). Every call is recorded.",
         {"echo", "gated_echo", "fail"}, true, true},
        {"no_tools_live", "Like no_tools, with a REAL model (live mode; needs --allow-live).", {}, true, true},
        {"shell",
         "run_shell (the mediated shell, confined to this session's own scratch directory; needs the driver "
         "started with --sandbox-root), read_sandbox_file and echo. Real-tool calls are recorded and replay "
         "offline (ADR-208).",
         {"run_shell", "read_sandbox_file", "echo"}, true},
    };
    return all;
}

[[nodiscard]] inline Fixture const* find_fixture(std::string_view name) {
    for (Fixture const& f : fixtures())
        if (f.name == name) return &f;
    return nullptr;
}

// Fixture and scenario names are file names under a host-fixed root: nothing path-shaped (§12 C-4).
[[nodiscard]] inline bool valid_scenario_name(std::string_view n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n) {
        bool const ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// ---- Real tools (ADR-208, P5) ------------------------------------------------------------------------
//
// A session whose fixture names a real tool gets the grant below and nothing else (I2: derived by the driver,
// never declared by a fixture), plus, when recording, a sandbox from `DriverConfig::sandbox_factory` over its
// own scratch directory. Every real-tool call passes through `recording_tool`, which captures its arguments,
// its outcome and the `sandbox_exec_*` events it emitted. A replay builds `double_tool`s instead: same
// descriptor, no sandbox, and each call must match the recording or the replay fails (like C8).

[[nodiscard]] inline bool is_real_tool(std::string_view n) { return n == "run_shell" || n == "read_sandbox_file"; }

// The real tools' declared shape (name, description, schema, ceiling, approval, effect class). The
// read_sandbox_file entry is the real tool: it acts only through `ctx.sandbox_fs`. run_shell's invoke is the
// unreachable stub; the live descriptor comes from the session's sandbox.
[[nodiscard]] inline std::vector<ToolDescriptor> real_tool_standins() {
    return {make_tool_descriptor<RunShellTool>(), make_tool_descriptor<tools::ReadSandboxFile>()};
}

[[nodiscard]] inline CapabilitySet real_tool_grant() {
    return CapabilitySet::grant_root(
        {Capability{cap::FsRead{std::string(kShellWorkMount), "", kRealToolReadCapBytes}},
         Capability{cap::FsWrite{std::string(kShellWorkMount), "", kRealToolQuotaBytes, kRealToolFileCap}}});
}

// Admission binds each declared requirement with `contains()`, and an uncapped declaration never fits under a
// capped grant (capability.hpp: a capped parent and an uncapped request read as widening). run_shell declares
// uncapped `FsRead<"work">`/`FsWrite<"work">`, so the driver narrows a real tool's declared ceiling to exactly
// the capped grant above: narrower than the tool's own declaration, never wider (I2). The shell then enforces
// the quota and size cap from the held grant (its gap-12 lookups). ADR-208 §7.
[[nodiscard]] inline ToolDescriptor with_granted_ceiling(ToolDescriptor d) {
    for (Capability& c : d.capability_ceiling) {
        if (auto const* r = std::get_if<cap::FsRead>(&c); r != nullptr && r->mount_id == kShellWorkMount) {
            c = Capability{cap::FsRead{r->mount_id, r->path_prefix, kRealToolReadCapBytes}};
        } else if (auto const* w = std::get_if<cap::FsWrite>(&c); w != nullptr && w->mount_id == kShellWorkMount) {
            c = Capability{cap::FsWrite{w->mount_id, w->path_prefix, kRealToolQuotaBytes, kRealToolFileCap}};
        }
    }
    return d;
}

// One session's sandbox, as the driver sees it. Built only by `DriverConfig::sandbox_factory`.
class RealToolSandbox {
public:
    virtual ~RealToolSandbox() = default;
    [[nodiscard]] virtual ToolDescriptor run_shell() = 0;
    [[nodiscard]] virtual FileSystemAdapter* filesystem() = 0;
};
using RealToolSandboxFactory = std::function<result<std::shared_ptr<RealToolSandbox>>(std::filesystem::path const&)>;

struct ExecEventRecord {
    run_event_kind kind = run_event_kind::sandbox_exec_started;
    std::string    backend;
    std::string    stage;
    bool           ok = true;
    std::string    error_code;
};

// One real-tool call as recorded, and as a double serves it back.
struct ToolExchange {
    std::string                  tool_name;
    std::string                  arguments;  // json::dump of the call's arguments
    bool                         is_error = false;
    Value                        result;     // when !is_error
    failure_class                klass = failure_class::fatal;
    std::string                  code;
    std::string                  message;
    std::vector<ExecEventRecord> exec_events;
};

struct ToolMismatch {
    std::size_t call_index = 0;
    std::string expected;
    std::string actual;
};

// Record mode: the exchanges so far. Replay mode: the recording and how far it has been served. Called from
// the session's worker; the monitor's reads come from the MCP thread, hence the mutex.
class RealToolLog {
public:
    explicit RealToolLog(std::optional<std::vector<ToolExchange>> doubles) : doubles_(std::move(doubles)) {}

    [[nodiscard]] bool replaying() const { return doubles_.has_value(); }
    [[nodiscard]] std::size_t calls() const {
        std::lock_guard lock(mu_);
        return replaying() ? served_ : recorded_.size();
    }
    void record(ToolExchange x) {
        std::lock_guard lock(mu_);
        recorded_.push_back(std::move(x));
    }
    [[nodiscard]] std::vector<ToolExchange> recorded() const {
        std::lock_guard lock(mu_);
        return recorded_;
    }
    [[nodiscard]] std::size_t expected() const { return doubles_ ? doubles_->size() : 0; }
    [[nodiscard]] std::optional<ToolMismatch> mismatch() const {
        std::lock_guard lock(mu_);
        return mismatch_;
    }
    // The recorded exchange for the next call, or why there is none. A mismatch is terminal, like C8's.
    [[nodiscard]] std::variant<ToolExchange, error> serve(std::string const& tool, std::string const& arguments) {
        std::lock_guard lock(mu_);
        std::size_t const n = served_++;
        auto diverged = [&](std::string expected) {
            if (!mismatch_) mismatch_ = ToolMismatch{n, std::move(expected), tool + " " + arguments};
            return error{failure_class::contract,
                         "test.replay_mismatch at tool call " + std::to_string(n) + ": the call differs from the recording",
                         "test.replay_mismatch"};
        };
        if (mismatch_) return diverged("(an earlier tool call already diverged)");
        if (n >= doubles_->size()) return diverged("(no recorded call)");
        ToolExchange const& x = (*doubles_)[n];
        if (x.tool_name != tool || x.arguments != arguments) return diverged(x.tool_name + " " + x.arguments);
        return x;
    }

private:
    mutable std::mutex                        mu_;
    std::optional<std::vector<ToolExchange>>  doubles_;
    std::vector<ToolExchange>                 recorded_;
    std::size_t                               served_ = 0;
    std::optional<ToolMismatch>               mismatch_;
};

[[nodiscard]] inline bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        auto const c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
        else return false;
        for (std::size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size()) return false;
            auto const cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        std::uint32_t const min = n == 1 ? 0x80 : n == 2 ? 0x800 : 0x10000;
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n + 1;
    }
    return true;
}

// Record mode (ADR-208 §2.4, G1, G2, M1): refuses once the session is cancelled or its call cap is spent,
// captures the call's sandbox_exec_* events at the sink, replaces an oversized or non-UTF-8 result with an
// error before the model or any event sees it, and records the outcome the session actually got.
[[nodiscard]] inline ToolDescriptor recording_tool(ToolDescriptor d, std::shared_ptr<RealToolLog> log,
                                                   std::shared_ptr<RealToolSandbox> keep_alive) {
    ToolDescriptor::InvokeFn inner = std::move(d.invoke);
    std::string const name = d.name;
    d.invoke = [inner = std::move(inner), log, keep_alive, name](Value const& args, EffectContext& ctx) -> result<Value> {
        ToolExchange x;
        x.tool_name = name;
        x.arguments = json::dump(args);
        result<Value> out = [&]() -> result<Value> {
            if (ctx.cancellation.stop_requested()) {
                return std::unexpected(error{failure_class::resource,
                                             "the session was cancelled; no further real-tool calls run",
                                             "test.real_tool_canceled"});
            }
            if (log->calls() >= kMaxRealToolCalls) {
                return std::unexpected(error{failure_class::resource,
                                             "this session has used all " + std::to_string(kMaxRealToolCalls) +
                                                 " of its real-tool calls",
                                             "test.real_tool_call_cap"});
            }
            auto previous = ctx.sandbox_exec_sink;
            ctx.sandbox_exec_sink = [&x, &previous](run_event_kind k, run_event_payload::SandboxExec p) {
                x.exec_events.push_back(ExecEventRecord{k, p.backend, p.stage, p.ok, p.error_code});
                if (previous) previous(k, std::move(p));
            };
            result<Value> r = inner(args, ctx);
            ctx.sandbox_exec_sink = std::move(previous);
            return r;
        }();
        if (out) {
            std::string const text = json::dump(*out);
            if (text.size() > kMaxRealToolResultBytes) {
                out = std::unexpected(error{failure_class::resource,
                                            "the tool result is over " + std::to_string(kMaxRealToolResultBytes) +
                                                " bytes; read less at a time",
                                            "test.real_tool_output_too_large"});
            } else if (!valid_utf8(text)) {
                out = std::unexpected(error{failure_class::contract, "the tool result is not valid UTF-8 text",
                                            "test.real_tool_output_not_utf8"});
            }
        }
        if (out) {
            x.result = *out;
        } else {
            x.is_error = true;
            x.klass = out.error().klass;
            x.code = out.error().code;
            x.message = out.error().message;
        }
        log->record(std::move(x));
        return out;
    };
    return d;
}

// Replay mode (ADR-208 §2.5): the same descriptor, whose invoke serves the recording and touches nothing.
[[nodiscard]] inline ToolDescriptor double_tool(ToolDescriptor d, std::shared_ptr<RealToolLog> log) {
    std::string const name = d.name;
    d.invoke = [log, name](Value const& args, EffectContext& ctx) -> result<Value> {
        auto served = log->serve(name, json::dump(args));
        if (auto const* e = std::get_if<error>(&served)) return std::unexpected(*e);
        ToolExchange const& x = std::get<ToolExchange>(served);
        std::size_t k = 0;
        for (ExecEventRecord const& ev : x.exec_events) {
            run_event_payload::SandboxExec p;
            p.exec_id = "double_" + std::to_string(++k);
            p.backend = ev.backend;
            p.stage = ev.stage;
            p.ok = ev.ok;
            p.error_code = ev.error_code;
            ctx.sandbox_exec_sink(ev.kind, std::move(p));
        }
        if (x.is_error) return std::unexpected(error{x.klass, x.message, x.code});
        return x.result;
    };
    return d;
}

// Removes `path` and everything under it without following a link (ADR-208 G7): a symbolic link or junction
// is removed as an entry, never descended into. On Windows the walk uses the \\?\ form, so a deep tree the
// mediated shell made (it uses that form too) is not left behind by MAX_PATH. Returns false if anything is left.
[[nodiscard]] inline bool remove_tree_no_follow(std::filesystem::path path) {
    std::error_code ec;
#ifdef _WIN32
    path = std::filesystem::absolute(path, ec);
    if (ec) return false;
    if (!path.native().starts_with(L"\\\\?\\")) path = std::filesystem::path(L"\\\\?\\" + path.native());
#endif
    auto const st = std::filesystem::symlink_status(path, ec);
    if (st.type() == std::filesystem::file_type::not_found) return true;
    if (ec) return false;
    bool ok = true;
    if (st.type() == std::filesystem::file_type::directory) {
        std::vector<std::filesystem::path> children;
        for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
            children.push_back(it->path());
        }
        if (ec) ok = false;
        for (std::filesystem::path const& c : children) ok = remove_tree_no_follow(c) && ok;
    }
    std::filesystem::remove(path, ec);
    return ok && !ec;
}

// ---- File fixtures (ADR-182 §21, P4) ------------------------------------------------------------------
//
// `<fixtures_root>/<name>.yaml` is a 015 Agent document, compiled by the engine's own
// `compile_agent_document`, so a driver fixture and a declarative agent mean the same thing (I6). What a
// file may decide: instructions, which of the driver's test tools the agent has, and its limits. What it
// may not: `spec.capabilities` is refused (a fixture never grants authority), `spec.tools` resolves only
// against the in-process test tools (an unknown name is refused, never skipped), and a name that is
// also a compiled-in fixture is refused. `x-test-driver: {suspend_for_approval: bool}` is the only
// driver extension. Live file fixtures are not supported.

struct FixtureLoad {
    std::optional<Fixture> fixture;
    std::string            code;  // on failure: test.unknown_fixture | test.fixture_untrusted | test.bad_fixture
    std::string            message;
};

[[nodiscard]] inline ToolRegistry const& test_tool_registry();  // defined after the test tools' table

[[nodiscard]] inline FixtureLoad load_file_fixture(std::filesystem::path const& root, std::string const& name,
                                                  std::function<result<std::string>(std::filesystem::path const&)> const& reader) {
    auto fail = [&](std::string code, std::string message) {
        return FixtureLoad{std::nullopt, std::move(code), std::move(message)};
    };
    if (root.empty() || !valid_scenario_name(name)) return fail("test.unknown_fixture", "no fixture named " + name);
    if (find_fixture(name) != nullptr) return fail("test.bad_fixture", name + " is a compiled-in fixture name");
    std::filesystem::path const path = root / (name + ".yaml");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return fail("test.unknown_fixture", "no fixture named " + name);
    std::string bytes;
    if (reader) {
        result<std::string> got = reader(path);
        if (!got) {
            return fail("test.fixture_untrusted",
                        "fixture file " + name + ".yaml is refused: " + got.error().message +
                            " (a fixture decides a session's tools and instructions, so only a committed file is used)");
        }
        bytes = std::move(*got);
    } else {
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec))) {
            return fail("test.fixture_untrusted", "fixture file " + name + ".yaml is a symbolic link");
        }
        std::ifstream in(path, std::ios::binary);
        std::stringstream buf;
        buf << in.rdbuf();
        bytes = buf.str();
    }
    auto doc = yaml::parse(bytes);
    if (!doc) return fail("test.bad_fixture", name + ".yaml is not valid YAML: " + doc.error().message);
    if (get_string(*doc, "kind") != "Agent") return fail("test.bad_fixture", name + ".yaml must be kind: Agent");
    Value const* spec = doc->find("spec");
    if (spec != nullptr && !spec->is_object()) return fail("test.bad_fixture", name + ".yaml: spec must be a mapping");
    if (spec != nullptr) {
        // §22: a 015 field the driver does not apply is refused, not silently ignored (I6: the same
        // document must mean the same thing here as in the engine).
        for (auto const& [k, v] : spec->as_object()) {
            if (k != "instructions" && k != "tools" && k != "limits" && k != "capabilities") {
                return fail("test.bad_fixture", name + ".yaml: spec." + k + " is not supported by the test driver");
            }
        }
        if (Value const* ins = spec->find("instructions"); ins != nullptr && !ins->is_string()) {
            return fail("test.bad_fixture", name + ".yaml: spec.instructions must be text");
        }
        if (Value const* tools = spec->find("tools"); tools != nullptr && !tools->is_array()) {
            return fail("test.bad_fixture", name + ".yaml: spec.tools must be a list of tool names");
        }
        if (Value const* caps = spec->find("capabilities");
            caps != nullptr && !(caps->is_null() || (caps->is_object() && caps->as_object().empty()) ||
                                 (caps->is_array() && caps->as_array().empty()))) {
            return fail("test.bad_fixture", name + ".yaml declares spec.capabilities; a fixture never grants capabilities");
        }
        if (Value const* tools = spec->find("tools"); tools != nullptr && tools->is_array()) {
            for (Value const& t : tools->as_array()) {
                if (!t.is_string()) return fail("test.bad_fixture", name + ".yaml: spec.tools entries must be tool names");
            }
        }
    }
    auto meta = compile_agent_document(*doc, &test_tool_registry());
    if (!meta) return fail("test.bad_fixture", name + ".yaml does not compile: " + meta.error().message);

    Fixture f;
    f.name = name;
    f.description = meta->agent_description.empty() ? "(file fixture)" : meta->agent_description;
    for (ToolDescriptor const& d : meta->tools.descriptors()) f.tools.push_back(d.name);
    f.instructions = meta->agent_instructions;
    if (spec != nullptr && spec->find("limits") != nullptr) {
        if (spec->find("limits")->find("max_turns") != nullptr) f.max_turns = meta->max_turns;
        f.token_budget = meta->token_budget;
    }
    if (Value const* ext = doc->find("x-test-driver"); ext != nullptr) {
        if (!ext->is_object()) return fail("test.bad_fixture", name + ".yaml: x-test-driver must be a mapping");
        for (auto const& [k, v] : ext->as_object()) {
            if (k == "suspend_for_approval" && v.is_bool()) {
                f.suspend_for_approval = v.as_bool();
            } else {
                return fail("test.bad_fixture", name + ".yaml: unknown or mistyped x-test-driver key '" + k + "'");
            }
        }
    }
    f.source = "file";
    return FixtureLoad{std::move(f), {}, {}};
}

// The name-keyed registry a file fixture's spec.tools resolves against: the test tools and the real tools'
// stand-ins (ADR-208), nothing else. Naming a real tool is how a fixture asks for one; the driver decides
// the grant and the sandbox.
[[nodiscard]] inline ToolRegistry const& test_tool_registry() {
    static ToolRegistry const registry = [] {
        ToolRegistry r;
        for (ToolDescriptor const& d : all_test_tool_descriptors()) (void)r.register_tool(d.name, d, tool_provenance::native);
        for (ToolDescriptor const& d : real_tool_standins()) (void)r.register_tool(d.name, d, tool_provenance::native);
        return r;
    }();
    return registry;
}

// ---- The session's context provider: history plus the fixture's tools ------------------------------

class DriverHistoryProvider {
public:
    DriverHistoryProvider() = default;
    // `AgentSession::fork_from` copies the provider. A copy gets the test tools only: never the real tools,
    // their sandbox or its filesystem view, so no fork can alias a session's scratch (ADR-208 G3, ADR-096 C2).
    DriverHistoryProvider(DriverHistoryProvider const& o) : tools_(o.tools_) {}
    DriverHistoryProvider& operator=(DriverHistoryProvider const& o) {
        if (this != &o) {
            tools_ = o.tools_;
            real_tools_.clear();
            sandbox_fs_ = nullptr;
            sandbox_.reset();
        }
        return *this;
    }
    DriverHistoryProvider(DriverHistoryProvider&&) = default;
    DriverHistoryProvider& operator=(DriverHistoryProvider&&) = default;

    void set_tools(std::vector<ToolDescriptor> tools) { tools_ = std::move(tools); }
    void set_real_tools(std::vector<ToolDescriptor> tools, std::shared_ptr<RealToolSandbox> sandbox) {
        real_tools_ = std::move(tools);
        sandbox_ = std::move(sandbox);
        sandbox_fs_ = sandbox_ ? sandbox_->filesystem() : nullptr;
    }
    [[nodiscard]] bool has_real_tools() const { return !real_tools_.empty() || sandbox_ != nullptr; }

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext& ctx) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = tools_;
        c.tools.insert(c.tools.end(), real_tools_.begin(), real_tools_.end());
        if (sandbox_fs_ != nullptr) ctx.sandbox_fs = sandbox_fs_;
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    std::vector<ToolDescriptor>      tools_;
    std::vector<ToolDescriptor>      real_tools_;
    std::shared_ptr<RealToolSandbox> sandbox_;
    FileSystemAdapter*               sandbox_fs_ = nullptr;
};

// ---- The model behind a session: scripted (default) or live (ADR-182 §3.3) ---------------------------
//
// A small type-erased seam so one `Session` type serves both modes. Virtual dispatch is fine here: this
// is a test tool under tools/, not the engine's hot path (CONVENTIONS.md's rule is about the engine).

class ModelBackend {
public:
    virtual ~ModelBackend() = default;
    [[nodiscard]] virtual ChatClientCapabilities capabilities() const = 0;
    [[nodiscard]] virtual task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext& ctx) = 0;
    [[nodiscard]] virtual stream<ChatResponseUpdate> chat_stream(ChatRequest const& request, EffectContext& ctx) = 0;
};

class ScriptedBackend final : public ModelBackend {
public:
    explicit ScriptedBackend(testing::ScriptedChatClient client) : client_(std::move(client)) {}
    [[nodiscard]] ChatClientCapabilities capabilities() const override { return client_.capabilities(); }
    [[nodiscard]] task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext& ctx) override {
        return client_.chat(request, ctx);
    }
    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest const& request, EffectContext& ctx) override {
        return client_.chat_stream(request, ctx);
    }

private:
    testing::ScriptedChatClient client_;
};

// Every request the engine sends the model, in both modes, for `model_requests`. Bounded (ADR-182 §12
// R9): the oldest entries are dropped past kMaxCapturedRequests; `total` keeps counting.
inline constexpr std::size_t kMaxCapturedRequests = 64;

class RequestLog {
public:
    void record(ChatRequest const& r) {
        std::lock_guard lock(mutex_);
        requests_.push_back(r);
        ++total_;
        if (requests_.size() > kMaxCapturedRequests) requests_.pop_front();
    }
    // (index of the first kept request, the kept requests)
    [[nodiscard]] std::pair<std::size_t, std::vector<ChatRequest>> snapshot() const {
        std::lock_guard lock(mutex_);
        return {total_ - requests_.size(), std::vector<ChatRequest>(requests_.begin(), requests_.end())};
    }
    [[nodiscard]] std::size_t total() const {
        std::lock_guard lock(mutex_);
        return total_;
    }

private:
    mutable std::mutex        mutex_;
    std::deque<ChatRequest>   requests_;
    std::size_t               total_ = 0;
};

// ---- What the engine asked the model: a canonical summary and its digest (ADR-182 C8, §19) ----------
//
// The summary is what the model is shown: every message's role and items (text, reasoning, tool calls
// with their ids and raw arguments, tool results with their text) and every offered tool's name,
// description and argument schema. It leaves out what differs by construction between a live session
// and its scripted replay (the client's sampling options, output-token limit, idempotency key) and
// content flags the model never sees (origin, taint). Ids that carry the driver's session id are
// normalized, so a replay in another session matches. The digest is FNV-1a 64 over the summary's JSON:
// deterministic and portable (std::hash is neither), and collisions only need to be unlikely, not
// adversarially hard -- a scenario is a checked-in test file, not an authority.

[[nodiscard]] inline Value normalize_ids(Value const& v, std::string const& session_id);  // defined below
[[nodiscard]] inline std::string content_text(std::vector<ContentItem> const& items);  // defined below

// One content list, item by item (a tool result's own content recursively), so "error: x" as text and an
// Error item, or ["a","b"] and ["ab"], digest differently (§22).
[[nodiscard]] inline Value summary_items(std::vector<ContentItem> const& content) {
    std::vector<Value> items;
    for (ContentItem const& c : content) {
        if (auto const* t = std::get_if<Text>(&c.value)) {
            items.push_back(obj({{"text", str(t->text)}}));
        } else if (auto const* rs = std::get_if<Reasoning>(&c.value)) {
            items.push_back(obj({{"reasoning", str(rs->text)}}));
        } else if (auto const* call = std::get_if<ToolCall>(&c.value)) {
            items.push_back(obj({{"tool_call", obj({{"call_id", str(call->call_id)},
                                                    {"name", str(call->tool_name)},
                                                    {"arguments", str(call->arguments_json)}})}}));
        } else if (auto const* res = std::get_if<ToolResult>(&c.value)) {
            items.push_back(obj({{"tool_result", obj({{"call_id", str(res->call_id)},
                                                      {"is_error", boolean(res->is_error)},
                                                      {"content", summary_items(res->content)}})}}));
        } else if (auto const* data = std::get_if<Data>(&c.value)) {
            items.push_back(obj({{"data", str(data->json)}, {"schema_id", str(data->schema_id.value_or(""))}}));
        } else if (auto const* e = std::get_if<Error>(&c.value)) {
            items.push_back(obj({{"error", str(e->message)}}));
        } else if (auto const* cu = std::get_if<Custom>(&c.value)) {
            items.push_back(obj({{"custom", str(cu->type_id)}, {"payload", str(cu->payload_json)}}));
        } else {
            // Media and Citation: named by kind only. No driver tool or scripted turn produces them (§22).
            items.push_back(obj({{"other", num(static_cast<double>(c.value.index()))}}));
        }
    }
    return arr(std::move(items));
}

[[nodiscard]] inline Value request_summary(ChatRequest const& r, std::string const& session_id) {
    std::vector<Value> messages;
    for (Message const& m : r.messages) {
        messages.push_back(obj({{"role", str(std::string(rt::role_to_wire_string(m.role)))}, {"items", summary_items(m.content)}}));
    }
    std::vector<Value> tools;
    for (ToolDescriptor const& t : r.tools) {
        tools.push_back(obj({{"name", str(t.name)}, {"description", str(t.description)}, {"schema", str(t.args_schema_json)}}));
    }
    return normalize_ids(obj({{"messages", arr(std::move(messages))},
                              {"tools", arr(std::move(tools))},
                              {"output_schema", str(r.output_schema_json.value_or(""))}}),
                         session_id);
}

[[nodiscard]] inline std::string digest_of(std::string_view text) {
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    return "fnv1a64:" + std::string(hex);
}

[[nodiscard]] inline std::string request_digest(ChatRequest const& r, std::string const& session_id) {
    return digest_of(json::dump(request_summary(r, session_id)));
}

// The request digests a replay expects, one per scripted turn, in the same FIFO order as the script.
// A turn pushed without one checks nothing. The first mismatch fails that model call with
// test.replay_mismatch and is kept for the snapshot, so a replay can name the call that diverged.
struct RequestMismatch {
    std::size_t call_index = 0;  // 0-based, over every model call this session made
    std::string expected;
    std::string actual;
    Value       actual_request;  // the summary, for the report
};

class RequestExpectations {
public:
    void push(std::vector<std::optional<std::string>> digests) {
        std::lock_guard lock(mutex_);
        for (auto& d : digests) expected_.push_back(std::move(d));
    }
    // Called once per model call. Returns the error to fail the call with, or nothing.
    [[nodiscard]] std::optional<error> check(ChatRequest const& r, std::string const& session_id) {
        std::lock_guard lock(mutex_);
        std::size_t const index = calls_++;
        // §22: once diverged, every later call fails too. The mismatched turn was not consumed, so the
        // digest queue and the script would otherwise fall out of step and later calls go unchecked.
        if (mismatch_) {
            return error{failure_class::contract,
                         "model call " + std::to_string(index) + ": this session already diverged from the recording at call " +
                             std::to_string(mismatch_->call_index),
                         "test.replay_mismatch"};
        }
        if (expected_.empty()) return std::nullopt;
        std::optional<std::string> want = std::move(expected_.front());
        expected_.pop_front();
        if (!want) return std::nullopt;
        ++checked_;
        std::string const got = request_digest(r, session_id);
        if (got == *want) return std::nullopt;
        if (!mismatch_) mismatch_ = RequestMismatch{index, *want, got, request_summary(r, session_id)};
        return error{failure_class::contract,
                     "model call " + std::to_string(index) + ": the engine's request differs from the recording (expected " +
                         *want + ", got " + got + ")",
                     "test.replay_mismatch"};
    }
    [[nodiscard]] std::optional<RequestMismatch> mismatch() const {
        std::lock_guard lock(mutex_);
        return mismatch_;
    }
    [[nodiscard]] std::size_t checked() const {
        std::lock_guard lock(mutex_);
        return checked_;
    }

private:
    mutable std::mutex                       mutex_;
    std::deque<std::optional<std::string>>   expected_;
    std::size_t                              calls_ = 0;
    std::size_t                              checked_ = 0;
    std::optional<RequestMismatch>           mismatch_;
};

// Every answer the model gave (or the failure in its place), in call order. This is what
// `scenario_export` turns into the replay script: a scenario replays the model's OBSERVED behaviour
// through the scripted client, whether the session was scripted or live (ADR-182 §16).
inline constexpr std::size_t kMaxExchanges = 256;

struct ModelExchange {
    std::optional<ChatResponse> response;
    std::optional<error>        failure;
    std::string                 request_digest;  // of the request this exchange answered (C8)
};

class ExchangeLog {
public:
    void record(result<ChatResponse> const& r, std::string request_digest) {
        std::lock_guard lock(mutex_);
        if (exchanges_.size() >= kMaxExchanges) {
            overflow_ = true;
            return;
        }
        ModelExchange x;
        if (r) x.response = *r;
        else x.failure = r.error();
        x.request_digest = std::move(request_digest);
        exchanges_.push_back(std::move(x));
    }
    // A streamed call's answer is not captured, so a session that made one cannot be exported.
    void mark_unrecordable() {
        std::lock_guard lock(mutex_);
        unrecordable_ = true;
    }
    [[nodiscard]] std::vector<ModelExchange> exchanges() const {
        std::lock_guard lock(mutex_);
        return exchanges_;
    }
    [[nodiscard]] bool overflow() const {
        std::lock_guard lock(mutex_);
        return overflow_;
    }
    [[nodiscard]] bool unrecordable() const {
        std::lock_guard lock(mutex_);
        return unrecordable_;
    }

private:
    mutable std::mutex         mutex_;
    std::vector<ModelExchange> exchanges_;
    bool                       overflow_ = false;
    bool                       unrecordable_ = false;
};

class DriverChatClient {
public:
    DriverChatClient() = default;
    DriverChatClient(std::shared_ptr<ModelBackend> backend, std::shared_ptr<RequestLog> log,
                     std::shared_ptr<ExchangeLog> exchanges, std::shared_ptr<RequestExpectations> expectations,
                     std::string session_id)
        : backend_(std::move(backend)),
          log_(std::move(log)),
          exchanges_(std::move(exchanges)),
          expectations_(std::move(expectations)),
          session_id_(std::move(session_id)) {}

    [[nodiscard]] ChatClientCapabilities capabilities() const { return backend_->capabilities(); }
    [[nodiscard]] task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext& ctx) const {
        log_->record(request);
        if (std::optional<error> mismatch = expectations_->check(request, session_id_)) {
            // A diverged replay is not a model answer: nothing is recorded, and the session cannot be exported.
            exchanges_->mark_unrecordable();
            co_return std::unexpected(std::move(*mismatch));
        }
        std::string digest = request_digest(request, session_id_);
        result<ChatResponse> r = co_await backend_->chat(request, ctx);
        exchanges_->record(r, std::move(digest));
        co_return r;
    }
    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest const& request, EffectContext& ctx) const {
        log_->record(request);
        exchanges_->mark_unrecordable();
        if (std::optional<error> mismatch = expectations_->check(request, session_id_)) {
            auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
            pair.producer.fail(std::move(*mismatch));
            return std::move(pair.consumer);
        }
        return backend_->chat_stream(request, ctx);
    }

private:
    std::shared_ptr<ModelBackend>        backend_;
    std::shared_ptr<RequestLog>          log_;
    std::shared_ptr<ExchangeLog>         exchanges_;
    std::shared_ptr<RequestExpectations> expectations_;
    std::string                          session_id_;
};
static_assert(ChatClient<DriverChatClient>);

using Session = rt::AgentSession<DriverChatClient, rt::NoSessionState, DriverHistoryProvider>;

// The secret name a live backend's key is stored under. A live session's grant holds exactly this
// cap::Secret and nothing else; a scripted session's grant is empty.
inline constexpr std::string_view kLiveSecretName = "test_driver.live_model_key";

// Host-side configuration, fixed for the process lifetime (from the command line). No tool argument
// can change any of it (ADR-182 §12 C-3).
struct DriverConfig {
    // Builds a live backend; empty = live mode not enabled (the binary was started without
    // --allow-live, or was built without HTTPS).
    std::function<std::shared_ptr<ModelBackend>(std::string const& session_id)> live_backend_factory;
    std::string live_description;  // e.g. "deepseek-flash @ api.deepseek.com", for fixtures_list
    // Strings that must never appear in any output line (ADR-182 §12 R8, P6): the live key.
    std::vector<std::string> secret_canaries;
    // Where scenario_export writes and scenario_replay reads `<name>.json` (ADR-182 §12 C-4).
    // Empty = both tools disabled.
    std::filesystem::path scenarios_root;
    // Where file fixtures live, `<name>.yaml` (§21). Empty = compiled-in fixtures only.
    std::filesystem::path fixtures_root;
    // How a fixture file's bytes are obtained: returns them, or why the file is refused. The driver binary
    // wires git_committed_fixture (fixture_trust.hpp), which returns the COMMITTED blob, never the working
    // file (§22). Empty = read the working file (no symlinks), which only the scenario runner and tests use.
    std::function<result<std::string>(std::filesystem::path const&)> fixture_reader;
    // ADR-208: real tools. Both empty = a fixture naming a real tool is refused (test.real_tools_disabled).
    // The driver creates its own directory under `sandbox_root`, and one directory per session under that.
    std::filesystem::path  sandbox_root;
    RealToolSandboxFactory sandbox_factory;
    // Replay only, set by replay_scenario from the scenario file, never by a tool argument (ADR-208 G4): a
    // real-tool session serves these instead of building a sandbox.
    std::optional<std::vector<ToolExchange>> tool_doubles;
    // ADR-210: wraps each workflow step's scripted backend (the driver's tests force an interleaving with it).
    // Host-only, like the rest of this struct; empty in the driver binary.
    std::function<std::shared_ptr<ModelBackend>(std::string const& executor_id, std::shared_ptr<ModelBackend>)>
        workflow_backend_wrapper;
};

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
        case run_event_kind::delegated_event: return "delegated_event";  // ADR-193
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
                // ADR-198: `stage` only when set, so a failure whose code says it all keeps its old shape.
                if (x.stage.empty()) return obj({{"error_code", str(x.error_code)}, {"message", str(x.message)}});
                return obj({{"error_code", str(x.error_code)}, {"message", str(x.message)}, {"stage", str(x.stage)}});
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
            [](rp::InteractionRef const& x) {
                // ADR-196 §7: the resolver only when named, so an anonymous resolve keeps its old shape.
                if (x.approver_id.empty()) return obj({{"interaction_id", str(x.interaction_id)}});
                return obj({{"interaction_id", str(x.interaction_id)}, {"approver_id", str(x.approver_id)}});
            },
            [](rp::ApprovalRequested const& x) {
                return obj({{"call_id", str(x.call_id)}, {"interaction_id", str(x.interaction_id)},
                            {"tool_name", str(x.tool_name)}, {"arguments", str(x.arguments_json)},
                            {"needs_approval", boolean(x.needs_approval)}});
            },
            [](rp::ApprovalResolved const& x) {
                // ADR-196: `approver_id` only when the host named one (an anonymous decision keeps the old shape).
                if (x.approver_id.empty()) {
                    return obj({{"call_id", str(x.call_id)}, {"approved", boolean(x.approved)},
                                {"interaction_id", str(x.interaction_id)}});
                }
                return obj({{"call_id", str(x.call_id)}, {"approved", boolean(x.approved)},
                            {"interaction_id", str(x.interaction_id)}, {"approver_id", str(x.approver_id)}});
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
            // ADR-193: a delegated agent's event carried in this run -- the inner event is reported as it is.
            [](rp::DelegatedEvent const& x) {
                if (!x.inner) return obj({{"child_run_id", str(x.child_run_id)}, {"depth", num(x.depth)}});
                return obj({{"child_run_id", str(x.child_run_id)},
                            {"depth", num(x.depth)},
                            {"kind", num(static_cast<double>(x.inner->kind))},
                            {"payload", payload_json(x.inner->payload)}});
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
    // Scripted sessions only: the script queue (shares state with the backend's copy).
    std::optional<testing::ScriptedChatClient> script;
    std::shared_ptr<RequestLog>      requests = std::make_shared<RequestLog>();
    std::shared_ptr<ExchangeLog>     exchanges = std::make_shared<ExchangeLog>();
    std::shared_ptr<RequestExpectations> expectations = std::make_shared<RequestExpectations>();
    // The user-side steps, in order, for scenario_export (ADR-182 §16). Interaction ids are stored
    // normalized (`<S>:interaction:N`).
    std::vector<Value>               steps;
    // A forked session's ancestry, oldest first (ADR-182 §20): for each ancestor, the model turns it had
    // consumed and the steps it had taken when it was forked, and the turn it was forked at. Replay runs
    // each segment in turn, forking between them, then this session's own turns and steps.
    std::vector<Value>               segments;
    std::string                      nondeterministic_reason;  // non-empty = cannot be exported
    // Real-tool sessions only (ADR-208): the recording or the doubles, and the scratch directory to remove.
    std::shared_ptr<RealToolLog>     real;
    std::filesystem::path            sandbox_dir;
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

[[nodiscard]] inline rt::task<void> resolve_job(DriverSession* s, std::string interaction_id, bool approve,
                                                std::optional<std::vector<rt::ApprovalCallDecision>> call_decisions = {},
                                                std::optional<std::string> approver_id = {}) {
    rt::ResolveInteraction request{interaction_id, approve};
    request.call_decisions = std::move(call_decisions);
    request.approver_id    = std::move(approver_id);
    result<rt::AgentResponse> r = co_await s->session->resolve_interaction(request);
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

// ---- The turn format: model_script_push input == scenario model_turns (ADR-182 §16) -----------------
//
// Accepted shapes:
//   short:   {"text": "...", "tool_calls": [{"name", "arguments" (object|raw string), "call_id"?}]}
//   exact:   {"content": [{"text": ...} | {"reasoning": ...} | {"tool_call": {"call_id","name","arguments"}}],
//             "usage": {"input_tokens", "output_tokens"}?}
//   failure: {"error": {"code", "message", "class"?: transient|policy|contract|resource|fatal}}
// Any shape may carry "request_digest": the digest of the request this turn answers (C8). A model call
// whose request has another digest fails with test.replay_mismatch instead of consuming the turn.
// Export writes the exact shape, so item order and raw argument text survive the round trip.

[[nodiscard]] inline std::string_view failure_class_name(failure_class k) noexcept {
    switch (k) {
        case failure_class::transient: return "transient";
        case failure_class::policy: return "policy";
        case failure_class::contract: return "contract";
        case failure_class::resource: return "resource";
        case failure_class::fatal: return "fatal";
    }
    return "fatal";
}
[[nodiscard]] inline std::optional<failure_class> failure_class_from(std::string_view n) noexcept {
    if (n == "transient") return failure_class::transient;
    if (n == "policy") return failure_class::policy;
    if (n == "contract") return failure_class::contract;
    if (n == "resource") return failure_class::resource;
    if (n == "fatal") return failure_class::fatal;
    return std::nullopt;
}

template <class V>
[[nodiscard]] ContentItem assistant_item(V v) {
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = std::move(v);
    return item;
}

[[nodiscard]] inline std::variant<testing::ScriptedTurn, ToolError> parse_turn(Value const& t,
                                                                               std::uint64_t& next_call_id,
                                                                               std::optional<std::string>& request_digest) {
    auto bad = [](std::string m) { return ToolError{"test.bad_arguments", std::move(m)}; };
    if (!t.is_object()) return bad("each turn must be an object");
    request_digest = get_string(t, "request_digest");
    if (Value const* fe = t.find("error"); fe != nullptr) {
        auto klass = failure_class_from(get_string(*fe, "class").value_or("fatal"));
        if (!klass) return bad("error.class must be transient|policy|contract|resource|fatal");
        return testing::failure_turn(error{*klass, get_string(*fe, "message").value_or("scripted model failure"),
                                           get_string(*fe, "code").value_or("test.scripted_failure")});
    }
    testing::ScriptedTurn turn;
    turn.message.role = role::assistant;
    turn.usage = Usage{1, 1, 0, 0, 0.0};
    if (Value const* u = t.find("usage"); u != nullptr) {
        turn.usage.input_tokens = get_u64(*u, "input_tokens").value_or(0);
        turn.usage.output_tokens = get_u64(*u, "output_tokens").value_or(0);
    }
    auto tool_call_of = [&](Value const& c) -> std::optional<ToolCall> {
        auto name = get_string(c, "name");
        if (!name) return std::nullopt;
        std::string arguments = "{}";
        if (Value const* a = c.find("arguments"); a != nullptr) {
            arguments = a->is_string() ? a->as_string() : json::dump(*a);
        }
        ToolCall call;
        call.call_id = get_string(c, "call_id").value_or("call_" + std::to_string(next_call_id++));
        call.tool_name = *name;
        call.arguments_json = std::move(arguments);
        call.provenance = call_provenance::vendor_structured;
        return call;
    };
    Value const* content = t.find("content");
    if (content != nullptr) {
        if (!content->is_array()) return bad("content must be an array");
        for (Value const& c : content->as_array()) {
            if (auto text = get_string(c, "text")) {
                turn.message.content.push_back(assistant_item(Text{*text}));
            } else if (auto reasoning = get_string(c, "reasoning")) {
                Reasoning r;
                r.text = *reasoning;
                turn.message.content.push_back(assistant_item(std::move(r)));
            } else if (Value const* tc = c.find("tool_call"); tc != nullptr) {
                auto call = tool_call_of(*tc);
                if (!call) return bad("each tool_call needs a name");
                turn.message.content.push_back(assistant_item(std::move(*call)));
            } else {
                return bad("content items are {text} | {reasoning} | {tool_call}");
            }
        }
    }
    if (auto text = get_string(t, "text"); text && !text->empty()) {
        turn.message.content.push_back(assistant_item(Text{*text}));
    }
    if (Value const* calls = t.find("tool_calls"); calls != nullptr) {
        if (!calls->is_array()) return bad("tool_calls must be an array");
        for (Value const& c : calls->as_array()) {
            auto call = tool_call_of(c);
            if (!call) return bad("each tool call needs a name");
            turn.message.content.push_back(assistant_item(std::move(*call)));
        }
    }
    // An exact-shape turn may legitimately be empty (a real model can answer with nothing).
    if (turn.message.content.empty() && content == nullptr) {
        return bad("a turn needs text, tool_calls, content or error");
    }
    return turn;
}

// The exact shape (see above) of one observed exchange.
[[nodiscard]] inline Value exchange_to_turn(ModelExchange const& x) {
    if (x.failure) {
        return obj({{"error", obj({{"code", str(x.failure->code)},
                                   {"message", str(x.failure->message)},
                                   {"class", str(std::string(failure_class_name(x.failure->klass)))}})},
                    {"request_digest", str(x.request_digest)}});
    }
    std::vector<Value> content;
    Usage u{};
    if (x.response) {
        u = x.response->usage;
        for (ContentItem const& c : x.response->message.content) {
            if (auto const* t = std::get_if<Text>(&c.value)) {
                content.push_back(obj({{"text", str(t->text)}}));
            } else if (auto const* r = std::get_if<Reasoning>(&c.value)) {
                content.push_back(obj({{"reasoning", str(r->text)}}));
            } else if (auto const* call = std::get_if<ToolCall>(&c.value)) {
                content.push_back(obj({{"tool_call", obj({{"call_id", str(call->call_id)},
                                                          {"name", str(call->tool_name)},
                                                          {"arguments", str(call->arguments_json)}})}}));
            }
        }
    }
    return obj({{"content", arr(std::move(content))},
                {"usage", obj({{"input_tokens", num(static_cast<double>(u.input_tokens))},
                               {"output_tokens", num(static_cast<double>(u.output_tokens))}})},
                {"request_digest", str(x.request_digest)}});
}

// Rewrites every string that starts with "<session_id>:" to start with "<S>:", recursively. Run and
// interaction ids carry the session id as their prefix; nothing else in the event stream depends on
// which driver session produced it (ADR-182 §12 R6).
// ADR-208: a recorded real-tool call in a scenario file. `arguments` and `result` are JSON values, compared
// and served as json::dump of each; exec events keep what the event stream shows (no exec_id).
[[nodiscard]] inline Value exchange_to_json(ToolExchange const& x) {
    std::vector<Value> evs;
    for (ExecEventRecord const& e : x.exec_events) {
        evs.push_back(obj({{"kind", str(std::string(kind_name(e.kind)))},
                           {"backend", str(e.backend)},
                           {"stage", str(e.stage)},
                           {"ok", boolean(e.ok)},
                           {"error_code", str(e.error_code)}}));
    }
    auto args = json::parse(x.arguments);
    Members m{{"tool_name", str(x.tool_name)}, {"arguments", args ? *args : str(x.arguments)}};
    if (x.is_error) {
        m.emplace_back("error", obj({{"class", str(std::string(failure_class_name(x.klass)))},
                                     {"code", str(x.code)},
                                     {"message", str(x.message)}}));
    } else {
        m.emplace_back("result", x.result);
    }
    m.emplace_back("exec_events", arr(std::move(evs)));
    return obj(std::move(m));
}

[[nodiscard]] inline result<ToolExchange> exchange_from_json(Value const& v) {
    auto bad = [](std::string m) {
        return std::unexpected(error{failure_class::contract, std::move(m), "test.bad_scenario"});
    };
    ToolExchange x;
    auto name = get_string(v, "tool_name");
    if (!name) return bad("no tool_name");
    x.tool_name = *name;
    Value const* a = v.find("arguments");
    if (a == nullptr) return bad("no arguments");
    x.arguments = json::dump(*a);
    Value const* r = v.find("result");
    Value const* e = v.find("error");
    if ((r == nullptr) == (e == nullptr)) return bad("needs exactly one of result and error");
    if (r != nullptr) {
        x.result = *r;
    } else {
        x.is_error = true;
        auto klass = failure_class_from(get_string(*e, "class").value_or(""));
        if (!klass) return bad("unknown error class");
        x.klass = *klass;
        x.code = get_string(*e, "code").value_or("");
        x.message = get_string(*e, "message").value_or("");
        if (x.code.empty()) return bad("an error needs a code");
    }
    if (Value const* evs = v.find("exec_events"); evs != nullptr) {
        if (!evs->is_array()) return bad("exec_events must be a list");
        for (Value const& ev : evs->as_array()) {
            ExecEventRecord rec;
            std::string const kind = get_string(ev, "kind").value_or("");
            if (kind == kind_name(run_event_kind::sandbox_exec_started)) {
                rec.kind = run_event_kind::sandbox_exec_started;
            } else if (kind == kind_name(run_event_kind::sandbox_exec_finished)) {
                rec.kind = run_event_kind::sandbox_exec_finished;
            } else {
                return bad("exec event kind '" + kind + "' is not a sandbox_exec event");
            }
            rec.backend = get_string(ev, "backend").value_or("");
            rec.stage = get_string(ev, "stage").value_or("");
            rec.ok = get_bool(ev, "ok").value_or(true);
            rec.error_code = get_string(ev, "error_code").value_or("");
            x.exec_events.push_back(std::move(rec));
        }
    }
    return x;
}

[[nodiscard]] inline Value normalize_ids(Value const& v, std::string const& session_id) {
    std::string const prefix = session_id + ":";
    switch (v.kind()) {
        case json::value_kind::string: {
            std::string const& s = v.as_string();
            if (s.starts_with(prefix)) return str("<S>:" + s.substr(prefix.size()));
            return v;
        }
        case json::value_kind::array: {
            std::vector<Value> out;
            for (Value const& e : v.as_array()) out.push_back(normalize_ids(e, session_id));
            return arr(std::move(out));
        }
        case json::value_kind::object: {
            Members out;
            for (auto const& [k, e] : v.as_object()) out.emplace_back(k, normalize_ids(e, session_id));
            return obj(std::move(out));
        }
        default: return v;
    }
}
// The inverse, for replaying a step that names an interaction.
[[nodiscard]] inline std::string denormalize_id(std::string const& id, std::string const& session_id) {
    if (id.starts_with("<S>:")) return session_id + ":" + id.substr(4);
    return id;
}

// ---- Workflows (ADR-210) -----------------------------------------------------------------------------
//
// A driven workflow is one `rt::WorkflowSupervisor` over a compiled fixture graph. Each agent step is a
// driver `Session` with its own scripted model, request log and C8 expectations; no session tool can
// reach it (I1). The run's owner and every step's (empty) grant are driver constants (I2). A port answer
// is a user message plus the caller's routes, which the engine alone admits or rejects (I3, ADR-169).
// Events are drained after each run/resume call returns: the structural stream's capacity is set above
// anything a fixture can emit, so the supervisor never blocks, and every step has been joined by then
// (ADR-210 §7 R1-R3, §8).

inline constexpr std::size_t   kMaxWorkflows = 4;
inline constexpr std::size_t   kWorkflowWorkers = 2;
inline constexpr std::uint32_t kMaxWorkflowRounds = 16;
inline constexpr std::size_t   kWorkflowStructuralCapacity = 8192;
inline constexpr std::size_t   kMaxResolveRoutes = 16;
inline constexpr std::string_view kWorkflowOwnerId = "test-driver:workflow-owner";
inline constexpr std::string_view kWorkflowTenant = "test";

[[nodiscard]] inline Principal workflow_principal(std::string id) {
    Principal p;
    p.id = std::move(id);
    p.tenant_id = std::string(kWorkflowTenant);
    return p;
}

struct WorkflowFixture {
    std::string          name;
    std::string          description;
    workflow::Workflow   graph;
    // Agent steps: executor id -> the test tools that step's session gets.
    std::map<std::string, std::vector<std::string>> agent_tools;
    // Function steps: executor id -> body (deterministic, no effects).
    std::map<std::string, rt::ExecutorBody> functions;
};

namespace workflow_fixture_detail {

[[nodiscard]] inline workflow::Executor node(std::string id, workflow::executor_kind kind) {
    workflow::Executor e;
    e.id = std::move(id);
    e.kind = kind;
    e.input_type = "T";
    e.output_type = "T";
    return e;
}
[[nodiscard]] inline workflow::Edge edge(std::string from, std::string to, workflow::edge_kind kind,
                                         std::string label = {}) {
    workflow::Edge e;
    e.from = std::move(from);
    e.to = std::move(to);
    e.kind = kind;
    e.case_label = std::move(label);
    return e;
}
// Joins every text item of the input and appends ">name": what ran, in what order, is visible in the output.
[[nodiscard]] inline rt::ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in, EffectContext&) -> result<rt::ExecutorOutcome> {
        std::string out;
        for (ContentItem const& c : in.content) {
            if (auto const* t = std::get_if<Text>(&c.value)) {
                if (!out.empty()) out += "+";
                out += t->text;
            }
        }
        return rt::ExecutorOutcome{user_message(out + ">" + name)};
    };
}

}  // namespace workflow_fixture_detail

[[nodiscard]] inline std::vector<WorkflowFixture> const& workflow_fixtures() {
    using namespace workflow_fixture_detail;
    using workflow::edge_kind;
    using workflow::executor_kind;
    static std::vector<WorkflowFixture> const all = [] {
        std::vector<WorkflowFixture> v;
        {
            WorkflowFixture f;
            f.name = "wf_review";
            f.description = "draft (agent, echo) -> review (request port) -> route 'approve' runs publish (agent), "
                            "'revise' runs draft again. Output: publish.";
            f.graph.id = f.name;
            f.graph.executors = {node("draft", executor_kind::agent), node("review", executor_kind::request_port),
                                 node("publish", executor_kind::agent)};
            f.graph.edges = {edge("draft", "review", edge_kind::direct),
                             edge("review", "publish", edge_kind::switch_case, "approve"),
                             edge("review", "draft", edge_kind::switch_case, "revise")};
            f.graph.start = "draft";
            f.graph.output_selection = {"publish"};
            f.graph.bound.max_rounds = 12;
            f.agent_tools = {{"draft", {"echo"}}, {"publish", {}}};
            v.push_back(std::move(f));
        }
        {
            WorkflowFixture f;
            f.name = "wf_fanout";
            f.description = "split (function) fans out to a and b (agents, echo, run in parallel), fan-in to join "
                            "(function). Output: join.";
            f.graph.id = f.name;
            f.graph.executors = {node("split", executor_kind::function), node("a", executor_kind::agent),
                                 node("b", executor_kind::agent), node("join", executor_kind::function)};
            f.graph.edges = {edge("split", "a", edge_kind::fan_out), edge("split", "b", edge_kind::fan_out),
                             edge("a", "join", edge_kind::fan_in), edge("b", "join", edge_kind::fan_in)};
            f.graph.start = "split";
            f.graph.output_selection = {"join"};
            f.graph.bound.max_rounds = 8;
            f.agent_tools = {{"a", {"echo"}}, {"b", {"echo"}}};
            f.functions = {{"split", appender("split")}, {"join", appender("join")}};
            v.push_back(std::move(f));
        }
        {
            WorkflowFixture f;
            f.name = "wf_two_ports";
            f.description = "split (function) fans out to two request ports in the same round: p1 (route 'go' runs "
                            "left, 'halt' runs halt) and p2 (runs right). left and right fan in to join. No agents.";
            f.graph.id = f.name;
            f.graph.executors = {node("split", executor_kind::function), node("p1", executor_kind::request_port),
                                 node("p2", executor_kind::request_port), node("left", executor_kind::function),
                                 node("halt", executor_kind::function), node("right", executor_kind::function),
                                 node("join", executor_kind::function)};
            f.graph.edges = {edge("split", "p1", edge_kind::fan_out), edge("split", "p2", edge_kind::fan_out),
                             edge("p1", "left", edge_kind::switch_case, "go"),
                             edge("p1", "halt", edge_kind::switch_case, "halt"),
                             edge("p2", "right", edge_kind::direct),
                             edge("left", "join", edge_kind::fan_in), edge("right", "join", edge_kind::fan_in)};
            f.graph.start = "split";
            f.graph.output_selection = {"join", "halt"};
            f.graph.bound.max_rounds = 8;
            f.functions = {{"split", appender("split")}, {"left", appender("left")}, {"halt", appender("halt")},
                           {"right", appender("right")}, {"join", appender("join")}};
            v.push_back(std::move(f));
        }
        return v;
    }();
    return all;
}

// The rules every workflow fixture must meet (ADR-210 §2.2, §7, §8): a valid, bounded graph; every agent step
// names only test tools that never need approval (an approval inside a step never reaches the tester: steps run
// with suspend_for_approval off, so a gated call is denied); no real tools; every function step has a body. Checked at workflow_start and
// by the driver's tests on every compiled fixture.
[[nodiscard]] inline result<void> check_workflow_fixture(WorkflowFixture const& f) {
    auto bad = [&](std::string m) {
        return std::unexpected(error{failure_class::contract, "workflow fixture " + f.name + ": " + m, "test.bad_fixture"});
    };
    if (auto ok = workflow::validate_workflow(f.graph); !ok) return bad(ok.error().message);
    if (!f.graph.bound.max_rounds || *f.graph.bound.max_rounds > kMaxWorkflowRounds) {
        return bad("max_rounds must be set and at most " + std::to_string(kMaxWorkflowRounds));
    }
    if (f.graph.bound.deadline_ms) return bad("a wall-clock deadline would make the run nondeterministic");
    std::vector<ToolDescriptor> const known = all_test_tool_descriptors();
    for (workflow::Executor const& e : f.graph.executors) {
        if (e.kind == workflow::executor_kind::sub_workflow) return bad("sub-workflows are not supported");
        if (!e.capability_ceiling.empty()) return bad("step " + e.id + " declares capabilities");
        if (e.kind == workflow::executor_kind::function && !f.functions.contains(e.id)) {
            return bad("function step " + e.id + " has no body");
        }
        if (e.kind != workflow::executor_kind::agent) continue;
        auto tools = f.agent_tools.find(e.id);
        if (tools == f.agent_tools.end()) return bad("agent step " + e.id + " names no tool list");
        for (std::string const& t : tools->second) {
            if (is_real_tool(t)) return bad("step " + e.id + " names the real tool " + t + " (ADR-208 tools are "
                                            "sessions only)");
            auto d = std::find_if(known.begin(), known.end(), [&](ToolDescriptor const& k) { return k.name == t; });
            if (d == known.end()) return bad("step " + e.id + " names an unknown tool " + t);
            if (d->approval != approval_mode::never_require) {
                return bad("step " + e.id + " names " + t + ", which needs approval: no approval inside a workflow step "
                           "can reach the tester, so the call could only ever be denied (ADR-210 §2.2, §8)");
            }
        }
    }
    return {};
}

[[nodiscard]] inline std::string_view workflow_event_kind_name(workflow::workflow_event_kind k) noexcept {
    using K = workflow::workflow_event_kind;
    switch (k) {
        case K::workflow_run_started: return "workflow_run_started";
        case K::workflow_run_suspended: return "workflow_run_suspended";
        case K::workflow_run_resumed: return "workflow_run_resumed";
        case K::workflow_run_completed: return "workflow_run_completed";
        case K::workflow_run_failed: return "workflow_run_failed";
        case K::superstep_started: return "superstep_started";
        case K::superstep_completed: return "superstep_completed";
        case K::executor_dispatched: return "executor_dispatched";
        case K::executor_completed: return "executor_completed";
        case K::message_routed: return "message_routed";
        case K::fan_out_dispatched: return "fan_out_dispatched";
        case K::fan_in_aggregated: return "fan_in_aggregated";
        case K::route_selected: return "route_selected";
        case K::request_port_opened: return "request_port_opened";
        case K::request_port_resolved: return "request_port_resolved";
        case K::checkpoint_saved: return "checkpoint_saved";
        case K::merge_completed: return "merge_completed";
        case K::merge_conflict: return "merge_conflict";
        case K::agent_turn_event: return "agent_turn_event";
        case K::moderator_stream_delta: return "moderator_stream_delta";
    }
    return "unknown";
}

[[nodiscard]] inline std::string_view edge_kind_name(workflow::edge_kind k) noexcept {
    switch (k) {
        case workflow::edge_kind::direct: return "direct";
        case workflow::edge_kind::fan_out: return "fan_out";
        case workflow::edge_kind::fan_in: return "fan_in";
        case workflow::edge_kind::switch_case: return "switch_case";
        case workflow::edge_kind::multi_selection: return "multi_selection";
        case workflow::edge_kind::chain: return "chain";
    }
    return "unknown";
}

[[nodiscard]] inline Value strings_json(std::vector<std::string> const& v) {
    std::vector<Value> out;
    for (std::string const& s : v) out.push_back(str(s));
    return arr(std::move(out));
}

// The structural payload, field by field (AgentTurn and ModeratorDelta go to the step logs instead).
[[nodiscard]] inline Value workflow_payload_json(workflow::WorkflowEventPayload const& p) {
    namespace wp = workflow::workflow_event_payload;
    return std::visit(
        overloaded{
            [](wp::Empty const&) { return obj({}); },
            [](wp::RunFailed const& x) { return obj({{"status", str(x.status_tag)}}); },
            [](wp::ExecutorRef const& x) { return obj({{"executor_id", str(x.executor_id)}}); },
            [](wp::ExecutorResult const& x) { return obj({{"executor_id", str(x.executor_id)}, {"ok", boolean(x.ok)}}); },
            [](wp::MessageRouted const& x) {
                return obj({{"from", str(x.from_executor_id)}, {"to", str(x.to_executor_id)},
                            {"edge", str(std::string(edge_kind_name(x.kind)))}, {"case_label", str(x.case_label)}});
            },
            [](wp::FanOut const& x) { return obj({{"from", str(x.from_executor_id)}, {"to", strings_json(x.to_executor_ids)}}); },
            [](wp::FanIn const& x) { return obj({{"to", str(x.to_executor_id)}, {"from", strings_json(x.from_executor_ids)}}); },
            [](wp::RouteSelected const& x) {
                return obj({{"executor_id", str(x.executor_id)}, {"chosen", strings_json(x.chosen_cases)},
                            {"available", strings_json(x.available_cases)}});
            },
            [](wp::PortRef const& x) {
                return obj({{"executor_id", str(x.executor_id)}, {"interaction_id", str(x.interaction_id)}});
            },
            [](wp::CheckpointSaved const& x) { return obj({{"round", num(x.round)}}); },
            [](wp::MergeRef const& x) { return obj({{"executor_id", str(x.executor_id)}}); },
            [](wp::SuperstepBounds const& x) { return obj({{"executors", strings_json(x.executor_ids)}}); },
            [](wp::AgentTurn const& x) { return obj({{"executor_id", str(x.executor_id)}}); },
            [](wp::ModeratorDelta const& x) { return obj({{"executor_id", str(x.executor_id)}}); },
        },
        p);
}

[[nodiscard]] inline Value workflow_result_json(rt::WorkflowResult const& r) {
    std::vector<Value> partial;
    for (rt::ExecutorOutput const& o : r.partial) {
        partial.push_back(obj({{"executor_id", str(o.executor_id)}, {"round", num(o.round)},
                               {"text", str(content_text(o.payload.content))}}));
    }
    std::vector<std::string> open;
    for (Interaction const& ix : r.open_interactions) open.push_back(ix.interaction_id);
    return obj({{"status", str(rt::workflow_status_tag(r.status))},
                {"rounds", num(r.rounds)},
                {"output", str(content_text(r.output.content))},
                {"partial", arr(std::move(partial))},
                {"failed_executor", str(r.failed_executor)},
                {"open_interactions", strings_json(open)},
                {"unopened_ports", strings_json(r.unopened_ports)}});
}

enum class workflow_state { ready, running, suspended, finished };
[[nodiscard]] inline std::string_view workflow_state_name(workflow_state s) noexcept {
    switch (s) {
        case workflow_state::ready: return "ready";
        case workflow_state::running: return "running";
        case workflow_state::suspended: return "suspended";
        case workflow_state::finished: return "finished";
    }
    return "?";
}

// One agent step: a driver session the workflow drives, never a driver session the tester can address.
struct WorkflowStep {
    std::string                          executor_id;
    std::string                          session_id;  // "<fixture>/<executor>": fixed, so request digests repeat
    std::optional<testing::ScriptedChatClient> script;
    std::shared_ptr<RequestLog>          requests = std::make_shared<RequestLog>();
    std::shared_ptr<ExchangeLog>         exchanges = std::make_shared<ExchangeLog>();
    std::shared_ptr<RequestExpectations> expectations = std::make_shared<RequestExpectations>();
    std::uint64_t                        next_call_id = 1;
    std::unique_ptr<Session>             session = std::make_unique<Session>();
};

class WorkflowMonitor {
public:
    void on_event(workflow::WorkflowEvent const& ev) {
        namespace wp = workflow::workflow_event_payload;
        std::lock_guard lock(mutex_);
        if (auto const* t = std::get_if<wp::AgentTurn>(&ev.payload)) {
            step_events_[t->executor_id].push_back(obj({{"attempt", num(t->attempt)},
                                                        {"kind", str(std::string(kind_name(t->inner.kind)))},
                                                        {"payload", payload_json(t->inner.payload)}}));
            return;
        }
        if (auto const* d = std::get_if<wp::ModeratorDelta>(&ev.payload)) {
            step_events_[d->executor_id].push_back(obj({{"attempt", num(d->attempt)},
                                                        {"kind", str("moderator_stream_delta")},
                                                        {"text", str(d->text_delta)}}));
            return;
        }
        events_.push_back(obj({{"seq", num(static_cast<double>(next_seq_++))},
                               {"kind", str(std::string(workflow_event_kind_name(ev.kind)))},
                               {"round", num(ev.round)},
                               {"payload", workflow_payload_json(ev.payload)}}));
    }
    void set_state(workflow_state s) {
        {
            std::lock_guard lock(mutex_);
            state_ = s;
        }
        cv_.notify_all();
    }
    // After a run/resume call returned and its events were drained.
    void finish_call(workflow_state s, Value result, std::uint64_t dropped) {
        {
            std::lock_guard lock(mutex_);
            state_ = cancelled_ ? workflow_state::finished : s;
            last_result_ = std::move(result);
            dropped_ = dropped;
        }
        cv_.notify_all();
    }
    void mark_cancelled(std::string driver_outcome) {
        {
            std::lock_guard lock(mutex_);
            cancelled_ = true;
            if (!driver_outcome.empty()) driver_outcome_ = std::move(driver_outcome);
            if (state_ != workflow_state::running) state_ = workflow_state::finished;
        }
        cv_.notify_all();
    }
    [[nodiscard]] workflow_state state() const {
        std::lock_guard lock(mutex_);
        return state_;
    }
    [[nodiscard]] bool cancelled() const {
        std::lock_guard lock(mutex_);
        return cancelled_;
    }
    [[nodiscard]] std::string driver_outcome() const {
        std::lock_guard lock(mutex_);
        return driver_outcome_;
    }
    [[nodiscard]] std::optional<Value> last_result() const {
        std::lock_guard lock(mutex_);
        return last_result_;
    }
    [[nodiscard]] std::uint64_t dropped() const {
        std::lock_guard lock(mutex_);
        return dropped_;
    }
    [[nodiscard]] std::vector<Value> events_since(std::uint64_t since, std::size_t limit) const {
        std::lock_guard lock(mutex_);
        std::vector<Value> out;
        for (Value const& e : events_) {
            if (get_u64(e, "seq").value_or(0) <= since) continue;
            out.push_back(e);
            if (out.size() >= limit) break;
        }
        return out;
    }
    [[nodiscard]] std::vector<Value> step_events(std::string const& executor_id) const {
        std::lock_guard lock(mutex_);
        auto it = step_events_.find(executor_id);
        return it == step_events_.end() ? std::vector<Value>{} : it->second;
    }
    [[nodiscard]] std::size_t event_count() const {
        std::lock_guard lock(mutex_);
        return events_.size();
    }
    [[nodiscard]] bool wait(std::function<bool(workflow_state)> const& pred, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return pred(state_); });
    }

private:
    mutable std::mutex                        mutex_;
    std::condition_variable                   cv_;
    std::vector<Value>                        events_;
    std::map<std::string, std::vector<Value>> step_events_;
    std::uint64_t                             next_seq_ = 1;
    std::uint64_t                             dropped_ = 0;
    workflow_state                            state_ = workflow_state::ready;
    bool                                      cancelled_ = false;
    std::string                               driver_outcome_;
    std::optional<Value>                      last_result_;
};

// One driven workflow. Members are destroyed bottom-up: the pool first (it finishes the job in flight,
// which the destructor has already cancelled), then the stream, the supervisor (joins its own pool), and
// only then the step sessions its bodies reference (ADR-210 §7 R10).
struct DriverWorkflow {
    std::string                                 id;
    std::string                                 fixture;
    std::vector<std::unique_ptr<WorkflowStep>>  steps;
    std::shared_ptr<CapabilitySet>              grant = std::make_shared<CapabilitySet>(CapabilitySet::grant_root({}));
    std::unique_ptr<rt::WorkflowSupervisor>     sup;
    std::optional<workflow::WorkflowEventStream> stream;
    std::shared_ptr<WorkflowMonitor>            monitor = std::make_shared<WorkflowMonitor>();
    std::vector<Value>                          script_log;  // steps for scenario_export
    std::string                                 nondeterministic_reason;
    bool                                        ran = false;
    std::unique_ptr<rt::ThreadPool>             pool = std::make_unique<rt::ThreadPool>(1);

    DriverWorkflow() = default;
    DriverWorkflow(DriverWorkflow const&) = delete;
    DriverWorkflow& operator=(DriverWorkflow const&) = delete;
    ~DriverWorkflow() {
        if (sup) sup->cancel();
        pool.reset();
    }

    [[nodiscard]] WorkflowStep* step(std::string_view executor_id) {
        for (auto& s : steps)
            if (s->executor_id == executor_id) return s.get();
        return nullptr;
    }
};

// Drains both queues (every step has been joined: the call returned) and records how the call ended.
inline void finish_workflow_call(DriverWorkflow& w, rt::WorkflowResult const& r) {
    while (std::optional<workflow::WorkflowEvent> ev = w.stream->next()) w.monitor->on_event(*ev);
    workflow_state const s = w.sup->open_interactions().empty() ? workflow_state::finished : workflow_state::suspended;
    w.monitor->finish_call(s, workflow_result_json(r), w.stream->multiplexed_dropped_count());
}

[[nodiscard]] inline rt::task<void> workflow_run_job(DriverWorkflow* w, std::string text) {
    rt::WorkflowResult r =
        co_await w->sup->run_workflow(rt::RunWorkflow{user_message(std::move(text)),
                                                      workflow_principal(std::string(kWorkflowOwnerId))});
    finish_workflow_call(*w, r);
    co_return;
}

[[nodiscard]] inline rt::task<void> workflow_resume_job(DriverWorkflow* w, rt::ResumeWorkflow request) {
    rt::WorkflowResult r = co_await w->sup->resume_workflow(std::move(request));
    finish_workflow_call(*w, r);
    co_return;
}

class Driver {
public:
    Driver() = default;
    explicit Driver(DriverConfig config) : config_(std::move(config)) {}
    Driver(Driver const&) = delete;
    Driver& operator=(Driver const&) = delete;
    // Sessions first (each drains its jobs and drops its sandbox), then this driver's own scratch directory
    // (ADR-208 C1, G7). A killed driver leaves that directory behind (disk only; ADR-208 residual).
    ~Driver() {
        workflows_.clear();
        sessions_.clear();
        if (!own_scratch_.empty()) (void)remove_tree_no_follow(own_scratch_);
    }

    // One JSON-RPC message in, zero or one JSON-RPC message out (notifications get no reply). Every
    // reply passes the secret canary filter (ADR-182 §12 R8, P6): a reply containing a canary string
    // is replaced by an error that names no content.
    [[nodiscard]] std::optional<std::string> handle_line(std::string_view line) {
        std::optional<std::string> reply = dispatch_line(line);
        if (!reply) return reply;
        for (std::string const& canary : config_.secret_canaries) {
            if (!canary.empty() && reply->find(canary) != std::string::npos) {
                ++secret_leaks_blocked_;
                return json::dump(rpc_error(reply_id(*reply), -32000,
                                            "test.secret_leak_blocked: this reply contained a configured "
                                            "secret and was withheld"));
            }
        }
        return reply;
    }

    [[nodiscard]] std::uint64_t secret_leaks_blocked() const { return secret_leaks_blocked_; }

private:
    // The id of a reply we built ourselves (for the canary path's replacement error).
    [[nodiscard]] static Value reply_id(std::string const& reply) {
        auto parsed = json::parse(reply);
        if (!parsed) return Value{};
        Value const* id = parsed->find("id");
        return id != nullptr ? *id : Value{};
    }

    [[nodiscard]] std::optional<std::string> dispatch_line(std::string_view line) {
        if (line.size() > kMaxLineBytes) {
            return json::dump(rpc_error(Value{}, -32600, "request line exceeds the size limit"));
        }
        // A scenario replay pushes every recorded model turn in one line, so allow more nodes than
        // the default; the line itself is already capped at kMaxLineBytes.
        json::ParseBudget budget;
        budget.max_nodes_visited = 4'000'000;
        auto parsed = json::parse(line, budget);
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

public:
    [[nodiscard]] std::size_t session_count() const { return sessions_.size(); }

    // Closes every session (stdin EOF / shutdown).
    void close_all() {
        std::vector<std::string> ids;
        for (auto const& [id, _] : sessions_) ids.push_back(id);
        for (std::string const& id : ids) (void)close_session(id);
        workflows_.clear();
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
        // Echo the client's version only when it is one we serve; otherwise answer with ours
        // (MCP lifecycle negotiation). The driver uses only tools/list and tools/call, which are
        // the same shape in every listed version. The MCP Inspector CLI sends 2025-11-25 (ADR-182 §17).
        constexpr std::array<std::string_view, 3> kSupported{kProtocolVersion, "2025-11-25", "2025-06-18"};
        std::string version(kProtocolVersion);
        if (auto asked = get_string(params, "protocolVersion");
            asked && std::ranges::find(kSupported, std::string_view(*asked)) != kSupported.end()) {
            version = *asked;
        }
        std::vector<Value> supported;
        for (auto v : kSupported) supported.push_back(str(std::string(v)));
        return obj({{"protocolVersion", str(version)},
                    {"supportedVersions", arr(std::move(supported))},
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
        Value const wid = prop("string", "Workflow handle returned by workflow_start.");
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
                                                     "class is transient|fatal|contract (default fatal).")}})},
                  {"request_digest",
                   prop("string", "Optional. The digest (model_requests shows it) of the request this turn "
                                  "answers; a different request fails the call with test.replay_mismatch.")}})},
        });
        return arr({
            tool_def("fixtures_list",
                     "List the session fixtures: compiled-in ones and the host's committed fixture files "
                     "(015 Agent YAML; a modified or untracked file is listed as refused).",
                     schema({}, {})),
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
                     "Pending approval requests, one entry per tool call that waits on the decision, with "
                     "tool name and arguments (ADR-196: a call that does not need approval is not listed).",
                     schema({{"session_id", sid}}, {"session_id"})),
            tool_def("interaction_resolve",
                     "Approve or deny an open interaction. `decision` applies to every listed call that "
                     "`call_decisions` does not name (ADR-196).",
                     schema({{"session_id", sid},
                             {"interaction_id", prop("string", "From interaction_list.")},
                             {"decision", prop("string", "approve | deny")},
                             {"call_decisions",
                              prop("array", "Optional per-call decisions: [{\"call_id\": ..., \"decision\": "
                                            "\"approve\" | \"deny\"}], each a call interaction_list shows.")},
                             {"approver_id", prop("string", "Optional: who decided, recorded on approval_resolved.")}},
                            {"session_id", "interaction_id", "decision"})),
            tool_def("session_cancel",
                     "Cancel the current run. On a suspended session this also denies every open "
                     "interaction, so the session returns to idle.",
                     schema({{"session_id", sid}}, {"session_id"})),
            tool_def("session_fork",
                     "Copy an idle session into a new one with the same fixture: its history up to at_turn "
                     "(a turn starts at a user message; default all), an empty script, no open interactions. "
                     "The source is unchanged. A fork can be exported; replay rebuilds its ancestry.",
                     schema({{"session_id", sid},
                             {"at_turn", prop("integer", "Keep this many turns (0 = empty history). Default all.")}},
                            {"session_id"})),
            tool_def("session_close", "Cancel anything in flight and discard the session.",
                     schema({{"session_id", sid}}, {"session_id"})),
            tool_def("model_requests",
                     "What the engine sent the model on each call: messages (role + text), tool names, and the "
                     "request's digest.",
                     schema({{"session_id", sid}, {"since_index", prop("integer", "Default 0.")}},
                            {"session_id"})),
            tool_def("scenario_export",
                     "Save this session as a replayable scenario <name>.json: the model's observed answers "
                     "(scripted or live) become the script, your send/resolve/cancel steps are replayed, and "
                     "the whole normalized event stream is the expected result. Session must not be running.",
                     schema({{"session_id", sid},
                             {"workflow_id", prop("string", "Export a workflow instead of a session (ADR-210).")},
                             {"name", prop("string", "[a-z0-9_-]{1,64}")},
                             {"description", prop("string", "What this scenario checks.")},
                             {"overwrite", prop("boolean", "Replace an existing scenario of that name.")}},
                            {"name"})),
            tool_def("scenario_replay",
                     "Replay a saved scenario with the scripted model (no network): each model request must match "
                     "its recorded digest (else test.replay_mismatch), then the event stream is diffed.",
                     schema({{"name", prop("string", "Scenario name.")}}, {"name"})),
            tool_def("workflow_start",
                     "Start a workflow from a compiled workflow fixture (fixtures_list: workflows). Returns its "
                     "workflow_id and executors. Push each agent step's turns with workflow_script_push first.",
                     schema({{"fixture", prop("string", "Workflow fixture name.")}}, {"fixture"})),
            tool_def("workflow_script_push",
                     "Append scripted model turns to one agent step (same turn shape as model_script_push). Only "
                     "while the workflow is ready or suspended; a step with no turns left fails the run.",
                     schema({{"workflow_id", wid},
                             {"executor_id", prop("string", "An agent step of this workflow.")},
                             {"turns", obj({{"type", str("array")}, {"items", obj({{"type", str("object")}})}})}},
                            {"workflow_id", "executor_id", "turns"})),
            tool_def("workflow_run",
                     "Start the workflow's one run with a user message; returns at once. Then workflow_wait_for.",
                     schema({{"workflow_id", wid}, {"text", prop("string", "Input message text.")}},
                            {"workflow_id", "text"})),
            tool_def("workflow_wait_for",
                     "Wait (<= 60 s) until: settled (not running, default) | suspended | finished.",
                     schema({{"workflow_id", wid},
                             {"until", prop("string", "settled | suspended | finished")},
                             {"timeout_ms", prop("integer", "Default 10000, max 60000.")}},
                            {"workflow_id"})),
            tool_def("workflow_snapshot",
                     "State, the last run/resume result (status, rounds, output, partial, failed executor, open "
                     "interactions, unopened ports), per-step script and request counters.",
                     schema({{"workflow_id", wid}}, {"workflow_id"})),
            tool_def("workflow_events",
                     "The structural event log (seq > since_seq), or one agent step's own event log (executor_id).",
                     schema({{"workflow_id", wid},
                             {"since_seq", prop("integer", "Default 0.")},
                             {"limit", prop("integer", "Default and max 200.")},
                             {"executor_id", prop("string", "An agent step: its own run events instead.")}},
                            {"workflow_id"})),
            tool_def("request_port_list", "Open request-port interactions: id, port, and the ask's text.",
                     schema({{"workflow_id", wid}}, {"workflow_id"})),
            tool_def("request_port_resolve",
                     "Answer an open request port with a user message; routes select among the port's declared "
                     "switch_case edges (the engine decides). caller defaults to the run's owner; another id is "
                     "refused by the engine (admission). Returns at once.",
                     schema({{"workflow_id", wid},
                             {"interaction_id", prop("string", "From request_port_list.")},
                             {"text", prop("string", "The answer.")},
                             {"routes", obj({{"type", str("array")}, {"items", obj({{"type", str("string")}})}})},
                             {"caller", prop("string", "Optional principal id of who answers.")}},
                            {"workflow_id", "interaction_id", "text"})),
            tool_def("workflow_cancel",
                     "Cancel the workflow; it never runs again. A cancel while running makes it non-exportable.",
                     schema({{"workflow_id", wid}}, {"workflow_id"})),
            tool_def("workflow_close", "Cancel anything in flight and discard the workflow.",
                     schema({{"workflow_id", wid}}, {"workflow_id"})),
            tool_def("workflow_model_requests", "What one agent step sent the model on each call, with digests.",
                     schema({{"workflow_id", wid},
                             {"executor_id", prop("string", "An agent step.")},
                             {"since_index", prop("integer", "Default 0.")}},
                            {"workflow_id", "executor_id"})),
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
            {"session_fork", &Driver::t_session_fork},
            {"session_close", &Driver::t_session_close},
            {"model_requests", &Driver::t_model_requests},
            {"scenario_export", &Driver::t_scenario_export},
            {"scenario_replay", &Driver::t_scenario_replay},
            {"workflow_start", &Driver::t_workflow_start},
            {"workflow_script_push", &Driver::t_workflow_script_push},
            {"workflow_run", &Driver::t_workflow_run},
            {"workflow_wait_for", &Driver::t_workflow_wait_for},
            {"workflow_snapshot", &Driver::t_workflow_snapshot},
            {"workflow_events", &Driver::t_workflow_events},
            {"request_port_list", &Driver::t_request_port_list},
            {"request_port_resolve", &Driver::t_request_port_resolve},
            {"workflow_cancel", &Driver::t_workflow_cancel},
            {"workflow_close", &Driver::t_workflow_close},
            {"workflow_model_requests", &Driver::t_workflow_model_requests},
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
    // so the job does not queue behind a long run. §22: on a timeout the job may still run later, so
    // `fn` must own everything it writes (a shared_ptr), never refer to the caller's frame.
    [[nodiscard]] static bool run_on_worker(DriverSession& s, std::function<void()> fn) {
        std::future<rt::JobOutcome> f = s.pool->submit(read_job(std::move(fn)));
        if (f.wait_for(kJobTimeout) != std::future_status::ready) return false;
        return !f.get().faulted;
    }
    // The same, with no timeout: for a job that must finish before the caller may continue (a fork
    // writes into a session the caller then hands out). Only on an idle session, so nothing is queued
    // ahead of it and it is bounded by the job's own work.
    static bool run_on_worker_to_completion(DriverSession& s, std::function<void()> fn) {
        std::future<rt::JobOutcome> f = s.pool->submit(read_job(std::move(fn)));
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
            {"model", str(s.fixture.live ? "live" : "scripted")},
            {"script_pending", num(static_cast<double>(s.script ? s.script->pending() : 0))},
            {"model_calls", num(static_cast<double>(s.requests->total()))},
            {"last_seq", num(static_cast<double>(s.monitor->last_seq()))},
            {"events_dropped", num(static_cast<double>(s.monitor->dropped()))},
            {"requests_checked", num(static_cast<double>(s.expectations->checked()))},
        };
        if (std::optional<RequestMismatch> const mm = s.expectations->mismatch()) {
            m.emplace_back("replay_mismatch", obj({{"call_index", num(static_cast<double>(mm->call_index))},
                                                   {"expected", str(mm->expected)},
                                                   {"actual", str(mm->actual)},
                                                   {"actual_request", mm->actual_request}}));
        }
        if (s.real) {
            m.emplace_back("tool_calls", obj({{"mode", str(s.real->replaying() ? "double" : "recorded")},
                                              {"made", num(static_cast<double>(s.real->calls()))},
                                              {"recorded", num(static_cast<double>(s.real->expected()))}}));
            if (std::optional<ToolMismatch> const tm = s.real->mismatch()) {
                m.emplace_back("tool_replay_mismatch", obj({{"call_index", num(static_cast<double>(tm->call_index))},
                                                            {"expected", str(tm->expected)},
                                                            {"actual", str(tm->actual)}}));
            }
        }
        if (st == run_state::running) {
#ifdef AGENTENGINE_TEST_DRIVER_C2_RACE
            // C2 positive control only (ADR-182 §8): reads the session's history from the MCP thread
            // while the worker owns it, which is the I1 violation TSan must report. Never defined in
            // a normal build.
            m.emplace_back("history_length", num(static_cast<double>(s.session->history().size())));
#endif
            m.emplace_back("partial", boolean(true));
            return obj(std::move(m));
        }
        struct Read {
            std::size_t        history_len = 0;
            std::vector<Value> history;
        };
        auto out = std::make_shared<Read>();  // owned by the job too: it may outlive this frame on a timeout
        Session* session = s.session.get();
        bool const ran = run_on_worker(s, [out, session, include_history] {
            out->history_len = session->history().size();
            if (include_history) {
                for (Message const& msg : session->history()) out->history.push_back(rt::message_to_json(msg));
            }
        });
        m.emplace_back("partial", boolean(!ran));
        m.emplace_back("history_length", num(static_cast<double>(ran ? out->history_len : 0)));
        if (include_history && ran) m.emplace_back("history", arr(std::move(out->history)));
        return obj(std::move(m));
    }

    // ---- real tools (ADR-208) ----

    [[nodiscard]] static bool uses_real_tools(Fixture const& f) {
        return std::any_of(f.tools.begin(), f.tools.end(), [](std::string const& t) { return is_real_tool(t); });
    }
    [[nodiscard]] bool real_tools_available() const {
        return config_.tool_doubles.has_value() ||
               (!config_.sandbox_root.empty() && static_cast<bool>(config_.sandbox_factory));
    }

    // `<sandbox_root>/d-<16 hex>/<session id>`, empty. The first call creates this driver's own directory
    // with an exclusive create, so two drivers on one root never share or remove each other's (ADR-208 C1).
    [[nodiscard]] result<std::filesystem::path> session_scratch(std::string const& session_id) {
        auto fail = [](std::string m) {
            return std::unexpected(error{failure_class::resource, std::move(m), "test.sandbox_failed"});
        };
        std::error_code ec;
        if (own_scratch_.empty()) {
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(config_.sandbox_root, ec))) {
                return fail("the sandbox root is a link; give the driver a real directory");
            }
            std::filesystem::create_directories(config_.sandbox_root, ec);
            std::random_device rd;
            for (int attempt = 0; attempt < 8 && own_scratch_.empty(); ++attempt) {
                std::uint64_t const r = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
                char hex[17];
                std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(r));
                std::filesystem::path const candidate = config_.sandbox_root / ("d-" + std::string(hex));
                // create_directory is false (no error) when the name exists: another driver's, so try again.
                if (std::filesystem::create_directory(candidate, ec) && !ec) own_scratch_ = candidate;
            }
            if (own_scratch_.empty()) return fail("cannot create this driver's directory under the sandbox root");
        }
        std::filesystem::path const dir = own_scratch_ / session_id;
        if (!remove_tree_no_follow(dir)) return fail("a leftover scratch directory for " + session_id + " could not be removed");
        if (!std::filesystem::create_directory(dir, ec) || ec) return fail("cannot create the session's scratch directory");
        return dir;
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
                               {"suspend_for_approval", boolean(f.suspend_for_approval)},
                               {"live", boolean(f.live)},
                               {"available", boolean((!f.live || static_cast<bool>(config_.live_backend_factory)) &&
                                                     (!uses_real_tools(f) || real_tools_available()))},
                               {"real_tools", boolean(uses_real_tools(f))},
                               {"source", str(f.source)}}));
        }
        // File fixtures (§21): every <name>.yaml under the host's root, with whether it would load now.
        std::vector<std::string> names;
        std::error_code ec;
        if (!config_.fixtures_root.empty() && std::filesystem::is_directory(config_.fixtures_root, ec)) {
            for (auto const& entry : std::filesystem::directory_iterator(config_.fixtures_root, ec)) {
                if (entry.path().extension() != ".yaml") continue;
                std::string const stem = entry.path().stem().string();
                if (valid_scenario_name(stem)) names.push_back(stem);
            }
        }
        std::sort(names.begin(), names.end());  // directory order is not stable (tools/list-style determinism)
        for (std::string const& n : names) {
            FixtureLoad const load = load_file_fixture(config_.fixtures_root, n, config_.fixture_reader);
            Members m{{"name", str(n)}, {"source", str("file")}, {"live", boolean(false)}};
            if (load.fixture) {
                std::vector<Value> tools;
                for (std::string const& t : load.fixture->tools) tools.push_back(str(t));
                m.emplace_back("description", str(load.fixture->description));
                m.emplace_back("tools", arr(std::move(tools)));
                m.emplace_back("suspend_for_approval", boolean(load.fixture->suspend_for_approval));
                m.emplace_back("has_instructions", boolean(!load.fixture->instructions.empty()));
                m.emplace_back("available", boolean(!uses_real_tools(*load.fixture) || real_tools_available()));
                m.emplace_back("real_tools", boolean(uses_real_tools(*load.fixture)));
            } else {
                m.emplace_back("available", boolean(false));
                m.emplace_back("refused", obj({{"code", str(load.code)}, {"message", str(load.message)}}));
            }
            out.push_back(obj(std::move(m)));
        }
        std::vector<Value> wfs;
        for (WorkflowFixture const& f : workflow_fixtures()) {
            std::vector<Value> steps;
            for (workflow::Executor const& x : f.graph.executors) steps.push_back(str(x.id));
            wfs.push_back(obj({{"name", str(f.name)}, {"description", str(f.description)}, {"executors", arr(std::move(steps))}}));
        }
        return obj({{"fixtures", arr(std::move(out))},
                    {"workflows", arr(std::move(wfs))},
                    {"live_model", str(config_.live_backend_factory ? config_.live_description
                                                                    : std::string("disabled (start with --allow-live)"))}});
    }

    ToolResultJson t_session_start(Value const& args) {
        auto name = get_string(args, "fixture");
        if (!name) return err("test.bad_arguments", "fixture is required");
        std::optional<Fixture> loaded;
        Fixture const* fixture = find_fixture(*name);
        if (fixture == nullptr) {
            FixtureLoad load = load_file_fixture(config_.fixtures_root, *name, config_.fixture_reader);
            if (!load.fixture) return err(load.code, load.message);
            loaded = std::move(load.fixture);
            fixture = &*loaded;
        }
        auto made = make_session(*fixture);
        if (auto* e = std::get_if<ToolError>(&made)) return *e;
        DriverSession& ref = *std::get<DriverSession*>(made);
        return obj({{"session_id", str(ref.id)}, {"snapshot", snapshot(ref, false)}});
    }

    // Builds and registers a fully configured session for a fixture: the one place a session's model,
    // grant, tools and tap are chosen, so a fork gets exactly what a start would (ADR-182 §12 R4).
    std::variant<DriverSession*, ToolError> make_session(Fixture const& fixture_ref) {
        Fixture const* fixture = &fixture_ref;
        if (fixture->live && !config_.live_backend_factory) {
            return err("test.live_disabled",
                       "live fixtures need the driver started with --allow-live and a key file (host decision)");
        }
        if (sessions_.size() >= kMaxSessions) {
            return err("test.too_many_sessions", "close a session first (max " + std::to_string(kMaxSessions) + ")");
        }
        // ADR-208: real tools only sandboxed (or as doubles in a replay), never in live mode (ADR-182 §11 Q2).
        bool const wants_real = uses_real_tools(*fixture);
        if (wants_real && fixture->live) {
            return err("test.bad_fixture", "a live fixture cannot have real tools (ADR-208, ADR-182 §11 Q2)");
        }
        if (wants_real && !real_tools_available()) {
            return err("test.real_tools_disabled", "fixture " + fixture->name +
                                                       " uses real tools; the driver must be started with "
                                                       "--sandbox-root (a host decision)");
        }
        // ADR-208 §10 B3: recorded real-tool calls for a fixture with no real tool would never be checked.
        if (!wants_real && config_.tool_doubles && !config_.tool_doubles->empty()) {
            return err("test.bad_fixture", "fixture " + fixture->name +
                                               " names no real tool, but the scenario records real-tool calls "
                                               "(tool_exchanges) that the replay could not check");
        }

        auto ds = std::make_unique<DriverSession>();
        ds->id = "s" + std::to_string(next_session_++);
        ds->fixture = *fixture;
        Session& session = *ds->session;
        session.initialize(ds->id, Principal{"test-driver/" + fixture->name, "test"}, fixture->token_budget,
                           std::min(fixture->max_turns.value_or(kMaxTurnsPerRun), kMaxTurnsPerRun));
        if (!fixture->instructions.empty()) session.set_static_instructions(fixture->instructions);
        std::shared_ptr<ModelBackend> backend;
        if (fixture->live) {
            backend = config_.live_backend_factory(ds->id);
            if (!backend) return err("test.live_unavailable", "the live backend could not be created");
            // The one grant a live session holds: use of the live key, at the point of use (I2).
            ds->held = CapabilitySet::grant_root(
                {Capability{cap::Secret{std::string(kLiveSecretName), std::chrono::seconds{0}}}});
        } else {
            ds->script.emplace();
            backend = std::make_shared<ScriptedBackend>(*ds->script);
        }
        // The grant a real-tool session holds, the same in record and replay (ADR-208 G5, D9).
        if (wants_real) ds->held = real_tool_grant();
        session.emplace_chat_client(std::move(backend), ds->requests, ds->exchanges, ds->expectations, ds->id);
        session.set_capabilities(&ds->held);
        session.set_suspend_for_approval(fixture->suspend_for_approval);
        std::vector<ToolDescriptor> tools;
        for (ToolDescriptor const& d : all_test_tool_descriptors()) {
            if (std::find(fixture->tools.begin(), fixture->tools.end(), d.name) != fixture->tools.end())
                tools.push_back(d);
        }
        session.history_provider().set_tools(std::move(tools));
        if (wants_real) {
            auto named = [&](std::string const& n) {
                return std::find(fixture->tools.begin(), fixture->tools.end(), n) != fixture->tools.end();
            };
            ds->real = std::make_shared<RealToolLog>(config_.tool_doubles);
            std::vector<ToolDescriptor> real;
            std::shared_ptr<RealToolSandbox> sandbox;
            if (config_.tool_doubles) {
                for (ToolDescriptor const& d : real_tool_standins())
                    if (named(d.name)) real.push_back(double_tool(with_granted_ceiling(d), ds->real));
            } else {
                auto dir = session_scratch(ds->id);
                if (!dir) return err("test.sandbox_failed", dir.error().message);
                auto made = config_.sandbox_factory(*dir);
                if (!made || !*made) {
                    (void)remove_tree_no_follow(*dir);
                    return err("test.sandbox_failed", made ? "the sandbox factory returned nothing" : made.error().message);
                }
                sandbox = *made;
                ds->sandbox_dir = *dir;
                for (ToolDescriptor const& d : real_tool_standins()) {
                    if (!named(d.name)) continue;
                    real.push_back(recording_tool(with_granted_ceiling(d.name == "run_shell" ? sandbox->run_shell() : d),
                                                  ds->real, sandbox));
                }
            }
            session.history_provider().set_real_tools(std::move(real), std::move(sandbox));
        }
        // Installed once, before any job runs (ADR-182 §12 R1).
        std::shared_ptr<SessionMonitor> monitor = ds->monitor;
        session.set_run_event_tap([monitor](RunEvent const& ev) { monitor->on_event(ev); });

        DriverSession* ref = ds.get();
        sessions_.emplace(ds->id, std::move(ds));
        return ref;
    }

    // ADR-182 §12 R4 / §20. The target is built from the source's fixture exactly as session_start would
    // build it (its own model, grant, tools and tap; a scripted target starts with an empty script). Then
    // `fork_from` copies the history prefix. That runs as a job on the SOURCE's worker, the only thread
    // allowed to read the source (I1). The target has run nothing yet, so writing it there is safe, and
    // waiting for the job orders that write before the target's first job. The wait has no timeout (§22):
    // the job writes into the target and this frame, so neither may be released until it has finished.
    ToolResultJson t_session_fork(Value const& args) {
        ToolError e;
        DriverSession* src = find_session(args, e);
        if (src == nullptr) return e;
        run_state const st = src->monitor->state();
        if (st == run_state::running) return err("test.session_running", "wait until the session settles before forking");
        if (st == run_state::suspended) {
            return err("test.session_suspended",
                       "fork only an idle session: fork_from drops open interactions, so a fork of a suspended "
                       "session could never be resumed (resolve or cancel first, or fork before the send)");
        }
        if (src->real) {
            return err("test.fork_unsupported",
                       "a session with real tools cannot be forked: the fork would share or have to copy its scratch "
                       "directory (ADR-208 §2.2)");
        }
        if (src->segments.size() >= kMaxForkDepth) {
            return err("test.fork_too_deep", "a fork chain is capped at " + std::to_string(kMaxForkDepth) + " forks");
        }
        std::optional<std::uint64_t> at_turn;
        if (args.find("at_turn") != nullptr) {
            at_turn = get_u64(args, "at_turn");
            if (!at_turn) return err("test.bad_arguments", "at_turn must be a non-negative integer");
        }
        auto made = make_session(src->fixture);
        if (auto* me = std::get_if<ToolError>(&made)) return *me;
        DriverSession* dst = std::get<DriverSession*>(made);

        Session* from = src->session.get();
        Session* to = dst->session.get();
        std::string const new_id = dst->id;
        std::size_t turns = 0;
        std::size_t kept = 0;
        bool in_range = false;
        bool const ran = run_on_worker_to_completion(*src, [&] {
            // A turn starts at a user message. The source is idle, so every turn in its history is complete.
            std::vector<Message> const& h = from->history();
            std::optional<std::size_t> cut;
            for (std::size_t i = 0; i < h.size(); ++i) {
                if (h[i].role != role::user) continue;
                if (at_turn && turns == *at_turn && !cut) cut = i;
                ++turns;
            }
            in_range = !at_turn || *at_turn <= turns;
            if (!in_range) return;
            kept = cut.value_or(h.size());
            to->fork_from(*from, new_id, kept);
        });
        if (!ran || !in_range) {
            (void)close_session(new_id);
            if (!ran) return err("test.fork_failed", "the fork job did not complete");
            return err("test.bad_arguments", "at_turn is " + std::to_string(*at_turn) + " but the session has only " +
                                                 std::to_string(turns) + " turn(s)");
        }
        std::uint64_t const fork_turn = at_turn.value_or(turns);

        // Lineage, so the fork can be exported and replayed (§20).
        dst->segments = src->segments;
        std::vector<Value> src_turns;
        for (ModelExchange const& x : src->exchanges->exchanges()) src_turns.push_back(exchange_to_turn(x));
        // The source's own events and end state too, so a replay checks the ancestor, not only what its
        // history hands the fork (§22: a divergence that shows only in events would otherwise pass).
        std::vector<Value> src_events;
        for (LoggedEvent const& ev : src->monitor->events_since(0, kEventRingCapacity)) {
            src_events.push_back(normalize_ids(event_json(ev), src->id));
        }
        if (src->monitor->dropped() != 0 && dst->nondeterministic_reason.empty()) {
            dst->nondeterministic_reason = "forked from a session whose event ring overflowed";
        }
        dst->segments.push_back(obj({{"model_turns", arr(std::move(src_turns))},
                                     {"steps", arr(src->steps)},
                                     {"fork_at_turn", num(static_cast<double>(fork_turn))},
                                     {"expected", normalize_ids(obj({{"state", str(std::string(run_state_name(st)))},
                                                                     {"outcome", outcome_json(src->monitor->last_outcome())},
                                                                     {"events", arr(std::move(src_events))}}),
                                                                src->id)}}));
        dst->nondeterministic_reason = src->nondeterministic_reason;
        if (dst->nondeterministic_reason.empty() && (src->exchanges->overflow() || src->exchanges->unrecordable())) {
            dst->nondeterministic_reason = "forked from a session whose model exchanges were not all captured";
        }
        dst->next_call_id = src->next_call_id;  // a scripted call id in the target never repeats one in its history
        return obj({{"session_id", str(new_id)},
                    {"forked_from", str(src->id)},
                    {"at_turn", num(static_cast<double>(fork_turn))},
                    {"turns_in_source", num(static_cast<double>(turns))},
                    {"history_length", num(static_cast<double>(kept))},
                    {"snapshot", snapshot(*dst, false)}});
    }

    ToolResultJson t_model_script_push(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        if (!s->script) {
            return err("test.live_session", "this session's model is live; there is no script to push to");
        }
        if (s->monitor->state() == run_state::running) {
            return err("test.session_running",
                       "push turns only while the session is idle or suspended (keeps runs deterministic)");
        }
        Value const* turns = args.find("turns");
        if (turns == nullptr || !turns->is_array() || turns->as_array().empty()) {
            return err("test.bad_arguments", "turns must be a non-empty array");
        }
        std::vector<testing::ScriptedTurn> parsed;
        std::vector<std::optional<std::string>> digests;
        for (Value const& t : turns->as_array()) {
            std::optional<std::string> digest;
            auto turn = parse_turn(t, s->next_call_id, digest);
            if (auto* bad = std::get_if<ToolError>(&turn)) return *bad;
            parsed.push_back(std::move(std::get<testing::ScriptedTurn>(turn)));
            digests.push_back(std::move(digest));
        }
        if (auto pushed = s->script->push(std::move(parsed)); !pushed) {
            return err(pushed.error().code, pushed.error().message);
        }
        s->expectations->push(std::move(digests));  // same order as the script (both are FIFOs)
        return obj({{"script_pending", num(static_cast<double>(s->script->pending()))}});
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
        s->steps.push_back(obj({{"op", str("send")}, {"text", str(*text)}}));
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
        // ADR-196 (issues #104/#108): per-call decisions and the approver. The session validates them (a call the
        // interaction did not ask about is refused there); here only the shape is checked.
        std::optional<std::vector<rt::ApprovalCallDecision>> call_decisions;
        Value recorded_decisions = Value::make_array({});
        if (Value const* cd = args.find("call_decisions"); cd != nullptr) {
            if (!cd->is_array()) return err("test.bad_arguments", "call_decisions must be an array");
            call_decisions.emplace();
            std::vector<Value> recorded;
            for (Value const& item : cd->as_array()) {
                auto call_id = get_string(item, "call_id");
                auto call_decision = get_string(item, "decision");
                if (!call_id || !call_decision || (*call_decision != "approve" && *call_decision != "deny")) {
                    return err("test.bad_arguments",
                               "each call_decisions entry needs call_id and decision (approve | deny)");
                }
                call_decisions->push_back(rt::ApprovalCallDecision{*call_id, *call_decision == "approve"});
                recorded.push_back(obj({{"call_id", str(*call_id)}, {"decision", str(*call_decision)}}));
            }
            recorded_decisions = Value::make_array(std::move(recorded));
        }
        std::optional<std::string> approver_id = get_string(args, "approver_id");
        if (args.find("approver_id") != nullptr && !approver_id) {
            return err("test.bad_arguments", "approver_id must be a string");
        }
        // Same rule as the session's (ADR-196 §2). Checked here because the resolve runs asynchronously: a
        // session refusal would leave the interaction open while the driver had already cleared it.
        if (approver_id && !agentengine::is_attributable_id(*approver_id)) {
            return err("test.bad_arguments", "approver_id must be non-blank with no control characters");
        }
        if (s->monitor->state() != run_state::suspended) {
            return err("test.not_suspended", "the session has no open interaction to resolve");
        }
        bool known = false;
        for (PendingCall const& c : s->monitor->pending())
            if (c.interaction_id == *interaction_id) known = true;
        if (!known) return err("test.unknown_interaction", "no open interaction " + *interaction_id);
        // Checked here too, not only by the session: a resolve the session refuses would leave the driver's
        // view of the run out of step with the session's (it resolves asynchronously).
        if (call_decisions) {
            std::vector<std::string> seen;
            for (rt::ApprovalCallDecision const& cd : *call_decisions) {
                bool listed = false;
                for (PendingCall const& c : s->monitor->pending())
                    if (c.interaction_id == *interaction_id && c.call_id == cd.call_id) listed = true;
                if (!listed || std::find(seen.begin(), seen.end(), cd.call_id) != seen.end()) {
                    return err("test.bad_arguments", "call_decisions may name each call interaction_list shows for "
                                                     "this interaction once; " + cd.call_id + " is not one");
                }
                seen.push_back(cd.call_id);
            }
        }
        Value step = obj({{"op", str("resolve")},
                          {"interaction_id", normalize_ids(str(*interaction_id), s->id)},
                          {"decision", str(*decision)}});
        if (call_decisions) step = with_field(std::move(step), "call_decisions", std::move(recorded_decisions));
        if (approver_id) step = with_field(std::move(step), "approver_id", str(*approver_id));
        s->steps.push_back(std::move(step));
        s->action_mark = s->monitor->last_seq();
        s->monitor->set_state(run_state::running);
        (void)s->pool->submit(resolve_job(s, *interaction_id, *decision == "approve", std::move(call_decisions),
                                          std::move(approver_id)));
        return obj({{"resumed", boolean(true)}, {"since_seq", num(static_cast<double>(s->action_mark))}});
    }

    ToolResultJson t_session_cancel(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        run_state const st = s->monitor->state();
        s->action_mark = s->monitor->last_seq();
        if (st == run_state::running) {
            // Where a cancel lands inside a running run depends on timing, so it cannot be replayed.
            s->nondeterministic_reason = "session_cancel while a run was in flight (timing-dependent)";
            s->session->cancel();  // thread-safe (stop source under cancel_mutex_)
            return obj({{"canceled", boolean(true)}, {"was", str("running")}});
        }
        if (st == run_state::suspended) {
            s->steps.push_back(obj({{"op", str("cancel")}}));
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
        std::filesystem::path const scratch = s.sandbox_dir;
        sessions_.erase(it);  // ~DriverSession: the pool finishes queued jobs first (§12 R10)
        // The sandbox is gone with the session; its directory goes now (ADR-208 G7).
        last_scratch_removed_.reset();
        if (!scratch.empty()) last_scratch_removed_ = remove_tree_no_follow(scratch);
        return ToolError{};
    }

    ToolResultJson t_session_close(Value const& args) {
        auto id = get_string(args, "session_id");
        if (!id) return err("test.bad_arguments", "session_id is required");
        ToolError const e = close_session(*id);
        if (!e.code.empty()) return e;
        if (last_scratch_removed_) {
            return obj({{"closed", boolean(true)}, {"scratch_removed", boolean(*last_scratch_removed_)}});
        }
        return obj({{"closed", boolean(true)}});
    }

    ToolResultJson t_model_requests(Value const& args) {
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        return requests_json(*s->requests, s->id, static_cast<std::size_t>(get_u64(args, "since_index").value_or(0)));
    }

    [[nodiscard]] static Value requests_json(RequestLog& log, std::string const& session_id, std::size_t since) {
        auto const [first_index, reqs] = log.snapshot();
        std::vector<Value> out;
        for (std::size_t k = 0; k < reqs.size() && out.size() < 50; ++k) {
            std::size_t const i = first_index + k;
            if (i < since) continue;
            std::vector<Value> messages;
            for (Message const& m : reqs[k].messages) {
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
            for (ToolDescriptor const& t : reqs[k].tools) tools.push_back(str(std::string(t.name)));
            out.push_back(obj({{"index", num(static_cast<double>(i))},
                               {"digest", str(request_digest(reqs[k], session_id))},
                               {"messages", arr(std::move(messages))},
                               {"tools", arr(std::move(tools))}}));
        }
        return obj({{"requests", arr(std::move(out))}, {"total", num(static_cast<double>(log.total()))}});
    }

    // ---- scenarios (ADR-182 §16) ----

    ToolResultJson t_scenario_export(Value const& args) {
        if (args.find("workflow_id") != nullptr) return t_workflow_export(args);
        ToolError e;
        DriverSession* s = find_session(args, e);
        if (s == nullptr) return e;
        if (config_.scenarios_root.empty()) {
            return err("test.export_disabled", "the driver was started without a scenarios root");
        }
        std::string const name = get_string(args, "name").value_or("");
        if (!valid_scenario_name(name)) return err("test.bad_name", "name must match [a-z0-9_-]{1,64}");
        if (s->monitor->state() == run_state::running) {
            return err("test.session_running", "wait until the session settles before exporting");
        }
        if (!s->nondeterministic_reason.empty()) {
            return err("test.nondeterministic", "cannot replay this session: " + s->nondeterministic_reason);
        }
        if (s->exchanges->overflow() || s->exchanges->unrecordable()) {
            return err("test.not_recordable", "this session's model exchanges were not all captured");
        }
        if (s->monitor->dropped() != 0) {
            return err("test.events_dropped", "the event ring overflowed; the expected stream is incomplete");
        }
        if (s->steps.empty()) return err("test.nothing_to_export", "the session has no steps yet");

        std::vector<Value> turns;
        for (ModelExchange const& x : s->exchanges->exchanges()) turns.push_back(exchange_to_turn(x));
        std::vector<Value> events;
        for (LoggedEvent const& ev : s->monitor->events_since(0, kEventRingCapacity)) {
            events.push_back(normalize_ids(event_json(ev), s->id));
        }
        std::string replay_fixture = s->fixture.name;
        if (replay_fixture.ends_with("_live")) replay_fixture.resize(replay_fixture.size() - 5);

        Members recorded{{"fixture", str(s->fixture.name)}, {"model", str(s->fixture.live ? "live" : "scripted")}};
        if (s->fixture.live) recorded.emplace_back("live_model", str(config_.live_description));
        std::size_t const n_turns = turns.size();
        std::size_t const n_events = events.size();
        std::size_t const n_steps = s->steps.size();
        bool const forked = !s->segments.empty();
        Value scenario = obj({
            {"format", num(forked ? 2 : 1)},
            {"name", str(name)},
            {"description", str(get_string(args, "description").value_or(""))},
            {"fixture", str(replay_fixture)},
            {"recorded_with", obj(std::move(recorded))},
            {"segments", arr(s->segments)},
            {"model_turns", arr(std::move(turns))},
            {"steps", arr(s->steps)},
            {"expected", normalize_ids(obj({{"state", str(std::string(run_state_name(s->monitor->state())))},
                                            {"outcome", outcome_json(s->monitor->last_outcome())},
                                            {"events", arr(std::move(events))}}),
                                       s->id)},
        });

        if (s->real) scenario = with_field(scenario, "tool_exchanges", tool_exchanges_json(*s->real));
        ToolResultJson written = write_scenario(name, scenario, args);
        if (auto* we = std::get_if<ToolError>(&written)) return *we;
        Value const* path = std::get<Value>(written).find("written");
        return obj({{"written", path != nullptr ? *path : Value{}},
                    {"steps", num(static_cast<double>(n_steps))},
                    {"model_turns", num(static_cast<double>(n_turns))},
                    {"events", num(static_cast<double>(n_events))}});
    }

    // Writes <scenarios_root>/<name>.json after the secret-canary check (ADR-182 §12 R8).
    ToolResultJson write_scenario(std::string const& name, Value const& scenario, Value const& args) {
        std::error_code ec;
        std::filesystem::create_directories(config_.scenarios_root, ec);
        std::filesystem::path const path = config_.scenarios_root / (name + ".json");
        if (std::filesystem::exists(path) && !get_bool(args, "overwrite").value_or(false)) {
            return err("test.exists", "scenario " + name + " exists; pass overwrite: true to replace it");
        }
        // A scenario file is output too: the same secret canary that guards every reply guards it
        // (ADR-182 §12 R8). Checked before anything is written.
        std::string const text = json::dump(scenario);
        for (std::string const& canary : config_.secret_canaries) {
            if (!canary.empty() && text.find(canary) != std::string::npos) {
                ++secret_leaks_blocked_;
                return err("test.secret_leak_blocked", "the scenario contained a configured secret and was not written");
            }
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return err("test.write_failed", "cannot write " + path.generic_string());
        out << text << "\n";
        return obj({{"written", str(path.generic_string())}});
    }

    // ---- workflows (ADR-210) ----

    DriverWorkflow* find_workflow(Value const& args, ToolError& e) {
        auto id = get_string(args, "workflow_id");
        if (!id) {
            e = err("test.bad_arguments", "workflow_id is required");
            return nullptr;
        }
        auto it = workflows_.find(*id);
        if (it == workflows_.end()) {
            e = err("test.unknown_workflow", "no workflow " + *id + " (workflow_start returns one)");
            return nullptr;
        }
        return it->second.get();
    }

    // A step's session is built here, never by make_session: it has no entry in sessions_, no slot under
    // kMaxSessions and no pool of its own; the workflow's supervisor is the only thing that runs it (I1).
    [[nodiscard]] std::unique_ptr<WorkflowStep> make_step(WorkflowFixture const& f, std::string const& executor_id) {
        auto step = std::make_unique<WorkflowStep>();
        step->executor_id = executor_id;
        step->session_id = f.name + "/" + executor_id;
        Session& session = *step->session;
        Fixture const defaults;
        session.initialize(step->session_id, workflow_principal("test-driver/" + step->session_id), defaults.token_budget,
                           kMaxTurnsPerRun);
        session.set_static_instructions("You are the '" + executor_id + "' step of the " + f.name + " workflow.");
        step->script.emplace();
        std::shared_ptr<ModelBackend> backend = std::make_shared<ScriptedBackend>(*step->script);
        if (config_.workflow_backend_wrapper) backend = config_.workflow_backend_wrapper(executor_id, std::move(backend));
        session.emplace_chat_client(std::move(backend), step->requests, step->exchanges, step->expectations,
                                    step->session_id);
        session.set_suspend_for_approval(false);
        std::vector<std::string> const& names = f.agent_tools.at(executor_id);
        std::vector<ToolDescriptor> tools;
        for (ToolDescriptor const& d : all_test_tool_descriptors()) {
            if (std::find(names.begin(), names.end(), d.name) != names.end()) tools.push_back(d);
        }
        session.history_provider().set_tools(std::move(tools));
        return step;
    }

    ToolResultJson t_workflow_start(Value const& args) {
        auto name = get_string(args, "fixture");
        if (!name) return err("test.bad_arguments", "fixture is required");
        auto const& all = workflow_fixtures();
        auto f = std::find_if(all.begin(), all.end(), [&](WorkflowFixture const& x) { return x.name == *name; });
        if (f == all.end()) {
            // Never a file (ADR-210 §7 M7): workflow fixtures are compiled only.
            return err("test.unknown_fixture", "no workflow fixture " + *name + " (fixtures_list shows them)");
        }
        if (auto ok = check_workflow_fixture(*f); !ok) return err(ok.error().code, ok.error().message);
        if (workflows_.size() >= kMaxWorkflows) {
            return err("test.too_many_workflows", "close a workflow first (max " + std::to_string(kMaxWorkflows) + ")");
        }
        auto w = std::make_unique<DriverWorkflow>();
        w->id = "w" + std::to_string(next_workflow_++);
        w->fixture = f->name;
        std::vector<rt::ExecutorBody> bodies;
        std::vector<EffectContext> contexts;
        std::vector<Value> executors;
        for (workflow::Executor const& e : f->graph.executors) {
            EffectContext ctx;
            ctx.capabilities = w->grant;  // empty for every step (ADR-210 §7 R7, W10)
            contexts.push_back(std::move(ctx));
            std::string kind;
            switch (e.kind) {
                case workflow::executor_kind::agent: {
                    w->steps.push_back(make_step(*f, e.id));
                    bodies.emplace_back(rt::agent_session_as_executor_body(*w->steps.back()->session));
                    kind = "agent";
                    break;
                }
                case workflow::executor_kind::function:
                    bodies.push_back(f->functions.at(e.id));
                    kind = "function";
                    break;
                default:
                    bodies.emplace_back();
                    kind = "request_port";
                    break;
            }
            executors.push_back(obj({{"executor_id", str(e.id)}, {"kind", str(kind)}}));
        }
        w->sup = std::make_unique<rt::WorkflowSupervisor>(kWorkflowWorkers);
        w->sup->set_principal(workflow_principal(std::string(kWorkflowOwnerId)));
        w->sup->set_require_caller(true);
        w->sup->initialize(f->graph, std::move(bodies), std::move(contexts));
        w->stream = w->sup->enable_event_stream(std::pmr::get_default_resource(),
                                                stream_config<workflow::WorkflowEvent>{kWorkflowStructuralCapacity});
        std::string const id = w->id;
        workflows_.emplace(id, std::move(w));
        return obj({{"workflow_id", str(id)}, {"fixture", str(f->name)}, {"executors", arr(std::move(executors))}});
    }

    ToolResultJson t_workflow_script_push(Value const& args) {
        ToolError e;
        DriverWorkflow* w = find_workflow(args, e);
        if (w == nullptr) return e;
        workflow_state const st = w->monitor->state();
        if (st == workflow_state::running) {
            return err("test.workflow_running", "push turns only while the workflow is ready or suspended");
        }
        if (st == workflow_state::finished) return err("test.workflow_finished", "the workflow has finished");
        auto executor = get_string(args, "executor_id");
        WorkflowStep* step = executor ? w->step(*executor) : nullptr;
        if (step == nullptr) return err("test.unknown_step", "executor_id must name an agent step of this workflow");
        Value const* turns = args.find("turns");
        if (turns == nullptr || !turns->is_array() || turns->as_array().empty()) {
            return err("test.bad_arguments", "turns must be a non-empty array");
        }
        std::vector<testing::ScriptedTurn> parsed;
        std::vector<std::optional<std::string>> digests;
        for (Value const& t : turns->as_array()) {
            std::optional<std::string> digest;
            auto turn = parse_turn(t, step->next_call_id, digest);
            if (auto* bad = std::get_if<ToolError>(&turn)) return *bad;
            parsed.push_back(std::move(std::get<testing::ScriptedTurn>(turn)));
            digests.push_back(std::move(digest));
        }
        if (auto pushed = step->script->push(std::move(parsed)); !pushed) {
            return err(pushed.error().code, pushed.error().message);
        }
        step->expectations->push(std::move(digests));
        return obj({{"executor_id", str(step->executor_id)},
                    {"script_pending", num(static_cast<double>(step->script->pending()))}});
    }

    ToolResultJson t_workflow_run(Value const& args) {
        ToolError e;
        DriverWorkflow* w = find_workflow(args, e);
        if (w == nullptr) return e;
        auto text = get_string(args, "text");
        if (!text) return err("test.bad_arguments", "text is required");
        if (w->monitor->cancelled()) return err("test.workflow_cancelled", "the workflow was cancelled");
        // One run per workflow (ADR-210 §7 R5): a second run_workflow would wipe open ports and reuse the steps.
        if (w->ran) return err("test.workflow_already_run", "a workflow runs once; start a new one");
        w->ran = true;
        w->script_log.push_back(obj({{"op", str("run")}, {"text", str(*text)}}));
        w->monitor->set_state(workflow_state::running);
        (void)w->pool->submit(workflow_run_job(w, *text));
        return obj({{"started", boolean(true)}});
    }

    ToolResultJson t_workflow_wait_for(Value const& args) {
        ToolError e;
        DriverWorkflow* w = find_workflow(args, e);
        if (w == nullptr) return e;
        std::string const until = get_string(args, "until").value_or("settled");
        std::uint64_t const timeout = std::min(get_u64(args, "timeout_ms").value_or(kDefaultWaitMs), kMaxWaitMs);
        std::function<bool(workflow_state)> pred;
        if (until == "settled") {
            pred = [](workflow_state s) { return s != workflow_state::running; };
        } else if (until == "suspended") {
            pred = [](workflow_state s) { return s == workflow_state::suspended; };
        } else if (until == "finished") {
            pred = [](workflow_state s) { return s == workflow_state::finished; };
        } else {
            return err("test.bad_arguments", "until must be settled | suspended | finished");
        }
        bool const matched = w->monitor->wait(pred, std::chrono::milliseconds(timeout));
        return obj({{"matched", boolean(matched)}, {"timed_out", boolean(!matched)}, {"snapshot", workflow_snapshot(*w)}});
    }

    // While running, only the monitor is read: the supervisor's accessors are not locked (ADR-210 §7 R4).
    [[nodiscard]] Value workflow_snapshot(DriverWorkflow& w) {
        workflow_state const st = w.monitor->state();
        Members m{{"workflow_id", str(w.id)},
                  {"fixture", str(w.fixture)},
                  {"state", str(std::string(workflow_state_name(st)))},
                  {"events", num(static_cast<double>(w.monitor->event_count()))},
                  {"events_dropped", num(static_cast<double>(w.monitor->dropped()))}};
        if (auto r = w.monitor->last_result()) m.emplace_back("last_result", *r);
        if (std::string const d = w.monitor->driver_outcome(); !d.empty()) m.emplace_back("driver_outcome", str(d));
        if (!w.nondeterministic_reason.empty()) m.emplace_back("nondeterministic_reason", str(w.nondeterministic_reason));
        if (st == workflow_state::running) {
            m.emplace_back("partial", boolean(true));
            return obj(std::move(m));
        }
        m.emplace_back("admission_denied", num(static_cast<double>(w.sup->admission_denied_count())));
        std::vector<Value> steps;
        for (auto const& s : w.steps) {
            Members sm{{"executor_id", str(s->executor_id)},
                       {"script_pending", num(static_cast<double>(s->script->pending()))},
                       {"model_calls", num(static_cast<double>(s->requests->total()))},
                       {"requests_checked", num(static_cast<double>(s->expectations->checked()))}};
            if (auto mm = s->expectations->mismatch()) {
                sm.emplace_back("replay_mismatch", obj({{"call_index", num(static_cast<double>(mm->call_index))},
                                                        {"expected", str(mm->expected)},
                                                        {"actual", str(mm->actual)}}));
            }
            steps.push_back(obj(std::move(sm)));
        }
        m.emplace_back("steps", arr(std::move(steps)));
        // W10: what each step's session actually runs under (the adapter sets it from its context on every call).
        std::size_t with_grant = 0;
        std::size_t kinds_held = 0;
        for (auto const& s : w.steps) {
            CapabilitySet const* c = s->session->capabilities();
            if (c == nullptr) continue;
            ++with_grant;
            for (int k = 0; k < 64; ++k) kinds_held += c->contains_kind(static_cast<capability_kind>(k)) ? 1 : 0;
        }
        m.emplace_back("steps_with_grant", num(static_cast<double>(with_grant)));
        m.emplace_back("step_grant_kinds", num(static_cast<double>(kinds_held)));
        return obj(std::move(m));
    }

    ToolResultJson t_workflow_snapshot(Value const& args) {
        ToolError e;
        DriverWorkflow* w = find_workflow(args, e);
        if (w == nullptr) return e;
        return workflow_snapshot(*w);
    }

    ToolResultJson t_workflow_events(Value const& args) {
        ToolError e;
        DriverWorkflow* w = find_workflow(args, e);
        if (w == nullptr) return e;
        if (auto executor = get_string(args, "executor_id")) {
            if (w->step(*executor) == nullptr) return err("test.unknown_step", "no agent step " + *executor);
            return obj({{"executor_id", str(*executor)}, {"events", arr(w->monitor->step_events(*executor))}});
        }
        std::uint64_t const since = get_u64(args, "since_seq").value_or(0);
        std::size_t const limit =
            static_cast<std::size_t>(std::min<std::uint64_t>(get_u64(args, "limit").value_or(kMaxEventsPerResult), kMaxEventsPerResult));
        return obj({{"events", arr(w->monitor->events_since(since, limit))}});
    }

    ToolResultJson t_request_port_list(Value const& args) {
        ToolError e;
        DriverWorkflow* w = find_workflow(args, e);
        if (w == nullptr) return e;
        if (w->monitor->state() == workflow_state::running) {
            return err("test.workflow_running", "wait until the workflow settles");
        }
        std::vector<Value> out;
        for (rt::WorkflowSupervisor::InteractionAsk const& a : w->sup->open_interaction_asks()) {
            std::string const& id = a.interaction.interaction_id;
            std::string port;
            if (auto at = id.find(":port:"); at != std::string::npos) {
                port = id.substr(at + 6);
                if (auto last = port.rfind(':'); last != std::string::npos) port.resize(last);
            }
            out.push_back(obj({{"interaction_id", str(id)}, {"port", str(port)}, {"ask", str(content_text(a.ask.content))}}));
        }
        return obj({{"open", arr(std::move(out))}});
    }

    // The answer is a user message and the tester's routes, passed as given: the engine alone admits the
    // caller (ADR-169) and decides what the routes select (I3). The driver fixes nothing up (W2).
    ToolResultJson t_request_port_resolve(Value const& args) {
        ToolError e;
        DriverWorkflow* w = find_workflow(args, e);
        if (w == nullptr) return e;
        workflow_state const st = w->monitor->state();
        if (st == workflow_state::running) return err("test.workflow_running", "wait until the workflow settles");
        if (w->monitor->cancelled()) return err("test.workflow_cancelled", "the workflow was cancelled");
        if (st != workflow_state::suspended) return err("test.workflow_not_suspended", "no request port is open");
        auto ix = get_string(args, "interaction_id");
        auto text = get_string(args, "text");
        if (!ix || !text) return err("test.bad_arguments", "interaction_id and text are required");
        std::vector<std::string> routes;
        if (Value const* r = args.find("routes"); r != nullptr) {
            if (!r->is_array() || r->as_array().size() > kMaxResolveRoutes) {
                return err("test.bad_arguments", "routes must be a list of at most " + std::to_string(kMaxResolveRoutes) + " strings");
            }
            for (Value const& v : r->as_array()) {
                if (!v.is_string() || v.as_string().empty() || v.as_string().size() > 64) {
                    return err("test.bad_arguments", "each route is a string of 1-64 characters");
                }
                routes.push_back(v.as_string());
            }
        }
        std::string caller(kWorkflowOwnerId);
        if (auto c = get_string(args, "caller")) {
            if (c->size() > 128 || !agentengine::is_attributable_id(*c)) {
                return err("test.bad_arguments", "caller must be an attributable id: at most 128 bytes, no control "
                                                 "characters, not only invisible characters");
            }
            caller = *c;
        }
        Members step{{"op", str("resolve")}, {"interaction_id", str(*ix)}, {"text", str(*text)}, {"routes", strings_json(routes)}};
        if (caller != kWorkflowOwnerId) step.emplace_back("caller", str(caller));
        w->script_log.push_back(obj(std::move(step)));
        w->monitor->set_state(workflow_state::running);
        (void)w->pool->submit(workflow_resume_job(
            w, rt::ResumeWorkflow{*ix, user_message(*text), std::move(routes), workflow_principal(caller)}));
        return obj({{"started", boolean(true)}});
    }

    // ADR-210 §7 C1: a cancel mid-run lands at a timing-dependent point, so that workflow cannot be exported.
    // A cancel while ready or suspended is the driver's own outcome, labelled as such. Either way it never runs
    // again (the supervisor's cancel is permanent).
    ToolResultJson t_workflow_cancel(Value const& args) {
        ToolError e;
        DriverWorkflow* w = find_workflow(args, e);
        if (w == nullptr) return e;
        workflow_state const st = w->monitor->state();
        if (st == workflow_state::finished) return err("test.workflow_finished", "the workflow has finished");
        w->sup->cancel();
        if (st == workflow_state::running) {
            w->nondeterministic_reason = "cancelled while a run was in flight: where the cancel lands is timing "
                                         "(ADR-210 §7 C1)";
            w->monitor->mark_cancelled("");
        } else {
            w->script_log.push_back(obj({{"op", str("cancel")}}));
            w->monitor->mark_cancelled("cancelled_by_driver");
        }
        return obj({{"cancelled", boolean(true)}, {"state_was", str(std::string(workflow_state_name(st)))}});
    }

    ToolResultJson t_workflow_close(Value const& args) {
        ToolError e;
        DriverWorkflow* w = find_workflow(args, e);
        if (w == nullptr) return e;
        std::string const id = w->id;
        workflows_.erase(id);  // ~DriverWorkflow: cancel, finish the job, then tear down in order
        return obj({{"closed", boolean(true)}});
    }

    ToolResultJson t_workflow_model_requests(Value const& args) {
        ToolError e;
        DriverWorkflow* w = find_workflow(args, e);
        if (w == nullptr) return e;
        auto executor = get_string(args, "executor_id");
        WorkflowStep* step = executor ? w->step(*executor) : nullptr;
        if (step == nullptr) return err("test.unknown_step", "executor_id must name an agent step of this workflow");
        if (w->monitor->state() == workflow_state::running) return err("test.workflow_running", "wait until the workflow settles");
        return requests_json(*step->requests, step->session_id,
                             static_cast<std::size_t>(get_u64(args, "since_index").value_or(0)));
    }

    // Format 3 (ADR-210 §2.5): per-step model turns, the steps, and the expected structural stream, each
    // step's own log and the final result. The cross-step interleaving is never compared.
    ToolResultJson t_workflow_export(Value const& args) {
        ToolError e;
        DriverWorkflow* w = find_workflow(args, e);
        if (w == nullptr) return e;
        if (config_.scenarios_root.empty()) return err("test.export_disabled", "the driver was started without a scenarios root");
        std::string const name = get_string(args, "name").value_or("");
        if (!valid_scenario_name(name)) return err("test.bad_name", "name must match [a-z0-9_-]{1,64}");
        if (w->monitor->state() == workflow_state::running) return err("test.workflow_running", "wait until the workflow settles");
        if (!w->nondeterministic_reason.empty()) {
            return err("test.nondeterministic", "cannot replay this workflow: " + w->nondeterministic_reason);
        }
        if (w->monitor->dropped() != 0) {
            return err("test.events_dropped", "step events were dropped; the expected logs are incomplete");
        }
        if (w->script_log.empty()) return err("test.nothing_to_export", "the workflow has no steps yet");
        Members turns;
        Members step_events;
        for (auto const& s : w->steps) {
            if (s->exchanges->overflow() || s->exchanges->unrecordable()) {
                return err("test.not_recordable", "step " + s->executor_id + "'s model exchanges were not all captured");
            }
            std::vector<Value> t;
            for (ModelExchange const& x : s->exchanges->exchanges()) t.push_back(exchange_to_turn(x));
            turns.emplace_back(s->executor_id, arr(std::move(t)));
            step_events.emplace_back(s->executor_id, arr(w->monitor->step_events(s->executor_id)));
        }
        std::vector<Value> events;
        for (Value const& ev : w->monitor->events_since(0, std::numeric_limits<std::size_t>::max())) {
            events.push_back(without_field(ev, "seq"));
        }
        Members expected{{"state", str(std::string(workflow_state_name(w->monitor->state())))},
                         {"result", w->monitor->last_result().value_or(Value{})},
                         {"events", arr(std::move(events))},
                         {"step_events", obj(std::move(step_events))}};
        if (std::string const d = w->monitor->driver_outcome(); !d.empty()) expected.emplace_back("driver_outcome", str(d));
        Value scenario = obj({{"format", num(3)},
                              {"target", str("workflow")},
                              {"name", str(name)},
                              {"description", str(get_string(args, "description").value_or(""))},
                              {"fixture", str(w->fixture)},
                              {"recorded_with", obj({{"fixture", str(w->fixture)}, {"model", str("scripted")}})},
                              {"model_turns", obj(std::move(turns))},
                              {"steps", arr(w->script_log)},
                              {"expected", obj(std::move(expected))}});
        return write_scenario(name, scenario, args);
    }

    ToolResultJson t_scenario_replay(Value const& args);  // defined after replay_scenario()

    [[nodiscard]] static Value tool_exchanges_json(RealToolLog const& log) {
        std::vector<Value> out;
        for (ToolExchange const& x : log.recorded()) out.push_back(exchange_to_json(x));
        return arr(std::move(out));
    }

    DriverConfig config_;
    std::map<std::string, std::unique_ptr<DriverSession>, std::less<>> sessions_;
    std::uint64_t next_session_ = 1;
    std::map<std::string, std::unique_ptr<DriverWorkflow>, std::less<>> workflows_;
    std::uint64_t next_workflow_ = 1;
    std::uint64_t secret_leaks_blocked_ = 0;
    std::filesystem::path own_scratch_;         // ADR-208: this driver's directory under the sandbox root
    std::optional<bool>   last_scratch_removed_;
};


// ---- Scenario replay (ADR-182 §16) -----------------------------------------------------------------
//
// Replays a scenario in a fresh Driver with the SCRIPTED model: the recorded model turns are pushed
// up front (the script is a FIFO, consumed in call order), each step is re-issued and followed by a
// wait until the session settles, and the whole normalized event stream plus the final state and
// outcome are compared with what was recorded. No network, no Claude, deterministic by construction:
// the only inputs are the scenario file and the engine.

struct ReplayReport {
    bool                     passed = false;
    std::vector<std::string> problems;
    std::size_t              events_compared = 0;
    std::size_t              requests_checked = 0;  // model calls whose request matched a recorded digest (C8)
    // The digest of every request the replay sent the model, in call order: what
    // `agentengine_scenario_runner --stamp-requests` writes into a scenario that predates C8.
    std::vector<std::string> observed_request_digests;
    std::size_t              turns_without_digest = 0;  // §22: any is a failure (C8 would be silently off)
};

namespace replay_detail {

struct Call {
    bool  is_error = false;
    Value body;
};

inline Call call(Driver& d, std::string const& tool, Value args, std::uint64_t& id) {
    Value req = obj({{"jsonrpc", str("2.0")},
                     {"id", num(static_cast<double>(id++))},
                     {"method", str("tools/call")},
                     {"params", obj({{"name", str(tool)}, {"arguments", std::move(args)}})}});
    Call out;
    auto reply = d.handle_line(json::dump(req));
    auto parsed = reply ? json::parse(*reply) : result<Value>(std::unexpected(error{}));
    Value const* res = parsed ? parsed->find("result") : nullptr;
    if (res == nullptr) {
        out.is_error = true;
        out.body = obj({{"error", obj({{"code", str("rpc")}, {"message", str(reply.value_or(""))}})}});
        return out;
    }
    if (Value const* e = res->find("isError"); e != nullptr && e->is_bool()) out.is_error = e->as_bool();
    if (Value const* sc = res->find("structuredContent"); sc != nullptr) out.body = *sc;
    return out;
}

inline std::string clip(std::string s, std::size_t n = 400) {
    if (s.size() > n) s = s.substr(0, n) + "...";
    return s;
}

}  // namespace replay_detail

// Where a replay finds file fixtures (§21). The runner passes the root with no trust check; the
// driver's scenario_replay passes its own root and check.
struct ReplayFixtures {
    std::filesystem::path                                          root;
    std::function<result<std::string>(std::filesystem::path const&)> reader;
};

// ADR-210 §2.5: replays a format 3 workflow scenario in a fresh driver. Each step's recorded turns are pushed
// to that step, the run/resolve/cancel steps are re-issued, and the structural stream, every step's own log
// and the final result are compared. The interleaving across steps is never compared (§3).
[[nodiscard]] inline ReplayReport replay_workflow_scenario(Value const& scenario) {
    using replay_detail::call;
    using replay_detail::Call;
    using replay_detail::clip;
    ReplayReport report;
    auto fail = [&](std::string m) {
        report.problems.push_back(std::move(m));
        return report;
    };
    if (get_u64(scenario, "format") != 3u) return fail("a workflow scenario must be format 3");
    auto fixture = get_string(scenario, "fixture");
    if (!fixture) return fail("scenario has no fixture");
    Value const* turns = scenario.find("model_turns");
    if (turns != nullptr && !turns->is_null() && !turns->is_object()) return fail("model_turns must map step ids to turns");
    Value const* expected = scenario.find("expected");
    if (expected == nullptr || !expected->is_object()) return fail("scenario has no expected block");

    Driver d{DriverConfig{}};
    std::uint64_t id = 1;
    Call started = call(d, "workflow_start", obj({{"fixture", str(*fixture)}}), id);
    if (started.is_error) return fail("workflow_start failed: " + json::dump(started.body));
    std::string const wid = get_string(started.body, "workflow_id").value_or("");
    auto with_wid = [&](Members extra) {
        Members m{{"workflow_id", str(wid)}};
        for (auto& kv : extra) m.push_back(std::move(kv));
        return obj(std::move(m));
    };

    std::size_t turns_total = 0;
    std::size_t turns_undigested = 0;
    if (turns != nullptr && turns->is_object()) {
        for (auto const& [step, list] : turns->as_object()) {
            if (!list.is_array()) return fail("model_turns." + step + " must be a list");
            for (Value const& t : list.as_array()) {
                ++turns_total;
                if (get_string(t, "request_digest").value_or("").empty()) ++turns_undigested;
            }
            if (list.as_array().empty()) continue;
            Call pushed = call(d, "workflow_script_push", with_wid({{"executor_id", str(step)}, {"turns", list}}), id);
            if (pushed.is_error) return fail("workflow_script_push to " + step + " failed: " + json::dump(pushed.body));
        }
    }

    // A step whose request diverged names itself (W4): the first step, in the fixture's order, with a mismatch.
    auto mismatch_in = [&](Value const& snap) -> std::optional<std::string> {
        Value const* steps = snap.find("steps");
        if (steps == nullptr || !steps->is_array()) return std::nullopt;
        for (Value const& s : steps->as_array()) {
            Value const* mm = s.find("replay_mismatch");
            if (mm == nullptr) continue;
            return "test.replay_mismatch at " + get_string(s, "executor_id").value_or("?") + " model call " +
                   std::to_string(get_u64(*mm, "call_index").value_or(0)) + ": the engine's request differs from the "
                   "recording (expected " + get_string(*mm, "expected").value_or("?") + ", got " +
                   get_string(*mm, "actual").value_or("?") + ")";
        }
        return std::nullopt;
    };

    Value const* steps = scenario.find("steps");
    if (steps == nullptr || !steps->is_array()) return fail("scenario has no steps");
    std::size_t index = 0;
    for (Value const& step : steps->as_array()) {
        std::string const op = get_string(step, "op").value_or("");
        std::string const at = "step " + std::to_string(index);
        Call r;
        if (op == "run") {
            r = call(d, "workflow_run", with_wid({{"text", str(get_string(step, "text").value_or(""))}}), id);
        } else if (op == "resolve") {
            Members m{{"interaction_id", str(get_string(step, "interaction_id").value_or(""))},
                      {"text", str(get_string(step, "text").value_or(""))}};
            if (Value const* routes = step.find("routes"); routes != nullptr) m.emplace_back("routes", *routes);
            if (auto caller = get_string(step, "caller")) m.emplace_back("caller", str(*caller));
            r = call(d, "request_port_resolve", with_wid(std::move(m)), id);
        } else if (op == "cancel") {
            r = call(d, "workflow_cancel", with_wid({}), id);
        } else {
            return fail(at + ": unknown op '" + op + "'");
        }
        if (r.is_error) {
            return fail(at + " (" + op + ") was refused: " + clip(json::dump(r.body)) +
                        " -- the replay has diverged before this step");
        }
        Call w = call(d, "workflow_wait_for", with_wid({{"until", str("settled")}, {"timeout_ms", num(60000)}}), id);
        if (get_bool(w.body, "timed_out").value_or(true)) return fail(at + " (" + op + "): the workflow did not settle within 60 s");
        if (Value const* snap = w.body.find("snapshot"); snap != nullptr) {
            if (auto mm = mismatch_in(*snap)) return fail(at + " (" + op + "): " + *mm);
        }
        ++index;
    }

    // The structural stream, in full.
    std::vector<Value> actual;
    for (std::uint64_t since = 0;;) {
        Call page = call(d, "workflow_events", with_wid({{"since_seq", num(static_cast<double>(since))}}), id);
        Value const* evs = page.body.find("events");
        if (evs == nullptr || !evs->is_array() || evs->as_array().empty()) break;
        for (Value const& e : evs->as_array()) {
            since = get_u64(e, "seq").value_or(since);
            actual.push_back(without_field(e, "seq"));
        }
    }
    auto compare_list = [&](std::vector<Value> const& want, std::vector<Value> const& got, std::string const& what) {
        std::size_t const n = std::min(want.size(), got.size());
        for (std::size_t i = 0; i < n; ++i) {
            std::string const a = json::dump(want[i]);
            std::string const b = json::dump(got[i]);
            if (a != b) {
                report.problems.push_back(what + " event " + std::to_string(i) + " differs\n  expected: " + clip(a) +
                                          "\n  actual:   " + clip(b));
                return;
            }
        }
        report.events_compared += n;
        if (want.size() != got.size()) {
            report.problems.push_back(what + " event count differs: expected " + std::to_string(want.size()) +
                                      ", actual " + std::to_string(got.size()));
        }
    };
    std::vector<Value> want_events;
    if (Value const* ev = expected->find("events"); ev != nullptr && ev->is_array()) want_events = ev->as_array();
    compare_list(want_events, actual, "structural");
    if (Value const* se = expected->find("step_events"); se != nullptr && se->is_object()) {
        for (auto const& [step, list] : se->as_object()) {
            Call got = call(d, "workflow_events", with_wid({{"executor_id", str(step)}}), id);
            std::vector<Value> got_list;
            if (Value const* g = got.body.find("events"); g != nullptr && g->is_array()) got_list = g->as_array();
            compare_list(list.is_array() ? list.as_array() : std::vector<Value>{}, got_list, "step " + step);
        }
    }
    Call snap = call(d, "workflow_snapshot", with_wid({}), id);
    std::string const want_state = get_string(*expected, "state").value_or("");
    std::string const got_state = get_string(snap.body, "state").value_or("");
    if (want_state != got_state) report.problems.push_back("final state differs: expected " + want_state + ", actual " + got_state);
    Value const* want_result = expected->find("result");
    Value const* got_result = snap.body.find("last_result");
    std::string const wr = want_result ? json::dump(*want_result) : "null";
    std::string const gr = got_result ? json::dump(*got_result) : "null";
    if (wr != gr) report.problems.push_back("final result differs\n  expected: " + clip(wr) + "\n  actual:   " + clip(gr));
    std::string const want_driver = get_string(*expected, "driver_outcome").value_or("");
    std::string const got_driver = get_string(snap.body, "driver_outcome").value_or("");
    if (want_driver != got_driver) {
        report.problems.push_back("driver outcome differs: expected '" + want_driver + "', actual '" + got_driver + "'");
    }
    if (Value const* ss = snap.body.find("steps"); ss != nullptr && ss->is_array()) {
        for (Value const& s : ss->as_array()) {
            report.requests_checked += static_cast<std::size_t>(get_u64(s, "requests_checked").value_or(0));
            if (auto left = get_u64(s, "script_pending"); left && *left != 0) {
                report.problems.push_back(std::to_string(*left) + " recorded model turn(s) of step " +
                                          get_string(s, "executor_id").value_or("?") + " were never requested");
            }
            std::string const step_id = get_string(s, "executor_id").value_or("");
            for (std::size_t since_index = 0;;) {
                Call page = call(d, "workflow_model_requests",
                                 with_wid({{"executor_id", str(step_id)}, {"since_index", num(static_cast<double>(since_index))}}), id);
                Value const* reqs = page.body.find("requests");
                if (reqs == nullptr || !reqs->is_array() || reqs->as_array().empty()) break;
                for (Value const& q : reqs->as_array()) {
                    report.observed_request_digests.push_back(step_id + ":" + get_string(q, "digest").value_or(""));
                    since_index = static_cast<std::size_t>(get_u64(q, "index").value_or(since_index)) + 1;
                }
            }
        }
    }
    (void)call(d, "workflow_close", with_wid({}), id);
    report.turns_without_digest = turns_undigested;
    if (turns_undigested != 0) {
        report.problems.push_back(std::to_string(turns_undigested) + " of " + std::to_string(turns_total) +
                                  " recorded model turn(s) carry no request_digest, so their requests were not checked");
    }
    report.passed = report.problems.empty();
    return report;
}

[[nodiscard]] inline ReplayReport replay_scenario(Value const& scenario, ReplayFixtures const& fixtures = {}) {
    if (get_string(scenario, "target") == "workflow") return replay_workflow_scenario(scenario);
    using replay_detail::call;
    using replay_detail::Call;
    using replay_detail::clip;
    ReplayReport report;
    auto fail = [&](std::string m) {
        report.problems.push_back(std::move(m));
        return report;
    };
    auto const format = get_u64(scenario, "format");
    if (format != 1u && format != 2u) return fail("unsupported scenario format (expected 1 or 2)");
    auto fixture = get_string(scenario, "fixture");
    if (!fixture) return fail("scenario has no fixture");

    DriverConfig replay_config;
    replay_config.fixtures_root = fixtures.root;
    replay_config.fixture_reader = fixtures.reader;
    // ADR-208: recorded real-tool calls become doubles, handed to the driver through its host config only.
    if (Value const* tx = scenario.find("tool_exchanges"); tx != nullptr && !tx->is_null()) {
        if (!tx->is_array()) return fail("tool_exchanges must be a list");
        if (Value const* sg = scenario.find("segments"); sg != nullptr && sg->is_array() && !sg->as_array().empty()) {
            return fail("a scenario with tool_exchanges cannot have segments (a real-tool session is never forked)");
        }
        std::vector<ToolExchange> doubles;
        for (Value const& v : tx->as_array()) {
            auto x = exchange_from_json(v);
            if (!x) return fail("tool exchange " + std::to_string(doubles.size()) + ": " + x.error().message);
            doubles.push_back(std::move(*x));
        }
        replay_config.tool_doubles = std::move(doubles);
    }
    Driver d(std::move(replay_config));
    std::uint64_t id = 1;
    Call started = call(d, "session_start", obj({{"fixture", str(*fixture)}}), id);
    if (started.is_error) return fail("session_start failed: " + json::dump(started.body));
    std::string sid = get_string(started.body, "session_id").value_or("");
    auto with_sid = [&](Members extra) {
        Members m{{"session_id", str(sid)}};
        for (auto& kv : extra) m.push_back(std::move(kv));
        return obj(std::move(m));
    };

    // Pushes one session's recorded turns and re-issues its steps. Returns a problem, or nothing.
    // `where` prefixes step numbers ("segment 0 step 2") when the scenario has an ancestry (§20).
    auto play = [&](Value const* turns, Value const* steps, std::string const& where) -> std::optional<std::string> {
        if (turns != nullptr && turns->is_array() && !turns->as_array().empty()) {
            Call pushed = call(d, "model_script_push", with_sid({{"turns", *turns}}), id);
            if (pushed.is_error) return where + "model_script_push failed: " + json::dump(pushed.body);
        }
        if (steps == nullptr || !steps->is_array()) return where + "no steps";
        std::size_t index = 0;
        for (Value const& step : steps->as_array()) {
            std::string const op = get_string(step, "op").value_or("");
            std::string const at = where + "step " + std::to_string(index);
            Call r;
            if (op == "send") {
                r = call(d, "session_send", with_sid({{"text", str(get_string(step, "text").value_or(""))}}), id);
            } else if (op == "resolve") {
                std::string const ix = denormalize_id(get_string(step, "interaction_id").value_or(""), sid);
                Value resolve_args =
                    with_sid({{"interaction_id", str(ix)}, {"decision", str(get_string(step, "decision").value_or(""))}});
                if (Value const* cd = step.find("call_decisions"); cd != nullptr) {
                    resolve_args = with_field(std::move(resolve_args), "call_decisions", *cd);
                }
                if (auto approver = get_string(step, "approver_id")) {
                    resolve_args = with_field(std::move(resolve_args), "approver_id", str(*approver));
                }
                r = call(d, "interaction_resolve", std::move(resolve_args), id);
            } else if (op == "cancel") {
                r = call(d, "session_cancel", with_sid({}), id);
            } else {
                return at + ": unknown op '" + op + "'";
            }
            if (r.is_error) {
                return at + " (" + op + ") was refused: " + clip(json::dump(r.body)) +
                       " -- the replay has diverged before this step";
            }
            Call w = call(d, "session_wait_for", with_sid({{"until", str("settled")}, {"timeout_ms", num(60000)}}), id);
            if (get_bool(w.body, "timed_out").value_or(true)) {
                return at + " (" + op + "): the session did not settle within 60 s";
            }
            // C8: the engine asked the model something other than what was recorded. Everything after this
            // point would diff too, so name the cause and stop.
            if (Value const* snap = w.body.find("snapshot"); snap != nullptr) {
                // ADR-208 D2: a real-tool call the recording does not have. Checked first: the double refused the call,
                // so the next model request differs too, and this is the cause.
                if (Value const* tm = snap->find("tool_replay_mismatch"); tm != nullptr) {
                    return at + " (" + op + "): test.replay_mismatch at tool call " +
                           std::to_string(get_u64(*tm, "call_index").value_or(0)) + ": the call differs from the recording" +
                           "\n  expected: " + clip(get_string(*tm, "expected").value_or("?")) +
                           "\n  actual:   " + clip(get_string(*tm, "actual").value_or("?"));
                }
                if (Value const* mm = snap->find("replay_mismatch"); mm != nullptr) {
                    Value const* req = mm->find("actual_request");
                    return at + " (" + op + "): test.replay_mismatch at model call " +
                           std::to_string(get_u64(*mm, "call_index").value_or(0)) +
                           ": the engine's request differs from the recording (expected " +
                           get_string(*mm, "expected").value_or("?") + ", got " +
                           get_string(*mm, "actual").value_or("?") + ")\n  actual request: " +
                           clip(req != nullptr ? json::dump(*req) : std::string("?"), 1200);
                }
            }
            ++index;
        }
        return std::nullopt;
    };

    // Compares the current session with an `expected` block ({state, outcome, events}): the whole
    // normalized event stream, the end state and outcome, and that every pushed turn was requested.
    // `where` prefixes each problem. Returns the snapshot it compared against.
    auto compare = [&](Value const& expected, std::string const& where) -> Value {
        std::vector<Value> actual;
        std::uint64_t since = 0;
        for (;;) {
            Call page = call(d, "session_events", with_sid({{"since_seq", num(static_cast<double>(since))}}), id);
            Value const* evs = page.body.find("events");
            if (evs == nullptr || !evs->is_array() || evs->as_array().empty()) break;
            for (Value const& e : evs->as_array()) {
                actual.push_back(normalize_ids(e, sid));
                since = get_u64(e, "seq").value_or(since);
            }
        }
        Call snap = call(d, "session_snapshot", with_sid({}), id);
        std::vector<Value> expected_events;
        if (Value const* ev = expected.find("events"); ev != nullptr && ev->is_array()) expected_events = ev->as_array();
        std::size_t const before = report.problems.size();
        std::size_t const n = std::min(expected_events.size(), actual.size());
        for (std::size_t i = 0; i < n; ++i) {
            std::string const want = json::dump(expected_events[i]);
            std::string const got = json::dump(actual[i]);
            if (want != got) {
                report.problems.push_back(where + "event " + std::to_string(i) + " differs\n  expected: " + clip(want) +
                                          "\n  actual:   " + clip(got));
                break;
            }
        }
        if (report.problems.size() == before && expected_events.size() != actual.size()) {
            report.problems.push_back(where + "event count differs: expected " + std::to_string(expected_events.size()) +
                                      ", actual " + std::to_string(actual.size()) +
                                      (actual.size() > n ? "; first extra: " + clip(json::dump(actual[n]))
                                                         : "; first missing: " + clip(json::dump(expected_events[n]))));
        }
        report.events_compared += n;
        std::string const want_state = get_string(expected, "state").value_or("");
        std::string const got_state = get_string(snap.body, "state").value_or("");
        if (want_state != got_state) {
            report.problems.push_back(where + "final state differs: expected " + want_state + ", actual " + got_state);
        }
        Value const* want_outcome = expected.find("outcome");
        Value const* got_outcome = snap.body.find("last_outcome");
        std::string const wo = want_outcome ? json::dump(*want_outcome) : "null";
        std::string const go = got_outcome ? json::dump(normalize_ids(*got_outcome, sid)) : "null";
        if (wo != go) {
            report.problems.push_back(where + "final outcome differs\n  expected: " + clip(wo) + "\n  actual:   " + clip(go));
        }
        if (auto left = get_u64(snap.body, "script_pending"); left && *left != 0) {
            report.problems.push_back(where + std::to_string(*left) +
                                      " recorded model turn(s) were never requested: the replay made fewer model calls");
        }
        report.requests_checked += static_cast<std::size_t>(get_u64(snap.body, "requests_checked").value_or(0));
        // ADR-208 D4: every recorded real-tool call must have been made.
        if (Value const* tc = snap.body.find("tool_calls"); tc != nullptr && get_string(*tc, "mode") == "double") {
            std::uint64_t const made = get_u64(*tc, "made").value_or(0);
            std::uint64_t const recorded = get_u64(*tc, "recorded").value_or(0);
            if (made < recorded) {
                report.problems.push_back(where + std::to_string(recorded - made) +
                                          " recorded tool call(s) were never made: the replay made fewer tool calls");
            }
        }
        return snap.body;
    };

    // Every recorded turn must carry its request digest (§22): a scenario without them would replay with
    // C8 silently off. `--stamp-requests` adds them to an older scenario.
    std::size_t turns_total = 0;
    std::size_t turns_undigested = 0;
    auto count_turns = [&](Value const* turns) {
        if (turns == nullptr || !turns->is_array()) return;
        for (Value const& t : turns->as_array()) {
            ++turns_total;
            if (get_string(t, "request_digest").value_or("").empty()) ++turns_undigested;
        }
    };

    // A forked session's ancestry (§20): replay each ancestor, compare it with what it recorded (§22),
    // then fork where it was forked.
    Value const* segs = scenario.find("segments");
    bool const has_segments = segs != nullptr && segs->is_array() && !segs->as_array().empty();
    if (has_segments && format != 2u) return fail("a scenario with segments must be format 2");
    if (!has_segments && format == 2u) return fail("a format 2 scenario must have segments");
    if (has_segments) {
        if (segs->as_array().size() > kMaxForkDepth) return fail("more segments than the fork depth cap");
        std::size_t k = 0;
        for (Value const& seg : segs->as_array()) {
            std::string const where = "segment " + std::to_string(k) + " ";
            auto const at_turn = get_u64(seg, "fork_at_turn");
            Value const* seg_expected = seg.find("expected");
            if (!at_turn) return fail(where + "has no fork_at_turn");
            if (seg_expected == nullptr) return fail(where + "has no expected block");
            count_turns(seg.find("model_turns"));
            if (auto problem = play(seg.find("model_turns"), seg.find("steps"), where)) return fail(*problem);
            (void)compare(*seg_expected, where);
            if (!report.problems.empty()) return report;  // the fork would inherit a divergence
            Call forked = call(d, "session_fork", with_sid({{"at_turn", num(static_cast<double>(*at_turn))}}), id);
            if (forked.is_error) return fail(where + "fork was refused: " + clip(json::dump(forked.body)));
            std::string const next = get_string(forked.body, "session_id").value_or("");
            (void)call(d, "session_close", with_sid({}), id);
            sid = next;
            ++k;
        }
    }
    count_turns(scenario.find("model_turns"));
    if (auto problem = play(scenario.find("model_turns"), scenario.find("steps"), "")) return fail(*problem);

    Value const* expected = scenario.find("expected");
    if (expected == nullptr) return fail("scenario has no expected block");
    (void)compare(*expected, "");
    for (std::size_t since_index = 0;;) {
        Call page = call(d, "model_requests", with_sid({{"since_index", num(static_cast<double>(since_index))}}), id);
        Value const* reqs = page.body.find("requests");
        if (reqs == nullptr || !reqs->is_array() || reqs->as_array().empty()) break;
        for (Value const& r : reqs->as_array()) {
            report.observed_request_digests.push_back(get_string(r, "digest").value_or(""));
            since_index = static_cast<std::size_t>(get_u64(r, "index").value_or(since_index)) + 1;
        }
    }
    (void)call(d, "session_close", with_sid({}), id);
    report.turns_without_digest = turns_undigested;
    if (turns_undigested != 0) {
        report.problems.push_back(std::to_string(turns_undigested) + " of " + std::to_string(turns_total) +
                                  " recorded model turn(s) carry no request_digest, so their requests were not checked "
                                  "(agentengine_scenario_runner --stamp-requests adds them)");
    }
    report.passed = report.problems.empty();
    return report;
}

[[nodiscard]] inline Value report_json(ReplayReport const& r) {
    std::vector<Value> problems;
    for (std::string const& p : r.problems) problems.push_back(str(p));
    return obj({{"passed", boolean(r.passed)},
                {"events_compared", num(static_cast<double>(r.events_compared))},
                {"requests_checked", num(static_cast<double>(r.requests_checked))},
                {"problems", arr(std::move(problems))}});
}

[[nodiscard]] inline result<Value> read_scenario_file(std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(error{failure_class::contract, "cannot read " + path.generic_string(), "test.no_scenario"});
    std::stringstream buf;
    buf << in.rdbuf();
    json::ParseBudget budget;
    budget.max_nodes_visited = 10'000'000;
    auto parsed = json::parse(buf.str(), budget);
    if (!parsed) return std::unexpected(error{failure_class::contract, "scenario is not valid JSON: " + parsed.error().message, "test.bad_scenario"});
    return *parsed;
}

inline ToolResultJson Driver::t_scenario_replay(Value const& args) {
    if (config_.scenarios_root.empty()) {
        return err("test.export_disabled", "the driver was started without a scenarios root");
    }
    std::string const name = get_string(args, "name").value_or("");
    if (!valid_scenario_name(name)) return err("test.bad_name", "name must match [a-z0-9_-]{1,64}");
    auto scenario = read_scenario_file(config_.scenarios_root / (name + ".json"));
    if (!scenario) return err(scenario.error().code, scenario.error().message);
    return report_json(replay_scenario(*scenario, ReplayFixtures{config_.fixtures_root, config_.fixture_reader}));
}

}  // namespace agentengine::test_driver
