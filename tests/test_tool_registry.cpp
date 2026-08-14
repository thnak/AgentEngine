// Proof for ADR-054 (2026-08-10-full-codebase-adr-gap-audit.md gap #4, and gap #5's name-keyed
// ToolTable-construction half): docs/planning/tool-capability-registry-design-draft.md's
// `ToolRegistry`/`ToolTable::from_names()` (core/tool_registry.hpp).
//
//   T1 -- a native tool registers with no outer_grant needed; find() returns it unchanged.
//   T2 -- a duplicate name fails closed; the FIRST registration is untouched; exclusion_reason()
//         explains the rejection (MUST-NAME 1 from the design draft's own self-red-team).
//   T3 -- a non-native (wasm_plugin) tool whose self-reported capability_ceiling IS covered by its
//         own provenance's outer_grant registers successfully.
//   T4 -- a non-native tool whose self-reported capability_ceiling is NOT covered by its outer_grant
//         is excluded (this ONE tool, not a whole-batch failure) -- find() returns nullopt, but
//         exclusion_reason() distinguishes "considered and excluded" from "never offered" (T5).
//   T5 -- a name never passed to register_tool() at all: find() AND exclusion_reason() both nullopt.
//   T6 -- mcp_server/a2a_agent provenance get the identical capability-trust treatment as wasm_plugin
//         -- not a wasm-only special case.
//   T7 -- ToolTable::from_names(): every name resolves -> a real ToolTable whose descriptors match,
//         delegating to the already-real from_descriptors() (no new ToolTable machinery).
//   T8 -- ToolTable::from_names(): a missing name fails closed with the real, SPECIFIC diagnostic
//         (agent.tool_not_found_in_registry), naming the tool, not a generic parse error.

#include <cstdio>
#include <string>

#include "agentengine/core/tool_registry.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

using agentengine::Capability;
using agentengine::ToolDescriptor;
using agentengine::ToolRegistry;
using agentengine::ToolTable;
using agentengine::tool_provenance;

ToolDescriptor make_descriptor(std::string name, std::vector<Capability> ceiling = {}) {
    ToolDescriptor d;
    d.name              = std::move(name);
    d.description       = "test tool";
    d.capability_ceiling = std::move(ceiling);
    d.invoke = [](agentengine::json::Value const&,
                  agentengine::EffectContext&) -> agentengine::result<agentengine::json::Value> {
        return agentengine::json::Value{};
    };
    return d;
}

}  // namespace

int main() {
    // --- T1: a native tool, no outer_grant needed --------------------------------------------------
    {
        ToolRegistry registry;
        auto reg = registry.register_tool("web_search", make_descriptor("web_search"),
                                           tool_provenance::native);
        check(reg.has_value(), "T1: a native tool registers with no outer_grant");
        auto found = registry.find("web_search");
        check(found.has_value() && found->name == "web_search",
              "T1: find() returns the registered descriptor unchanged");
    }

    // --- T2: duplicate name fails closed; first registration untouched -------------------------------
    {
        ToolRegistry registry;
        auto first = registry.register_tool("code_interpreter", make_descriptor("code_interpreter"),
                                             tool_provenance::native);
        check(first.has_value(), "T2 setup: the first registration succeeds");
        auto second = registry.register_tool("code_interpreter",
                                              make_descriptor("code_interpreter", {}),
                                              tool_provenance::native);
        check(!second.has_value(), "T2: a second registration under the same name is rejected");
        if (!second.has_value()) {
            check(second.error().code == "tool_registry.duplicate_name",
                  "T2: rejected with the real duplicate_name error_code");
        }
        check(registry.find("code_interpreter").has_value(),
              "T2: the FIRST registration is untouched by the rejected second one");
        auto reason = registry.exclusion_reason("code_interpreter");
        check(reason.has_value() && !reason->empty(),
              "T2: exclusion_reason() explains the rejection (MUST-NAME 1)");
    }

    // --- T3: non-native, capability_ceiling covered by outer_grant -- registers -----------------------
    {
        ToolRegistry registry;
        std::vector<Capability> const ceiling = {agentengine::cap::NetOut{{"api.search.example"}, {}, {}}};
        std::vector<Capability> const outer_grant = {
            agentengine::cap::NetOut{{"api.search.example"}, {}, {}}};
        auto reg = registry.register_tool("plugin_search", make_descriptor("plugin_search", ceiling),
                                           tool_provenance::wasm_plugin, outer_grant);
        check(reg.has_value(),
              "T3: a wasm_plugin tool whose self-reported ceiling IS covered by its outer_grant "
              "registers");
        check(registry.find("plugin_search").has_value(), "T3: it is then findable");
    }

    // --- T4: non-native, capability_ceiling NOT covered -- excluded, not whole-batch failure ----------
    {
        ToolRegistry registry;
        std::vector<Capability> const ceiling = {
            agentengine::cap::NetOut{{"api.search.example"}, {}, {}}};
        std::vector<Capability> const narrow_outer_grant = {
            agentengine::cap::NetOut{{"api.other.example"}, {}, {}}};  // a DIFFERENT host -- no cover
        auto reg = registry.register_tool("plugin_bad", make_descriptor("plugin_bad", ceiling),
                                           tool_provenance::wasm_plugin, narrow_outer_grant);
        check(!reg.has_value(),
              "T4: a wasm_plugin tool whose self-reported ceiling is NOT covered is rejected");
        if (!reg.has_value()) {
            check(reg.error().code == "tool_registry.capability_ceiling_not_covered",
                  "T4: rejected with the real capability_ceiling_not_covered error_code");
        }
        check(!registry.find("plugin_bad").has_value(), "T4: find() returns nullopt for the excluded tool");
        auto reason = registry.exclusion_reason("plugin_bad");
        check(reason.has_value() && !reason->empty(),
              "T4: exclusion_reason() distinguishes \"considered and excluded\" from \"never offered\"");

        // A SIBLING tool from the same plugin, with a covered ceiling, still registers -- this is a
        // per-tool rejection, not a whole-plugin failure (design draft's own MUST-NAME 1 framing).
        std::vector<Capability> const ok_ceiling = {
            agentengine::cap::NetOut{{"api.other.example"}, {}, {}}};
        auto sibling = registry.register_tool("plugin_good", make_descriptor("plugin_good", ok_ceiling),
                                               tool_provenance::wasm_plugin, narrow_outer_grant);
        check(sibling.has_value(),
              "T4: a SIBLING tool from the same batch, whose own ceiling IS covered, still registers "
              "-- rejecting one tool does not fail the whole plugin");
    }

    // --- T5: a name never offered at all -- find() AND exclusion_reason() both nullopt ---------------
    {
        ToolRegistry registry;
        check(!registry.find("never_offered").has_value(), "T5: find() is nullopt for an unknown name");
        check(!registry.exclusion_reason("never_offered").has_value(),
              "T5: exclusion_reason() is ALSO nullopt -- distinguishing \"never existed\" from T4's "
              "\"considered and excluded\"");
    }

    // --- T6: mcp_server / a2a_agent get the identical capability-trust treatment ----------------------
    {
        ToolRegistry registry;
        std::vector<Capability> const ceiling = {agentengine::cap::FsRead{"m-1", "", {}}};
        std::vector<Capability> const outer_grant = {agentengine::cap::FsRead{"m-1", "", {}}};
        auto mcp = registry.register_tool("mcp_tool", make_descriptor("mcp_tool", ceiling),
                                           tool_provenance::mcp_server, outer_grant);
        check(mcp.has_value(), "T6: an mcp_server tool covered by its outer_grant registers");
        auto a2a = registry.register_tool("a2a_skill", make_descriptor("a2a_skill", ceiling),
                                           tool_provenance::a2a_agent, {});
        check(!a2a.has_value(),
              "T6: an a2a_agent tool with NO outer_grant at all (empty) is rejected -- the same "
              "fail-closed rule, not a provenance-specific carve-out");
    }

    // --- T7: ToolTable::from_names() -- every name resolves -------------------------------------------
    {
        ToolRegistry registry;
        (void)registry.register_tool("alpha", make_descriptor("alpha"), tool_provenance::native);
        (void)registry.register_tool("beta", make_descriptor("beta"), tool_provenance::native);
        auto table = ToolTable::from_names({"alpha", "beta"}, registry);
        check(table.has_value(), "T7: from_names() succeeds when every name resolves");
        if (table.has_value()) {
            check(table->descriptors().size() == 2, "T7: the resulting ToolTable has exactly 2 tools");
            check(table->find("alpha") != nullptr && table->find("beta") != nullptr,
                  "T7: both resolved descriptors are present and findable");
        }
    }

    // --- T8: ToolTable::from_names() -- a missing name fails closed with a real diagnostic -----------
    {
        ToolRegistry registry;
        (void)registry.register_tool("alpha", make_descriptor("alpha"), tool_provenance::native);
        auto table = ToolTable::from_names({"alpha", "does_not_exist"}, registry);
        check(!table.has_value(), "T8: from_names() fails closed when a name isn't in the registry");
        if (!table.has_value()) {
            check(table.error().code == "agent.tool_not_found_in_registry",
                  "T8: rejected with the real, specific error_code");
            check(table.error().message.find("does_not_exist") != std::string::npos,
                  "T8: the error message names the SPECIFIC missing tool, not a generic parse error");
        }
    }

    if (g_failures == 0) {
        std::printf("test_tool_registry: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_tool_registry: %d failure(s)\n", g_failures);
    return 1;
}
