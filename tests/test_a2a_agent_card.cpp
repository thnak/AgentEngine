// Milestone 7 Phase D2 (012-A2A-Conformance.md §2.1, docs/research/2026-a2a-and-agui-detail.md §A.3,
// docs/planning/milestone-7-protocol-conformance-breakdown.md). Proves `to_agent_card()`
// (protocol/a2a/agent_card.hpp) generates an Agent Card from a REAL, `register_agent<A>()`-compiled
// `AgentMetadata` -- skills derived one-per-tool from the real `ToolTable`, never hand-maintained --
// and that it never advertises a capability/binding this codebase cannot yet serve (§2.1's own rule).

#include <cstdio>
#include <string>

#include "agentengine/protocol/a2a/agent_card.hpp"

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

namespace ae   = agentengine;
namespace a2a  = agentengine::a2a;
namespace json = agentengine::json;

struct EchoArgs { std::string message; };
AE_JSON_SCHEMA(EchoArgs, message)
struct EchoReply { std::string echoed; };
AE_JSON_SCHEMA(EchoReply, echoed)

struct EchoTool : ae::Tool<EchoTool> {
    static constexpr std::string_view name        = "echo";
    static constexpr std::string_view description = "Echoes its input back.";
    using Args  = EchoArgs;
    using Reply = EchoReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.message}; }
};

struct SearchArgs { std::string query; };
AE_JSON_SCHEMA(SearchArgs, query)
struct SearchReply { std::string result; };
AE_JSON_SCHEMA(SearchReply, result)

struct SearchTool : ae::Tool<SearchTool> {
    static constexpr std::string_view name        = "search";
    static constexpr std::string_view description = "Searches for the given query.";
    using Args  = SearchArgs;
    using Reply = SearchReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) { return Reply{"n/a"}; }
};

struct TwoToolAgent : ae::Agent<TwoToolAgent, ae::ChatClientId<"anthropic:claude-opus-5">,
                                 ae::Tools<EchoTool, SearchTool>> {
    static constexpr std::string_view name         = "two-tool-agent";
    static constexpr std::string_view instructions = "Declares two tools, for a card with two skills.";
};

struct NoToolAgent : ae::Agent<NoToolAgent, ae::ChatClientId<"anthropic:claude-opus-5">> {
    static constexpr std::string_view name         = "no-tool-agent";
    static constexpr std::string_view instructions = "Declares no tools at all.";
};

}  // namespace

int main() {
    // --- D2-1: skills are derived one-per-tool, in registration order, name/description from the ----
    // --- real ToolDescriptor -- never hand-maintained.                                              ---
    {
        auto meta = ae::register_agent<TwoToolAgent>();
        check(meta.has_value(), "D2-1: TwoToolAgent registers cleanly");
        if (meta.has_value()) {
            a2a::AgentCardIdentity identity;
            identity.description = "A test agent with two tools.";
            identity.version     = "1.0.0";
            a2a::AgentCard card = a2a::to_agent_card(*meta, identity);

            check(card.name == "two-tool-agent", "D2-1: the card's name comes from the real agent_name");
            check(card.skills.size() == 2, "D2-1: two tools produce two skills");
            if (card.skills.size() == 2) {
                check(card.skills[0].id == "echo" && card.skills[0].name == "echo" &&
                          card.skills[0].description == "Echoes its input back.",
                      "D2-1: skill[0] carries echo's real name/description, registration order first");
                check(card.skills[1].id == "search" && card.skills[1].description ==
                                                            "Searches for the given query.",
                      "D2-1: skill[1] carries search's real name/description");
            }

            // --- D2-2: description/version are honestly caller-supplied, never fabricated from ------
            // --- agent_instructions (AgentMetadata has no description/version field yet).           ---
            check(card.description == "A test agent with two tools." && card.version == "1.0.0",
                  "D2-2: description/version are exactly what the caller supplied");

            // --- D2-3: with no interfaces/capabilities supplied, the card advertises NONE -- §2.1's ---
            // --- own "advertises only what's proven" rule, since no binding exists yet.             ---
            check(card.supported_interfaces.empty(),
                  "D2-3: supportedInterfaces defaults empty -- no real binding exists yet");
            check(!card.capabilities.streaming && !card.capabilities.push_notifications,
                  "D2-3: streaming/pushNotifications default false -- neither is built yet");

            // --- D2-5: field names on the wire are camelCase, per §A.3. -------------------------------
            std::string dumped = json::dump(a2a::to_json(card));
            check(dumped.find("\"supportedInterfaces\"") != std::string::npos &&
                      dumped.find("\"defaultInputModes\"") != std::string::npos &&
                      dumped.find("\"defaultOutputModes\"") != std::string::npos &&
                      dumped.find("\"pushNotifications\"") != std::string::npos,
                  "D2-5: the wire form uses A2A's own camelCase field names");

            // --- D2-7: tags is always present (spec: "all required") even though empty -- no tag ----
            // --- vocabulary exists anywhere in this codebase yet, named not invented.               ---
            check(dumped.find("\"tags\":[]") != std::string::npos,
                  "D2-7: skills carry a present-but-empty \"tags\" array, honestly, not a fabricated one");
        }
    }

    // --- D2-4: an agent with NO tools produces an empty skills array, not an error -------------------
    {
        auto meta = ae::register_agent<NoToolAgent>();
        check(meta.has_value(), "D2-4: NoToolAgent registers cleanly");
        if (meta.has_value()) {
            a2a::AgentCardIdentity identity;
            identity.description = "No tools.";
            identity.version     = "0.1.0";
            a2a::AgentCard card = a2a::to_agent_card(*meta, identity);
            check(card.skills.empty(), "D2-4: zero tools produces zero skills, cleanly");
        }
    }

    // --- D2-6: once a caller DOES have a real binding (D3+), supplying an AgentInterface round-trips -
    // --- through this generator untouched -- it doesn't get in the way of the real case.             -
    {
        auto meta = ae::register_agent<NoToolAgent>();
        check(meta.has_value(), "D2-6: NoToolAgent registers cleanly (reused for the interface check)");
        if (meta.has_value()) {
            a2a::AgentCardIdentity identity;
            identity.description = "Has one interface.";
            identity.version     = "0.1.0";
            a2a::AgentInterface iface;
            iface.url              = "https://example.invalid/a2a";
            iface.protocol_binding = "JSONRPC";
            iface.protocol_version = "1.0";
            iface.tenant           = "tenant-x";
            identity.supported_interfaces.push_back(iface);
            a2a::AgentCard card = a2a::to_agent_card(*meta, identity);
            check(card.supported_interfaces.size() == 1, "D2-6: the supplied interface is carried through");
            std::string dumped = json::dump(a2a::to_json(card));
            check(dumped.find("\"protocolBinding\":\"JSONRPC\"") != std::string::npos &&
                      dumped.find("\"tenant\":\"tenant-x\"") != std::string::npos,
                  "D2-6: the interface's own fields, including optional tenant, round-trip to the wire");
        }
    }

    if (g_failures == 0) {
        std::printf("test_a2a_agent_card: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_a2a_agent_card: %d failure(s)\n", g_failures);
    return 1;
}
