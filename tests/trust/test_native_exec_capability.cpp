// Design -> red-team -> prove -> judge for decisions/ADR-071-native-unsandboxed-process-execution-
// providers.md's `cap::NativeExec` capability kind (trust/capability.hpp): the host-granted
// allowlist entry that is the ONLY source of authority for NativeShellProvider/NativeBashProvider/
// NativePythonProvider/NativeNodeProvider to invoke a real, unsandboxed host executable. ADR-070
// property 3 ("narrows or decides among already-possessed authority only") is the load-bearing
// claim this file proves: a provider's PATH-scan discovery must never itself be able to grant
// reach to a program the host didn't already name in a grant -- every negative-result claim below
// is paired with a positive control (022 §5: "a test that cannot fail proves nothing").

#include <iostream>
#include <string>

#include "agentengine/trust/capability.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    // ---- N1: exact-name grant covers only that exact request -----------------------------------
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::NativeExec{"node", "workdir", std::nullopt, std::nullopt, std::nullopt}},
        });
        AE_CHECK(root.contains(cap::NativeExec{"node", "workdir", std::nullopt, std::nullopt, std::nullopt}),
                  "N1 (positive control): an exact-name request against an exact-name grant is allowed");
        AE_CHECK(!root.contains(cap::NativeExec{"python3", "workdir", std::nullopt, std::nullopt, std::nullopt}),
                  "N1: a request for a DIFFERENT, ungranted program name is denied");
    }

    // ---- N2: a prefix grant ("python*") covers matching concrete names, nothing else -----------
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::NativeExec{"python*", "workdir", std::nullopt, std::nullopt, std::nullopt}},
        });
        AE_CHECK(root.contains(cap::NativeExec{"python3.11", "workdir", std::nullopt, std::nullopt, std::nullopt}),
                  "N2 (positive control): a concrete name matching the granted prefix is allowed");
        AE_CHECK(root.contains(cap::NativeExec{"python", "workdir", std::nullopt, std::nullopt, std::nullopt}),
                  "N2: the bare prefix itself (no suffix) also matches");
        AE_CHECK(!root.contains(cap::NativeExec{"node", "workdir", std::nullopt, std::nullopt, std::nullopt}),
                  "N2: a name NOT matching the granted prefix is denied");
        AE_CHECK(!root.contains(cap::NativeExec{"pythonic_evil", "other_mount", std::nullopt, std::nullopt, std::nullopt}),
                  "N2: matching the name prefix but requesting a DIFFERENT worktree mount is denied");
    }

    // ---- R-N3: PATH-scan discovery output can never itself grant reach (ADR-070 property 3) ----
    // Simulates the exact failure mode the design must rule out: a provider's scan() surfaces every
    // executable on PATH, but only entries covered by an ALREADY-HELD grant may become invocable.
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::NativeExec{"node", "workdir", std::nullopt, std::nullopt, std::nullopt}},
        });
        std::vector<std::string> const path_scan_results = {"node", "python3", "bash", "curl", "rm"};
        std::vector<std::string> invocable;
        for (auto const& name : path_scan_results) {
            if (root.contains(cap::NativeExec{name, "workdir", std::nullopt, std::nullopt, std::nullopt})) {
                invocable.push_back(name);
            }
        }
        AE_CHECK(invocable.size() == 1 && invocable[0] == "node",
                  "R-N3: of five PATH-scanned candidates, only the one already-granted name is "
                  "invocable -- scanning discovered four programs that remain uncallable");
    }

    // ---- R-N4: no re-widening -- a REQUESTED pattern must never itself carry a wildcard ---------
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::NativeExec{"python*", "workdir", std::nullopt, std::nullopt, std::nullopt}},
        });
        AE_CHECK(!root.contains(cap::NativeExec{"*", "workdir", std::nullopt, std::nullopt, std::nullopt}),
                  "R-N4: requesting a bare '*' against a prefix grant is denied, not treated as "
                  "'covered by the prefix'");
        AE_CHECK(!root.contains(cap::NativeExec{"py*", "workdir", std::nullopt, std::nullopt, std::nullopt}),
                  "R-N4: requesting a DIFFERENT, broader wildcard is denied");
        // Positive control: attenuate() to the identical grant (re-request of the same pattern,
        // the shape every other exact-string kind in this file already permits) still succeeds.
        auto identical = root.attenuate(
            {Capability{cap::NativeExec{"python*", "workdir", std::nullopt, std::nullopt, std::nullopt}}});
        AE_CHECK(identical.has_value(),
                  "R-N4 (positive control): re-attenuating to the SAME pattern still succeeds");
    }

    // ---- N5: attenuate() narrows a wildcard grant down to one concrete invocation --------------
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::NativeExec{"python*", "workdir", 30000, 30000, 512ull * 1024 * 1024}},
        });
        auto narrowed = root.attenuate(
            {Capability{cap::NativeExec{"python3.11", "workdir", 5000, 5000, 64ull * 1024 * 1024}}});
        AE_CHECK(narrowed.has_value(), "N5: narrowing a prefix grant to a concrete invocation succeeds");
        if (narrowed.has_value()) {
            AE_CHECK(!narrowed->contains(cap::NativeExec{"node", "workdir", std::nullopt, std::nullopt, std::nullopt}),
                      "N5: the narrowed set does not carry the parent's wider (unrelated-name) reach");
            auto over_budget = narrowed->attenuate(
                {Capability{cap::NativeExec{"python3.11", "workdir", 999999, std::nullopt, std::nullopt}}});
            AE_CHECK(!over_budget.has_value(),
                      "N5: a further attenuation exceeding the already-narrowed cpu_ms_cap is rejected");
        }
    }

    // ---- R-N6: mismatched worktree_mount_id is rejected even for a matching program name -------
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::NativeExec{"node", "trusted_workdir", std::nullopt, std::nullopt, std::nullopt}},
        });
        AE_CHECK(!root.contains(cap::NativeExec{"node", "different_mount", std::nullopt, std::nullopt, std::nullopt}),
                  "R-N6: a request naming the granted program but a DIFFERENT worktree mount is denied "
                  "-- worktree confinement cannot be bypassed by keeping the program name and "
                  "changing the mount");
    }

    // ---- R-N7: text_derived model output can never auto-declassify a native-exec call ----------
    // The load-bearing tripwire (ADR-023 §6 point 4 / 007 §4): is_inert_for_text_derived_
    // declassification MUST return false for native_exec, or a model-influenced tool call could
    // auto-approve invoking an unsandboxed host process with no human/policy decision at all.
    {
        AE_CHECK(!is_inert_for_text_derived_declassification(capability_kind::native_exec),
                  "R-N7: native_exec is classified NON-inert -- a text_derived call can never "
                  "auto-declassify into invoking an unsandboxed host executable");
        // Positive control: an actually-inert kind (fs_read) is still classified inert -- proves
        // the function still discriminates rather than having been made to fail closed for
        // everything.
        AE_CHECK(is_inert_for_text_derived_declassification(capability_kind::fs_read),
                  "R-N7 (positive control): fs_read is still classified inert -- the function "
                  "discriminates by kind, not a blanket false");
    }

    // ---- N8: kind derivation and kind-only lookup round-trip -------------------------------------
    {
        Capability const c = cap::NativeExec{"bash", "workdir", std::nullopt, std::nullopt, std::nullopt};
        AE_CHECK(capability_kind_of(c) == capability_kind::native_exec,
                  "N8: capability_kind_of() correctly tags a NativeExec instance");
        auto root = CapabilitySet::grant_root({c});
        AE_CHECK(root.contains_kind(capability_kind::native_exec),
                  "N8: kind-only lookup finds the granted native_exec capability");
    }

    // ---- N9: compile-time declaration tag round-trips to the matching runtime grant -------------
    {
        cap::decl::NativeExec<"node", "workdir"> const tag{};
        Capability const converted = to_capability(tag);
        auto root = CapabilitySet::grant_root({converted});
        AE_CHECK(root.contains(cap::NativeExec{"node", "workdir", std::nullopt, std::nullopt, std::nullopt}),
                  "N9: cap::decl::NativeExec<\"node\", \"workdir\"> converts to a covering runtime grant");
        AE_CHECK(!root.contains(cap::NativeExec{"python3", "workdir", std::nullopt, std::nullopt, std::nullopt}),
                  "N9: the converted grant does not cover an undeclared program name (I2 -- no "
                  "ambient native-exec access)");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All cap::NativeExec (ADR-071) checks passed.\n";
    return 0;
}
