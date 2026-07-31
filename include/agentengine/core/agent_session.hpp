#pragma once
// Implements 005-Sessions-State-and-Memory.md §1 and 001-Execution-Model.md §1. Terminology
// (027 §7): the type is `AgentSession`, not the bare `Session` an earlier draft used — "session"
// remains the ordinary word everywhere in prose.

#include <chrono>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine {

// One Quark actor instance, keyed by session_id (005 §1). This struct is the durable shape; the
// actor behaviour (activation, passivation, single-executor serialization) is Quark's, inherited
// per 001 §1, not reimplemented here.
struct AgentSession {
    std::string                          session_id;
    Principal                            principal;
    std::vector<Message>                 history;  // append-mostly; rewriting is an audited op
    std::chrono::system_clock::time_point created_at{};
    std::chrono::system_clock::time_point updated_at{};
};

} // namespace agentengine
