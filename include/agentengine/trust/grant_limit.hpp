#pragma once
// Implements decisions/ADR-244-required-choices-step-1b.md §2.1 / §2.2 (slice 5, G-C1/G-C2): how a declarative
// capability grant states one quantity cap. A cap is a positive integer or the NAMED value `unbounded`; an absent
// key is `choice.missing.grant.<field>` (never "uncapped"), `0` is `choice.zero_bound`, and `-1`, a fraction, `null`
// or any other spelling of "none" is refused. Host-authored configuration only (I3).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "agentengine/core/choice_error.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::trust {

inline constexpr std::array<std::string_view, 2> kGrantLimitOptions{"a positive integer", "unbounded"};

// `key` absent -> choice.missing.grant.<key>; present -> the stated limit, or `grant_limit.invalid` / `choice.zero_bound`.
[[nodiscard]] inline agentengine::result<cap::Limit> parse_grant_limit(json::Value const& obj, std::string_view key) {
    auto const* f = obj.find(key);
    if (f == nullptr) {
        return std::unexpected(choice_missing("grant." + std::string(key), kGrantLimitOptions));
    }
    if (f->is_string() && f->as_string() == "unbounded") return cap::Limit::unbounded();
    auto n = json::as_bounded_integer(*f);
    if (!n.has_value()) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "'" + std::string(key) + "' must be a positive integer or the value unbounded",
            "grant_limit.invalid"});
    }
    auto limit = cap::Limit::at_most(*n);
    if (auto ok = check_bound(key, limit); !ok) return std::unexpected(ok.error());
    return limit;
}

}  // namespace agentengine::trust
