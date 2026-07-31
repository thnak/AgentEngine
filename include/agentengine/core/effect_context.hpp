#pragma once
// Implements 007-Capability-and-Trust-Model.md and I4 — the mandatory attribution parameter
// carried into every effect. Never an ambient thread-local (CONVENTIONS.md "Security rules").

#include <chrono>
#include <string>

#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine {

struct EffectContext {
    Principal                             principal;
    CapabilitySet const*                  capabilities = nullptr;  // borrowed; never owned here
    std::chrono::steady_clock::time_point deadline{};
    std::string                           trace_id;
    std::string                           span_id;
};

} // namespace agentengine
