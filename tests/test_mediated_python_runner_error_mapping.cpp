// Milestone 3 Phase G4 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, 026 §3,
// §9 Q2) -- proof that the error-mapping rows this phase touched actually behave the way 026 §3's
// table (and its Q2 resolution, "sourced from real occurrences, never hand-authored") requires under
// a REAL embedded interpreter, not just as generated-text assertions:
//   - "Path outside a mount" -> a real, win32-code-sourced FileNotFoundError, never a hand-authored
//     approximation and never a host diagnostic string (no "GetLastError"/"CreateFileW" in what the
//     guest sees).
//   - "Quota exhausted" -> live, on-disk usage checked before a new write-mode open() -- both the
//     quota_bytes and file_count_cap axes -- raising exactly `OSError("No space left on device")`.
//   - "Host not permitted" -> `ConnectionRefusedError` (a real `ConnectionError` subclass), both on
//     the raw-socket `_ae_internal.do_connect` denial path and on a bridged tool that propagates a
//     `net_egress_proxy` (ADR-011) `net.address_blocked` failure verbatim.
// "Tool denied by policy" (row 6) is already proven by test_mediated_python_runner_agent_tools.cpp's
// G1-N1 -- unchanged this phase, not re-proven here. "Command not found" (row 8, shell-only) is
// test_mediated_shell_runner_smoke.cpp's E3-N1. Only built when AGENTENGINE_BUILD_PYTHON_RUNNER is ON.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/tool_pipeline.hpp"
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

// Returns a tool.invoke() result whose error carries `code` VERBATIM -- the shape a real tool that
// internally calls net_egress_proxy and propagates its `result<T>` failure unchanged would produce
// (tool_pipeline.hpp step 9 passes a tool's own `error` through untouched, `.code` included).
struct BlockedArgs {
    std::string unused;
};
AE_JSON_SCHEMA(BlockedArgs, unused)
struct BlockedReply {
    std::string unused;
};
AE_JSON_SCHEMA(BlockedReply, unused)
struct BlockedNetTool : Tool<BlockedNetTool, Capabilities<cap::decl::Entropy>> {
    static constexpr std::string_view name = "blocked_net_tool";
    static constexpr std::string_view description = "Always fails as if net_egress_proxy blocked the address.";
    using Args = BlockedArgs;
    using Reply = BlockedReply;
    static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(
            error{failure_class::policy, "address is in a blocked range", "net.address_blocked"});
    }
};

}  // namespace

int main() {
    std::string const base = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "C:/Windows/Temp") +
                              "/ae_g4_error_mapping_test";
    std::filesystem::path work_dir = std::filesystem::path(base) / "work";
    std::filesystem::path quota_dir = std::filesystem::path(base) / "quota";
    std::filesystem::path count_dir = std::filesystem::path(base) / "count";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(work_dir);
    std::filesystem::create_directories(quota_dir);
    std::filesystem::create_directories(count_dir);

    auto widen = [](std::filesystem::path const& p) { return p.wstring(); };

    // ================================================================================
    // Row 1: a real, win32-code-sourced FileNotFoundError -- never a hand-authored approximation,
    // never a leaked host diagnostic.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.mount_roots["work"] = widen(work_dir);

        MediatedPythonRunner runner(std::move(cfg));
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "G4-setup: a MediatedPythonRunner with a 'work' mount initializes cleanly");

        ExecState state{};
        EffectContext ctx{};
        auto caps = CapabilitySet::grant_root({
            Capability{cap::FsRead{"work", "", std::nullopt}},
        });
        ctx.capabilities = &caps;

        ExecRequest req{"python",
                         "try:\n"
                         "    open('/work/does_not_exist.txt', 'r')\n"
                         "except FileNotFoundError as e:\n"
                         "    print('KIND:', type(e).__name__)\n"
                         "    print('IS_OSERROR:', isinstance(e, OSError))\n"
                         "    print('TEXT:', str(e))"};
        auto out = runner.run(req, state, ctx);
        AE_CHECK(out.has_value(), "G4-R1: setup -- the script runs");
        AE_CHECK(out.has_value() && out->stdout_text.find("KIND: FileNotFoundError") != std::string::npos,
                 "G4-R1: a read of a nonexistent path under a granted mount raises the real "
                 "FileNotFoundError type, not a generic OSError");
        AE_CHECK(out.has_value() && out->stdout_text.find("IS_OSERROR: True") != std::string::npos,
                 "G4-R1: FileNotFoundError is still an OSError, matching real CPython's own hierarchy");
        AE_CHECK(out.has_value() && out->stdout_text.find("GetLastError") == std::string::npos &&
                     out->stdout_text.find("CreateFileW") == std::string::npos,
                 "G4-R1: the exception text carries NO host diagnostic (026 §3's own rule) -- it is "
                 "CPython's real, win32-code-derived wording, not worktree_mount_fs.cpp's internal "
                 "host-side message");
    }

    // ================================================================================
    // Row 2: live quota enforcement -- both quota_bytes and file_count_cap, checked against real
    // on-disk usage before a new write-mode open() is granted.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.mount_roots["quota"] = widen(quota_dir);
        cfg.mount_roots["count"] = widen(count_dir);

        MediatedPythonRunner runner(std::move(cfg));
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "G4-setup: a MediatedPythonRunner with quota/count mounts initializes cleanly");

        // -- quota_bytes axis: a 10-byte cap on the 'quota' mount. --------------------------------
        {
            ExecState state{};
            EffectContext ctx{};
            auto caps = CapabilitySet::grant_root({
                Capability{cap::FsWrite{"quota", "", std::uint64_t{10}, std::nullopt}},
            });
            ctx.capabilities = &caps;

            // a.txt: 5 bytes, usage before this open() is 0 (<=10) -- allowed.
            auto a = runner.run(ExecRequest{"python", "open('/quota/a.txt','w').write('hello')"}, state, ctx);
            AE_CHECK(a.has_value(), "G4-R2-Q1: a write within quota (usage 0/10 before open) succeeds");

            // b.txt: 6 bytes. Pre-open usage is 5 (<=10) -- allowed, even though writing 6 more bytes
            // pushes real on-disk usage to 11, past the cap -- this pass's own named, narrower scope:
            // checked at the open() boundary, not intercepted mid-write on an already-open handle.
            auto b = runner.run(ExecRequest{"python", "open('/quota/b.txt','w').write('world!')"}, state, ctx);
            AE_CHECK(b.has_value(),
                     "G4-R2-Q2: a write whose pre-open usage (5/10) is still under quota succeeds, even "
                     "though its own bytes push on-disk usage past the cap -- checked at open(), not "
                     "per-byte on an in-flight handle");

            // c.txt: pre-open usage is now 11 (>10) -- denied before any Win32 call.
            auto c = runner.run(ExecRequest{"python",
                                             "try:\n"
                                             "    open('/quota/c.txt','w')\n"
                                             "except OSError as e:\n"
                                             "    print('KIND:', type(e).__name__)\n"
                                             "    print('TEXT:', str(e))"},
                                 state, ctx);
            AE_CHECK(c.has_value() && c->stdout_text.find("KIND: OSError") != std::string::npos,
                     "G4-R2-Q3: a write attempted once usage already exceeds quota_bytes raises OSError");
            AE_CHECK(c.has_value() && c->stdout_text.find("TEXT: No space left on device") != std::string::npos,
                     "G4-R2-Q3: the message is the EXACT text 026 §3's table names -- the same literal "
                     "core/worktree.hpp's own mount_write raises, not re-authored here");
        }

        // -- file_count_cap axis: a 1-file cap on the 'count' mount. -------------------------------
        {
            ExecState state{};
            EffectContext ctx{};
            auto caps = CapabilitySet::grant_root({
                Capability{cap::FsWrite{"count", "", std::nullopt, std::uint32_t{1}}},
            });
            ctx.capabilities = &caps;

            auto x = runner.run(ExecRequest{"python", "open('/count/x.txt','w').write('x')"}, state, ctx);
            AE_CHECK(x.has_value(), "G4-R2-C1: the first file (pre-open count 0/1) is allowed");

            // y.txt: pre-open count is 1, not > 1 -- allowed (matches quota_bytes' own "checked before
            // this open, not retroactively" boundary).
            auto y = runner.run(ExecRequest{"python", "open('/count/y.txt','w').write('y')"}, state, ctx);
            AE_CHECK(y.has_value(), "G4-R2-C2: a second file whose pre-open count (1/1) is not yet OVER the cap is allowed");

            // z.txt: pre-open count is now 2 (>1) -- denied.
            auto z = runner.run(ExecRequest{"python",
                                             "try:\n"
                                             "    open('/count/z.txt','w')\n"
                                             "except OSError as e:\n"
                                             "    print('TEXT:', str(e))"},
                                 state, ctx);
            AE_CHECK(z.has_value() && z->stdout_text.find("TEXT: No space left on device") != std::string::npos,
                     "G4-R2-C3: a third file, once file_count_cap is already exceeded, is denied with "
                     "the same spec-exact message");
        }
    }

    // ================================================================================
    // Row 3: "Host not permitted" -> ConnectionRefusedError, both on the raw-socket denial path and
    // on a bridged tool that propagates a net_egress_proxy denial verbatim.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        ToolBridgeConfig bridge;
        bridge.bridged_tools = ToolTable::from_tools<BlockedNetTool>();
        bridge.capabilities = {cap::Entropy{}};
        bridge.approved = true;
        cfg.tool_bridge = std::move(bridge);

        MediatedPythonRunner runner(std::move(cfg));
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "G4-setup: a MediatedPythonRunner with a blocked-net tool bridge initializes cleanly");

        ExecState state{};
        EffectContext ctx{};
        auto caps = CapabilitySet::grant_root({});  // no cap::NetOut granted at all
        ctx.capabilities = &caps;

        // G4-R3-1: the raw-socket path -- a connect() attempt with no NetOut capability.
        {
            ExecRequest req{"python",
                             "import socket\n"
                             "s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)\n"
                             "try:\n"
                             "    s.connect(('example.com', 80))\n"
                             "except ConnectionRefusedError as e:\n"
                             "    print('KIND:', type(e).__name__)\n"
                             "    print('IS_CONN_ERROR:', isinstance(e, ConnectionError))"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("KIND: ConnectionRefusedError") != std::string::npos,
                     "G4-R3-1: socket.connect() denied for lack of NetOut raises ConnectionRefusedError, "
                     "not PermissionError -- the host stays indistinguishable from a genuinely "
                     "unreachable one (026 §1a)");
            AE_CHECK(out.has_value() && out->stdout_text.find("IS_CONN_ERROR: True") != std::string::npos,
                     "G4-R3-1: ConnectionRefusedError is still a ConnectionError, matching 026 §3's own "
                     "sanctioned exception family for this row");
        }

        // G4-R3-2: a bridged tool that propagates net_egress_proxy's own "net.address_blocked" code
        // verbatim -- the SAME exception shape, reached through call_tool instead of raw sockets.
        {
            ExecRequest req{"python",
                             "from agent import tools\n"
                             "try:\n"
                             "    tools.blocked_net_tool(unused='')\n"
                             "except ConnectionRefusedError as e:\n"
                             "    print('KIND:', type(e).__name__)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("KIND: ConnectionRefusedError") != std::string::npos,
                     "G4-R3-2: a bridged tool's own 'net.address_blocked' failure surfaces as "
                     "ConnectionRefusedError through agent.tools too, not RuntimeError's fallback");
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All MediatedPythonRunner error-mapping (Milestone 3 Phase G4) checks passed.\n");
    return 0;
}
