#pragma once
// Implements 001-Execution-Model.md §2 — `Interaction`, the one internal correlation identity
// behind `InputRequired`/`AuthRequired` ("the run itself only ever knows `Interaction` records,
// never a protocol's id shape"). Milestone 4 Phase E1
// (docs/planning/milestone-4-sessions-durability-memory-breakdown.md, decision 2): 001 §2 already
// specifies this shape in prose (`Interaction{interaction_id, run_id, reason ∈ {input, auth},
// opened_at, expires_at?}`) — this header makes it a real C++ type, implementing already-Reviewed
// text rather than reopening design.
//
// All scalar/string fields — unlike `Message`/`ContentItem` (003, A4's own named serialization
// gap) or `Capability` (D1's own named gap), `Interaction` has NO variant and is `Described`
// (`QUARK_SERIALIZE`-able) from the start, so it can be embedded directly in a durable checkpoint
// (`AgentSessionRecord.open_interactions`, agent_session.hpp) — 019 §1's "pending approvals/input
// requests" checkpoint-content item, which D1 could only leave as an always-empty placeholder
// before this type existed for real.

#include <cstdint>
#include <string>

#include "quark/core/describe.hpp"

namespace agentengine {

// 001 §2: "reason ∈ {input, auth}" — the tag that lets one mechanism serve both `InputRequired`
// and `AuthRequired` without a caller needing to guess from context which kind of "waiting" it is
// looking at.
enum class interaction_reason { input, auth };  // ae-naming-lint: allow interaction_reason — 001 §2 names this concept normatively; 027 has not been updated to list it

struct Interaction {  // ae-naming-lint: allow Interaction — 001 §2 names this concept normatively; 027 has not been updated to list it
    std::string        interaction_id;
    std::string        run_id;
    interaction_reason reason = interaction_reason::input;
    // 001 §7's usual caveat applies here too: these are raw nanosecond counts with no real
    // wall-clock source wired in anywhere in this project yet (Clock is not a wired capability).
    // `expires_at_ns == 0` means "no expiry" (001 §2's `expires_at?` is optional).
    std::int64_t        opened_at_ns = 0;
    std::int64_t        expires_at_ns = 0;

    friend bool operator==(Interaction const&, Interaction const&) = default;
};
QUARK_SERIALIZE(Interaction, (1, interaction_id), (2, run_id), (3, reason), (4, opened_at_ns),
                 (5, expires_at_ns))

} // namespace agentengine
