#pragma once
// Implements docs/planning/tool-capability-registry-design-draft.md, closing 2026-08-10-full-
// codebase-adr-gap-audit.md gap #4 (and gap #5's name-keyed ToolTable construction half). See
// decisions/ADR-054-tool-registry-name-keyed-resolution.md for the full design writeup; summarized
// here for the reader looking at this file directly.
//
// A `ToolRegistry` answers, for a bare string in a declarative document's `spec.tools`, exactly the
// three questions the design draft's own §1 poses: what ToolDescriptor does this name resolve to; is
// that descriptor's own capability_ceiling trustworthy; what happens if two sources claim the same
// name. HOST-CURATED ONLY, never auto-discovered -- nothing is ever added to a registry except by an
// explicit `register_tool()` call the host itself makes, after whatever validation this file performs.
// This structurally answers namespace squatting (nothing self-registers) rather than adding a check
// for it.
//
// Capability-ceiling trust (the design draft's own §2b, "the actual new part"): for `native`
// provenance, unchanged -- the compiler is the trust boundary, a hand-authored ToolDescriptor is
// trustworthy by construction, no outer_grant check runs. For every other provenance (a self-reported
// descriptor -- a WASM plugin's own `list-tools` response, an MCP server's tool schema, an A2A agent's
// skill listing), `descriptor.capability_ceiling` is validated against the CALLER-supplied
// `outer_grant` (that provenance's own already-host-decided authority: a WASM instance's ADR-010
// `requested_capabilities`, an MCP connection's configured grant, an A2A delegation's configured
// grant) via the SAME `CapabilitySet::contains()`-per-entry shape `agent_registry.hpp`'s own
// `check_capability_ceiling()` already uses for native tools, called again here at a different layer
// -- reuse, not a new enforcement primitive. A tool whose self-report claims more than its own
// provenance's outer grant covers is EXCLUDED from the registry (this one tool only, not the whole
// batch), never silently widened or clamped.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// ae-naming-lint: allow tool_provenance -- new vocabulary from the gap-4 design draft; 027 has not
// been updated to list it.
// ae-naming-lint: allow tool_provenance — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class tool_provenance { native, wasm_plugin, mcp_server, a2a_agent };

// ae-naming-lint: allow ToolRegistry -- new vocabulary from the gap-4 design draft; 027 has not been
// updated to list it.
// ae-naming-lint: allow ToolRegistry — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class ToolRegistry {
public:
    // Fails closed on a duplicate name -- the SAME "two declared tools share a name" shape
    // `check_tool_name_collision()` already enforces for a single native agent's own tool set
    // (agent_registry.hpp), extended here to the whole registry. Fails closed on a non-native
    // descriptor whose self-reported `capability_ceiling` isn't fully covered by `outer_grant`
    // (ignored for `native` provenance, where no outer grant is meaningful -- see file banner).
    // Either failure records a real reason in `exclusion_reason()` -- MUST-NAME 1 from the design
    // draft's own self-red-team (§4 there): a name considered and excluded must stay
    // distinguishable from "never existed" to a later document that references it.
    [[nodiscard]] result<void> register_tool(std::string name, ToolDescriptor descriptor,
                                              tool_provenance provenance,
                                              std::vector<Capability> const& outer_grant = {}) {
        if (entries_.find(name) != entries_.end()) {
            std::string reason = "a tool named '" + name + "' is already registered in this registry";
            exclusions_[name]  = reason;
            return std::unexpected(
                error{failure_class::contract, reason, "tool_registry.duplicate_name"});
        }

        if (provenance != tool_provenance::native) {
            CapabilitySet const grant = CapabilitySet::grant_root(outer_grant);
            for (Capability const& requirement : descriptor.capability_ceiling) {
                if (!grant.contains(requirement)) {
                    std::string reason = "tool '" + name +
                                          "' self-reported a capability_ceiling entry not covered by "
                                          "its own provenance's outer grant";
                    exclusions_[name] = reason;
                    return std::unexpected(error{failure_class::policy, reason,
                                                  "tool_registry.capability_ceiling_not_covered"});
                }
            }
        }

        entries_.emplace(std::move(name), Entry{std::move(descriptor), provenance});
        return {};
    }

    [[nodiscard]] std::optional<ToolDescriptor> find(std::string_view name) const {
        auto it = entries_.find(std::string(name));
        if (it == entries_.end()) return std::nullopt;
        return it->second.descriptor;
    }

    // MUST-NAME 1 (design draft §4): why a name isn't in the registry, when it was actually
    // CONSIDERED and rejected rather than simply never offered. `std::nullopt` here means neither --
    // this exact name was never passed to `register_tool()` at all.
    [[nodiscard]] std::optional<std::string> exclusion_reason(std::string_view name) const {
        auto it = exclusions_.find(std::string(name));
        if (it == exclusions_.end()) return std::nullopt;
        return it->second;
    }

private:
    struct Entry {
        ToolDescriptor  descriptor;
        tool_provenance provenance;
    };
    std::unordered_map<std::string, Entry>       entries_;
    std::unordered_map<std::string, std::string> exclusions_;
};

// Defined here, declared in tool_pipeline.hpp's `ToolTable` (that header cannot depend on this one --
// this file already needs ToolDescriptor/ToolTable from it). §3 of the design draft: "now a thin
// consequence of §2" -- for each name, a real diagnostic naming the SPECIFIC missing tool
// (`agent.tool_not_found_in_registry`), never a generic parse error; the descriptor-keyed half
// (`from_descriptors()`) already does the actual work, so this adds no new ToolTable machinery.
[[nodiscard]] inline result<ToolTable> ToolTable::from_names(std::vector<std::string> const& names,
                                                               ToolRegistry const&            registry) {
    std::vector<ToolDescriptor> descriptors;
    descriptors.reserve(names.size());
    for (std::string const& name : names) {
        auto found = registry.find(name);
        if (!found.has_value()) {
            return std::unexpected(error{failure_class::contract,
                                          "tool '" + name + "' not found in the supplied ToolRegistry",
                                          "agent.tool_not_found_in_registry"});
        }
        descriptors.push_back(std::move(*found));
    }
    return ToolTable::from_descriptors(std::move(descriptors));
}

}  // namespace agentengine
