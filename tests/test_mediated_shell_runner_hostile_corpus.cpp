// Milestone 3 Phase E4 -- 010-Python-Code-Interpreter.md §9 G2's ShellRunner-specific classes
// against the REAL `MediatedShellRunner` (E3): "PATH hijacking, command substitution reaching an
// unregistered binary, function/alias shadowing of builtins -- must not merely be contained, must
// not exist as a reachable code path." The structural half (no process-creation symbol anywhere in
// the library's object code) is test_mediated_shell_runner_no_process_creation.cpp; this file is
// the BEHAVIORAL half: concrete hostile scripts, each with a positive control proving the probe
// itself is real, not vacuous (022 §5).
//
// Per-class scoping, stated rather than assumed:
//   - PATH hijacking: `resolve()` (mediated_command_registry.hpp) never reads ExecState.env at
//     all -- there is no PATH-consultation code path to hijack in the first place. Proven
//     behaviorally: a hostile PATH pointing at a directory containing a same-named decoy file has
//     zero effect on which implementation runs.
//   - command substitution `$(...)`: not in the grammar (mediated_shell_grammar.hpp's EBNF has no
//     production for it) -- `$(` lexes as ordinary literal text (the lexer's '$' handling only
//     recognizes `$NAME`/`${NAME}`), so `$(cat secret)` becomes literal argument text, never a
//     nested command dispatch.
//   - function/alias shadowing: neither `function` nor `alias` is grammar syntax -- a script using
//     either lexes as an ordinary simple_command whose NAME is the literal word "function"/"alias",
//     resolved through the exact same three-way CommandRegistry lookup as anything else (i.e.
//     command-not-found, since neither is a builtin or ever registered).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_command_registry.hpp"
#include "backends/native_jail/mediated_filesystem_adapter.hpp"
#include "backends/native_jail/mediated_shell_runner.hpp"

using namespace agentengine;
using namespace agentengine::native_jail::mediated_shell;

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

}  // namespace

int main() {
    std::string const scratch = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "C:/Windows/Temp") +
                                 "/ae_e4_shell_mount";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    std::wstring scratch_w(scratch.begin(), scratch.end());

    auto adapter = MediatedFileSystemAdapter::create(scratch_w);
    AE_CHECK(adapter.has_value(), "E4-SH setup: MediatedFileSystemAdapter::create succeeds");

    DefaultCommandRegistry registry;
    MediatedShellRunner shell(*adapter, registry, "work");

    CapabilitySet caps = CapabilitySet::grant_root({
        Capability{cap::FsRead{"work", "", std::nullopt}},
        Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
    });
    ExecState state{};
    EffectContext ctx{};
    ctx.capabilities = &caps;

    // Real content the mount's own `cat`/`ls` should see -- the target of every hijack/substitution
    // attempt below, so a successful attack would be trivially observable (the decoy or a
    // substituted value leaking into stdout) rather than needing to infer non-observation.
    {
        std::ofstream f((std::filesystem::path(scratch) / "real.txt"));
        f << "REAL_CONTENT_42";
    }

    // ---- PATH hijacking: resolve() never consults ExecState.env["PATH"] at all -------------------
    {
        // A decoy "cat" file sitting where a PATH-based lookup would find it, if one existed.
        std::filesystem::create_directories(scratch + "/hijack_dir");
        {
            std::ofstream f((std::filesystem::path(scratch) / "hijack_dir" / "cat"));
        }
        // Set directly on ExecState.env, bypassing 'export's own EnvWrite gate entirely -- this
        // probe is about whether command RESOLUTION ever consults PATH, not about export's own
        // capability check (already proven separately by E3-D3/E3-C10).
        state.env["PATH"] = "/work/hijack_dir";

        auto cat_out = shell.run(ExecRequest{"shell", "cat real.txt"}, state, ctx);
        AE_CHECK(cat_out.has_value() && cat_out->stdout_text == "REAL_CONTENT_42",
                 "E4-SH1: 'cat' still resolves to the real builtin (reading the real mounted file) "
                 "regardless of ExecState.env[\"PATH\"] -- no PATH-consultation code path exists "
                 "for command resolution to hijack");
    }

    // ---- command substitution $(...): not in the grammar, never dispatches a nested command ------
    {
        auto out = shell.run(ExecRequest{"shell", "echo $(cat real.txt)"}, state, ctx);
        AE_CHECK(out.has_value(), "E4-SH2: setup -- a script containing '$(...)' still parses (it's "
                                   "ordinary literal text to this grammar, not a syntax error)");
        AE_CHECK(out.has_value() && out->stdout_text == "$(cat real.txt)\n",
                 "E4-SH2: '$(cat real.txt)' is echoed back VERBATIM -- never substituted, never "
                 "dispatched as a nested 'cat' invocation reaching real.txt's content a second way");
    }

    // ---- function/alias shadowing: no such grammar production exists ------------------------------
    {
        auto out = shell.run(ExecRequest{"shell", "alias cat=evil"}, state, ctx);
        AE_CHECK(!out.has_value() && out.error().code == "shell.command_not_found",
                 "E4-SH3: 'alias cat=evil' is not a recognized construct -- 'alias' itself resolves "
                 "as an ordinary, unregistered command name (command-not-found), never as a "
                 "builtin-redefinition mechanism");

        auto out2 = shell.run(ExecRequest{"shell", "function cat() { echo evil; }"}, state, ctx);
        AE_CHECK(!out2.has_value(),
                 "E4-SH4: a 'function cat() {...}' definition attempt fails to execute as written "
                 "(no function-definition grammar exists to give it meaning)");

        // Positive control: after BOTH shadowing attempts, 'cat' still reads the real file --
        // proves the attempts genuinely had zero effect, not that this Runner happened to reject
        // the syntax while leaving some other state mutated.
        auto cat_out = shell.run(ExecRequest{"shell", "cat real.txt"}, state, ctx);
        AE_CHECK(cat_out.has_value() && cat_out->stdout_text == "REAL_CONTENT_42",
                 "E4-SH3/4 positive control: 'cat' still resolves to the real builtin and reads the "
                 "real file after both shadowing attempts -- neither left any trace");
    }

    // ---- registration-time reserved-name rejection: the host-side half of "shadowing cannot exist"
    {
        for (char const* reserved : {"cat", "ls", "rm", "cd", "export"}) {
            auto r = registry.register_tool(
                RegisteredTool{reserved, [](std::vector<std::string> const&, EffectContext&) -> result<ExecOutcome> {
                                    ExecOutcome o{};
                                    o.stdout_text = "SHADOWED\n";
                                    return o;
                                }});
            AE_CHECK(!r.has_value() && r.error().code == "shell.command_name_reserved",
                     ("E4-SH5: registering a Tool named '" + std::string(reserved) +
                      "' (an existing builtin) is rejected at registration time")
                         .c_str());
        }
        // Positive control: 'cat' is provably still the real builtin, never the rejected Tool.
        auto cat_out = shell.run(ExecRequest{"shell", "cat real.txt"}, state, ctx);
        AE_CHECK(cat_out.has_value() && cat_out->stdout_text == "REAL_CONTENT_42" &&
                     cat_out->stdout_text.find("SHADOWED") == std::string::npos,
                 "E4-SH5 positive control: 'cat' still runs the real builtin after every rejected "
                 "shadow-registration attempt");
    }

    // ---- command-not-found hostile-name corpus: absolute paths, traversal, well-known binaries ----
    {
        char const* hostile_names[] = {
            "C:\\Windows\\System32\\cmd.exe", "../../../../Windows/System32/cmd.exe",
            "\\\\?\\C:\\Windows\\System32\\cmd.exe", "powershell.exe", "wscript.exe", "curl.exe",
            "nc.exe", "bash",
        };
        for (char const* name : hostile_names) {
            auto out = shell.run(ExecRequest{"shell", std::string(name)}, state, ctx);
            AE_CHECK(!out.has_value() && out.error().code == "shell.command_not_found",
                     (std::string("E4-SH6: '") + name + "' is command-not-found, never dispatched")
                         .c_str());
        }
    }

    std::filesystem::remove_all(scratch);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All MediatedShellRunner hostile-corpus checks passed.\n");
    return 0;
}
