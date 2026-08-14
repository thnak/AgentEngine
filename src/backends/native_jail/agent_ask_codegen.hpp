#pragma once
// Implements 026-Agent-Facing-Runtime-Surface.md §5's `agent.ask(prompt: str) -> str`, under
// decisions/ADR-057-agent-ask-suspend-without-deadlock.md's Design B (abort-and-replay) -- the
// mechanism ADR-057 §4's red-team pass proved is the only one of the three candidates buildable
// against this codebase's real runtime substrate (Design A deadlocks on the real, single-worker
// `rt::ThreadPool`; Design C needs multi-thread CPython hardening this embedding has never had).
//
// Same "pure C++, Python-free, unit-testable without an embedded interpreter" separation
// agent_tools_codegen.hpp/agent_files_data_codegen.hpp already establish (see either header's own
// top comment): the generated Python source is ordinary string building here; the `PyRun_String`
// wiring that actually executes it, and `_ae_internal.ask_or_raise`'s real C implementation (the
// preseeded-answer consumption / `AskPending` sentinel-exception mechanism ADR-057 §9 specifies),
// live in mediated_python_runner.cpp alongside every other CPython-C-API caller.
//
// Unlike agent_tools_codegen.hpp (one function per bridged tool, schema-derived) or
// agent_files_data_codegen.hpp (a fixed multi-function pair of modules), `agent.ask` is exactly ONE
// function with a fixed, hand-authored signature (026 §5's own "small and boring, guessable from its
// name" bar) -- so this header, like agent_files_data_codegen.hpp, is a static source string, not a
// generator over caller-supplied metadata.

#include <string>

#include "agentengine/trust/agent_library_manifest.hpp"  // module_one_line("ask") -- 026 §5a/§9 G7's
                                                            // same docstring-sourcing convention every
                                                            // other agent.* module already follows.
#include "backends/native_jail/agent_tools_codegen.hpp"    // python_string_literal -- shared escaping
                                                             // helper, not duplicated here.

namespace agentengine::native_jail {

// `_agent_module = _sys.modules.get('agent') or a fresh one` -- MUST reuse an existing `agent`
// module object if another bootstrap (agent.tools/agent.files/agent.data) already created one this
// session, for the identical reason those headers' own comments give: a naive fresh
// `_ModuleType('agent')` here would silently detach whatever submodule an earlier bootstrap already
// attached to the PREVIOUS `agent` object, even though `sys.modules` itself would still resolve it.
[[nodiscard]] inline std::string generate_agent_ask_module_source() {
    std::string src =
        "import sys as _sys\n"
        "_ModuleType = type(_sys)\n"
        "_agent_module = _sys.modules.get('agent')\n"
        "if _agent_module is None:\n"
        "    _agent_module = _ModuleType('agent')\n"
        "    _sys.modules['agent'] = _agent_module\n"
        "\n"
        "def ask(prompt):\n"
        "    \"\"\"Ask the human a question and get their answer back as a str. If no answer is "
        "available yet, this call ends the current execute_code call (ADR-057 Design B: "
        "abort-and-replay) -- side effects that already ran before this call are NOT undone, and a "
        "re-run of the SAME script (once an answer is available) will run them again. Write code "
        "before agent.ask() idempotent, or move it after the ask, when that matters.\"\"\"\n"
        "    return _ae_internal.ask_or_raise(prompt)\n"
        "_agent_module.ask = ask\n"
        "\n";

    // Milestone 3 Phase G3's own docstring convention (026 §5a/§9 G7), extended to `agent.ask` --
    // sourced from trust/agent_library_manifest.hpp's registry (already lists "ask" -- see that
    // header's `agent_library_registry()`), never a second hand-authored copy of the one-liner text.
    // Appends to `agent.__doc__` rather than overwriting it, matching every sibling bootstrap's own
    // reuse-not-recreate fix, since another agent.* bootstrap may already have set a line there.
    src += "_agent_module.__doc__ = (getattr(_agent_module, '__doc__', None) or '') + 'agent.ask: ' + " +
           python_string_literal(std::string(trust::module_one_line("ask"))) + " + '\\n'\n";
    return src;
}

}  // namespace agentengine::native_jail
