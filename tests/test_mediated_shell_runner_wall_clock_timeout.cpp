// ADR-100 §4 F3 fix, `docs/planning/adr-096-098-sandbox-layering-reconciliation-design-draft.md`:
// `mediated_shell_grammar.hpp` bounds source size/token count/nesting depth, but bounded SOURCE
// size never bounded EXECUTION TIME -- a `for` loop re-executes its already-parsed, fixed-cost
// body once per item, and NESTED loops multiply that: `kMaxNestingDepth` (32, shared across if/for)
// levels at N items per level is N^32 body executions from a script small enough to fit comfortably
// under `kMaxTokens`. A model-supplied `run_shell` script exploiting this ran synchronously with no
// kill mechanism at all before this fix -- a live, present-day DoS gap, unlike
// `NativeJailBackend::create_python_worker()`'s real watchdog for Python.
//
// Proves: (1) an ordinary short script still completes normally under the default budget (no false
// positive); (2) a script whose real execution time is many orders of magnitude larger than a
// short, explicit `wall_clock_budget` is nonetheless killed and returns a real, inspectable
// `ExecOutcome{klass: timeout}` within a small real bound on wall-clock time -- not "eventually,"
// not "if you wait long enough," but bounded, measured against `std::chrono::steady_clock` in this
// test process, the same standard `test_job_object_limits.cpp`'s own wall-clock-kill proofs use.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/pal/env.hpp"
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

CapabilitySet full_caps() {
    return CapabilitySet::grant_root({
        Capability{cap::FsRead{"work", "", std::nullopt}},
        Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
    });
}

// 20 levels of nested `for` loops, 3 items each -- 3^20 ~= 3.49 billion body executions -- from a
// script of well under 1,000 tokens (`kMaxTokens` is 50,000), well under `kMaxNestingDepth` (32,
// shared across if/for). No parser bound rejects this script; only the wall-clock fix stops it.
std::string exponential_blowup_script() {
    std::string open;
    std::string close;
    for (int level = 0; level < 20; ++level) {
        std::string var = "v" + std::to_string(level);
        open += "for " + var + " in a b c do ";
        close += "done ";
    }
    return open + "echo x " + close;
}

}  // namespace

int main() {
    std::string const scratch =
        ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") + "/ae_wall_clock_timeout";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    std::wstring scratch_w(scratch.begin(), scratch.end());

    auto adapter = MediatedFileSystemAdapter::create(scratch_w);
    AE_CHECK(adapter.has_value(), "WCT-0: setup -- MediatedFileSystemAdapter::create succeeds");
    DefaultCommandRegistry registry;

    // ---- Positive control: an ordinary short script still completes normally, well under the
    // default budget -- the fix must not make legitimate scripts spuriously time out. -------------
    {
        MediatedShellRunner shell(*adapter, registry, "work");
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        auto out = shell.run(ExecRequest{"shell", "for x in a b c do echo item done"}, state, ctx);
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok,
                 "WCT-1 (positive control): an ordinary bounded for-loop completes normally, not timed out");
        AE_CHECK(out.has_value() && out->stdout_text.find("item") != std::string::npos,
                 "WCT-1: the ordinary loop's real output is still produced");
    }

    // ---- The real fix: an exponential-blowup script (3^20 body executions) is killed within a
    // small, real wall-clock bound when given a short explicit budget -- never runs to "completion"
    // (which would not happen in this process's lifetime, let alone this test's). -------------------
    {
        MediatedShellRunner shell(*adapter, registry, "work", agentengine::native_jail::kDefaultOutputCapBytes,
                                   std::chrono::milliseconds{20});
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        auto const started = std::chrono::steady_clock::now();
        auto out = shell.run(ExecRequest{"shell", exponential_blowup_script()}, state, ctx);
        auto const elapsed = std::chrono::steady_clock::now() - started;

        AE_CHECK(out.has_value(),
                 "WCT-2: an exponential-blowup script returns a real, inspectable ExecOutcome, "
                 "not a propagated error");
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::timeout,
                 "WCT-2: the outcome is classified as a real timeout, matching "
                 "NativeJailBackend's own wall-clock-kill classification");
        AE_CHECK(out.has_value() && out->stderr_text.find("wall-clock") != std::string::npos,
                 "WCT-2: stderr names the real reason, not a generic failure");
        // A generous real bound (2s) against a 20ms budget -- proves this is a genuine kill, not a
        // coincidental fast completion, while tolerating slow CI/debug-build overhead between
        // per-statement deadline checks.
        AE_CHECK(elapsed < std::chrono::seconds{2},
                 "WCT-2: the real wall-clock time to return is small and bounded, not '3^20 "
                 "iterations' worth of runtime");
    }

    // ---- Negative-control granularity check: the SAME exponential-blowup script, given a
    // budget generous enough to let a FEW outer-loop iterations start, still gets killed -- proves
    // the check fires from inside nested loop bodies (evaluate_statement), not only once at the
    // top level before any loop body ever runs. ------------------------------------------------
    {
        MediatedShellRunner shell(*adapter, registry, "work", agentengine::native_jail::kDefaultOutputCapBytes,
                                   std::chrono::milliseconds{50});
        ExecState state{};
        CapabilitySet caps = full_caps();
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        auto const started = std::chrono::steady_clock::now();
        auto out = shell.run(ExecRequest{"shell", exponential_blowup_script()}, state, ctx);
        auto const elapsed = std::chrono::steady_clock::now() - started;

        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::timeout,
                 "WCT-3: a slightly larger (but still short) budget still results in a real timeout "
                 "for a script this explosive -- the check fires deep inside nested loop bodies");
        AE_CHECK(elapsed < std::chrono::seconds{2},
                 "WCT-3: still bounded, real wall-clock time");
    }

    std::filesystem::remove_all(scratch);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All wall-clock timeout checks passed.\n");
    return 0;
}
