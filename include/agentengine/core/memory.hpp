#pragma once
// Implements 029-Memory-System.md §3 — structured, provenanced memory records. Never a wrapped
// vector database; default retrieval is a pure function of stored fields (029 §5).

#include <optional>
#include <string>
#include <vector>

#include "agentengine/trust/principal.hpp"

namespace agentengine {

enum class memory_kind { episodic, semantic, procedural };  // ae-naming-lint: allow memory_kind — pre-existing M0 scaffolding, reconcile at owning milestone

// A trust signal (I3), not decoration (029 §3). A ModelInferred item must never be rendered or
// used as if it had UserStated's standing (029 §6).
enum class memory_source { user_stated, model_inferred, tool_derived, agent_authored };  // ae-naming-lint: allow memory_source — pre-existing M0 scaffolding, reconcile at owning milestone

struct MemoryOrigin {  // ae-naming-lint: allow MemoryOrigin — pre-existing M0 scaffolding, reconcile at owning milestone
    memory_source source;
    std::string   run_id;
    std::string   turn_id;
    Principal     principal;
};

struct MemoryItem {  // ae-naming-lint: allow MemoryItem — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string              id;  // content digest (025 §2) — identity, not assigned
    memory_kind               kind;
    std::string               content;
    std::vector<std::string>  tags;
    float                     salience = 0.0f;
    MemoryOrigin              origin;
    std::optional<std::string> expires_at;  // ISO-8601; elided Timestamp type
};

} // namespace agentengine
