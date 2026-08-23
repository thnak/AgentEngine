// agentengine_python_worker's entry point -- the jailed Python worker process
// (008-Sandbox-and-Isolation.md §1b/§3, 010-Python-Code-Interpreter.md §2/§6; the design superseding
// native_jail_backend.hpp's former "Correction (2026-08-23)" comment). Spawned exclusively by
// `NativeJailBackend::create_python_worker()` (native_jail_backend.cpp) -- CREATE_SUSPENDED, inside an
// AppContainer, assigned to a Job Object, with exactly two inherited pipe handles and NOTHING else
// (no CapabilitySet, no ToolBridgeConfig, no EffectContext ever crosses into this process -- I2). The
// two handle values arrive as the process's only two command-line arguments (argv[1]/argv[2], decimal
// HANDLE values valid only within THIS process's own handle table, meaningless if observed from
// outside it -- the ordinary Windows idiom for handing an inherited handle to a child rather than a
// name/path it could look up itself).
//
// Protocol: one `init_request`, then a loop of `exec_request`/`refresh_tools_request`/`shutdown`,
// each exec possibly interleaved with `worker_query`/`worker_query_response` round trips the mediation
// engine (python_worker_mediation.hpp) issues via its `QueryFn` callback -- built here as "wrap in the
// envelope, send, block for the reply, hand the raw reply back" (this file owns exec_seq/call_id
// bookkeeping; the mediation engine itself never sees the wire envelope, see that header's own
// comment). This process trusts nothing about its own compromise state -- if the host observes an
// out-of-turn `exec_seq` or a malformed frame, it terminates this whole process outright
// (native_jail_backend.cpp's exec_session() dispatch loop, §8a) rather than expecting this file to
// self-police.

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/json_value.hpp"
#include "backends/native_jail/jailed_worker_rpc.hpp"
#include "backends/native_jail/mediated_python_worker_protocol.hpp"
#include "backends/native_jail/python_worker_mediation.hpp"

namespace {

namespace wp = agentengine::native_jail::worker_protocol;
using agentengine::json::Value;
using agentengine::native_jail::FramedChannel;
namespace worker = agentengine::native_jail::worker;

std::uint64_t g_current_exec_seq = 0;   // 0 == no exec_request currently in flight
std::uint64_t g_next_call_id = 1;

// The one bridge `worker::initialize()` is handed (python_worker_mediation.hpp's `QueryFn`). Builds
// the wire envelope (call_id/exec_seq -- neither of which the mediation engine itself ever
// constructs, see that header's file comment), sends it, and returns the host's raw reply frame
// unchanged: the mediation engine's own `query_or_raise` reads "ok"/"error_code"/"reply_json"/etc.
// straight off it, ignoring the envelope fields it doesn't care about.
worker::QueryFn make_query_fn(FramedChannel const& channel) {
    return [&channel](std::string const& kind, Value payload) -> agentengine::result<Value> {
        Value query = Value::make_object({
            {"type", Value::make_string(wp::kWorkerQuery)},
            {"call_id", Value::make_number(static_cast<double>(g_next_call_id++))},
            {"exec_seq", Value::make_number(static_cast<double>(g_current_exec_seq))},
            {"kind", Value::make_string(kind)},
            {"payload", std::move(payload)},
        });
        if (auto sent = channel.send(query); !sent) return std::unexpected(sent.error());
        return channel.recv();
    };
}

Value build_init_response(agentengine::result<void> const& outcome) {
    if (outcome.has_value()) {
        return Value::make_object({{"type", Value::make_string(wp::kInitResponse)},
                                    {"ok", Value::make_bool(true)}});
    }
    return Value::make_object({{"type", Value::make_string(wp::kInitResponse)},
                                {"ok", Value::make_bool(false)},
                                {"error_message", Value::make_string(outcome.error().message)}});
}

Value build_exec_response(std::uint64_t exec_seq, worker::WorkerExecResult const& r,
                           std::string const& cwd, std::unordered_map<std::string, std::string> const& env) {
    return Value::make_object({
        {"type", Value::make_string(wp::kExecResponse)},
        {"exec_seq", Value::make_number(static_cast<double>(exec_seq))},
        {"klass", Value::make_string(r.klass)},
        {"stdout_text", Value::make_string(r.stdout_text)},
        {"stderr_text", Value::make_string(r.stderr_text)},
        {"result_repr", Value::make_string(r.result_repr)},
        {"ask_prompt", Value::make_string(r.ask_prompt)},
        {"cwd", Value::make_string(cwd)},
        {"env", wp::make_string_map(env)},
    });
}

Value build_exec_error_response(std::uint64_t exec_seq, std::string const& message) {
    // A run() failure this deep (interpreter not initialized, an internal PyRun_String plumbing
    // failure) is not an ordinary script exception -- there is no ExecOutcome klass for it, so the
    // stderr channel carries the message and klass stays "ok" with empty stdout/result, matching
    // native_jail_backend.cpp's own fallback: the host still gets a well-formed exec_response rather
    // than a silently-dropped call.
    return Value::make_object({
        {"type", Value::make_string(wp::kExecResponse)},
        {"exec_seq", Value::make_number(static_cast<double>(exec_seq))},
        {"klass", Value::make_string("ok")},
        {"stdout_text", Value::make_string("")},
        {"stderr_text", Value::make_string("internal worker error: " + message)},
        {"result_repr", Value::make_string("")},
        {"ask_prompt", Value::make_string("")},
        {"cwd", Value::make_string("")},
        {"env", Value::make_object({})},
    });
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 2;  // contract violation: create_python_worker() always passes both handles

    HANDLE downstream_read = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(std::atoll(argv[1])));
    HANDLE upstream_write = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(std::atoll(argv[2])));
    FramedChannel channel(downstream_read, upstream_write);

    auto init_frame = channel.recv();
    if (!init_frame || wp::get_string(*init_frame, "type") != wp::kInitRequest) {
        return 3;  // host protocol violation before we even have a channel worth reporting on
    }

    worker::WorkerInitConfig config;
    config.python_home = wp::get_string(*init_frame, "python_home");
    for (auto const& p : wp::get_string_array(*init_frame, "extra_sys_path")) config.extra_sys_path.push_back(p);
    for (auto const& n : wp::get_string_array(*init_frame, "package_policy_allowlist")) {
        config.package_policy_allowlist.insert(n);
    }
    for (auto const& n : wp::get_string_array(*init_frame, "caller_gated_modules")) {
        config.caller_gated_modules.insert(n);
    }
    config.expose_agent_files_data = wp::get_bool(*init_frame, "expose_agent_files_data");
    config.expose_agent_ask = wp::get_bool(*init_frame, "expose_agent_ask");
    config.output_cap_bytes = static_cast<std::uint64_t>(wp::get_number(*init_frame, "output_cap_bytes"));
    config.agent_tools_module_source = wp::get_string(*init_frame, "agent_tools_module_source");

    worker::QueryFn query_fn = make_query_fn(channel);
    auto init_result = worker::initialize(std::move(config), query_fn);
    if (auto sent = channel.send(build_init_response(init_result)); !sent) return 4;
    if (!init_result) return 5;  // host already saw the failure in init_response; exit quietly

    for (;;) {
        auto frame = channel.recv();
        if (!frame) break;  // host pipe closed (destroy()/shutdown, or the connection just died)

        std::string const type = wp::get_string(*frame, "type");
        if (type == wp::kExecRequest) {
            g_current_exec_seq = wp::get_exec_seq(*frame);
            std::string source = wp::get_string(*frame, "source");
            std::vector<std::string> preseeded_answers = wp::get_string_array(*frame, "preseeded_answers");
            std::string cwd = wp::get_string(*frame, "cwd");
            std::unordered_map<std::string, std::string> env = wp::get_string_map(*frame, "env");

            auto outcome = worker::run(source, preseeded_answers, cwd, env);
            Value response = outcome ? build_exec_response(g_current_exec_seq, *outcome, cwd, env)
                                      : build_exec_error_response(g_current_exec_seq, outcome.error().message);
            g_current_exec_seq = 0;
            if (auto sent = channel.send(response); !sent) break;
        } else if (type == wp::kRefreshToolsRequest) {
            std::string module_source = wp::get_string(*frame, "module_source");
            auto refreshed = worker::refresh_agent_tools(module_source);
            Value response =
                refreshed
                    ? Value::make_object({{"type", Value::make_string(wp::kRefreshToolsResponse)},
                                           {"ok", Value::make_bool(true)}})
                    : Value::make_object({{"type", Value::make_string(wp::kRefreshToolsResponse)},
                                           {"ok", Value::make_bool(false)},
                                           {"error_message", Value::make_string(refreshed.error().message)}});
            if (auto sent = channel.send(response); !sent) break;
        } else if (type == wp::kShutdown) {
            break;
        } else {
            // Unrecognized frame type -- the host is the trusted side of this relationship (this
            // process does not police the host, see this file's own header comment), but there is
            // nothing meaningful to do with a frame we don't understand either; drop it and keep
            // serving, rather than crashing on a forward-compatible message from a newer host.
            continue;
        }
    }

    worker::finalize();
    return 0;
}
