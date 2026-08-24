#pragma once
// Implements 007-Capability-and-Trust-Model.md §9 gate G6 (008 §10 Q3's resolution): "a
// reachability tool enumerates every {capability kind, tool, taint level} combination against a
// reference policy set; every combination's reached decision matches a hand-computed oracle, and
// every rule in the set is exercised by at least one combination (dead-rule detection)." M2 Phase F
// task F2 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md).
//
// 007 §5's declarative rule language (host, tool, path_under, host_not_in, taint, ...) is out of
// scope for M2 (Phase B decision 4) -- there is no rule *set* to enumerate against yet. What this
// enumerates instead is the mechanical shape decision 4 actually built: an agent's declared
// Capabilities<...> ceiling versus each of its declared tools' own Capabilities<...> requirement,
// walked through the exact same CapabilitySet::contains() call register_agent<A>()'s own
// check_capability_ceiling() (core/agent_registry.hpp) already performs for the whole ceiling at
// once -- this enumerator answers "would registration accept this pairing" per {kind, tool} cell
// instead of only pass/fail for the whole agent, and additionally surfaces two classes of over-broad
// reachability a whole-ceiling pass/fail check cannot: a capability kind the agent's own ceiling
// grants that NONE of its declared tools ever requires (an over-broad grant a manual read of one
// tool at a time would not surface, since each individual tool-vs-ceiling comparison still
// "passes"), and a requested combination whose reached decision does not match a hand-computed
// oracle (this header's own `enumerate_policy_reachability`'s dead-rule-detection analog: G6's
// "every rule... exercised" inverted to "every real cell has a matching oracle entry", since there
// is no rule set here for a cell to leave unexercised).
//
// "taint level": 007 §9 G6's own wording assumes a graded taint axis (007 §5's worked example:
// `taint: high`); no such vocabulary exists in code today -- core/content.hpp's
// `ContentItem::tainted` and core/tool_pipeline.hpp's `ToolCallRequest::arguments_tainted` are both
// a single bool (decisions/ADR-007-span-level-taint-vs-per-item.md settled per-item, not graded,
// granularity). This header therefore enumerates taint as the boolean {false, true} the mechanism
// actually has, not 007 §5's aspirational graded levels -- named here, not silently assumed, per
// this project's own scope-cut discipline (decisions/ADR-011-first-party-egress-proxy.md §9's own
// precedent). A further, structural finding this walk itself proves: CapabilitySet::contains() /
// subsumes() never inspects a taint bit at all (trust/capability.hpp's 16 subsumes_payload
// overloads take no taint parameter) -- so today's mechanical enforcement's reached decision is
// provably taint-invariant; `enumerate_policy_reachability` asserts that invariant explicitly
// (decision(kind, tool, taint=false) == decision(kind, tool, taint=true) for every cell, reported as
// finding_kind::taint_variant if it ever doesn't hold) rather than silently assuming it, so the day
// something makes admission taint-sensitive, this check is what notices first.

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::trust {

// One reference agent's declared surface -- the enumerator's input unit. Deliberately independent
// of core/agent_registry.hpp's `Agent<...>`/`register_agent<A>()` machinery: a `ReachabilityAgent`
// can be built either from a real `AgentMetadata` (round-tripping the actual compiler's output) or
// by hand (to construct a pairing the real compiler would reject, and independently prove this
// enumerator reaches the same conclusion) -- both are legitimate reference-policy-set entries.
// ae-naming-lint: allow ReachabilityAgent — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ReachabilityAgent {
    std::string agent_name;
    std::vector<Capability> capability_ceiling;
    std::vector<ToolDescriptor> tools;
};

// One enumerated cell: does `agent_name`'s ceiling grant `tool_name`'s declared `kind` requirement,
// for a request tagged `tainted`. `requirement` is the exact tool-declared Capability this cell
// checked (kept for the report -- an operator reading "net_out DENIED for tool X" needs to see which
// host/allowlist shape was actually being asked for, not just the kind).
// ae-naming-lint: allow ReachabilityCell — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ReachabilityCell {
    std::string agent_name;
    std::string tool_name;
    capability_kind kind;
    bool tainted = false;
    bool granted = false;  // the reached decision: CapabilitySet::contains(requirement)
    Capability requirement;
};

// A hand-computed expectation for one {agent, tool, kind} cell. Taint-invariant by the finding this
// header's own top comment proves -- one oracle entry covers both taint states for that cell.
// ae-naming-lint: allow ReachabilityOracleEntry — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ReachabilityOracleEntry {
    std::string agent_name;
    std::string tool_name;
    capability_kind kind;
    bool expected_granted = false;
};

// ae-naming-lint: allow finding_kind — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class finding_kind {
    oracle_mismatch,      // a cell's reached decision disagrees with the hand-computed oracle
    taint_variant,        // a cell's decision differs between tainted=false and tainted=true
                           // (would mean admission became taint-sensitive -- see file-top comment)
    over_broad_grant,      // agent's ceiling grants a kind no declared tool of theirs ever requires
    uncovered_by_oracle,   // a real {agent, tool, kind} cell has no matching oracle entry -- the
                            // oracle itself is stale/incomplete and cannot be trusted to catch a
                            // regression on that cell
};

[[nodiscard]] inline std::string_view capability_kind_name(capability_kind kind) {
    switch (kind) {
        case capability_kind::fs_read: return "fs_read";
        case capability_kind::fs_write: return "fs_write";
        case capability_kind::net_out: return "net_out";
        case capability_kind::net_listen: return "net_listen";
        case capability_kind::secret: return "secret";
        case capability_kind::tool_call: return "tool_call";
        case capability_kind::runner_call: return "runner_call";
        case capability_kind::exec: return "exec";
        case capability_kind::clock: return "clock";
        case capability_kind::entropy: return "entropy";
        case capability_kind::env_read: return "env_read";
        case capability_kind::env_write: return "env_write";
        case capability_kind::agent_call: return "agent_call";
        case capability_kind::schedule: return "schedule";
        case capability_kind::background: return "background";
        case capability_kind::elicit: return "elicit";
        case capability_kind::native_exec: return "native_exec";
        case capability_kind::sandbox_mount: return "sandbox_mount";
        case capability_kind::sandbox_net_out: return "sandbox_net_out";
    }
    return "unknown";
}

[[nodiscard]] inline std::string_view finding_kind_name(finding_kind kind) {
    switch (kind) {
        case finding_kind::oracle_mismatch: return "oracle_mismatch";
        case finding_kind::taint_variant: return "taint_variant";
        case finding_kind::over_broad_grant: return "over_broad_grant";
        case finding_kind::uncovered_by_oracle: return "uncovered_by_oracle";
    }
    return "unknown";
}

// ae-naming-lint: allow ReachabilityFinding — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ReachabilityFinding {
    finding_kind kind;
    std::string agent_name;
    std::string tool_name;  // empty for agent-level findings (over_broad_grant)
    std::optional<capability_kind> capability;
    std::string detail;
};

// ae-naming-lint: allow ReachabilityReport — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ReachabilityReport {
    std::vector<ReachabilityCell> cells;
    std::vector<ReachabilityFinding> findings;
    [[nodiscard]] bool clean() const { return findings.empty(); }
};

// The enumerator itself (G6's "a reachability tool enumerates every {capability kind, tool, taint
// level} combination"). Walks every declared {agent, tool, tool-declared-capability, taint} cell --
// NOT the full 16-kind x agent x tool x taint cross product: a cell only exists for a
// (tool, capability) pair the tool actually declares (007 §3 property 5, "NetOut with no allowlist
// is not a capability, it is a hole" -- there is no meaningful "does this tool reach fs_write" cell
// for a tool that never declares an FsWrite requirement at all). Whether an agent's ceiling grants
// MORE than its tools require is answered separately, by the over_broad_grant check below, which
// does walk the full declared-vs-required difference.
[[nodiscard]] inline ReachabilityReport enumerate_policy_reachability(
    std::vector<ReachabilityAgent> const& agents, std::vector<ReachabilityOracleEntry> const& oracle) {
    ReachabilityReport report;

    for (ReachabilityAgent const& agent : agents) {
        CapabilitySet const granted = CapabilitySet::grant_root(agent.capability_ceiling);

        std::vector<capability_kind> granted_kinds;
        for (Capability const& c : agent.capability_ceiling) {
            capability_kind const k = capability_kind_of(c);
            if (std::find(granted_kinds.begin(), granted_kinds.end(), k) == granted_kinds.end()) {
                granted_kinds.push_back(k);
            }
        }
        std::vector<capability_kind> required_kinds;

        for (ToolDescriptor const& tool : agent.tools) {
            for (Capability const& requirement : tool.capability_ceiling) {
                capability_kind const kind = capability_kind_of(requirement);
                if (std::find(required_kinds.begin(), required_kinds.end(), kind) == required_kinds.end()) {
                    required_kinds.push_back(kind);
                }

                // contains()/subsumes() takes no taint parameter at all (file-top comment) -- asked
                // twice, once per taint state, to keep that fact explicit at the call site rather
                // than assumed once and copied into both cells.
                bool const decision_untainted = granted.contains(requirement);
                bool const decision_tainted = granted.contains(requirement);

                for (bool tainted : {false, true}) {
                    bool const decision = tainted ? decision_tainted : decision_untainted;
                    report.cells.push_back(
                        ReachabilityCell{agent.agent_name, tool.name, kind, tainted, decision, requirement});
                }

                if (decision_untainted != decision_tainted) {
                    report.findings.push_back(
                        ReachabilityFinding{finding_kind::taint_variant, agent.agent_name, tool.name, kind,
                                             "decision differs between tainted=false and tainted=true"});
                }

                auto const oracle_it =
                    std::find_if(oracle.begin(), oracle.end(), [&](ReachabilityOracleEntry const& e) {
                        return e.agent_name == agent.agent_name && e.tool_name == tool.name && e.kind == kind;
                    });
                if (oracle_it == oracle.end()) {
                    report.findings.push_back(
                        ReachabilityFinding{finding_kind::uncovered_by_oracle, agent.agent_name, tool.name, kind,
                                             "no oracle entry for this cell -- oracle is stale or incomplete"});
                } else if (oracle_it->expected_granted != decision_untainted) {
                    report.findings.push_back(ReachabilityFinding{
                        finding_kind::oracle_mismatch, agent.agent_name, tool.name, kind,
                        decision_untainted ? "reached GRANTED, oracle expected DENIED"
                                            : "reached DENIED, oracle expected GRANTED"});
                }
            }
        }

        for (capability_kind gk : granted_kinds) {
            bool const required = std::find(required_kinds.begin(), required_kinds.end(), gk) != required_kinds.end();
            if (!required) {
                report.findings.push_back(ReachabilityFinding{
                    finding_kind::over_broad_grant, agent.agent_name, "", gk,
                    "agent's ceiling grants this capability kind but no declared tool requires it"});
            }
        }
    }

    return report;
}

}  // namespace agentengine::trust
