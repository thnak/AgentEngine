#pragma once
// The jailed Python worker's mediation engine -- RELOCATED from mediated_python_runner.cpp (that
// file's own Stages A/B/D, ADR-002/003's carried-forward findings: the Layer-0 keep-set, the
// isolated=1/site_import=0 embedding shape, "gate by module name before any loader runs") into the
// process this design's ADR moves real CPython execution into. See mediated_python_runner.hpp's own
// (now much shorter) file header for what changed and why; see 008-Sandbox-and-Isolation.md §1b/§3
// and 010-Python-Code-Interpreter.md §2/§6 for the spec this whole worker-process split implements.
//
// The load-bearing difference from the code this was relocated from: `Internal_call_tool`/
// `Internal_open`/`Internal_listdir`/`Internal_do_connect` no longer call host-side C++ directly
// (`bridge_tool_call`, `open_within_mount_root`, ...) -- they can't; this engine runs in a SEPARATE,
// AppContainer+Job-Object-jailed OS process from the host, and per this design's own I2/I3 posture
// (native_jail_backend.hpp's file header), the worker never holds a `CapabilitySet`,
// `ToolBridgeConfig`, or `EffectContext` at all. Every one of those four bridges now goes through
// `QueryFn` -- a single narrow callback, supplied once at `initialize()` time, that the OWNING process
// (python_worker_main.cpp) implements as "serialize {kind, payload} into a worker_query frame, send it
// over the FramedChannel to the host, block for the matching worker_query_response, return its
// payload." This engine has no idea a pipe or a host process exists on the other end of `QueryFn` --
// it is exactly as host-agnostic as a call to a virtual method would be, which is the point: the same
// engine could be pointed at a `LoopbackQueryChannel`-style in-process stub in a decision-logic test,
// though this pass does not build that seam (see this file's .cpp header comment for the stated
// scope reduction).
//
// `agent.ask` is the one deliberate exception (026 §5 / ADR-057 §9): it needs NO `QueryFn` round trip
// at all -- see `mediated_python_runner.hpp`'s file header ("agent.ask -- no IPC") and
// `Internal_ask_or_raise` in the .cpp for why: it is a purely worker-local decision (consult this
// call's own preseeded-answer list, or raise the AskPending sentinel), never a real effect.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine::native_jail::worker {

// Host-configured, sent once in `init_request` (mediated_python_worker_protocol.hpp) and consumed
// exactly once by `initialize()`. Mirrors `MediatedPythonConfig` (mediated_python_runner.hpp) minus
// the fields that only ever made sense host-side (`mount_roots`, `tool_bridge` -- both carry either a
// live host path or an `InvokeFn` closure over host state, neither of which may ever cross into this
// process, I2) and PLUS `agent_tools_module_source`: the host PRE-RENDERS `agent.tools`' generated
// Python source text from its own `ToolBridgeConfig::bridged_tools` (which holds real `std::function`
// tool implementations this process must never see) via `generate_agent_tools_module_source`
// (agent_tools_codegen.hpp, called from native_jail_backend.cpp, host side) and hands this engine only
// the resulting TEXT -- the worker's `run_agent_tools_bootstrap` below just `PyRun_String`s it, the
// identical shape `run_agent_files_data_bootstrap`/`run_agent_ask_bootstrap` already have for their
// own static (non-generated) module sources.
struct WorkerInitConfig {
    std::string python_home;
    std::vector<std::string> extra_sys_path;
    std::unordered_set<std::string> package_policy_allowlist;
    std::unordered_set<std::string> caller_gated_modules;  // accepted, UNUSED -- Stage C residual,
                                                              // unchanged from mediated_python_runner.hpp
    bool expose_agent_files_data = false;
    bool expose_agent_ask = false;
    // decisions/ADR-154-agent-output-codeact-module.md, 026 §5's `agent.output` (zero capability,
    // same opt-in-per-module shape `expose_agent_ask` above already establishes for another
    // zero-capability module -- a session may hold no reason to want it even though it costs nothing).
    bool expose_agent_output = false;
    // decisions/ADR-155-agent-progress-codeact-module.md, 026 §5's `agent.progress` (zero capability).
    bool expose_agent_progress = false;
    std::uint64_t output_cap_bytes = 0;       // 0 -> this engine's own default (output_discipline.hpp)
    std::string agent_tools_module_source;    // empty -> agent.tools is not bridged this session
};

// Mirrors `ExecOutcome`'s worker-observable subset (sandbox/sandbox.hpp) -- `klass` is spelled as a
// STRING, not `exec_outcome_class`, deliberately: this engine can only ever report the outcomes a
// running script can observe about ITSELF ("ok" or "ask_pending"). `timeout`/`oom`/`crash`/
// `policy_violation`/`escape_attempt` are all outcomes the HOST observes from OUTSIDE (a killed
// process, a broken pipe, a protocol violation) -- python_worker_main.cpp never constructs a
// WorkerExecResult for those; native_jail_backend.cpp's exec_session() classifies them directly from
// the process's death, never from anything this struct claims about itself (I3: the guest process's
// own self-report is not an authoritative signal of its own kill reason).
struct WorkerExecResult {
    std::string klass;  // "ok" | "ask_pending"
    std::string stdout_text;
    std::string stderr_text;
    std::string result_repr;   // meaningful only when klass == "ok"
    std::string ask_prompt;    // meaningful only when klass == "ask_pending"
    // decisions/ADR-154-agent-output-codeact-module.md: JSON-encoded value from `agent.output.set(...)`,
    // meaningful only when klass == "ok" -- empty means never called, same convention as result_repr.
    std::string structured_output_json;
};

// The ONE bridge out of this process. `kind` is one of mediated_python_worker_protocol.hpp's
// `kQuery*` constants; `payload` is this call's own arguments (e.g. `{tool_name, args_json}` for
// `call_tool`). Returns the RESPONSE payload (`json::Value`) the host sent back, or a transport-level
// error (broken pipe, malformed frame) distinct from an ordinary policy denial -- the latter is
// expressed as a `{"ok": false, "error_code": ..., "message": ...}` VALUE inside the successful
// `result<json::Value>`, not as this function's own `result` failing (a policy denial is not a
// transport failure).
using QueryFn = std::function<result<json::Value>(std::string const& kind, json::Value payload)>;

// Must be called exactly once, before `run()`. Mirrors `MediatedPythonRunner::initialize()`'s own
// sequencing (Layer-0 sweep, meta-path finder install, Stage D wrapper bootstrap, agent.tools/
// agent.files/agent.data/agent.ask bootstraps gated on their own opt-in fields) -- see the .cpp for
// exactly which steps carried forward unchanged and which now route through `query_fn` instead of a
// direct C++ call.
[[nodiscard]] result<void> initialize(WorkerInitConfig config, QueryFn query_fn);

[[nodiscard]] bool initialized();

// Executes `source`. `cwd`/`env` are BOTH in/out (the ExecState sync-in/sync-out contract
// `MediatedPythonRunner::run()` already had, 010 §3a) -- python_worker_main.cpp deserializes them
// from the incoming `exec_request` before calling this, and reserializes them into the outgoing
// `exec_response` after. `preseeded_answers` is this call's own ADR-057 §9 replay list -- consumed by
// `agent.ask()` in call order, exactly as `MediatedPythonRunner::run()` already did (no IPC, see this
// file's own header note).
[[nodiscard]] result<WorkerExecResult> run(std::string const& source,
                                            std::vector<std::string> const& preseeded_answers,
                                            std::string& cwd,
                                            std::unordered_map<std::string, std::string>& env);

// Mirrors `MediatedPythonRunner::refresh_agent_tools()` -- re-runs the agent.tools bootstrap against
// a FRESH, host-rendered `module_source` (the identical "host renders text, worker just executes it"
// split `WorkerInitConfig::agent_tools_module_source` already establishes for the construction-time
// case), without tearing down the one embedded interpreter.
[[nodiscard]] result<void> refresh_agent_tools(std::string const& module_source);

// Py_Finalize, if `initialized()`. Called from python_worker_main.cpp's shutdown path (best-effort
// clean exit) -- the Job Object's KILL_ON_JOB_CLOSE backstop (job_object_limits.hpp) is what actually
// guarantees teardown; this is the orderly path when it isn't needed.
void finalize();

}  // namespace agentengine::native_jail::worker
