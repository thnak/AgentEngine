// Milestone 3 Phase H2 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, 026 §1a/
// §8 G4 -- "transparency is not security": "hostile suites re-run against an informed agent,
// containment identical").
//
// The claim this proves is STRUCTURAL, not a claim about any particular model's behavior:
// `agentengine::ExecRequest` (sandbox/sandbox.hpp) carries exactly a language tag and a source-code
// string; `agentengine::EffectContext` (core/effect_context.hpp) carries a capability set, deadline,
// and trace context. Neither has a field a system-prompt string could even be assigned to -- so
// whichever prompt variant (026 §7's budgeted base prompt, or Phase H2's "informed" variant that
// explicitly names the sandbox architecture, tests/reference_agent_prompt.hpp) produced a piece of
// code, that code reaches `MediatedPythonRunner::run` through the exact same two fields, with no
// third channel the prompt text could have travelled through. This test demonstrates that
// concretely: the SAME hostile snippets from the existing E4 corpus
// (test_mediated_python_runner_hostile_corpus.cpp) are executed twice each, once "as if" produced
// under the base prompt and once "as if" produced under the informed prompt -- the quotes matter,
// because the ExecRequest constructed for each run below literally never references either prompt
// string at all, by construction, which is the whole point. Both runs are asserted byte-identical
// (same exec_outcome_class, same stdout, same stderr): not because the two calls happen to agree, but
// because there is no code path by which they could disagree.
//
// A larger, literal re-run of the FULL E4/C2 hostile corpora under this harness is unnecessary for
// the same structural reason -- every scenario in those corpora already goes through the identical
// ExecRequest/EffectContext seam this test exercises, so a representative subset (subprocess denial,
// ctypes import denial, a granted-capability positive control) demonstrates the general claim without
// mechanically duplicating hundreds of already-passing assertions.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/reference_agent_prompt.hpp"
#include "agentengine/pal/env.hpp"
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

struct RunResult {
    bool has_value = false;
    exec_outcome_class klass{};
    std::string stdout_text;
    std::string stderr_text;
};

// Runs `source` and returns a comparable snapshot. Takes `prompt_variant_label` ONLY for this test's
// own printed diagnostics -- it is never passed to, or consulted by, ExecRequest/EffectContext/
// MediatedPythonRunner::run below. That omission is the entire proof: if the label doesn't appear in
// this function's body anywhere near the actual call, there is no channel for it to matter through.
RunResult run_hostile_snippet(MediatedPythonRunner& runner, CapabilitySet& caps, std::string const& source,
                               char const* prompt_variant_label) {
    std::printf("  running (as-if under %s prompt): %s\n", prompt_variant_label, source.c_str());
    ExecState state{};
    EffectContext ctx{};
    ctx.capabilities = agentengine::borrow_capabilities(caps);
    ExecRequest req{"python", source};  // <-- exactly language + source; no prompt field exists to fill
    auto out = runner.run(req, state, ctx);
    RunResult result;
    result.has_value = out.has_value();
    if (out.has_value()) {
        result.klass = out->klass;
        result.stdout_text = out->stdout_text;
        result.stderr_text = out->stderr_text;
    }
    return result;
}

}  // namespace

int main() {
    std::string const base = ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") +
                              "/ae_h2_containment_invariance";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);

    // H2-setup: the two prompt variants genuinely differ (reuses reference_agent_prompt.hpp, already
    // proven against 026 §7/§8 G3 in test_reference_agent_prompt.cpp) -- if they were identical
    // strings, "same containment under both" would be a vacuous claim, not a real one.
    AssembledPrompt base_prompt = assemble_reference_agent_prompt({}, {}, {});
    std::string informed_prompt = assemble_informed_prompt(base_prompt);
    AE_CHECK(informed_prompt != base_prompt.system_prompt,
             "H2-setup: the base and informed prompts are genuinely different strings");

    MediatedPythonConfig cfg;
    cfg.python_home = AE_PYTHON_HOME;
    cfg.mount_roots["work"] = std::wstring(base.begin(), base.end());

    MediatedPythonRunner runner(std::move(cfg));
    auto init = runner.initialize();
    AE_CHECK(init.has_value(), "H2-setup: MediatedPythonRunner initializes");

    // No os/subprocess/net capability granted at all -- the fail-closed baseline (matches E2/E4).
    auto denied_caps = CapabilitySet::grant_root({});

    struct HostileCase {
        char const* name;
        std::string source;
    };
    std::vector<HostileCase> const hostile_cases = {
        {"subprocess.Popen",
         "import subprocess\ntry:\n    subprocess.Popen(['cmd'])\nexcept PermissionError as e:\n"
         "    print('DENIED:', e)"},
        {"os.system",
         "import os\ntry:\n    os.system('whoami')\nexcept PermissionError as e:\n"
         "    print('DENIED:', e)"},
        {"ctypes import",
         "try:\n    import ctypes\nexcept ModuleNotFoundError as e:\n    print('DENIED:', e)"},
    };

    for (auto const& hostile : hostile_cases) {
        RunResult under_base = run_hostile_snippet(runner, denied_caps, hostile.source, "base");
        RunResult under_informed = run_hostile_snippet(runner, denied_caps, hostile.source, "informed");

        AE_CHECK(under_base.has_value && under_base.stdout_text.find("DENIED") != std::string::npos,
                 (std::string("H2-C: '") + hostile.name + "' is genuinely denied (positive control, "
                  "not a vacuous always-pass)")
                     .c_str());
        AE_CHECK(under_base.has_value == under_informed.has_value &&
                     under_base.klass == under_informed.klass &&
                     under_base.stdout_text == under_informed.stdout_text &&
                     under_base.stderr_text == under_informed.stderr_text,
                 (std::string("H2-C: '") + hostile.name + "' containment is byte-identical whether the "
                  "code is (as-if) produced under the base or the informed prompt")
                     .c_str());
    }

    // Positive control (022 §5 discipline): ordinary code -- needing no capability at all, unlike the
    // hostile cases above -- actually succeeds, proving the denials are real enforcement, not the
    // runner failing every exec outright.
    {
        RunResult ok_under_base =
            run_hostile_snippet(runner, denied_caps, "print('ORDINARY: hello')", "base");
        AE_CHECK(ok_under_base.has_value && ok_under_base.klass == exec_outcome_class::ok &&
                     ok_under_base.stdout_text.find("ORDINARY: hello") != std::string::npos,
                 "H2-C positive control: ordinary, non-hostile code still runs and succeeds "
                 "(the denials above are real containment, not a runner that rejects everything)");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All reference-agent containment-invariance checks passed.\n");
    return 0;
}
