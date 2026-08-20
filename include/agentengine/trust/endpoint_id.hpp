#pragma once
// ADR-061 §13.5/§31.1 (decisions/ADR-061-host-provided-inbound-transport.md): a minted-not-indexed
// endpoint identifier, closing finding T8 -- §8.3's original `EndpointId{std::uint32_t index}` was a
// dense, guessable index into operator config, uncontained across audience-adjacent resources the
// moment an index is lied about or misconfigured. This is minted by the operator's own configuration
// step with CSPRNG entropy instead: opaque, unguessable, presented verbatim by the host rather than
// selected positionally. The host still only SELECTS AMONG operator-approved endpoints; it never
// asserts a value the operator didn't mint -- unchanged from §13.5's own constraint.
//
// Deliberately does NOT carry `endpoint_surface`/admin-vs-public-API refusal (§8.3's second half,
// T16's "partial re-expression of 020 §4") -- that check has no consumer yet (`rt::AgentSession` has a
// tool table, not a notion of "admin methods"; surface enforcement belongs wherever `McpServer`'s
// method dispatch lives, ADR-039 §3a's still-open territory, not this file's). Building it here without
// anything that enforces it would be dead code asserting a guarantee nothing checks.

#include <string>
#include <unordered_map>
#include <utility>

#include "agentengine/core/error.hpp"
#include "agentengine/trust/secure_random.hpp"

namespace agentengine::trust {

// Minted once, at operator configuration time, never per-request and never by a host. Opaque and
// unguessable (§13.5/T8) -- deliberately NOT parsed, decoded, or treated as carrying meaning; its only
// job is to be an unenumerable key into EndpointRegistry's own config.
// ae-naming-lint: allow EndpointId — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct EndpointId {
    std::string token;
    [[nodiscard]] bool operator==(EndpointId const&) const = default;
};

// Same CSPRNG, same entropy budget as `server_detail::generate_task_id()` (protocol/mcp/server.hpp) --
// reused, not independently chosen, per this ADR's own "one shared primitive" discipline.
[[nodiscard]] inline result<EndpointId> mint_endpoint_id() {
    auto hex = secure_random_hex(16);
    if (!hex) return std::unexpected(hex.error());
    return EndpointId{*std::move(hex)};
}

// What an operator-configured endpoint actually carries -- audience/issuer feed `verify_bearer_token`'s
// own caller-supplied `expected_aud`/`expected_iss` (§13.5's "host selects among operator-approved
// endpoints, never asserts a value the operator didn't mint" constraint, unchanged).
// ae-naming-lint: allow EndpointConfig — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct EndpointConfig {
    std::string audience;
    std::string issuer;
};

// Real, in-process registry. `resolve()` fails closed on an absent key -- no positional fallback, no
// "endpoint 0", matching §13.5's "the host presents the value verbatim" (there is no default to fall
// back to because there is no ordering to fall back into).
// ae-naming-lint: allow EndpointRegistry — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class EndpointRegistry {
public:
    void configure(EndpointId const& id, EndpointConfig config) { by_token_[id.token] = std::move(config); }

    [[nodiscard]] result<EndpointConfig const*> resolve(EndpointId const& id) const {
        auto it = by_token_.find(id.token);
        if (it == by_token_.end()) {
            return std::unexpected(ae::error{failure_class::contract, "unknown endpoint id",
                                              "endpoint_registry.unknown_id"});
        }
        return &it->second;
    }

private:
    std::unordered_map<std::string, EndpointConfig> by_token_;
};

}  // namespace agentengine::trust
