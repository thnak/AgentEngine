#pragma once
// Implements decisions/ADR-155-agent-progress-codeact-module.md -- GitHub issue #31,
// 026-Agent-Facing-Runtime-Surface.md §5's `agent.progress` ("Report progress on long work → run
// event stream", zero capability). Same "pure C++, Python-free, unit-testable without an embedded
// interpreter" separation `agent_tools_codegen.hpp`/`agent_ask_codegen.hpp`/`agent_output_codegen.hpp`
// already establish: the generated Python source is ordinary string building here; the `PyRun_String`
// wiring and `_ae_internal.report_progress`'s real C implementation (a synchronous `g_query_fn` round
// trip, see `python_worker_mediation.cpp`'s own comment on `Internal_report_progress` for why this is
// a request/response call, not a genuinely one-way send) live in that file.
//
// Exactly ONE function with a fixed, hand-authored signature, matching `agent_ask_codegen.hpp`'s/
// `agent_output_codegen.hpp`'s own shape for the identical reason: not schema-derived from
// caller-supplied tool metadata the way `agent.tools` is.

#include <string>

#include "agentengine/trust/agent_library_manifest.hpp"  // module_one_line("progress")
#include "backends/native_jail/agent_tools_codegen.hpp"  // python_string_literal -- shared helper

namespace agentengine::native_jail {

// `_agent_module = _sys.modules.get('agent') or a fresh one` -- MUST reuse an existing `agent` module
// object if another bootstrap already created one this session, the SAME reuse-not-recreate
// discipline every sibling `agent.*` bootstrap's own header comment already explains in full.
[[nodiscard]] inline std::string generate_agent_progress_module_source() {
    std::string src =
        "import sys as _sys\n"
        "_ModuleType = type(_sys)\n"
        "_agent_module = _sys.modules.get('agent')\n"
        "if _agent_module is None:\n"
        "    _agent_module = _ModuleType('agent')\n"
        "    _sys.modules['agent'] = _agent_module\n"
        "_progress_module = _ModuleType('agent.progress')\n"
        "\n"
        "def report(text):\n"
        "    \"\"\"Report progress on long-running work as a short text update. Returns once the "
        "host has recorded it -- this does not end the script or suspend it, unlike agent.ask(). "
        "Call it as often as useful; each call is one event, there is no accumulation or "
        "history.\"\"\"\n"
        "    _ae_internal.report_progress(str(text))\n"
        "_progress_module.report = report\n"
        "_agent_module.progress = _progress_module\n"
        "_sys.modules['agent.progress'] = _progress_module\n"
        "\n";

    // 026 §5a's docstring convention, sourced from `agent_library_manifest.hpp`'s registry.
    src += "_progress_module.__doc__ = " +
           python_string_literal(std::string(trust::module_one_line("progress"))) + "\n";
    src += "_agent_module.__doc__ = (getattr(_agent_module, '__doc__', None) or '') + "
           "'agent.progress: ' + " +
           python_string_literal(std::string(trust::module_one_line("progress"))) + " + '\\n'\n";
    return src;
}

}  // namespace agentengine::native_jail
