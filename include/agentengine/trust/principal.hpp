#pragma once
// Implements 007-Capability-and-Trust-Model.md — the authenticated identity a run executes on
// behalf of. Propagated into every effect and every outbound protocol call (I4).

#include <string>

namespace agentengine {

struct Principal {
    std::string id;         // stable identity, opaque to the core
    std::string tenant_id;  // multi-tenancy scope (018); empty for single-tenant deployments
};

} // namespace agentengine
