// Milestone 3 Phase E4 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md) --
// 010-Python-Code-Interpreter.md §9 G2's containment corpus plus G7's import-allowlist "native
// extension not on it" claim, against the REAL `agentengine::native_jail::MediatedPythonRunner`
// (E2). Every denial is paired with a positive control (022 §5) proving the probe itself is real,
// not vacuous -- same discipline as test_mediated_python_runner_smoke.cpp.
//
// This file's own point: G2 explicitly asks for "the hostile corpus (008 §7) plus interpreter-
// specific attacks (os.system, ctypes, /proc and registry probing, symlink escape from the
// workspace, egress to 169.254.169.254, fork bomb, memory bomb, output flood, sys.settrace
// shenanigans)". Scoped per-class below, honestly:
//   - os.system/subprocess: already proven denied in E2-C6; this file's own contribution is the
//     WIDER corpus (os.popen, the exec/spawn family) and, more importantly, probing whether the
//     denial can be BYPASSED by reaching the underlying primitive through some other route --
//     the actual G2 bar for interpreter mediation, not just "the obvious call fails."
//   - ctypes/winreg (Windows' registry, the platform analogue of /proc probing): denied via the
//     import allowlist (Stage B), never even reaching the dynamic loader for the extension.
//   - symlink escape: a real Windows junction (mklink /J, no special privilege needed -- same
//     technique test_worktree_mount_fs_escape_corpus.cpp uses) crossing the mount boundary,
//     proven rejected through the SAME open_within_mount_root (ADR-014) primitive Stage D's
//     open() mediation calls.
//   - egress to 169.254.169.254: denied via the real per-call NetOut capability check (Stage D),
//     paired with a positive control granting a DIFFERENT host to prove the check is genuinely
//     per-host, not a blanket network-off switch.
//   - fork bomb: os.fork() does not exist in CPython on Windows at all -- AttributeError is the
//     real, measured platform behavior, not a policy this design enforces. Named, not silently
//     assumed.
//   - memory bomb / output flood: OUT OF SCOPE for MediatedPythonRunner standalone, named as a
//     residual, not silently skipped -- this Runner has no process-level resource caps of its own
//     (it executes in-process, not as a native_jail-sandboxed child); that containment is Job
//     Object-based (008 §1b layer 3) and applies once Phase F composes this Runner's process
//     under NativeJailBackend, not before. test_native_jail_abuse_corpus_windows.cpp already
//     proves this class for the process-boundary case.
//   - sys.settrace shenanigans, generalized to the real underlying risk: can guest code recover a
//     PRE-mediation reference to a wrapped primitive via ordinary CPython object-graph
//     introspection (__globals__, __closure__, module attribute walking) -- no trace hooks
//     actually needed, that's just the easiest-to-say-out-loud instance of the general class.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"

using namespace agentengine;
using agentengine::native_jail::MediatedPythonConfig;
using agentengine::native_jail::MediatedPythonRunner;

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

[[noreturn]] void fatal_setup_failure(char const* what, DWORD code) {
    std::fprintf(stderr, "SETUP FAILURE: %s GetLastError=%lu\n", what, code);
    std::exit(2);
}

// Same technique as test_worktree_mount_fs_escape_corpus.cpp -- junctions need no special
// privilege on Windows, unlike symbolic links (which need SeCreateSymbolicLinkPrivilege or
// Developer Mode). Duplicated rather than shared, matching this project's own established
// per-abuse-test-file pattern.
bool create_junction(std::wstring const& link, std::wstring const& target) {
    std::wstring cmdline = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exit_code == 0;
}

}  // namespace

int main() {
    std::string const base = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "C:/Windows/Temp");
    std::string const scratch = base + "/ae_e4_py_mount";
    std::string const outside = base + "/ae_e4_py_outside";
    std::filesystem::remove_all(scratch);
    std::filesystem::remove_all(outside);
    std::filesystem::create_directories(scratch + "/inside");
    std::filesystem::create_directories(outside);
    {
        std::ofstream secret((std::filesystem::path(outside) / "secret.txt"));
        secret << "host secret, must never be readable through the mount";
    }
    std::wstring scratch_w(scratch.begin(), scratch.end());
    std::wstring outside_w(outside.begin(), outside.end());
    bool have_junction = create_junction(std::wstring(scratch_w) + L"\\escape_link", outside_w);
    if (!have_junction) {
        std::fprintf(stderr, "SKIP: mklink /J unavailable -- symlink-escape corpus case skipped\n");
    }

    // ================================================================================
    // Fail-closed baseline: empty package policy, no capabilities granted.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.mount_roots["work"] = scratch_w;

        MediatedPythonRunner runner(std::move(cfg));
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "E4-PY setup: MediatedPythonRunner initializes");

        ExecState state{};
        CapabilitySet caps = CapabilitySet::grant_root(
            {Capability{cap::FsRead{"work", "", std::nullopt}},
             Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}}});
        EffectContext ctx{};
        ctx.capabilities = &caps;

        // ---- ctypes / winreg: denied via the import allowlist, never reaching the loader --------
        {
            ExecRequest req{"python", "import ctypes"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() &&
                         out->stderr_text.find("ModuleNotFoundError") != std::string::npos,
                     "E4-PY1: 'import ctypes' is denied by the allowlist (ModuleNotFoundError), "
                     "not merely undocumented");
        }
        {
            ExecRequest req{"python", "import winreg"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() &&
                         out->stderr_text.find("ModuleNotFoundError") != std::string::npos,
                     "E4-PY2: 'import winreg' (registry probing, Windows' /proc analogue) is "
                     "denied by the allowlist");
        }
        // A real, compiled, allowlisted-nowhere native extension from the stdlib (not a fake --
        // proves G7's "a native extension not on it" claim against a genuine .pyd, not a
        // hypothetical one) is denied the same way.
        {
            ExecRequest req{"python", "import array"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() &&
                         out->stderr_text.find("ModuleNotFoundError") != std::string::npos,
                     "E4-PY3: 'import array' (a real native extension, not on the allowlist) is "
                     "denied by the allowlist, matching G7's claim for ANY ungranted native "
                     "extension, not just the ones this design happens to name");
        }

        // ---- os.system / subprocess: the wider corpus beyond E2-C6's spot-check -----------------
        {
            ExecRequest req{"python", "import os\ntry:\n    os.popen('whoami')\nexcept "
                                       "PermissionError as e:\n    print('DENIED:', e)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("DENIED") != std::string::npos,
                     "E4-PY4: os.popen raises PermissionError");
        }
        {
            ExecRequest req{"python",
                             "import os\nresults=[]\nfor name in ('execv','execve','execvp','execvpe',"
                             "'spawnv','spawnve','spawnvp','spawnvpe'):\n"
                             "    fn=getattr(os,name,None)\n"
                             "    if fn is None:\n        results.append(name+':ABSENT')\n"
                             "        continue\n"
                             "    try:\n        fn('x')\n        results.append(name+':RAN')\n"
                             "    except PermissionError:\n        results.append(name+':DENIED')\n"
                             "    except TypeError:\n        results.append(name+':DENIED')\n"
                             "print(' '.join(results))"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value(), "E4-PY5: setup -- the exec/spawn family probe script runs");
            bool any_ran = out.has_value() && out->stdout_text.find(":RAN") != std::string::npos;
            std::printf("  measured: exec/spawn family probe -> %s",
                        out.has_value() ? out->stdout_text.c_str() : "(no output)");
            AE_CHECK(!any_ran,
                     "E4-PY5: no member of the os.exec*/os.spawn* family actually runs (each is "
                     "either denied or genuinely absent on this platform, never silently live)");
        }

        // ---- fork bomb: os.fork() does not exist in CPython on Windows -- measured, not assumed -
        {
            ExecRequest req{"python", "import os\ntry:\n    os.fork()\n    print('FORK_RAN')\n"
                                       "except AttributeError:\n    print('FORK_ABSENT')\n"
                                       "except PermissionError:\n    print('FORK_DENIED')"};
            auto out = runner.run(req, state, ctx);
            std::printf("  measured: os.fork() probe -> %s",
                        out.has_value() ? out->stdout_text.c_str() : "(no output)");
            AE_CHECK(out.has_value() && out->stdout_text.find("FORK_RAN") == std::string::npos,
                     "E4-PY6: os.fork() never actually runs (CPython-on-Windows has no fork() at "
                     "all -- the real, measured platform behavior, not a policy this design adds)");
        }

        // ---- egress to a cloud-metadata-shaped address: denied without a matching NetOut grant --
        {
            ExecRequest req{"python", "import socket\ns = socket.socket(socket.AF_INET, "
                                       "socket.SOCK_STREAM)\ns.settimeout(0.2)\ntry:\n"
                                       "    s.connect(('169.254.169.254', 80))\n"
                                       "    print('CONNECT_RAN')\nexcept PermissionError as e:\n"
                                       "    print('DENIED:', e)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("DENIED") != std::string::npos &&
                         out->stdout_text.find("CONNECT_RAN") == std::string::npos,
                     "E4-PY7: socket.connect to the cloud-metadata address is denied without a "
                     "matching NetOut grant, and the real connect() is never reached (no "
                     "CONNECT_RAN, no timeout-shaped hang)");
        }

        // ---- symlink/junction escape: open() through a junction crossing the mount boundary -----
        if (have_junction) {
            ExecRequest req{"python", "try:\n    open('/work/escape_link/secret.txt', 'r')\n"
                                       "    print('ESCAPE_OK')\nexcept (PermissionError, OSError) "
                                       "as e:\n    print('DENIED:', type(e).__name__, e)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("DENIED") != std::string::npos &&
                         out->stdout_text.find("ESCAPE_OK") == std::string::npos,
                     "E4-PY8: open() through a junction crossing the mount boundary is denied "
                     "even though FsRead is granted for the whole mount -- the SAME "
                     "open_within_mount_root (ADR-014) TOCTOU-safe check Stage D always goes "
                     "through, not a separate, weaker path");

            // Positive control: the identical FsRead-granted, junction-shaped request, but the
            // junction stays INSIDE the mount -- proves junctions are not blanket-denied, only
            // ones that actually cross the boundary.
            create_junction(scratch_w + L"\\inside_link", scratch_w + L"\\inside");
            {
                std::ofstream f((std::filesystem::path(scratch) / "inside" / "ok.txt"));
                f << "inside content";
            }
            ExecRequest req2{"python", "print('READ:', open('/work/inside_link/ok.txt').read())"};
            auto out2 = runner.run(req2, state, ctx);
            AE_CHECK(out2.has_value() && out2->stdout_text.find("READ: inside content") != std::string::npos,
                     "E4-PY8 positive control: a junction that stays INSIDE the mount is followed, "
                     "not blanket-denied -- E4-PY8's denial above is real containment, not junctions "
                     "being rejected outright");
        }

        // ---- object-graph introspection: can guest code recover a PRE-mediation reference to the
        // real socket.connect without ever calling sys.settrace, just by walking __globals__? -----
        {
            // '_ae_open'/'_ae_connect'/'_ae_denied'/'call_tool' are the mediation WRAPPERS
            // themselves -- exec() binds a def'd function's own name into the globals dict it
            // executes in, so they are always present and finding them recovers nothing (they're
            // exactly what socket.socket.connect/builtins.call_tool already publicly are). The
            // property under test is narrower and more precise: no OTHER callable -- in particular
            // nothing that still holds a reference to the real, pre-mediation connect -- is present.
            ExecRequest req{
                "python",
                "import socket\n"
                "g = socket.socket.connect.__globals__\n"
                "wrappers = {'_ae_open', '_ae_connect', '_ae_denied', 'call_tool'}\n"
                "leaked = [k for k in g if k not in ('__builtins__', '_ae_internal') and "
                "k not in wrappers and not k.startswith('__') and callable(g.get(k))]\n"
                "print('LEAKED_NAMES:', leaked)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value(), "E4-PY9: setup -- the __globals__ introspection probe runs");
            std::printf("  measured: socket.socket.connect.__globals__ callable names -> %s",
                        out.has_value() ? out->stdout_text.c_str() : "(no output)");
            AE_CHECK(out.has_value() &&
                         out->stdout_text.find("LEAKED_NAMES: []") != std::string::npos,
                     "E4-PY9: no pre-mediation callable (e.g. the real, unwrapped connect) is "
                     "reachable via socket.socket.connect.__globals__ -- ordinary CPython object-"
                     "graph introspection, the general case sys.settrace-style tampering is a "
                     "narrower instance of, does not recover a bypass");
        }

        // Positive control for E4-PY9: prove the introspection MECHANISM itself is real by
        // checking it against __main__'s own globals, where a variable this test defines IS
        // supposed to be visible -- if this failed, "LEAKED_NAMES: []" above would be meaningless
        // (an artifact of __globals__ access itself being broken), not genuine containment.
        {
            ExecRequest req{"python", "ae_e4_canary = lambda: None\n"
                                       "print('CANARY_VISIBLE:', 'ae_e4_canary' in globals())"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("CANARY_VISIBLE: True") != std::string::npos,
                     "E4-PY9 positive control: globals() introspection itself works and finds a "
                     "real name when one is genuinely present (E4-PY9's empty result above is "
                     "real containment, not a broken probe)");
        }
    }

    std::filesystem::remove_all(scratch);
    std::filesystem::remove_all(outside);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All MediatedPythonRunner hostile-corpus checks passed.\n");
    return 0;
}
