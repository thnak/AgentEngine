#pragma once
// Implements 026-Agent-Facing-Runtime-Surface.md §5 -- Milestone 3 Phase G2
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md). `agent.files`/`agent.data`:
// ordinary-Python convenience wrappers over primitives already mediated one layer down --
// `_ae_internal.open` (Stage D, mediated_python_runner.cpp's own `Internal_open`) and
// `_ae_internal.listdir` (this phase's new `list_within_mount_root`, core/worktree_mount_fs.hpp) --
// never a second capability-checking path of their own. Every call these functions make still goes
// through the SAME per-call `cap::FsRead`/`cap::FsWrite` check `open()`/`listdir()` already enforce
// against `g_current_ctx`; nothing here widens what a call can reach.
//
// Unlike agent_tools_codegen.hpp, there is no per-session-variable schema to derive source from --
// the function set is fixed by 026 §5's table, not generated from caller-supplied metadata -- so this
// header is a pair of static Python source strings, not a generator over a schema. It stays a
// separate, pure-C++, Python-free header purely so the exact generated TEXT stays readable/diffable/
// testable without an embedded interpreter (tests/test_agent_files_data_codegen.cpp), matching
// agent_tools_codegen.hpp's own established shape; the `PyRun_String` execution wiring
// (`run_agent_files_data_bootstrap`) lives in mediated_python_runner.cpp, alongside every other
// CPython-C-API caller.
//
// SCOPE, stated plainly rather than assumed (this project's residual-naming discipline):
//   agent.files.input(name)           -- reads the WHOLE file at /input/<name> into memory as bytes.
//   agent.files.artifact(name, data)  -- writes `data` (str, encoded utf-8, or bytes) to /out/<name>.
//                                         Harvesting the real, written file back into the worktree's
//                                         Tree as a ContentItem (025 §7's "the agent saves a file, the
//                                         user receives an artifact") is Phase F1's `harvest_mount`,
//                                         a HOST-side, post-run step -- this function's whole job is
//                                         just the ordinary write, matching 026 §3's "ordinary Python
//                                         experience" framing.
//   agent.files.list(path)            -- lists the entries under a guest mount path (e.g. "/work"),
//                                         backed by `_ae_internal.listdir` (this phase).
//   agent.data.read_json(path)        -- `json.load` over the whole file -- a small-file convenience,
//                                         not itself a "stays out of memory" claim.
//   agent.data.read_json_lines(path)  -- a GENERATOR yielding one parsed value per physical line
//                                         (NDJSON) -- iterating a real Python file object already
//                                         streams line-by-line without materializing the whole file,
//                                         which is what makes 026 §5's own "without loading them
//                                         wholly into memory" claim literally true here, not aspirational.
//   agent.data.read_csv_rows(path, delimiter=',') -- a GENERATOR streaming the same way, yielding one
//                                         list-of-fields per line via a plain `str.split(delimiter)`.
//                                         Deliberately NOT RFC4180-compliant (no quoted-field/embedded-
//                                         delimiter support) -- named here as a narrower scope than a
//                                         real CSV parser, the same "fails safe over silently wrong"
//                                         tradeoff mediated_python_runner.cpp's own F3 trailing-
//                                         expression split already makes for an analogous reason.
// `agent.memory`/`agent.notes` (026's own table) are 029's job, not built here -- named, not silently
// dropped, matching the breakdown doc's own G2 scope statement.
//
// `agent.files`/`agent.data` share ONE bootstrap and are gated TOGETHER on whether this session has
// any mount configured at all (`MediatedPythonConfig::mount_roots` non-empty) -- there is no
// meaningful "files but not data" or "data but not files" host configuration this design needs to
// express (both read the SAME mounts through the SAME `open()`/`listdir()` primitives), so one gate
// keeps the host-config surface as small as the module surface itself.

#include <string>

#include "agentengine/trust/agent_library_manifest.hpp"  // Milestone 3 Phase G3, 026 §5a/§9 G7
#include "backends/native_jail/agent_tools_codegen.hpp"  // python_string_literal -- shared escaping
                                                           // helper, not duplicated here.

namespace agentengine::native_jail {

// `_agent_module = _sys.modules.get('agent') or a fresh one` -- MUST reuse an existing `agent` module
// object if Phase G1's `agent.tools` bootstrap already created one this session (order-independent:
// whichever of G1's/this phase's bootstrap runs first creates it, the other attaches its own
// submodule to the SAME object). Creating a SECOND, unrelated `agent` module object here -- the way a
// naive `_agent_module = _ModuleType('agent')` would -- would silently detach `agent.tools` from the
// `agent` name `import agent` resolves to next, even though `sys.modules['agent.tools']` itself would
// still be intact: a real, if narrow, regression this phase's own arrival is what would trigger. A
// dedicated small fix to agent_tools_codegen.hpp's own generated source (`generate_agent_tools_module_
// source`) makes G1's side of this reuse-safe too -- see that header's own updated comment.
[[nodiscard]] inline std::string generate_agent_files_data_module_source() {
    std::string src =
        "import sys as _sys\n"
        // Idempotent with G1's own `import json as _json` (agent_tools_codegen.hpp) if that ALSO
        // ran this session -- CPython caches an already-imported module in sys.modules, so a second
        // `import json` is a no-op, never a re-execution. Safe for the SAME reason G1's own comment
        // gives: this bootstrap only ever runs for a session that is already getting a real
        // `agent.files`/`agent.data`, never unconditionally the way F2's raw bridge does.
        "import json as _json\n"
        "_ModuleType = type(_sys)\n"
        "_agent_module = _sys.modules.get('agent')\n"
        "if _agent_module is None:\n"
        "    _agent_module = _ModuleType('agent')\n"
        "    _sys.modules['agent'] = _agent_module\n"
        "\n"
        "_files_module = _ModuleType('agent.files')\n"
        "\n"
        "def _files_input(name):\n"
        "    \"\"\"Reads the whole file at /input/<name> and returns its bytes.\"\"\"\n"
        "    with _ae_internal.open('/input/' + name, 'rb') as _f:\n"
        "        return _f.read()\n"
        "_files_module.input = _files_input\n"
        "\n"
        "def _files_artifact(name, data):\n"
        "    \"\"\"Writes `data` (str or bytes) to /out/<name>.\"\"\"\n"
        "    if isinstance(data, str):\n"
        "        data = data.encode('utf-8')\n"
        "    with _ae_internal.open('/out/' + name, 'wb') as _f:\n"
        "        _f.write(data)\n"
        "_files_module.artifact = _files_artifact\n"
        "\n"
        "def _files_list(path):\n"
        "    \"\"\"Lists the entries under a guest mount path (e.g. '/work'), each as {'name', "
        "'is_dir', 'size'}.\"\"\"\n"
        "    return _json.loads(_ae_internal.listdir(path))\n"
        "_files_module.list = _files_list\n"
        "\n"
        "_agent_module.files = _files_module\n"
        "_sys.modules['agent.files'] = _files_module\n"
        "\n"
        "_data_module = _ModuleType('agent.data')\n"
        "\n"
        "def _data_read_json(path):\n"
        "    \"\"\"Reads the whole file at `path` and parses it as one JSON value.\"\"\"\n"
        "    with _ae_internal.open(path, 'r') as _f:\n"
        "        return _json.load(_f)\n"
        "_data_module.read_json = _data_read_json\n"
        "\n"
        "def _data_read_json_lines(path):\n"
        "    \"\"\"Yields one parsed JSON value per non-empty line (NDJSON), streamed -- never "
        "loads the whole file into memory.\"\"\"\n"
        "    with _ae_internal.open(path, 'r') as _f:\n"
        "        for _line in _f:\n"
        "            _stripped = _line.strip()\n"
        "            if _stripped:\n"
        "                yield _json.loads(_stripped)\n"
        "_data_module.read_json_lines = _data_read_json_lines\n"
        "\n"
        "def _data_read_csv_rows(path, delimiter=','):\n"
        "    \"\"\"Yields one list of fields per non-empty line, streamed. A plain "
        "str.split(delimiter) -- no quoted-field/embedded-delimiter support.\"\"\"\n"
        "    with _ae_internal.open(path, 'r') as _f:\n"
        "        for _line in _f:\n"
        "            _stripped = _line.rstrip('\\r\\n')\n"
        "            if _stripped:\n"
        "                yield _stripped.split(delimiter)\n"
        "_data_module.read_csv_rows = _data_read_csv_rows\n"
        "\n"
        "_agent_module.data = _data_module\n"
        "_sys.modules['agent.data'] = _data_module\n";

    // Milestone 3 Phase G3 (026 §5a/§9 G7) -- see agent_tools_codegen.hpp's own matching comment on
    // its `_tools_module.__doc__` assignment for the full rationale (single source of truth in
    // trust/agent_library_manifest.hpp, append-not-overwrite on `agent.__doc__` since the OTHER
    // bootstrap may already have set it first).
    src += "_files_module.__doc__ = " + python_string_literal(std::string(trust::module_one_line("files"))) +
           "\n";
    src += "_data_module.__doc__ = " + python_string_literal(std::string(trust::module_one_line("data"))) + "\n";
    src += "_agent_module.__doc__ = (getattr(_agent_module, '__doc__', None) or '') + 'agent.files: ' + " +
           python_string_literal(std::string(trust::module_one_line("files"))) + " + '\\n'\n";
    src += "_agent_module.__doc__ += 'agent.data: ' + " +
           python_string_literal(std::string(trust::module_one_line("data"))) + " + '\\n'\n";

    return src;
}

}  // namespace agentengine::native_jail
