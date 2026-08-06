// Milestone 3 Phase G1 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, 026
// §4/§5) -- proof that `agent_tools_codegen.hpp`'s generated Python source actually RUNS under a
// real embedded interpreter: `from agent import tools; tools.<name>(...)` as an ordinary Python
// call, dir()/help() discoverability, the fail-closed default when no tool bridge is configured,
// and that a call still traverses the REAL 006 §3 pipeline (denied without the bridge's own
// capability, same as test_tool_bridge.cpp's own F2-C1 proves for the raw bridge). Only built when
// AGENTENGINE_BUILD_PYTHON_RUNNER is ON.
//
// Generation-only correctness (the TEXT of the generated source: signatures, escaping, the
// zero-argument/bad-identifier edge cases) is covered separately, without any CPython dependency, in
// tests/test_agent_tools_codegen.cpp -- this file is deliberately narrow: does the generated code,
// once actually executed, behave the way an ordinary Python library would.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/agent_library_manifest.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"
#include "backends/native_jail/tool_bridge.hpp"

using namespace agentengine;
using agentengine::native_jail::MediatedPythonConfig;
using agentengine::native_jail::MediatedPythonRunner;
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

struct EchoArgs {
    std::string message;
};
AE_JSON_SCHEMA(EchoArgs, message)
struct EchoReply {
    std::string message;
};
AE_JSON_SCHEMA(EchoReply, message)

// Needs cap::decl::Entropy purely as a stand-in "this tool requires some capability" declaration,
// the same pattern test_tool_bridge.cpp's own EchoTool already uses.
struct EchoTool : Tool<EchoTool, Capabilities<cap::decl::Entropy>> {
    static constexpr std::string_view name = "echo_tool";
    static constexpr std::string_view description = "Echoes its message argument back.";
    using Args = EchoArgs;
    using Reply = EchoReply;
    static result<Reply> invoke(Args a, EffectContext&) { return Reply{a.message}; }
};

}  // namespace

int main() {
    // ================================================================================
    // Scenario 1: a tool bridge configured with the required capability granted AND bundled
    // approval -- agent.tools.echo_tool should work exactly like an ordinary Python function.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        ToolBridgeConfig bridge;
        bridge.bridged_tools = ToolTable::from_tools<EchoTool>();
        bridge.capabilities = {cap::Entropy{}};
        bridge.approved = true;
        cfg.tool_bridge = std::move(bridge);

        MediatedPythonRunner runner(std::move(cfg));
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "G1-setup: a MediatedPythonRunner with a tool bridge initializes cleanly");

        ExecState state{};
        EffectContext ctx{};

        // G1-I1: an ordinary `from agent import tools` import, then an ordinary keyword call.
        {
            ExecRequest req{"python",
                             "from agent import tools\n"
                             "r = tools.echo_tool(message='hello from agent.tools')\n"
                             "print('GOT:', r.message)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value(), "G1-I1: setup -- the script runs");
            AE_CHECK(out.has_value() && out->stdout_text.find("GOT: hello from agent.tools") != std::string::npos,
                     "G1-I1: tools.echo_tool(message=...) is an ordinary keyword call returning an "
                     "attribute-accessible reply, real round trip through the F2 bridge and back");
        }

        // G1-I2: dir()/help() discoverability (026 §4) -- real function objects, real docstrings,
        // no special-casing needed since these are ordinary Python objects.
        {
            ExecRequest req{"python",
                             "import agent\n"
                             "print('HAS_ECHO:', 'echo_tool' in dir(agent.tools))\n"
                             "print('DOC:', agent.tools.echo_tool.__doc__)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("HAS_ECHO: True") != std::string::npos,
                     "G1-I2: agent.tools.echo_tool is discoverable via dir(), matching 026 §4's claim");
            AE_CHECK(out.has_value() &&
                         out->stdout_text.find("DOC: Echoes its message argument back.") != std::string::npos,
                     "G1-I2: the tool's real description is a real Python docstring -- help() works");
        }

        // G3 (026 §5a/§9 G7): the `agent.tools` MODULE itself, and the top-level `agent` namespace,
        // both get real `__doc__` text sourced from trust/agent_library_manifest.hpp's registry --
        // the SAME data 026 §5's own table lists -- so `help(agent)`/`help(agent.tools)` show
        // something real rather than nothing, and the text can never drift from the canonical
        // one-liner since it is read from that single registry, never hand-duplicated here.
        {
            ExecRequest req{"python",
                             "import agent\n"
                             "print('TOOLS_DOC:', agent.tools.__doc__)\n"
                             "print('AGENT_DOC:', repr(agent.__doc__))"};
            auto out = runner.run(req, state, ctx);
            std::string expected_one_line(agentengine::trust::module_one_line("tools"));
            AE_CHECK(out.has_value() && out->stdout_text.find("TOOLS_DOC: " + expected_one_line) !=
                                             std::string::npos,
                     "G3: agent.tools.__doc__ matches trust/agent_library_manifest.hpp's own "
                     "'tools' one-liner exactly -- one source of truth, no drift");
            AE_CHECK(out.has_value() &&
                         out->stdout_text.find("agent.tools: " + expected_one_line) != std::string::npos,
                     "G3: the top-level agent.__doc__ mentions agent.tools with the same one-liner text");
        }

        // G1-I3: an omitted optional-style keyword still round-trips correctly through JSON (proves
        // real encode/decode, not a literal string echo) -- reuse message containing characters that
        // would break a naive, unescaped JSON encoding if one were hand-rolled instead of using
        // Python's own json module.
        {
            ExecRequest req{"python",
                             "from agent import tools\n"
                             "r = tools.echo_tool(message='quote\" and backslash\\\\ and newline\\n')\n"
                             "print('ROUNDTRIP:', repr(r.message))"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() &&
                         out->stdout_text.find(R"(ROUNDTRIP: 'quote" and backslash\\ and newline\n')") !=
                             std::string::npos,
                     "G1-I3: special characters round-trip exactly through the real json.dumps/json.loads "
                     "wire encoding, not a hand-rolled approximation");
        }
    }

    // ================================================================================
    // Scenario 2 (negative control, 022 §5 pairing against Scenario 1's positive case): the SAME
    // tool bridged WITHOUT the required capability -- the call still traverses the real 006 §3
    // pipeline and is denied, proving agent.tools is a thin wrapper over the real bridge, never a
    // shortcut that bypasses capability enforcement.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        ToolBridgeConfig bridge;
        bridge.bridged_tools = ToolTable::from_tools<EchoTool>();
        bridge.capabilities = {};  // no cap::Entropy granted
        bridge.approved = true;
        cfg.tool_bridge = std::move(bridge);

        MediatedPythonRunner runner(std::move(cfg));
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "G1-N1: setup -- a second interpreter initializes cleanly");

        ExecState state{};
        EffectContext ctx{};
        ExecRequest req{"python",
                         "from agent import tools\n"
                         "try:\n"
                         "    tools.echo_tool(message='should be denied')\n"
                         "except PermissionError as e:\n"
                         "    print('DENIED:', e)"};
        auto out = runner.run(req, state, ctx);
        AE_CHECK(out.has_value() && out->stdout_text.find("DENIED") != std::string::npos,
                 "G1-N1: agent.tools.echo_tool() without the bridge's required capability raises "
                 "PermissionError -- the real pipeline denial, not a bypass");
    }

    // ================================================================================
    // Scenario 3 (negative control): no tool_bridge configured at all -- `agent` is simply absent,
    // fail-closed by default, matching every other host-configured surface in this file.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        // cfg.tool_bridge left at its default nullopt.

        MediatedPythonRunner runner(std::move(cfg));
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "G1-N2: setup -- a third interpreter (no tool bridge) initializes cleanly");

        ExecState state{};
        EffectContext ctx{};
        ExecRequest req{"python",
                         "try:\n"
                         "    import agent\n"
                         "except ModuleNotFoundError as e:\n"
                         "    print('NO_AGENT:', e)"};
        auto out = runner.run(req, state, ctx);
        AE_CHECK(out.has_value() && out->stdout_text.find("NO_AGENT") != std::string::npos,
                 "G1-N2: with no tool bridge configured, 'import agent' fails ModuleNotFoundError -- "
                 "an ungranted module is simply absent (026 §5a), not present-but-empty");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All MediatedPythonRunner agent.tools checks passed.\n");
    return 0;
}
