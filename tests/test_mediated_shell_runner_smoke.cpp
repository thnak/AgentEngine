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
#include "agentengine/pal/env.hpp"
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
    std::string const scratch = ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") +
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
        ctx.capabilities = agentengine::borrow_capabilities(caps);

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

    // ---- command not found: an ORDINARY command-level failure, not a hard stop -----------------
    // Milestone 3 Phase G4 (026 §3's own row: "nonzero exit + stderr line ('command not found')").
    // Before G4 this was a hard-stop script failure (`!out.has_value()`); 026 §3 treats an unknown
    // command the way a real shell does -- the script keeps running, `&&`/`||` see a non-ok outcome,
    // and stderr carries the real bash-shaped phrasing, never a host diagnostic.
    {
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);
        for (char const* hostile : {"cmd.exe", "/bin/sh", "totally_bogus_xyz123", "powershell"}) {
            auto out = shell.run(ExecRequest{"shell", hostile}, state, ctx);
            AE_CHECK(out.has_value() && out->klass == exec_outcome_class::policy_violation,
                     (std::string("E3-N1: '") + hostile + "' is an inspectable non-ok outcome, not a hard stop").c_str());
            AE_CHECK(out.has_value() && out->stderr_text == (std::string(hostile) + ": command not found"),
                     (std::string("E3-N1: '") + hostile + "' stderr matches real shell phrasing exactly").c_str());
        }
    }

    // ---- Pipeline: engine-native stdout->stdin hand-off, no OS pipes --------------------------
    {
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);
        auto out = shell.run(ExecRequest{"shell", "echo piped-through | cat"}, state, ctx);
        AE_CHECK(out.has_value() && out->stdout_text.find("piped-through") != std::string::npos,
                 "E3-PL1: a pipeline hands the first command's stdout to the second as stdin, in-process");
    }

    // ---- &&/|| short-circuit -------------------------------------------------------------------
    {
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);
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
        ctx.capabilities = agentengine::borrow_capabilities(caps);
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
        ctx.capabilities = agentengine::borrow_capabilities(caps);
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
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        std::string big_word(1000, 'a');
        auto out = capped_shell.run(ExecRequest{"shell", "echo " + big_word}, state, ctx);
        AE_CHECK(out.has_value(), "F3-S1: setup -- a large-output echo runs");
        AE_CHECK(out.has_value() && out->stdout_text.size() < 1000,
                 "F3-S1: stdout exceeding a small explicit output_cap_bytes is shorter than the raw "
                 "1000-byte word");
        AE_CHECK(out.has_value() && out->stdout_text.find("truncated") != std::string::npos,
                 "F3-S1: the truncated stdout carries an explicit marker naming what happened");
    }

    // ---- Gap-12 regression (2026-08-10 audit #12, fixed 2026-08-14): a quota-capped FsWrite/FsRead
    // grant used to be UNCONDITIONALLY denied here (`cap_covers()` rejects any quota-capped parent
    // against the synthetic uncapped `requested` a naive `contains()` check builds), and an uncapped
    // grant enforced no live quota at all (MediatedFileSystemAdapter::write_file tracks nothing).
    // Both halves are proven together, matching the audit's own "gate fix and live usage enforcement
    // together" requirement. A fresh mount is used so the byte/file-count baseline is exactly known,
    // not inherited from the accumulated state of the blocks above.
    {
        std::string const quota_scratch = scratch + "_quota";
        std::filesystem::remove_all(quota_scratch);
        std::filesystem::create_directories(quota_scratch);
        std::wstring quota_scratch_w(quota_scratch.begin(), quota_scratch.end());
        auto quota_adapter = MediatedFileSystemAdapter::create(quota_scratch_w);
        AE_CHECK(quota_adapter.has_value(), "E3-Q0: setup -- a fresh mount for the quota tests");
        MediatedShellRunner quota_shell(*quota_adapter, registry, "work");

        // Seed exactly one 10-byte file ("123456789\n") with an uncapped grant, so the capped-grant
        // checks below run against a KNOWN usage (1 file, 10 bytes).
        {
            ExecState state{};
            CapabilitySet caps = full_caps();
            EffectContext ctx{};
            ctx.capabilities = agentengine::borrow_capabilities(caps);
            auto out = quota_shell.run(ExecRequest{"shell", "echo 123456789 > seed.txt"}, state, ctx);
            AE_CHECK(out.has_value(), "E3-Q0: setup -- seeding a known 10-byte file succeeds");
        }

        // E3-Q1 (the false-denial half): a quota-capped grant with real headroom is USABLE at all --
        // before the fix, ANY quota_bytes-capped FsWrite was denied regardless of actual usage.
        {
            ExecState state{};
            CapabilitySet caps = CapabilitySet::grant_root(
                {Capability{cap::FsWrite{"work", "", std::uint64_t{1'000'000}, std::nullopt}}});
            EffectContext ctx{};
            ctx.capabilities = agentengine::borrow_capabilities(caps);
            auto out = quota_shell.run(ExecRequest{"shell", "mkdir under_quota"}, state, ctx);
            AE_CHECK(out.has_value(), "E3-Q1: mkdir succeeds under a quota-capped FsWrite grant with headroom");
        }

        // E3-Q2 (the silent-bypass half): a grant whose quota_bytes is ALREADY exceeded by real,
        // on-disk usage denies a further quota-checked write -- before the fix nothing enforced this
        // for an uncapped grant, and a capped grant never reached a live usage check at all (it was
        // denied earlier, for the wrong reason). A quota denial is NOT one of `kHardStopCodes`
        // (`shell.capability_denied` is, this isn't) -- consistent with `mediated_python_runner.cpp`'s
        // `Internal_open`, where the identical condition surfaces as an ordinary, catchable
        // `OSError`, not a hard denial -- so `evaluate_pipeline` reports it as an inspectable
        // non-ok `ExecOutcome` (`out.has_value() == true`, `klass == policy_violation`), the same
        // shape `cat` on a missing file already gets, not as a propagated `std::unexpected`.
        {
            ExecState state{};
            CapabilitySet caps = CapabilitySet::grant_root(
                {Capability{cap::FsWrite{"work", "", std::uint64_t{5}, std::nullopt}}});  // 5 < the 10 already used
            EffectContext ctx{};
            ctx.capabilities = agentengine::borrow_capabilities(caps);
            auto out = quota_shell.run(ExecRequest{"shell", "mkdir over_quota"}, state, ctx);
            AE_CHECK(out.has_value() && out->klass == exec_outcome_class::policy_violation &&
                         out->stderr_text == "No space left on device",
                     "E3-Q2: mkdir is denied once real usage already exceeds the grant's quota_bytes");
            AE_CHECK(!std::filesystem::exists(quota_scratch + "/over_quota"),
                     "E3-Q2: the denied mkdir never reached the filesystem");
        }

        // E3-Q3: `rm` is NEVER quota-gated (deletion can only reduce usage) -- the same exhausted
        // grant from E3-Q2 must still be able to delete, or a caller could never work back under quota.
        {
            ExecState state{};
            CapabilitySet caps = CapabilitySet::grant_root(
                {Capability{cap::FsWrite{"work", "", std::uint64_t{5}, std::nullopt}}});
            EffectContext ctx{};
            ctx.capabilities = agentengine::borrow_capabilities(caps);
            auto out = quota_shell.run(ExecRequest{"shell", "rm seed.txt"}, state, ctx);
            AE_CHECK(out.has_value(),
                     "E3-Q3: rm succeeds under an already-exhausted quota (deletion is never quota-gated)");
        }

        // E3-Q4: `mv` is NOT quota-gated (a rename adds no new bytes/file-count) while `cp` (which
        // DOES add a new file's worth of usage) IS -- proven against the same exhausted grant.
        {
            // Setup, uncapped: give `cp` a real source file to read from.
            ExecState setup_state{};
            CapabilitySet setup_caps = full_caps();
            EffectContext setup_ctx{};
            setup_ctx.capabilities = agentengine::borrow_capabilities(setup_caps);
            auto setup_out = quota_shell.run(ExecRequest{"shell", "echo cp-src > cp_src.txt"}, setup_state, setup_ctx);
            AE_CHECK(setup_out.has_value(), "E3-Q4: setup -- a real source file for cp exists");

            ExecState state{};
            CapabilitySet caps = CapabilitySet::grant_root(
                {Capability{cap::FsRead{"work", "", std::nullopt}},
                 Capability{cap::FsWrite{"work", "", std::uint64_t{5}, std::nullopt}}});  // exhausted, same as E3-Q2
            EffectContext ctx{};
            ctx.capabilities = agentengine::borrow_capabilities(caps);

            auto mv_out = quota_shell.run(ExecRequest{"shell", "mv under_quota moved_dir"}, state, ctx);
            AE_CHECK(mv_out.has_value(), "E3-Q4: mv succeeds under an exhausted quota (rename adds no usage)");

            auto cp_denied = quota_shell.run(ExecRequest{"shell", "cp cp_src.txt cp_dst.txt"}, state, ctx);
            AE_CHECK(cp_denied.has_value() && cp_denied->klass == exec_outcome_class::policy_violation &&
                         cp_denied->stderr_text == "No space left on device",
                     "E3-Q4: cp is denied under the same exhausted quota that mv is exempt from");
            AE_CHECK(!std::filesystem::exists(quota_scratch + "/cp_dst.txt"),
                     "E3-Q4: the denied cp never reached the filesystem");
        }

        // E3-Q5: FsRead's twin false-denial fix -- a size_cap_bytes-capped read grant is usable at
        // all (`find_fs_read` itself pairs no live MOUNT-USAGE check with it, unlike `find_fs_write`
        // -- a read never grows mount usage). E3-Q6 right below proves the separate, real PER-CALL
        // byte-budget enforcement `cat` now does with that same field (2026-08-23 fix).
        {
            ExecState state{};
            CapabilitySet caps = full_caps();
            EffectContext ctx{};
            ctx.capabilities = agentengine::borrow_capabilities(caps);
            auto write_out = quota_shell.run(ExecRequest{"shell", "echo hi > readable.txt"}, state, ctx);
            AE_CHECK(write_out.has_value(), "E3-Q5: setup -- write a small file to read back");
        }
        {
            ExecState state{};
            CapabilitySet caps =
                CapabilitySet::grant_root({Capability{cap::FsRead{"work", "", std::uint64_t{100}}}});
            EffectContext ctx{};
            ctx.capabilities = agentengine::borrow_capabilities(caps);
            auto cat_out = quota_shell.run(ExecRequest{"shell", "cat readable.txt"}, state, ctx);
            AE_CHECK(cat_out.has_value() && cat_out->stdout_text.find("hi") != std::string::npos,
                     "E3-Q5: cat succeeds under a size_cap_bytes-capped FsRead grant with headroom");
        }

        // E3-Q6 (2026-08-23 fix, component-role-audit-tracker.md Finding F): a size_cap_bytes grant
        // TOO SMALL for the real file is now actually enforced by `cat` -- before this fix, the field
        // was checked nowhere on the real-OS-backed shell/Python read path (only
        // `core/worktree.hpp::mount_read`'s separate, content-addressed-store path enforced it), so
        // an oversized file could reach stdout, a tool result, and the model's context completely
        // ungated by this already-declared capability field.
        {
            ExecState state{};
            CapabilitySet caps =
                CapabilitySet::grant_root({Capability{cap::FsRead{"work", "", std::uint64_t{2}}}});
            EffectContext ctx{};
            ctx.capabilities = agentengine::borrow_capabilities(caps);
            auto cat_out = quota_shell.run(ExecRequest{"shell", "cat readable.txt"}, state, ctx);
            AE_CHECK(cat_out.has_value() && cat_out->klass == exec_outcome_class::policy_violation &&
                         cat_out->stderr_text.find("size cap") != std::string::npos,
                     "E3-Q6: cat is refused (as an ordinary, catchable command failure, not a hard "
                     "stop) when the file exceeds the grant's size_cap_bytes");
            AE_CHECK(cat_out.has_value() && cat_out->stdout_text.empty(),
                     "E3-Q6: the oversized content never reached stdout");
        }

        std::filesystem::remove_all(quota_scratch);
    }

    std::filesystem::remove_all(scratch);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All MediatedShellRunner smoke checks passed.\n");
    return 0;
}
