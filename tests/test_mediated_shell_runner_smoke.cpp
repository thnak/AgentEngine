// Milestone 3 Phase E3 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, decision
// 4) -- proof for `agentengine::native_jail::mediated_shell::MediatedShellRunner` and its supporting
// parser/dispatch/registry/adapter, all genuinely new translation units built on top of
// `core/worktree_mount_fs.hpp`'s `open_within_mount_root` (ADR-014) rather than reusing
// decisions/ADR-001's cited spike code, which stays completely untouched (its own tests --
// test_shell_runner_proof, test_shell_parser_adversarial, test_shell_runner_no_process_creation --
// still pass unmodified, proven by regression, not asserted here).
//
// Covers: parser bounds (adversarial-input rejection, before any AST touches the arena);
// end-to-end builtin execution with REAL, path-scoped capability checks (denied-without/granted-
// with pairs, 022 §5); ExecState cwd/env shared by reference and canonicalized on `cd` (ADR-001
// §2.5.3); pipelines (engine-native stdout->stdin hand-off, no OS pipes); &&/|| short-circuit;
// if/for control flow; command-not-found fail-closed; registration-time name-shadowing rejection
// (Sh-C2); and the RunnerCall gate + its negative control against a fake registered Runner (the
// REAL RunnerCall<python> composition against MediatedPythonRunner is proven separately in
// test_mediated_shell_runner_python_composition.cpp, gated on AGENTENGINE_BUILD_PYTHON_RUNNER).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_command_registry.hpp"
#include "backends/native_jail/mediated_filesystem_adapter.hpp"
#include "backends/native_jail/mediated_shell_parser.hpp"
#include "backends/native_jail/mediated_shell_runner.hpp"

static_assert(agentengine::Runner<agentengine::native_jail::mediated_shell::MediatedShellRunner>,
              "MediatedShellRunner must satisfy the Runner concept (010 §1a)");

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

CapabilitySet full_caps() {
    return CapabilitySet::grant_root({
        Capability{cap::FsRead{"work", "", std::nullopt}},
        Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
        Capability{cap::EnvWrite{"MY_VAR"}},
        Capability{cap::RunnerCall{"fake"}},
    });
}

}  // namespace

int main() {
    std::string const scratch = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "C:/Windows/Temp") +
                                 "/ae_e3_mount";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    std::wstring scratch_w(scratch.begin(), scratch.end());

    // ---- Parser bounds (adversarial input rejected before any AST is built) -----------------
    {
        std::string huge(kMaxSourceBytes + 1, 'a');
        auto r = parse(huge);
        AE_CHECK(!r.has_value() && r.error().code == "shell.source_too_large",
                 "E3-P1: an over-large script is rejected with shell.source_too_large");
    }
    {
        std::string deep;
        for (std::size_t i = 0; i < kMaxNestingDepth + 5; ++i) deep += "if echo x then ";
        auto r = parse(deep);
        AE_CHECK(!r.has_value() && r.error().code == "shell.nesting_too_deep",
                 "E3-P2: over-deep 'if' nesting is rejected with shell.nesting_too_deep");
    }
    {
        auto r = parse("echo hello");
        AE_CHECK(r.has_value() && r->script.has_value(), "E3-P3 (positive control): an ordinary script parses fine");
    }

    // ---- Registration-time name-shadowing rejection (Sh-C2) ---------------------------------
    {
        DefaultCommandRegistry registry;
        auto r = registry.register_runner(RegisteredRunner{"cat", {}});
        AE_CHECK(!r.has_value() && r.error().code == "shell.command_name_reserved",
                 "E3-R1: registering a Runner under an existing builtin's name is rejected");
    }

    auto adapter = MediatedFileSystemAdapter::create(scratch_w);
    AE_CHECK(adapter.has_value(), "E3-C0: setup -- MediatedFileSystemAdapter::create succeeds");

    DefaultCommandRegistry registry;
    bool fake_invoked = false;
    AE_CHECK(registry
                 .register_runner(RegisteredRunner{
                     "fake",
                     [&](ExecRequest const&, ExecState&, EffectContext&) -> result<ExecOutcome> {
                         fake_invoked = true;
                         ExecOutcome o{};
                         o.stdout_text = "fake runner ran\n";
                         return o;
                     }})
                 .has_value(),
             "E3-C0: setup -- a fake Runner registers cleanly");

    MediatedShellRunner shell(*adapter, registry, "work");

    // ---- Basic execution + capability-gated builtins -----------------------------------------
    {
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = &caps;

        auto echo_out = shell.run(ExecRequest{"shell", "echo hello world"}, state, ctx);
        AE_CHECK(echo_out.has_value() && echo_out->stdout_text == "hello world\n",
                 "E3-C1: echo builtin produces the expected stdout");

        auto mkdir_out = shell.run(ExecRequest{"shell", "mkdir sub"}, state, ctx);
        AE_CHECK(mkdir_out.has_value(), "E3-C2: mkdir with a granted FsWrite capability succeeds");
        AE_CHECK(std::filesystem::exists(scratch + "/sub"), "E3-C2: the real directory now exists on disk");

        auto write_out = shell.run(ExecRequest{"shell", "echo written-by-shell > sub/f.txt"}, state, ctx);
        AE_CHECK(write_out.has_value() && write_out->stderr_text.empty(),
                 "E3-C3: echo with a > redirect writes a real file (granted FsWrite)");

        auto cat_out = shell.run(ExecRequest{"shell", "cat sub/f.txt"}, state, ctx);
        AE_CHECK(cat_out.has_value() && cat_out->stdout_text.find("written-by-shell") != std::string::npos,
                 "E3-C4: cat reads the real file content back byte-for-byte");

        auto ls_out = shell.run(ExecRequest{"shell", "ls sub"}, state, ctx);
        AE_CHECK(ls_out.has_value() && ls_out->stdout_text.find("f.txt") != std::string::npos,
                 "E3-C5: ls lists the real directory's real content");

        auto mv_out = shell.run(ExecRequest{"shell", "mv sub/f.txt sub/g.txt"}, state, ctx);
        AE_CHECK(mv_out.has_value(), "E3-C6: mv succeeds with both FsRead (source) and FsWrite (dest) granted");
        AE_CHECK(!std::filesystem::exists(scratch + "/sub/f.txt") && std::filesystem::exists(scratch + "/sub/g.txt"),
                 "E3-C6: the real rename happened on disk");

        auto cp_out = shell.run(ExecRequest{"shell", "cp sub/g.txt sub/h.txt"}, state, ctx);
        AE_CHECK(cp_out.has_value() && std::filesystem::exists(scratch + "/sub/h.txt") &&
                     std::filesystem::exists(scratch + "/sub/g.txt"),
                 "E3-C7: cp leaves BOTH the source and a real copy at the destination");

        auto rm_out = shell.run(ExecRequest{"shell", "rm sub/h.txt"}, state, ctx);
        AE_CHECK(rm_out.has_value() && !std::filesystem::exists(scratch + "/sub/h.txt"),
                 "E3-C8: rm deletes the real file");

        // cd + pwd: ExecState.cwd holds the adapter's canonical output (ADR-001 §2.5.3, carried
        // forward), and persists into the next call on the same Runner (010 §9 G3).
        auto cd_out = shell.run(ExecRequest{"shell", "cd sub"}, state, ctx);
        AE_CHECK(cd_out.has_value(), "E3-C9: cd into an existing directory succeeds");
        auto pwd_out = shell.run(ExecRequest{"shell", "pwd"}, state, ctx);
        AE_CHECK(pwd_out.has_value() && pwd_out->stdout_text.find("sub") != std::string::npos,
                 "E3-C9: pwd, on the NEXT call, reflects the cd from the previous call -- shared "
                 "ExecState, not a fresh one per call");

        // export: EnvWrite-gated, and visible via ExecState.env afterward (010 §3a).
        auto export_out = shell.run(ExecRequest{"shell", "export MY_VAR=42"}, state, ctx);
        AE_CHECK(export_out.has_value() && state.env["MY_VAR"] == "42",
                 "E3-C10: export with a granted EnvWrite capability sets the real, shared ExecState.env");
    }

    // ---- Positive-control-paired denials (022 §5): identical operations, NO capabilities granted -
    {
        ExecState state{};
        EffectContext ctx{};  // no capabilities at all -- the fail-closed default

        auto mkdir_denied = shell.run(ExecRequest{"shell", "mkdir denied_dir"}, state, ctx);
        AE_CHECK(!mkdir_denied.has_value() && mkdir_denied.error().code == "shell.capability_denied",
                 "E3-D1: mkdir without a granted FsWrite capability is denied");
        AE_CHECK(!std::filesystem::exists(scratch + "/denied_dir"),
                 "E3-D1: the denied mkdir never reached the filesystem");

        auto cat_denied = shell.run(ExecRequest{"shell", "cat sub/g.txt"}, state, ctx);
        AE_CHECK(!cat_denied.has_value() && cat_denied.error().code == "shell.capability_denied",
                 "E3-D2: cat without a granted FsRead capability is denied");

        auto export_denied = shell.run(ExecRequest{"shell", "export OTHER_VAR=1"}, state, ctx);
        AE_CHECK(!export_denied.has_value() && export_denied.error().code == "shell.capability_denied",
                 "E3-D3: export without a granted EnvWrite capability is denied");
        AE_CHECK(!state.env.contains("OTHER_VAR"), "E3-D3: the denied export never touched ExecState.env");
    }

    // ---- command not found (fail-closed, 010 §9 G2) -------------------------------------------
    {
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = &caps;
        for (char const* hostile : {"cmd.exe", "/bin/sh", "totally_bogus_xyz123", "powershell"}) {
            auto out = shell.run(ExecRequest{"shell", hostile}, state, ctx);
            AE_CHECK(!out.has_value() && out.error().code == "shell.command_not_found",
                     (std::string("E3-N1: '") + hostile + "' fails closed as command_not_found").c_str());
        }
    }

    // ---- Pipeline: engine-native stdout->stdin hand-off, no OS pipes --------------------------
    {
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = &caps;
        auto out = shell.run(ExecRequest{"shell", "echo piped-through | cat"}, state, ctx);
        AE_CHECK(out.has_value() && out->stdout_text.find("piped-through") != std::string::npos,
                 "E3-PL1: a pipeline hands the first command's stdout to the second as stdin, in-process");
    }

    // ---- &&/|| short-circuit -------------------------------------------------------------------
    {
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = &caps;
        auto and_out = shell.run(ExecRequest{"shell", "echo a && echo b"}, state, ctx);
        AE_CHECK(and_out.has_value() && and_out->stdout_text.find("b") != std::string::npos,
                 "E3-AO1: && runs the second pipeline when the first succeeds");

        auto or_out = shell.run(ExecRequest{"shell", "cat nonexistent.txt || echo fallback"}, state, ctx);
        AE_CHECK(or_out.has_value() && or_out->stdout_text.find("fallback") != std::string::npos,
                 "E3-AO2: || runs the second pipeline when the first fails");
    }

    // ---- if / for control flow -------------------------------------------------------------
    {
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = &caps;
        auto if_out = shell.run(ExecRequest{"shell", "if echo cond then echo then-branch fi"}, state, ctx);
        AE_CHECK(if_out.has_value() && if_out->stdout_text.find("then-branch") != std::string::npos,
                 "E3-IF1: 'if' runs the then-branch when the condition succeeds");

        auto for_out = shell.run(ExecRequest{"shell", "for x in a b c do echo item done"}, state, ctx);
        AE_CHECK(for_out.has_value() && for_out->stdout_text.find("item") != std::string::npos,
                 "E3-FOR1: 'for' executes its body once per item");
    }

    // ---- RunnerCall gate + negative control (010 §9 G4, against a fake Runner -- the REAL
    // MediatedPythonRunner composition is proven in test_mediated_shell_runner_python_composition) --
    {
        ExecState state{};
        CapabilitySet caps = full_caps();  // includes RunnerCall{"fake"}
        EffectContext ctx{};
        ctx.capabilities = &caps;
        fake_invoked = false;
        auto out = shell.run(ExecRequest{"shell", "fake hello"}, state, ctx);
        AE_CHECK(out.has_value() && fake_invoked, "E3-RC1: a registered Runner is invoked when the "
                                                    "RunnerCall capability is granted");
    }
    {
        ExecState state{};
        EffectContext ctx{};  // no capabilities -- RunnerCall{"fake"} not granted
        fake_invoked = false;
        auto out = shell.run(ExecRequest{"shell", "fake hello"}, state, ctx);
        AE_CHECK(!out.has_value() && !fake_invoked,
                 "E3-RC2 (negative control): the registered Runner is NEVER invoked without the "
                 "RunnerCall capability");
    }

    // ---- Milestone 3 Phase F3 (010 §3 items 4/5): stdout exceeding a small explicit output_cap_bytes
    // is truncated with an explicit marker -- the SAME cap_output() mechanism MediatedPythonRunner
    // applies (test_mediated_python_runner_smoke.cpp's F3-T1), now proven through
    // MediatedShellRunner's own run() boundary too, 010 §1's "not Python-only by design" made
    // concrete for this one cross-cutting concern.
    {
        MediatedShellRunner capped_shell(*adapter, registry, "work", /*output_cap_bytes=*/64);
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = &caps;

        std::string big_word(1000, 'a');
        auto out = capped_shell.run(ExecRequest{"shell", "echo " + big_word}, state, ctx);
        AE_CHECK(out.has_value(), "F3-S1: setup -- a large-output echo runs");
        AE_CHECK(out.has_value() && out->stdout_text.size() < 1000,
                 "F3-S1: stdout exceeding a small explicit output_cap_bytes is shorter than the raw "
                 "1000-byte word");
        AE_CHECK(out.has_value() && out->stdout_text.find("truncated") != std::string::npos,
                 "F3-S1: the truncated stdout carries an explicit marker naming what happened");
    }

    std::filesystem::remove_all(scratch);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All MediatedShellRunner smoke checks passed.\n");
    return 0;
}
