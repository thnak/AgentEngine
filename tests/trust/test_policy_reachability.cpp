// Proves M2 Phase F task F2 (007 §9 gate G6): tools/policy_reachability.cpp's own enumerator
// (include/agentengine/trust/policy_reachability.hpp), called directly against
// policy_reachability_fixture.hpp's reference set, reaches the exact findings the fixture's own
// comments claim -- not merely "the CLI ran without crashing".
//
//   1. Clean pass: the CLI's own default reference set (build_reference_fixture()) reaches zero
//      findings against its hand-computed oracle -- no oracle_mismatch, no taint_variant, no
//      uncovered_by_oracle, no over_broad_grant. Individual GRANTED/DENIED decisions and
//      taint-invariance are spot-checked directly against the fixture's own per-agent claims.
//   2. The exit criterion's own demanded positive control: appending
//      add_over_broad_positive_control()'s agent to the clean set produces exactly one
//      over_broad_grant finding, naming the right agent and capability kind -- proving the detector
//      catches "a class of over-broad reachability a manual review would miss" (the exit criterion's
//      own wording), even though the CLI's own default run never exercises it (see that function's
//      comment for why).
//   3. Detector positive controls: a deliberately wrong oracle entry is caught as exactly one
//      oracle_mismatch; a deliberately removed oracle entry is caught as exactly one
//      uncovered_by_oracle. Proves the detectors themselves work, not just that today's fixture
//      happens to be clean.

#include <algorithm>
#include <cstdio>
#include <string>

#include "agentengine/trust/policy_reachability.hpp"
#include "policy_reachability_fixture.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

}  // namespace

int main() {
    using namespace agentengine;
    using namespace agentengine::trust;

    std::vector<ReachabilityAgent> agents;
    std::vector<ReachabilityOracleEntry> oracle;
    policy_reachability_fixture::build_reference_fixture(agents, oracle);
    check(oracle.size() == 7, "clean reference fixture declares exactly 7 {agent,tool,kind} oracle entries");

    auto find_cell = [](ReachabilityReport const& report, std::string_view agent_name, std::string_view tool_name,
                         capability_kind kind, bool tainted) -> ReachabilityCell const* {
        for (auto const& c : report.cells) {
            if (c.agent_name == agent_name && c.tool_name == tool_name && c.kind == kind && c.tainted == tainted) {
                return &c;
            }
        }
        return nullptr;
    };
    auto count_findings = [](ReachabilityReport const& report, finding_kind kind) -> std::size_t {
        std::size_t n = 0;
        for (auto const& f : report.findings) {
            if (f.kind == kind) ++n;
        }
        return n;
    };

    // -- 1. clean pass: the CLI's own default set (build_reference_fixture()) is fully clean --------
    {
        ReachabilityReport const report = enumerate_policy_reachability(agents, oracle);

        // 7 declared (agent,tool,kind) requirement pairs x 2 taint states.
        check(report.cells.size() == 14, "14 cells enumerated (7 declared requirements x 2 taint states)");
        check(count_findings(report, finding_kind::taint_variant) == 0,
              "no taint_variant findings -- admission is taint-invariant today");
        check(count_findings(report, finding_kind::oracle_mismatch) == 0,
              "no oracle_mismatch findings -- the hand-computed oracle is correct");
        check(count_findings(report, finding_kind::uncovered_by_oracle) == 0,
              "no uncovered_by_oracle findings -- the oracle covers every real cell");
        check(count_findings(report, finding_kind::over_broad_grant) == 0,
              "no over_broad_grant findings -- the CLI's default set is clean by design");

        if (auto const* c = find_cell(report, "reader-agent-covering", "read-data", capability_kind::fs_read, false)) {
            check(c->granted, "reader-agent-covering's ceiling grants read-data's fs_read requirement");
        } else {
            check(false, "reader-agent-covering/read-data/fs_read cell exists");
        }
        if (auto const* c =
                find_cell(report, "reader-agent-too-narrow", "read-data", capability_kind::fs_read, false)) {
            check(!c->granted, "reader-agent-too-narrow's empty ceiling denies read-data's fs_read requirement");
        } else {
            check(false, "reader-agent-too-narrow/read-data/fs_read cell exists");
        }

        // taint-invariance, spot-checked per-cell too (not just the aggregate count above).
        auto const* c_false = find_cell(report, "echo-agent-registered", "echo", capability_kind::entropy, false);
        auto const* c_true = find_cell(report, "echo-agent-registered", "echo", capability_kind::entropy, true);
        check(c_false != nullptr && c_true != nullptr && c_false->granted == c_true->granted,
              "the same {agent,tool,kind} cell reaches the same decision regardless of taint");
    }

    // -- 2. the exit criterion's own demanded positive control: over_broad_grant --------------------
    policy_reachability_fixture::add_over_broad_positive_control(agents, oracle);
    check(oracle.size() == 8, "adding the positive control brings the oracle to 8 entries");
    {
        ReachabilityReport const report = enumerate_policy_reachability(agents, oracle);
        check(report.cells.size() == 16, "16 cells enumerated (8 declared requirements x 2 taint states)");
        check(count_findings(report, finding_kind::oracle_mismatch) == 0, "still no oracle_mismatch findings");
        check(count_findings(report, finding_kind::taint_variant) == 0, "still no taint_variant findings");
        check(count_findings(report, finding_kind::uncovered_by_oracle) == 0, "still no uncovered_by_oracle findings");
        check(count_findings(report, finding_kind::over_broad_grant) == 1, "exactly one over_broad_grant finding");

        auto const it = std::find_if(report.findings.begin(), report.findings.end(),
                                      [](auto const& f) { return f.kind == finding_kind::over_broad_grant; });
        if (it != report.findings.end()) {
            check(it->agent_name == "over-broad-agent", "the over_broad_grant finding names the right agent");
            check(it->capability.has_value() && *it->capability == capability_kind::net_out,
                  "the over_broad_grant finding names the right capability kind (net_out)");
        } else {
            check(false, "an over_broad_grant finding exists to inspect");
        }
    }

    // -- 3. detector positive control: oracle_mismatch ----------------------------------------------
    {
        std::vector<ReachabilityOracleEntry> broken_oracle = oracle;
        bool flipped = false;
        for (auto& e : broken_oracle) {
            if (e.agent_name == "reader-agent-covering") {
                e.expected_granted = !e.expected_granted;
                flipped = true;
                break;
            }
        }
        check(flipped, "test setup: found the oracle entry to flip");
        ReachabilityReport const report = enumerate_policy_reachability(agents, broken_oracle);
        std::size_t mismatches = 0;
        for (auto const& f : report.findings) {
            if (f.kind == finding_kind::oracle_mismatch) ++mismatches;
        }
        check(mismatches == 1, "a deliberately wrong oracle entry is caught as exactly one oracle_mismatch");
    }

    // -- 4. detector positive control: uncovered_by_oracle -------------------------------------------
    {
        std::vector<ReachabilityOracleEntry> incomplete_oracle;
        for (auto const& e : oracle) {
            if (e.agent_name == "broad-agent" && e.tool_name == "now") continue;  // drop one entry
            incomplete_oracle.push_back(e);
        }
        check(incomplete_oracle.size() == oracle.size() - 1, "test setup: dropped exactly one oracle entry");
        ReachabilityReport const report = enumerate_policy_reachability(agents, incomplete_oracle);
        std::size_t uncovered = 0;
        for (auto const& f : report.findings) {
            if (f.kind == finding_kind::uncovered_by_oracle) ++uncovered;
        }
        check(uncovered == 1, "a missing oracle entry is caught as exactly one uncovered_by_oracle");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_policy_reachability: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_policy_reachability: %d FAILURE(S)\n", g_failures);
    return 1;
}
