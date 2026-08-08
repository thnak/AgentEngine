#pragma once
// Implements 012-A2A-Conformance.md §3/§4a -- consuming a remote A2A agent (client role). Milestone 7
// Phase D4 (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// Transport-agnostic, exactly like `A2aServer` (server.hpp) and `McpClient` (protocol/mcp/client.hpp):
// handed a `RemoteAgentTransport` (three plain callables -- send/get/cancel) and a `CardFetcher`
// rather than a socket or an HTTP client. A later transport sub-phase supplies real Streamable-HTTP-
// backed callables; this phase's own test wires them directly into a real `A2aServer::send_message`/
// `get_task`/`cancel_task` (D3), the same "same process, two roles, real machinery underneath" shape
// every prior Phase C/D client test already used.
//
// Scope: pass-through `send_message`/`get_task`/`cancel_task` (§A.2's client-facing operations this
// codebase can meaningfully drive today), plus §3/§4a's digest-pinned Agent Card caching -- "a card
// whose skills or schemas change is re-approved rather than silently trusted," the identical
// discipline `McpClient`'s own `rug_pull_detected()` already proves for MCP tool listings, reused
// here for A2A's own Agent Card (§4a).
//
// NOT built here (named, not claimed): binding a remote agent as a local `Tool<T>` (§3's "bound as a
// tool... with the same declaration syntax as a local one," 012 §8's own G5 gate) -- that touches
// `core/tool.hpp`'s zero-cost CRTP machinery (ADR-007) and is Phase G's own promotion-gate proof, not
// built speculatively here; JWS card-signature verification (§4a); deadline/cancellation PROPAGATION
// to a remote task (§3's own "deadlines propagate as remaining duration," "cancellation propagates")
// -- this client relays a caller's `cancel_task()` call, but nothing here derives or forwards a
// deadline yet; `Suspended`-state mapping for a long-running remote task (§3, needs 019's own
// Suspended machinery wired to a remote poll loop, not built).

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/a2a/agent_card.hpp"
#include "agentengine/protocol/a2a/types.hpp"

namespace agentengine::a2a {

// What a real transport sub-phase would implement as three RPC calls (§A.2's own table: `SendMessage`,
// `GetTask`, `CancelTask`) -- exactly the "no real transport yet, take what a transport would supply
// as given" layering `A2aServer`'s own `RunStarter` already establishes for the server side.
struct RemoteAgentTransport {
    std::function<result<Task>(Message const&)>     send_message;
    std::function<result<Task>(std::string const&)> get_task;
    std::function<result<Task>(std::string const&)> cancel_task;
};

// §A.3: `GET /.well-known/agent-card.json`. A plain callable rather than a URL this phase would have
// to fetch over a real HTTP client that does not exist yet (011 §7/012 §7 both defer Streamable HTTP
// to a later sub-phase).
using CardFetcher = std::function<result<AgentCard>()>;

namespace client_detail {

// The same FNV-1a "deterministic hash, change-DETECTION only, not a cryptographic commitment" idiom
// `protocol/mcp/client.hpp`'s own `digest_of()` already establishes for the identical rug-pull-defense
// job (§8 there, §4a here) -- reproduced rather than shared through an unrelated MCP-specific header
// for one function, matching this codebase's own established per-file duplication precedent for this
// exact idiom (json_schema.hpp/tool_pipeline.hpp/protocol/mcp/client.hpp all do the same).
[[nodiscard]] inline std::string digest_of(AgentCard const& card) {
    std::string const dumped = json::dump(to_json(card));
    std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
    for (unsigned char c : dumped) {
        h ^= c;
        h *= 0x0000'0100'0000'01B3ULL;
    }
    return std::to_string(h);
}

}  // namespace client_detail

class A2aClient {
public:
    A2aClient(RemoteAgentTransport transport, CardFetcher card_fetcher, std::string client_name)
        : transport_(std::move(transport)), card_fetcher_(std::move(card_fetcher)),
          client_name_(std::move(client_name)) {}

    [[nodiscard]] result<Task> send_message(Message const& msg) { return transport_.send_message(msg); }
    [[nodiscard]] result<Task> get_task(std::string const& task_id) const { return transport_.get_task(task_id); }
    [[nodiscard]] result<Task> cancel_task(std::string const& task_id) const {
        return transport_.cancel_task(task_id);
    }

    // §3/§4a: fetches (or re-fetches) the remote agent's card. §4a's own rule ("a card whose skills
    // or schemas change is re-approved rather than silently trusted") is enforced by comparing this
    // fetch's own digest against the PRIOR cached one -- a change flips `rug_pull_detected()`, never
    // silently overwrites the trust decision.
    [[nodiscard]] result<AgentCard> fetch_agent_card() {
        result<AgentCard> fetched = card_fetcher_();
        if (!fetched) return fetched;
        std::string const digest = client_detail::digest_of(*fetched);
        if (cached_card_.has_value() && digest != cached_digest_) {
            rug_pull_detected_ = true;
        }
        cached_card_   = *fetched;
        cached_digest_ = digest;
        return fetched;
    }

    [[nodiscard]] bool rug_pull_detected() const noexcept { return rug_pull_detected_; }
    [[nodiscard]] std::optional<AgentCard> const& cached_card() const noexcept { return cached_card_; }

private:
    RemoteAgentTransport      transport_;
    CardFetcher               card_fetcher_;
    std::string               client_name_;
    std::optional<AgentCard>  cached_card_;
    std::string                cached_digest_;
    bool                        rug_pull_detected_ = false;
};

}  // namespace agentengine::a2a
