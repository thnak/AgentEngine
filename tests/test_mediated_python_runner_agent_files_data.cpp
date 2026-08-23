// Milestone 3 Phase G2 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, 026 §5)
// -- proof that `agent_files_data_codegen.hpp`'s generated Python source actually RUNS under a real
// embedded interpreter: `agent.files.artifact`/`agent.files.input`/`agent.files.list`,
// `agent.data.read_json`/`read_json_lines`/`read_csv_rows`, all against a real scratch mount
// directory, real `_ae_internal.open`/`_ae_internal.listdir` mediation, and real per-call
// `cap::FsRead`/`cap::FsWrite` enforcement -- never a shortcut around F2/Stage D's own pipeline. Also
// proves G1's own `agent_tools_codegen.hpp` fix (the "reuse an existing agent module" change): when
// BOTH a tool bridge and mount_roots are configured for the same session, `agent.tools` and
// `agent.files`/`agent.data` all coexist on the SAME `agent` module object, regardless of which
// bootstrap happened to run first.
//
// Generation-only correctness (the TEXT of the generated source) is covered separately, without any
// CPython dependency, in tests/test_agent_files_data_codegen.cpp -- this file is deliberately narrow:
// does the generated code, once actually executed, behave the way an ordinary Python library would.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/trust/agent_library_manifest.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"
#include "backends/native_jail/tool_bridge.hpp"

using namespace agentengine;
using agentengine::native_jail::MediatedPythonConfig;
using agentengine::native_jail::MediatedPythonRunner;
using agentengine::native_jail::NativeJailBackend;
using agentengine::native_jail::ToolBridgeConfig;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s at %s:%d\n", (label), __FILE__, __LINE__);              \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::printf("  ok: %s\n", (label));                                                    \
        }                                                                                           \
    } while (0)

void write_file(std::filesystem::path const& path, std::string const& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

struct EchoArgs {
    std::string message;
};
AE_JSON_SCHEMA(EchoArgs, message)
struct EchoReply {
    std::string message;
};
AE_JSON_SCHEMA(EchoReply, message)

// Needs cap::decl::Entropy purely as a stand-in "this tool requires some capability" declaration,
// matching test_mediated_python_runner_agent_tools.cpp's own EchoTool -- Scenario 2 below only checks
// that agent.tools/agent.files/agent.data all coexist, not tool-call enforcement (already proven by
// G1's own test file), so the bridge's capability set is left ungranted deliberately.
struct EchoTool : Tool<EchoTool, Capabilities<cap::decl::Entropy>> {
    static constexpr std::string_view name = "echo_tool";
    static constexpr std::string_view description = "Echoes its message argument back.";
    using Args = EchoArgs;
    using Reply = EchoReply;
    static result<Reply> invoke(Args a, EffectContext&) { return Reply{a.message}; }
};

}  // namespace

int main() {
    // Jailed-Python-worker design: one NativeJailBackend for this whole test binary's life, shared
    // across the several sequential MediatedPythonRunner instances constructed below (each fully
    // destructed -- tearing down its jailed worker process -- before the next is constructed).
    NativeJailBackend backend;

    std::string const base = ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") +
                              "/ae_g2_files_data_test";
    std::filesystem::path input_dir = std::filesystem::path(base) / "input";
    std::filesystem::path work_dir = std::filesystem::path(base) / "work";
    std::filesystem::path out_dir = std::filesystem::path(base) / "out";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(input_dir);
    std::filesystem::create_directories(work_dir);
    std::filesystem::create_directories(out_dir);

    write_file(input_dir / "greeting.txt", "hello from /input");
    write_file(input_dir / "data.ndjson", "{\"n\": 1}\n{\"n\": 2}\n\n{\"n\": 3}\n");
    write_file(input_dir / "table.csv", "a,b,c\n1,2,3\n4,5,6\n");

    auto widen = [](std::filesystem::path const& p) { return p.wstring(); };

    // ================================================================================
    // Scenario 1: mount_roots configured, no tool_bridge -- agent.files/agent.data present,
    // agent.tools absent (026 §5a's "ungranted is absent", the mirror image of G1's own N2 control).
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.mount_roots["input"] = widen(input_dir);
        cfg.mount_roots["work"] = widen(work_dir);
        cfg.mount_roots["out"] = widen(out_dir);
        cfg.expose_agent_files_data = true;

        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "G2-setup: a MediatedPythonRunner with mount_roots initializes cleanly");

        ExecState state{};
        EffectContext ctx{};
        auto caps = CapabilitySet::grant_root({
            Capability{cap::FsRead{"input", "", std::nullopt}},
            Capability{cap::FsRead{"work", "", std::nullopt}},
            Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
            Capability{cap::FsRead{"out", "", std::nullopt}},
            Capability{cap::FsWrite{"out", "", std::nullopt, std::nullopt}},
        });
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        // G2-I1: agent.files.input reads /input/<name> as bytes.
        {
            ExecRequest req{"python",
                             "from agent import files\n"
                             "b = files.input('greeting.txt')\n"
                             "print('GOT:', b)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("GOT: b'hello from /input'") != std::string::npos,
                     "G2-I1: agent.files.input reads /input/<name> as real bytes");
        }

        // G2-I2: agent.files.artifact writes /out/<name>, real bytes land on disk.
        {
            ExecRequest req{"python",
                             "from agent import files\n"
                             "files.artifact('result.txt', 'ok from artifact')\n"
                             "print('WROTE')"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("WROTE") != std::string::npos,
                     "G2-I2: agent.files.artifact(str) runs without error");
            std::ifstream check(out_dir / "result.txt", std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>());
            AE_CHECK(content == "ok from artifact",
                     "G2-I2: agent.files.artifact actually wrote the real file to /out on disk");
        }

        // G2-I3: agent.files.list sees real entries, with the right name/is_dir/size shape.
        {
            ExecRequest req{"python",
                             "from agent import files\n"
                             "entries = files.list('/input')\n"
                             "names = sorted(e['name'] for e in entries)\n"
                             "print('NAMES:', names)\n"
                             "print('IS_DIR:', all(e['is_dir'] == False for e in entries))"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() &&
                         out->stdout_text.find("NAMES: ['data.ndjson', 'greeting.txt', 'table.csv']") !=
                             std::string::npos,
                     "G2-I3a: agent.files.list('/input') sees the real files, by name");
            AE_CHECK(out.has_value() && out->stdout_text.find("IS_DIR: True") != std::string::npos,
                     "G2-I3b: agent.files.list reports is_dir correctly for plain files");
        }

        // G3 (026 §5a/§9 G7): agent.files/agent.data get real __doc__ text sourced from
        // trust/agent_library_manifest.hpp's own registry -- the same one-liners 026 §5's table
        // lists -- so help(agent.files)/help(agent.data) show something real, and the top-level
        // agent.__doc__ mentions both (mirroring test_mediated_python_runner_agent_tools.cpp's own
        // matching G3 check for agent.tools).
        {
            ExecRequest req{"python",
                             "from agent import files, data\n"
                             "print('FILES_DOC:', files.__doc__)\n"
                             "print('DATA_DOC:', data.__doc__)\n"
                             "import agent\n"
                             "print('AGENT_DOC:', repr(agent.__doc__))"};
            auto out = runner.run(req, state, ctx);
            std::string files_one_line(agentengine::trust::module_one_line("files"));
            std::string data_one_line(agentengine::trust::module_one_line("data"));
            AE_CHECK(out.has_value() && out->stdout_text.find("FILES_DOC: " + files_one_line) != std::string::npos,
                     "G3: agent.files.__doc__ matches trust/agent_library_manifest.hpp's own "
                     "'files' one-liner exactly");
            AE_CHECK(out.has_value() && out->stdout_text.find("DATA_DOC: " + data_one_line) != std::string::npos,
                     "G3: agent.data.__doc__ matches trust/agent_library_manifest.hpp's own "
                     "'data' one-liner exactly");
            AE_CHECK(out.has_value() && out->stdout_text.find("agent.files: " + files_one_line) != std::string::npos &&
                         out->stdout_text.find("agent.data: " + data_one_line) != std::string::npos,
                     "G3: the top-level agent.__doc__ mentions both agent.files and agent.data with "
                     "the same one-liner text");
        }

        // G2-I4: agent.data.read_json_lines streams NDJSON, one parsed value per non-empty line,
        // skipping the blank line -- proving it's a real per-line generator, not json.load on the
        // whole file (which would raise on this file's multiple top-level values).
        {
            ExecRequest req{"python",
                             "from agent import data\n"
                             "import types\n"
                             "gen = data.read_json_lines('/input/data.ndjson')\n"
                             "is_gen = isinstance(gen, types.GeneratorType)\n"
                             "values = list(gen)\n"
                             "print('IS_GEN:', is_gen)\n"
                             "print('VALUES:', values)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("IS_GEN: True") != std::string::npos,
                     "G2-I4a: agent.data.read_json_lines returns a real generator, streamed per line");
            AE_CHECK(out.has_value() &&
                         out->stdout_text.find("VALUES: [{'n': 1}, {'n': 2}, {'n': 3}]") != std::string::npos,
                     "G2-I4b: agent.data.read_json_lines yields one parsed value per non-empty line, "
                     "blank line skipped");
        }

        // G2-I5: agent.data.read_csv_rows streams rows as lists of string fields.
        {
            ExecRequest req{"python",
                             "from agent import data\n"
                             "rows = list(data.read_csv_rows('/input/table.csv'))\n"
                             "print('ROWS:', rows)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() &&
                         out->stdout_text.find(
                             "ROWS: [['a', 'b', 'c'], ['1', '2', '3'], ['4', '5', '6']]") != std::string::npos,
                     "G2-I5: agent.data.read_csv_rows yields one list-of-fields per line");
        }

        // G2-I6: agent.data.read_json reads a small whole file in one call.
        {
            write_file(work_dir / "small.json", "{\"ok\": true}");
            ExecRequest req{"python",
                             "from agent import data\n"
                             "v = data.read_json('/work/small.json')\n"
                             "print('VALUE:', v)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("VALUE: {'ok': True}") != std::string::npos,
                     "G2-I6: agent.data.read_json reads and parses a whole small file");
        }

        // G2-N1 (negative control): agent.tools is absent -- no tool_bridge was configured this
        // session, matching G1's own "ungranted module is simply absent" precedent.
        {
            ExecRequest req{"python",
                             "try:\n"
                             "    from agent import tools\n"
                             "except ImportError as e:\n"
                             "    print('NO_TOOLS:', e)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("NO_TOOLS") != std::string::npos,
                     "G2-N1: with no tool_bridge configured, agent.tools is absent even though "
                     "agent.files/agent.data are present");
        }

        // G2-N2 (negative control, paired with G2-I3): listing a path without a granted FsRead
        // capability raises PermissionError through the real per-call check -- agent.files.list is
        // never a bypass around the SAME mediation open()/listdir() already enforce.
        {
            EffectContext narrow_ctx{};
            auto narrow_caps = CapabilitySet::grant_root({});  // nothing granted at all
            narrow_ctx.capabilities = agentengine::borrow_capabilities(narrow_caps);
            ExecRequest req{"python",
                             "from agent import files\n"
                             "try:\n"
                             "    files.list('/input')\n"
                             "except PermissionError as e:\n"
                             "    print('DENIED:', e)"};
            auto out = runner.run(req, state, narrow_ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("DENIED") != std::string::npos,
                     "G2-N2: agent.files.list without a granted FsRead capability raises "
                     "PermissionError -- the real per-call pipeline, never a shortcut");
        }
    }

    // ================================================================================
    // Scenario 2: BOTH a tool bridge AND mount_roots configured -- proves G1's own fix (reusing an
    // existing `agent` module rather than recreating it) actually holds under a real interpreter:
    // agent.tools, agent.files, and agent.data all coexist on the SAME `agent` object.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.mount_roots["input"] = widen(input_dir);
        cfg.expose_agent_files_data = true;
        ToolBridgeConfig bridge;
        bridge.bridged_tools = ToolTable::from_tools<EchoTool>();
        bridge.capabilities = {};
        bridge.approved = true;
        cfg.tool_bridge = std::move(bridge);

        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "G2-coexist: setup -- a runner with BOTH tool_bridge and "
                                    "mount_roots initializes cleanly");

        ExecState state{};
        EffectContext ctx{};
        auto caps = CapabilitySet::grant_root({Capability{cap::FsRead{"input", "", std::nullopt}}});
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        ExecRequest req{"python",
                         "import agent\n"
                         "print('HAS_TOOLS:', hasattr(agent, 'tools'))\n"
                         "print('HAS_FILES:', hasattr(agent, 'files'))\n"
                         "print('HAS_DATA:', hasattr(agent, 'data'))\n"
                         "b = agent.files.input('greeting.txt')\n"
                         "print('FILES_OK:', b == b'hello from /input')\n"
                         "print('AGENT_DOC:', repr(agent.__doc__))"};
        auto out = runner.run(req, state, ctx);
        AE_CHECK(out.has_value() && out->stdout_text.find("HAS_TOOLS: True") != std::string::npos,
                 "G2-coexist: agent.tools is present when a tool_bridge is configured");
        AE_CHECK(out.has_value() && out->stdout_text.find("HAS_FILES: True") != std::string::npos,
                 "G2-coexist: agent.files is ALSO present on the SAME agent object, not a second one");
        AE_CHECK(out.has_value() && out->stdout_text.find("HAS_DATA: True") != std::string::npos,
                 "G2-coexist: agent.data is ALSO present on the SAME agent object");
        // G3: since BOTH bootstraps ran this session, agent.__doc__ must carry all three lines --
        // proves the append-not-overwrite composition (agent_tools_codegen.hpp's and this phase's
        // own generator both write to it) actually holds end to end, not just in isolation.
        AE_CHECK(out.has_value() &&
                     out->stdout_text.find("agent.tools: " +
                                            std::string(agentengine::trust::module_one_line("tools"))) !=
                         std::string::npos &&
                     out->stdout_text.find("agent.files: " +
                                            std::string(agentengine::trust::module_one_line("files"))) !=
                         std::string::npos &&
                     out->stdout_text.find("agent.data: " +
                                            std::string(agentengine::trust::module_one_line("data"))) !=
                         std::string::npos,
                 "G3-coexist: agent.__doc__ carries all three lines (tools/files/data) when all "
                 "three bootstraps ran this session -- append, never overwrite");
        AE_CHECK(out.has_value() && out->stdout_text.find("FILES_OK: True") != std::string::npos,
                 "G2-coexist: agent.files still works correctly when agent.tools coexists with it");
    }

    // ================================================================================
    // Scenario 3 (regression control): `mount_roots` configured but `expose_agent_files_data` left
    // at its default `false` -- agent.files/agent.data must stay absent, AND plain `import json` must
    // still be denied. This is the EXACT shape of a real bug this phase found and fixed: an earlier
    // version of this code gated the bootstrap on `!mount_roots.empty()` alone, which silently made
    // `import json` succeed for every PRE-EXISTING test that configures a mount for open()/os
    // mediation but never asked for agent.files/agent.data (test_mediated_python_runner_smoke.cpp's
    // own E2-C5 fail-closed assertion caught this). Proven here too, not just relied upon via that
    // other file, so a future regression on THIS gate fails inside the phase that owns it.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.mount_roots["input"] = widen(input_dir);
        // cfg.expose_agent_files_data left at its default false.

        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "G2-N3: setup -- a runner with mount_roots but no "
                                    "expose_agent_files_data opt-in initializes cleanly");

        ExecState state{};
        EffectContext ctx{};
        ExecRequest req{"python",
                         "try:\n"
                         "    import agent\n"
                         "except ImportError as e:\n"
                         "    print('NO_AGENT:', e)\n"
                         "try:\n"
                         "    import json\n"
                         "except ImportError as e:\n"
                         "    print('NO_JSON:', e)"};
        auto out = runner.run(req, state, ctx);
        AE_CHECK(out.has_value() && out->stdout_text.find("NO_AGENT") != std::string::npos,
                 "G2-N3a: with mount_roots configured but expose_agent_files_data unset, agent is "
                 "absent -- mount_roots alone never implies the library is exposed");
        AE_CHECK(out.has_value() && out->stdout_text.find("NO_JSON") != std::string::npos,
                 "G2-N3b: plain `import json` still fails-closed -- this session's importable set was "
                 "not silently widened by mount_roots being configured for open()/os mediation");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All MediatedPythonRunner agent.files/agent.data checks passed.\n");
    return 0;
}
