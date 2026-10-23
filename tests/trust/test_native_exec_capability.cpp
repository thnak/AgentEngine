// Design -> red-team -> prove -> judge for decisions/archive/ADR-071-native-unsandboxed-process-execution-
// providers.md's `cap::NativeExec` capability kind (trust/capability.hpp): the host-granted
// allowlist entry that is the ONLY source of authority for NativeShellProvider/NativeBashProvider/
// NativePythonProvider/NativeNodeProvider to invoke a real, unsandboxed host executable. ADR-070
// property 3 ("narrows or decides among already-possessed authority only") is the load-bearing
// claim this file proves: a provider's PATH-scan discovery must never itself be able to grant
// reach to a program the host didn't already name in a grant -- every negative-result claim below
// is paired with a positive control (022 §5: "a test that cannot fail proves nothing").

#include <iostream>
#include <string>

#include "agentengine/core/json_value.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/native_exec_grant.hpp"

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
            Capability{cap::NativeExec{"node", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}},
        });
        AE_CHECK(root.contains(cap::NativeExec{"node", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
                  "N1 (positive control): an exact-name request against an exact-name grant is allowed");
        AE_CHECK(!root.contains(cap::NativeExec{"python3", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
                  "N1: a request for a DIFFERENT, ungranted program name is denied");
    }

    // ---- N2: a prefix grant ("python*") covers matching concrete names, nothing else -----------
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::NativeExec{"python*", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}},
        });
        AE_CHECK(root.contains(cap::NativeExec{"python3.11", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
                  "N2 (positive control): a concrete name matching the granted prefix is allowed");
        AE_CHECK(root.contains(cap::NativeExec{"python", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
                  "N2: the bare prefix itself (no suffix) also matches");
        AE_CHECK(!root.contains(cap::NativeExec{"node", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
                  "N2: a name NOT matching the granted prefix is denied");
        AE_CHECK(!root.contains(cap::NativeExec{"pythonic_evil", "other_mount", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
                  "N2: matching the name prefix but requesting a DIFFERENT worktree mount is denied");
    }

    // ---- R-N3: PATH-scan discovery output can never itself grant reach (ADR-070 property 3) ----
    // Simulates the exact failure mode the design must rule out: a provider's scan() surfaces every
    // executable on PATH, but only entries covered by an ALREADY-HELD grant may become invocable.
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::NativeExec{"node", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}},
        });
        std::vector<std::string> const path_scan_results = {"node", "python3", "bash", "curl", "rm"};
        std::vector<std::string> invocable;
        for (auto const& name : path_scan_results) {
            if (root.contains(cap::NativeExec{name, "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()})) {
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
            Capability{cap::NativeExec{"python*", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}},
        });
        AE_CHECK(!root.contains(cap::NativeExec{"*", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
                  "R-N4: requesting a bare '*' against a prefix grant is denied, not treated as "
                  "'covered by the prefix'");
        AE_CHECK(!root.contains(cap::NativeExec{"py*", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
                  "R-N4: requesting a DIFFERENT, broader wildcard is denied");
        // Positive control: attenuate() to the identical grant (re-request of the same pattern,
        // the shape every other exact-string kind in this file already permits) still succeeds.
        auto identical = root.attenuate(
            {Capability{cap::NativeExec{"python*", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}});
        AE_CHECK(identical.has_value(),
                  "R-N4 (positive control): re-attenuating to the SAME pattern still succeeds");
    }

    // ---- N5: attenuate() narrows a wildcard grant down to one concrete invocation --------------
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::NativeExec{"python*", "workdir", cap::Limit::at_most(30000), cap::Limit::at_most(30000), cap::Limit::at_most(512ull * 1024 * 1024), false, cap::Limit::unbounded(), cap::Limit::unbounded()}},
        });
        auto narrowed = root.attenuate(
            {Capability{cap::NativeExec{"python3.11", "workdir", cap::Limit::at_most(5000), cap::Limit::at_most(5000), cap::Limit::at_most(64ull * 1024 * 1024), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}});
        AE_CHECK(narrowed.has_value(), "N5: narrowing a prefix grant to a concrete invocation succeeds");
        if (narrowed.has_value()) {
            AE_CHECK(!narrowed->contains(cap::NativeExec{"node", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
                      "N5: the narrowed set does not carry the parent's wider (unrelated-name) reach");
            auto over_budget = narrowed->attenuate(
                {Capability{cap::NativeExec{"python3.11", "workdir", cap::Limit::at_most(999999), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}});
            AE_CHECK(!over_budget.has_value(),
                      "N5: a further attenuation exceeding the already-narrowed cpu_ms_cap is rejected");
        }
    }

    // ---- R-N6: mismatched worktree_mount_id is rejected even for a matching program name -------
    {
        auto root = CapabilitySet::grant_root({
            Capability{cap::NativeExec{"node", "trusted_workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}},
        });
        AE_CHECK(!root.contains(cap::NativeExec{"node", "different_mount", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
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
        Capability const c = cap::NativeExec{"bash", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()};
        AE_CHECK(capability_kind_of(c) == capability_kind::native_exec,
                  "N8: capability_kind_of() correctly tags a NativeExec instance");
        auto root = CapabilitySet::grant_root({c});
        AE_CHECK(root.contains_kind(capability_kind::native_exec),
                  "N8: kind-only lookup finds the granted native_exec capability");
    }

    // ---- N9: compile-time declaration tag round-trips to the matching runtime grant -------------
    {
        cap::decl::NativeExec<"node", "workdir", agentengine::unbounded(), agentengine::unbounded(), agentengine::unbounded()> const tag{};
        Capability const converted = to_capability(tag);
        auto root = CapabilitySet::grant_root({converted});
        AE_CHECK(root.contains(cap::NativeExec{"node", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
                  "N9: cap::decl::NativeExec<\"node\", \"workdir\"> converts to a covering runtime grant");
        AE_CHECK(!root.contains(cap::NativeExec{"python3", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}),
                  "N9: the converted grant does not cover an undeclared program name (I2 -- no "
                  "ambient native-exec access)");
    }

    // ---- ADR-209 §9 / C13: the held-native-shell opt-in -----------------------------------------------
    auto grant = [](bool live, std::optional<std::uint64_t> wall, std::optional<std::uint32_t> procs) {
        cap::NativeExec g{"pwsh", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()};
        g.live_session = live;
        if (wall) g.session_wall_ms_cap = cap::Limit::at_most(*wall);
        if (procs) g.max_processes = cap::Limit::at_most(*procs);
        return g;
    };
    std::uint64_t const kMax64 = static_cast<std::uint64_t>(-1);
    std::uint32_t const kMax32 = static_cast<std::uint32_t>(-1);

    // N10: monotone. A parent WITHOUT live_session never yields a child with it -- not even one setting both
    // caps to their maximum, which is the attenuated grant "caps set as the opt-in" would have let through.
    {
        auto root = CapabilitySet::grant_root({Capability{grant(false, std::nullopt, std::nullopt)}});
        AE_CHECK(!root.attenuate({Capability{grant(true, kMax64, kMax32)}}).has_value(),
                 "N10: a one-shot grant cannot be attenuated into a live one, even with UINT64_MAX caps");
        AE_CHECK(!root.contains(grant(true, 1000, 1)), "N10: a one-shot grant does not contain a live request");
        AE_CHECK(root.contains(grant(false, std::nullopt, std::nullopt)),
                 "N10 (positive control): the same grant still covers the one-shot request");
    }
    // N11: a live parent: narrower caps OK, wider caps refused, dropping live OK.
    {
        auto root = CapabilitySet::grant_root({Capability{grant(true, 3600000, 64)}});
        AE_CHECK(root.attenuate({Capability{grant(true, 60000, 8)}}).has_value(), "N11: narrower live caps attenuate");
        AE_CHECK(!root.attenuate({Capability{grant(true, 7200000, 8)}}).has_value(),
                 "N11: a longer session_wall_ms_cap is refused");
        AE_CHECK(!root.attenuate({Capability{grant(true, 60000, 128)}}).has_value(),
                 "N11: a larger max_processes is refused");
        AE_CHECK(!root.attenuate({Capability{grant(true, std::nullopt, 8)}}).has_value(),
                 "N11: an uncapped session_wall_ms_cap under a capped parent is refused");
        AE_CHECK(root.contains(grant(false, 1000, 1)), "N11: a live grant also covers a one-shot request");
    }
    // N12: use-site validation -- a live grant missing either cap is unusable.
    {
        AE_CHECK(!trust::live_session_grant_usable(grant(false, 1000, 1)), "N12: no opt-in -> unusable");
        AE_CHECK(!trust::live_session_grant_usable(grant(true, std::nullopt, std::nullopt)),
                 "N12: opt-in with no caps -> unusable");
        AE_CHECK(!trust::live_session_grant_usable(grant(true, 1000, std::nullopt)), "N12: no max_processes -> unusable");
        AE_CHECK(!trust::live_session_grant_usable(grant(true, std::nullopt, 4)), "N12: no session cap -> unusable");
        AE_CHECK(!trust::live_session_grant_usable(grant(true, 0, 4)), "N12: a zero session cap -> unusable");
        AE_CHECK(trust::live_session_grant_usable(grant(true, 1000, 4)), "N12 (positive control): opt-in + both caps");
        // ADR-209 §15.5: a session cap that would wrap negative as a signed duration bounds nothing.
        AE_CHECK(!trust::live_session_grant_usable(grant(true, UINT64_MAX, 4)), "N12: a session cap above 2^62 ms -> unusable");
        AE_CHECK(trust::live_session_grant_usable(grant(true, trust::kMaxLiveSessionWallMs, 4)),
                 "N12 (positive control): a session cap of exactly 2^62 ms is usable");
    }
    // N13: I6 -- the declarative grant parses to exactly what the CRTP declaration converts to.
    {
        auto doc = json::parse(R"({"program":"pwsh","worktree_mount":"workdir","live_session":true,)"
                               R"("cpu_ms_cap":"unbounded","wall_ms_cap":"unbounded","memory_bytes_cap":"unbounded",)"
                               R"("session_wall_ms_cap":3600000,"max_processes":64})");
        auto parsed = doc ? trust::parse_native_exec_grant(*doc) : result<cap::NativeExec>(std::unexpected(doc.error()));
        Capability const crtp = to_capability(cap::decl::NativeExec<"pwsh", "workdir", agentengine::unbounded(), agentengine::unbounded(), agentengine::unbounded(), true, agentengine::at_most(3600000), agentengine::at_most(64)>{});
        auto const* c = std::get_if<cap::NativeExec>(&crtp);
        bool const equal = parsed.has_value() && c != nullptr && parsed->program_pattern == c->program_pattern &&
                           parsed->worktree_mount_id == c->worktree_mount_id && parsed->live_session == c->live_session &&
                           parsed->session_wall_ms_cap.limit() == c->session_wall_ms_cap.limit() &&
                           parsed->max_processes.limit() == c->max_processes.limit() && parsed->cpu_ms_cap.limit() == c->cpu_ms_cap.limit() &&
                           parsed->wall_ms_cap.limit() == c->wall_ms_cap.limit() && parsed->memory_bytes_cap.limit() == c->memory_bytes_cap.limit();
        AE_CHECK(equal, "N13: a declarative live grant equals the CRTP cap::decl::NativeExec<..., true, ...> grant");
        Capability const plain = to_capability(cap::decl::NativeExec<"node", "workdir", agentengine::unbounded(), agentengine::unbounded(), agentengine::unbounded()>{});
        auto const* p = std::get_if<cap::NativeExec>(&plain);
        AE_CHECK(p != nullptr && !p->live_session && p->session_wall_ms_cap.is_unbounded() && p->max_processes.is_unbounded(),
                 "N13: a two-argument declaration is still a one-shot grant");
        auto refused = [](char const* text) {
            auto d = json::parse(text);
            return d.has_value() && !trust::parse_native_exec_grant(*d).has_value();
        };
        std::string const caps = R"("cpu_ms_cap":"unbounded","wall_ms_cap":"unbounded","memory_bytes_cap":"unbounded")";
        auto const with = [&](std::string extra) { return "{\"program\":\"pwsh\",\"worktree_mount\":\"w\"," + caps + extra + "}"; };
        AE_CHECK(refused(with(R"(,"live_sesion":true)").c_str()),
                 "N13: a misspelt key is refused, never ignored");
        AE_CHECK(refused(with(R"(,"live_session":"yes")").c_str()), "N13: a non-bool flag is refused");
        AE_CHECK(refused(with(R"(,"max_processes":-1)").c_str()), "N13: a negative cap is refused");
        AE_CHECK(refused(with(R"(,"max_processes":4294967296)").c_str()),
                 "N13: a max_processes past 2^32-1 is refused");
        AE_CHECK(refused(R"({"program":"pwsh"})"), "N13: a missing worktree_mount is refused");
        AE_CHECK(!refused(with("").c_str()), "N13 (positive control): a well-formed grant that states each cap parses");
        // ADR-244 slice 5 (G-C1/G-C2): an omitted cap is a missing CHOICE, never "uncapped"; zero and every other
        // spelling of "none" is refused; `unbounded` is the one way to say it.
        auto code_of = [](std::string const& text) {
            auto d = json::parse(text);
            auto g = d ? trust::parse_native_exec_grant(*d) : result<cap::NativeExec>(std::unexpected(d.error()));
            return g.has_value() ? std::string("ok") : g.error().code;
        };
        auto const only = [](std::string const& fields) { return "{\"program\":\"pwsh\",\"worktree_mount\":\"w\"," + fields + "}"; };
        AE_CHECK(code_of(only(R"("wall_ms_cap":1,"memory_bytes_cap":1)")) == "choice.missing.grant.cpu_ms_cap",
                 "N14: an absent cpu_ms_cap is choice.missing.grant.cpu_ms_cap, not an uncapped grant");
        AE_CHECK(code_of(only(R"("cpu_ms_cap":1,"wall_ms_cap":1)")) == "choice.missing.grant.memory_bytes_cap",
                 "N14: an absent memory_bytes_cap is choice.missing.grant.memory_bytes_cap");
        AE_CHECK(code_of(with(R"(,"live_session":true)")) == "choice.missing.grant.session_wall_ms_cap",
                 "N14: a live grant must state session_wall_ms_cap");
        AE_CHECK(code_of(with(R"(,"live_session":true,"session_wall_ms_cap":1000)")) == "choice.missing.grant.max_processes",
                 "N14: ... and max_processes");
        AE_CHECK(code_of(only(R"("cpu_ms_cap":0,"wall_ms_cap":1,"memory_bytes_cap":1)")) == "choice.zero_bound",
                 "N14: cpu_ms_cap: 0 is choice.zero_bound (0 is not 'no limit')");
        for (char const* bad : {R"("cpu_ms_cap":-1)", R"("cpu_ms_cap":null)", R"("cpu_ms_cap":"Unbounded")",
                                R"("cpu_ms_cap":"none")", R"("cpu_ms_cap":1.5)"}) {
            AE_CHECK(code_of(only(std::string(bad) + R"(,"wall_ms_cap":1,"memory_bytes_cap":1)")) == "grant_limit.invalid",
                     std::string("N14: refused: ") + bad);
        }
        AE_CHECK(code_of(only(R"("cpu_ms_cap":"unbounded","wall_ms_cap":7,"memory_bytes_cap":"unbounded")")) == "ok",
                 "N14 (positive control): `unbounded` and a positive number are the accepted spellings");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All cap::NativeExec (ADR-071) checks passed.\n";
    return 0;
}
