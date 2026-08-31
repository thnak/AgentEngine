#pragma once
// Implements decisions/ADR-154-agent-output-codeact-module.md -- 026-Agent-Facing-Runtime-Surface.md
// §5's `agent.output` ("Emit structured output conforming to the run's schema", zero capability,
// `trust/agent_library_manifest.hpp`'s `{"output", ...}` row). Same "pure C++, Python-free,
// unit-testable without an embedded interpreter" separation `agent_tools_codegen.hpp`/
// `agent_ask_codegen.hpp` already establish (see either header's own top comment): the generated
// Python source is ordinary string building here; the `PyRun_String` wiring and `_ae_internal.
// set_output`'s real C implementation (a plain synchronous global write, no IPC round trip, no
// suspend/abort -- see `python_worker_mediation.cpp`'s own comment on `Internal_set_output`) live in
// that file alongside every other CPython-C-API caller.
//
// Exactly ONE function with a fixed, hand-authored signature (026 §5's own "small and boring,
// guessable from its name" bar), matching `agent_ask_codegen.hpp`'s own shape for the identical
// reason: this is not schema-derived from caller-supplied tool metadata the way `agent.tools` is.

#include <string>

#include "agentengine/trust/agent_library_manifest.hpp"  // module_one_line("output")
#include "backends/native_jail/agent_tools_codegen.hpp"  // python_string_literal -- shared helper

namespace agentengine::native_jail {

// `_agent_module = _sys.modules.get('agent') or a fresh one` -- MUST reuse an existing `agent` module
// object if another bootstrap already created one this session, the SAME reuse-not-recreate
// discipline `agent_ask_codegen.hpp`'s own header comment explains in full.
[[nodiscard]] inline std::string generate_agent_output_module_source() {
    std::string src =
        "import json as _json\n"
        "import sys as _sys\n"
        "_ModuleType = type(_sys)\n"
        "_agent_module = _sys.modules.get('agent')\n"
        "if _agent_module is None:\n"
        "    _agent_module = _ModuleType('agent')\n"
        "    _sys.modules['agent'] = _agent_module\n"
        "_output_module = _ModuleType('agent.output')\n"
        "\n"
        "def set(value):\n"
        "    \"\"\"Declare this run's structured output. `value` must be JSON-serializable (a dict, "
        "list, str, number, bool, or None). Calling this more than once replaces the previously "
        "declared value -- only the LAST call before the script returns is kept. This does not end "
        "the script or the run; it only records what this execute_code call's own result will "
        "carry as its structured output.\"\"\"\n"
        "    _ae_internal.set_output(_json.dumps(value))\n"
        "_output_module.set = set\n"
        "_agent_module.output = _output_module\n"
        "_sys.modules['agent.output'] = _output_module\n"
        "\n";

    // 026 §5a's docstring convention, sourced from `agent_library_manifest.hpp`'s registry -- the
    // same single source of truth every sibling `agent.*` bootstrap already reads from.
    src += "_output_module.__doc__ = " +
           python_string_literal(std::string(trust::module_one_line("output"))) + "\n";
    src += "_agent_module.__doc__ = (getattr(_agent_module, '__doc__', None) or '') + "
           "'agent.output: ' + " +
           python_string_literal(std::string(trust::module_one_line("output"))) + " + '\\n'\n";
    return src;
}

}  // namespace agentengine::native_jail
