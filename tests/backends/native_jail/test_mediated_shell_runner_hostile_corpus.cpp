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
#include "agentengine/pal/env.hpp"
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
    // std::filesystem::temp_directory_path() (portable) rather than a hand-read TEMP env var with a
    // Windows-only fallback path (2026-08-28, ADR-103, the Linux-parity pass).
    std::string const scratch = (std::filesystem::temp_directory_path() / "ae_e4_shell_mount").string();
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);

    auto adapter = MediatedFileSystemAdapter::create(scratch);
    AE_CHECK(adapter.has_value(), "E4-SH setup: MediatedFileSystemAdapter::create succeeds");

    DefaultCommandRegistry registry;
    MediatedShellRunner shell(*adapter, registry, "work");

    CapabilitySet caps = CapabilitySet::grant_root({
        Capability{cap::FsRead{"work", "", std::nullopt}},
        Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
    });
    ExecState state{};
    EffectContext ctx{};
    ctx.capabilities = agentengine::borrow_capabilities(caps);

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
    // Milestone 3 Phase G4 (026 §3): command-not-found is an ORDINARY command-level failure, not a
    // hard stop, since that phase -- the security-relevant claim these checks protect ("never
    // dispatched") is unchanged, only the shape of "this didn't run" is (an inspectable non-ok
    // outcome instead of `!has_value()`).
    {
        auto out = shell.run(ExecRequest{"shell", "alias cat=evil"}, state, ctx);
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::policy_violation &&
                     out->stderr_text == "alias: command not found",
                 "E4-SH3: 'alias cat=evil' is not a recognized construct -- 'alias' itself resolves "
                 "as an ordinary, unregistered command name (command-not-found), never as a "
                 "builtin-redefinition mechanism");

        auto out2 = shell.run(ExecRequest{"shell", "function cat() { echo evil; }"}, state, ctx);
        AE_CHECK(out2.has_value() && out2->klass == exec_outcome_class::policy_violation,
                 "E4-SH4: a 'function cat() {...}' definition attempt does not execute as written "
                 "(no function-definition grammar exists to give it meaning -- 'function' itself "
                 "resolves as command-not-found)");

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
            AE_CHECK(out.has_value() && out->klass == exec_outcome_class::policy_violation &&
                         out->stderr_text == (std::string(name) + ": command not found"),
                     (std::string("E4-SH6: '") + name + "' is command-not-found, never dispatched")
                         .c_str());
        }
    }

    // ---- Code-review fix (2026-08-07), CRITICAL: mkdir mount escape via an unmediated leaf --------
    // `create_one_directory` (mediated_filesystem_adapter.cpp) used to split its input on the LAST
    // '/' only and hand the resulting leaf straight to CreateDirectoryW with no validation --
    // backslash has no meaning to this shell's grammar (confirmed above: no escape handling in the
    // lexer), so a payload like "..\\..\\evil" contains no '/' at all and sailed through as one
    // opaque, unchecked leaf segment. No test in this corpus exercised 'mkdir' before this fix.
    //
    // Windows/Linux divergence, named rather than silently ported or silently skipped (2026-08-28,
    // ADR-103, the Linux-parity pass; "isolation parity is a gate, not identical shape,"
    // CONVENTIONS.md): every payload in this block smuggles its `..` walk-up (or, for SH9, its
    // forbidden-character test) THROUGH a literal backslash -- Windows-specific BY CONSTRUCTION.
    // `\` is an ordinary, legal filename character on POSIX (no separator meaning at all, matching
    // worktree_mount_fs_posix.cpp's own `validate_fs_segment` reasoning), so every one of these
    // payloads can only ever create a real, harmless, oddly-NAMED directory INSIDE the mount root on
    // Linux -- there is no walk-up to smuggle and no forbidden character to reject. The two
    // platforms' EXPECTED outcomes are therefore genuinely different, not just differently-worded:
    // Windows must reject each as a policy violation; Linux must accept each as an ordinary (if
    // odd-looking) in-mount directory creation -- proven positively below, not merely "did not
    // escape," since a no-op mkdir would trivially also satisfy a purely negative check.
#ifdef _WIN32
    {
        std::string const temp_dir =
            ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp");
        std::string const sibling_marker = temp_dir + "/ae_e4_mkdir_escape_marker";
        std::filesystem::remove_all(sibling_marker);  // clean slate regardless of prior runs

        // Single '..' walks exactly one level up from the mount root (scratch) to its parent
        // (TEMP itself) -- if create_one_directory's leaf were still unvalidated, this would create
        // a real, empty directory OUTSIDE the mount at `<TEMP>/ae_e4_mkdir_escape_marker`.
        // `worktree.mount_path_forbidden_character` is not in dispatch.cpp's own kHardStopCodes set
        // (evaluate_pipeline), so -- like a failed `cat` on a missing file -- this surfaces as an
        // ORDINARY, inspectable non-ok ExecOutcome (has_value() == true, klass == policy_violation),
        // not a hard `!has_value()` result. Matches this corpus's own E3-N1/E4-SH3 precedent.
        auto escape_out = shell.run(ExecRequest{"shell", "mkdir ..\\ae_e4_mkdir_escape_marker"}, state, ctx);
        AE_CHECK(escape_out.has_value() && escape_out->klass == exec_outcome_class::policy_violation &&
                     escape_out->stderr_text.find("not permitted") != std::string::npos,
                 "E4-SH7: 'mkdir ..\\<name>' (backslash leaf) is rejected with the forbidden-"
                 "character diagnostic, not silently created outside the mount");
        AE_CHECK(!std::filesystem::exists(sibling_marker),
                 "E4-SH7: no directory was actually created outside the mount root");

        // Same escape, via the parents=true ('mkdir -p') code path -- separately vulnerable before
        // this fix, since the per-segment probe's own rejection was previously ignored and fallen
        // through to the same unvalidated create_one_directory call.
        auto escape_p_out =
            shell.run(ExecRequest{"shell", "mkdir -p ..\\ae_e4_mkdir_escape_marker"}, state, ctx);
        AE_CHECK(escape_p_out.has_value() && escape_p_out->klass == exec_outcome_class::policy_violation,
                 "E4-SH8: 'mkdir -p ..\\<name>' is ALSO rejected, not silently created via the "
                 "parents=true fall-through path");
        AE_CHECK(!std::filesystem::exists(sibling_marker),
                 "E4-SH8: no directory was actually created outside the mount root via -p either");

        // A backslash-laden leaf with no '..' at all is rejected purely on the forbidden character,
        // proving the fix isn't merely pattern-matching on "..".
        auto backslash_out = shell.run(ExecRequest{"shell", "mkdir sub\\evil_leaf"}, state, ctx);
        AE_CHECK(backslash_out.has_value() && backslash_out->klass == exec_outcome_class::policy_violation &&
                     backslash_out->stderr_text.find("not permitted") != std::string::npos,
                 "E4-SH9: a backslash inside an ordinary (non-'..') leaf is rejected too");

        // Positive control: an ordinary, well-formed mkdir still works after every rejection above --
        // proves the fix denies the hostile shape specifically, not mkdir as a whole.
        auto legit_out = shell.run(ExecRequest{"shell", "mkdir legit_after_escape_attempts"}, state, ctx);
        AE_CHECK(legit_out.has_value(), "E4-SH10 positive control: an ordinary mkdir still succeeds");
        AE_CHECK(std::filesystem::exists(scratch + "/legit_after_escape_attempts"),
                 "E4-SH10 positive control: the real directory was created, inside the mount");

        std::filesystem::remove_all(sibling_marker);  // best-effort cleanup, should be a no-op
    }
#else
    {
        // Linux counterpart to SH7-SH10: every backslash-laden payload above lands INSIDE the mount
        // as a literal, odd-looking directory name -- none of them has a walk-up or a forbidden
        // character to reject on this platform.
        auto in_mount_out = shell.run(ExecRequest{"shell", "mkdir ..\\ae_e4_mkdir_escape_marker"}, state, ctx);
        AE_CHECK(in_mount_out.has_value() && in_mount_out->klass == exec_outcome_class::ok,
                 "E4-SH7 (Linux): 'mkdir ..\\<name>' has no walk-up meaning on POSIX -- succeeds as an "
                 "ordinary in-mount directory creation, not a rejected escape attempt");
        AE_CHECK(std::filesystem::exists(scratch + "/..\\ae_e4_mkdir_escape_marker"),
                 "E4-SH7 (Linux): the literal, backslash-containing directory name was actually "
                 "created, INSIDE the mount root -- confirms this is a real create, not a silent no-op");

        auto in_mount_p_out =
            shell.run(ExecRequest{"shell", "mkdir -p ..\\ae_e4_mkdir_escape_marker2"}, state, ctx);
        AE_CHECK(in_mount_p_out.has_value() && in_mount_p_out->klass == exec_outcome_class::ok,
                 "E4-SH8 (Linux): 'mkdir -p ..\\<name>' (parents=true path) likewise has no walk-up "
                 "meaning on POSIX -- succeeds as an ordinary in-mount directory creation");
        AE_CHECK(std::filesystem::exists(scratch + "/..\\ae_e4_mkdir_escape_marker2"),
                 "E4-SH8 (Linux): the literal directory name was created, INSIDE the mount root");

        auto backslash_out = shell.run(ExecRequest{"shell", "mkdir sub\\evil_leaf"}, state, ctx);
        AE_CHECK(backslash_out.has_value() && backslash_out->klass == exec_outcome_class::ok,
                 "E4-SH9 (Linux): a backslash inside an ordinary (non-'..') leaf is an ordinary, legal "
                 "filename character on POSIX -- no forbidden-character rejection applies");
        AE_CHECK(std::filesystem::exists(scratch + "/sub\\evil_leaf"),
                 "E4-SH9 (Linux): the literal, backslash-containing directory name was created, "
                 "INSIDE the mount root");

        // Positive control: an ordinary, well-formed mkdir still works -- same shape and same
        // meaning on both platforms.
        auto legit_out = shell.run(ExecRequest{"shell", "mkdir legit_after_escape_attempts"}, state, ctx);
        AE_CHECK(legit_out.has_value(), "E4-SH10 positive control: an ordinary mkdir still succeeds");
        AE_CHECK(std::filesystem::exists(scratch + "/legit_after_escape_attempts"),
                 "E4-SH10 positive control: the real directory was created, inside the mount");
    }
#endif

    std::filesystem::remove_all(scratch);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All MediatedShellRunner hostile-corpus checks passed.\n");
    return 0;
}
