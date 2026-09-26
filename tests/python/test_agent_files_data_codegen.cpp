// Proof for Milestone 3 Phase G2 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md,
// 026 §5) -- `src/backends/native_jail/agent_files_data_codegen.hpp`'s static Python source, in
// isolation from any embedded CPython interpreter. Unlike test_agent_tools_codegen.cpp, there is no
// per-session-variable schema to drive this generation (the function set is fixed by 026 §5's table),
// so this file's checks are simpler: the fixed TEXT contains the right function definitions, module
// registrations, and the "reuse an existing `agent` module" logic G1's own agent_tools_codegen.hpp
// needed a matching fix for. The generated source actually RUNNING -- a real
// `agent.files.artifact(...)`/`agent.data.read_json_lines(...)` round trip, negative controls -- is
// proven separately, Python-gated, in tests/test_mediated_python_runner_agent_files_data.cpp.

#include <cstdio>
#include <string>

#include "backends/native_jail/agent_files_data_codegen.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stdout, "  ok: %s\n", what);
    }
}

}  // namespace

int main() {
    using namespace agentengine::native_jail;

    std::string src = generate_agent_files_data_module_source();

    // ---- G2-C1: json is imported (agent.data needs it; safe by the same "only runs when this
    // module is already being built" argument G1's own comment gives). ----
    check(src.find("import json as _json") != std::string::npos,
          "G2-C1: the module source imports json for agent.data's read_json/read_json_lines");

    // ---- G2-C2: the `agent` module object is REUSED if one already exists (G1's `agent.tools`
    // bootstrap may have created it first), never unconditionally recreated -- the fix this phase's
    // own header comment names as necessary for order-independence with G1. ----
    check(src.find("_agent_module = _sys.modules.get('agent')") != std::string::npos &&
              src.find("if _agent_module is None:") != std::string::npos,
          "G2-C2: the agent module is reused via sys.modules.get, not unconditionally recreated");

    // ---- G2-C3: agent.files gets input/artifact/list, each attached as a real module attribute,
    // and both agent.files itself and agent (with .files set) land in sys.modules. ----
    check(src.find("def _files_input(name):") != std::string::npos &&
              src.find("_files_module.input = _files_input") != std::string::npos,
          "G2-C3a: agent.files.input is defined and attached");
    check(src.find("def _files_artifact(name, data):") != std::string::npos &&
              src.find("_files_module.artifact = _files_artifact") != std::string::npos,
          "G2-C3b: agent.files.artifact is defined and attached");
    check(src.find("def _files_list(path):") != std::string::npos &&
              src.find("_files_module.list = _files_list") != std::string::npos,
          "G2-C3c: agent.files.list is defined and attached");
    check(src.find("_agent_module.files = _files_module") != std::string::npos &&
              src.find("_sys.modules['agent.files'] = _files_module") != std::string::npos,
          "G2-C3d: agent.files is registered both as an attribute of agent and in sys.modules");

    // ---- G2-C4: agent.files.artifact writes to /out/<name>, agent.files.input reads /input/<name>
    // -- the RFC's own canonical mount framing (026 §2), never a caller-supplied mount id. ----
    check(src.find("'/input/' + name") != std::string::npos,
          "G2-C4a: agent.files.input reads from the canonical /input mount");
    check(src.find("'/out/' + name") != std::string::npos,
          "G2-C4b: agent.files.artifact writes to the canonical /out mount");

    // ---- G2-C5: agent.files.list is backed by _ae_internal.listdir (this phase's new primitive),
    // decoded via the real json module, never a hand-rolled parse. ----
    check(src.find("_json.loads(_ae_internal.listdir(path))") != std::string::npos,
          "G2-C5: agent.files.list decodes _ae_internal.listdir's JSON wire text via real json.loads");

    // ---- G2-C6: agent.data gets read_json/read_json_lines/read_csv_rows, each attached, and
    // agent.data lands in sys.modules the same way agent.files does. ----
    check(src.find("def _data_read_json(path):") != std::string::npos &&
              src.find("_data_module.read_json = _data_read_json") != std::string::npos,
          "G2-C6a: agent.data.read_json is defined and attached");
    check(src.find("def _data_read_json_lines(path):") != std::string::npos &&
              src.find("_data_module.read_json_lines = _data_read_json_lines") != std::string::npos,
          "G2-C6b: agent.data.read_json_lines is defined and attached");
    check(src.find("def _data_read_csv_rows(path, delimiter=','):") != std::string::npos &&
              src.find("_data_module.read_csv_rows = _data_read_csv_rows") != std::string::npos,
          "G2-C6c: agent.data.read_csv_rows is defined and attached, with a ',' default delimiter");
    check(src.find("_agent_module.data = _data_module") != std::string::npos &&
              src.find("_sys.modules['agent.data'] = _data_module") != std::string::npos,
          "G2-C6d: agent.data is registered both as an attribute of agent and in sys.modules");

    // ---- G2-C7: read_json_lines/read_csv_rows are real GENERATORS (`yield`, not `return [...]`) --
    // the concrete mechanism behind 026 §5's "without loading them wholly into memory" claim. ----
    check(src.find("yield _json.loads(_stripped)") != std::string::npos,
          "G2-C7a: read_json_lines yields per-line, never materializing the whole file as a list");
    check(src.find("yield _stripped.split(delimiter)") != std::string::npos,
          "G2-C7b: read_csv_rows yields per-line, never materializing the whole file as a list");

    if (g_failures == 0) {
        std::fprintf(stdout, "test_agent_files_data_codegen: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_files_data_codegen: %d check(s) failed\n", g_failures);
    return 1;
}
