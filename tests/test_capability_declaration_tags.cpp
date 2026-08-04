// Milestone 2 Phase A, task A2 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// compile-time capability declaration tags (trust/capability.hpp's `cap::decl::*`), usable in
// `Capabilities<Cs...>` at Tool/Agent declaration sites (002 §3, 006 §1) the way the RFCs' own
// examples show — `Capabilities<NetOut<"api.search.example">>` — not the bare `capability_kind`
// enum the pre-A2 stub took. Proves (a) the tags compile in the actual declaration position, and
// (b) `to_capability()` turns each one into the runtime `Capability` a `CapabilitySet` grant is
// actually checked against, for a representative sample (every alternative has its own overload,
// but exhaustively re-testing all sixteen adds little beyond what ADR-009's own suite already
// covers for the runtime `cap::*` shapes this converts into).

#include <iostream>
#include <string_view>
#include <type_traits>

#include "agentengine/core/agent.hpp"
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

// The actual declaration-site shape 002 §2 / 006 §1 show: a policy tag taking declaration-tag
// TYPES, host/mount spelled out at the use site, not a bare enum value. This is a compile-time-only
// check -- if this translation unit compiles, the claim already holds.
struct DemoTool {
    using capability_ceiling = Capabilities<cap::decl::NetOut<"api.search.example">,
                                             cap::decl::FsRead<"workspace">>;
};

static_assert(std::is_same_v<DemoTool::capability_ceiling,
                              Capabilities<cap::decl::NetOut<"api.search.example">,
                                           cap::decl::FsRead<"workspace">>>,
              "Capabilities<Cs...> must accept declaration-tag TYPES, matching 002 §2 / 006 §1's "
              "own Capabilities<NetOut<\"...\">> examples literally");

}  // namespace

int main() {
    // A NetOut declaration tag converts to a runtime NetOut capability whose allowlist contains
    // exactly the declared host, and that capability is checkable against a CapabilitySet the same
    // way any other NetOut grant is (ADR-009's subsumes()).
    {
        Capability c = to_capability(cap::decl::NetOut<"api.search.example">{});
        AE_CHECK(capability_kind_of(c) == capability_kind::net_out,
                  "A2: NetOut<\"...\"> converts to a net_out-kind Capability");
        auto set = CapabilitySet::grant_root({c});
        AE_CHECK(set.contains(cap::NetOut{{"api.search.example"}, std::nullopt, {}}),
                  "A2: the converted grant actually covers the declared host");
        AE_CHECK(!set.contains(cap::NetOut{{"evil.example"}, std::nullopt, {}}),
                  "A2: the converted grant does NOT cover an undeclared host (I2 -- no ambient net access)");
    }

    // FsRead<"mount"> converts to a whole-mount grant (no path/cap narrowing at declaration time --
    // that is 002 §6's metadata-compiler job, Phase E, not this conversion's).
    {
        Capability c = to_capability(cap::decl::FsRead<"workspace">{});
        auto set = CapabilitySet::grant_root({c});
        AE_CHECK(set.contains(cap::FsRead{"workspace", "/any/subpath", 1}),
                  "A2: FsRead<\"mount\"> covers the whole declared mount, any subpath/cap");
        AE_CHECK(!set.contains(cap::FsRead{"other-mount", "", std::nullopt}),
                  "A2: FsRead<\"mount\"> does NOT cover a different, undeclared mount");
    }

    // ToolCall<"name"> -- the one kind whose runtime type collides with core/content.hpp's
    // unrelated ToolCall (the reason cap::/cap::decl:: are namespaced at all) -- still round-trips
    // correctly through the SAME name at a different qualification depth.
    {
        Capability c = to_capability(cap::decl::ToolCall<"web_search">{});
        auto set = CapabilitySet::grant_root({c});
        AE_CHECK(set.contains(cap::ToolCall{"web_search"}),
                  "A2: ToolCall<\"name\"> round-trips to the matching runtime ToolCall capability");
        AE_CHECK(!set.contains(cap::ToolCall{"other_tool"}),
                  "A2: ToolCall<\"name\"> does not grant a differently-named tool");
    }

    // AgentCall<"agent", depth> reuses ADR-006's SpawnBudget for its depth parameter (not a second
    // counter) -- confirm the declared depth actually lands in the converted grant.
    {
        Capability c = to_capability(cap::decl::AgentCall<"researcher", 3>{});
        auto set = CapabilitySet::grant_root({c});
        AE_CHECK(set.contains(cap::AgentCall{"researcher", trust::SpawnBudget::mint_root(2)}),
                  "A2: a depth-2 request is covered by a declared depth-3 AgentCall grant (attenuation)");
        AE_CHECK(!set.contains(cap::AgentCall{"researcher", trust::SpawnBudget::mint_root(4)}),
                  "A2: a depth-4 request exceeds the declared depth-3 grant and is denied");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All capability declaration tag (A2) checks passed.\n";
    return 0;
}
