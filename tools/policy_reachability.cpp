// 007 §9 gate G6 / M2 Phase F task F2: CI-runnable policy-reachability tool. Enumerates every
// declared {agent, tool, capability kind, taint} cell in policy_reachability_fixture.hpp's CLEAN
// reference set against the real CapabilitySet::contains() mechanism
// (include/agentengine/trust/capability.hpp, decisions/ADR-009-capability-set-enforcement-
// mechanism.md) via include/agentengine/trust/policy_reachability.hpp's enumerator, and reports every
// finding: oracle mismatches, taint-variance, and over-broad grants (an agent-declared capability
// kind no declared tool of theirs ever requires). Exit 0 if clean, 1 otherwise -- the same
// CI-consumed pass/fail convention tools/naming_lint.py already uses.
//
// This CLI intentionally runs against the clean set only, not the fixture's own
// add_over_broad_positive_control() addition -- a CI gate that is permanently red over a known,
// accepted fixture entry is not a useful gate (007 §10 Q3: findings are "for an operator to
// review"). The detector's ability to actually catch that positive control is proven separately, by
// tests/test_policy_reachability.cpp (ctest), not by this binary's own default run.

#include <cstdio>
#include <string>

#include "agentengine/trust/policy_reachability.hpp"
#include "policy_reachability_fixture.hpp"

int main() {
    using namespace agentengine;
    using namespace agentengine::trust;

    std::vector<ReachabilityAgent> agents;
    std::vector<ReachabilityOracleEntry> oracle;
    policy_reachability_fixture::build_reference_fixture(agents, oracle);

    ReachabilityReport const report = enumerate_policy_reachability(agents, oracle);

    std::fprintf(stderr, "policy-reachability: %zu cells enumerated across %zu agents\n", report.cells.size(),
                 agents.size());
    for (ReachabilityCell const& cell : report.cells) {
        std::fprintf(stderr, "  %-28s %-16s %-12s taint=%-5s -> %s\n", cell.agent_name.c_str(),
                     cell.tool_name.c_str(), std::string(capability_kind_name(cell.kind)).c_str(),
                     cell.tainted ? "true" : "false", cell.granted ? "GRANTED" : "DENIED");
    }

    if (report.findings.empty()) {
        std::fprintf(stderr, "policy-reachability: 0 findings, clean\n");
        return 0;
    }

    std::fprintf(stderr, "policy-reachability: %zu finding(s):\n", report.findings.size());
    for (ReachabilityFinding const& f : report.findings) {
        std::string const kind_str =
            f.capability.has_value() ? std::string(capability_kind_name(*f.capability)) : std::string{};
        std::fprintf(stderr, "  [%s] agent=%s tool=%s%s%s: %s\n", std::string(finding_kind_name(f.kind)).c_str(),
                     f.agent_name.c_str(), f.tool_name.empty() ? "-" : f.tool_name.c_str(),
                     f.capability.has_value() ? " kind=" : "", kind_str.c_str(), f.detail.c_str());
    }
    return 1;
}
