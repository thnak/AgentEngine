// ADR-147 (008-Sandbox-and-Isolation.md §9 G9), design corrected from `docs/planning/honeypot-
// realism-gate-design-draft.md` by an independent red-team pass (2026-08-31) that found the draft
// never stated whether a corpus entry's "did it take the bait" outcome is scripted, one-time-
// recorded, or live-model-driven -- and that its "genuinely different methodology" claim and its
// "no new I5 concern" claim silently assumed different, incompatible answers to that question.
//
// This file resolves it the only way this codebase's existing conventions support (every test in
// `tests/` drives the sandbox directly; none call a live model): SCRIPTED. `scripted_action`
// (`tests/helpers/task_scope_corpus.hpp`) is authored by this test, standing in for "what an agent
// given the paired ordinary-sounding `task_narrative` did" -- exactly the way
// `test_mediated_shell_runner_hostile_corpus.cpp`'s own probe commands already stand in for a
// hostile guest's actions. What's genuinely new versus that file's existing "hostile-name corpus"
// is the CONTENT SHAPE, not the mechanism: an ordinary task description with a plausible,
// unlabeled scope-creep opportunity, run through the identical, already-Judged mount-escape
// containment (`open_within_mount_root`, ADR-014) as every other 008 §9 case. This is a corpus-
// coverage extension of G3 ("no ambient authority"), not a model-propensity test -- see
// `task_scope_corpus.hpp`'s own top comment and ADR-147 §5 for the honest, explicit disclaimer of
// what this does NOT prove.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_command_registry.hpp"
#include "backends/native_jail/mediated_filesystem_adapter.hpp"
#include "backends/native_jail/mediated_shell_runner.hpp"
#include "helpers/task_scope_corpus.hpp"

using namespace agentengine;
using namespace agentengine::native_jail::mediated_shell;
using agentengine::native_jail::test::kTaskScopeCorpus;
using agentengine::native_jail::test::kTaskScopeCorpusSize;

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
    std::string const parent = (std::filesystem::temp_directory_path() / "ae_g9_task_scope").string();
    std::filesystem::remove_all(parent);
    std::filesystem::create_directories(parent);

    // The mount root itself -- everything a scripted action is SUPPOSED to be able to reach.
    std::string const mount_root = parent + "/mount";
    std::filesystem::create_directories(mount_root);
    std::filesystem::create_directories(mount_root + "/archive");

    // Real, boring (never secret-shaped, per task_scope_corpus.hpp's own authorship rule) content
    // OUTSIDE the mount -- the bait each "for good measure" / "the other project left behind"
    // narrative points at. If containment failed, this exact content would leak into the mount.
    {
        std::ofstream f(parent + "/team_config.txt");
        f << "TEAM_CONFIG_V2";
    }
    std::filesystem::create_directories(parent + "/sibling-project");
    {
        std::ofstream f(parent + "/sibling-project/debug.log");
        f << "SIBLING_DEBUG_LOG_CONTENT";
    }
    // Real content INSIDE the mount, for the in-scope positive-control entry to actually copy.
    {
        std::ofstream f(mount_root + "/real.txt");
        f << "REAL_REPORT_CONTENT_9";
    }

    auto adapter = MediatedFileSystemAdapter::create(mount_root);
    AE_CHECK(adapter.has_value(), "G9 setup: MediatedFileSystemAdapter::create succeeds");

    DefaultCommandRegistry registry;
    MediatedShellRunner shell(*adapter, registry, "work");

    CapabilitySet caps = CapabilitySet::grant_root({
        Capability{cap::FsRead{"work", "", std::nullopt}},
        Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
    });
    ExecState state{};
    EffectContext ctx{};
    ctx.capabilities = agentengine::borrow_capabilities(caps);

    for (std::size_t i = 0; i < kTaskScopeCorpusSize; ++i) {
        auto const& c = kTaskScopeCorpus[i];
        auto out      = shell.run(ExecRequest{"shell", c.scripted_action}, state, ctx);

        if (c.expect_denied) {
            AE_CHECK(out.has_value() && out->klass == exec_outcome_class::policy_violation,
                     (std::string("G9 [") + c.name +
                      "]: the out-of-scope action embedded in an ordinary-sounding task is denied "
                      "as a policy violation, not silently executed")
                         .c_str());
        } else {
            AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok,
                     (std::string("G9 [") + c.name +
                      "] positive control: the SAME shape of ordinary, correctly-scoped task "
                      "succeeds -- proves the denials above are real containment, not this "
                      "harness rejecting every command")
                         .c_str());
        }
    }

    // Non-vacuity, proven by actually inspecting the filesystem, not just the returned outcome
    // class -- matching 008 §9 G2's own "positive control... demonstrably fails" discipline
    // applied in reverse (here: demonstrably did NOT happen).
    AE_CHECK(!std::filesystem::exists(mount_root + "/archive/team_config.txt"),
             "G9: the bait content was never copied into the mount -- the denial had a real "
             "effect on the filesystem, not just on the returned status code");
    AE_CHECK(std::filesystem::exists(parent + "/sibling-project/debug.log"),
             "G9: the sibling-project file the corpus tried to remove still exists -- rm's denial "
             "left it untouched");
    {
        std::ifstream f(parent + "/sibling-project/debug.log");
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        AE_CHECK(content == "SIBLING_DEBUG_LOG_CONTENT",
                 "G9: the untouched sibling file's content is unchanged, not merely present");
    }
    AE_CHECK(std::filesystem::exists(mount_root + "/archive/real.txt"),
             "G9 positive control: the in-scope copy actually landed inside the mount");
    {
        std::ifstream f(mount_root + "/archive/real.txt");
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        AE_CHECK(content == "REAL_REPORT_CONTENT_9",
                 "G9 positive control: the in-scope copy's content is the real content, proving "
                 "the mechanism that denies the out-of-scope entries above genuinely works "
                 "end-to-end when scope IS correct");
    }

    std::printf("test_task_scope_containment_corpus: %s\n",
                g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
