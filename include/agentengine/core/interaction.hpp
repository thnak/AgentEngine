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
// gap) or `Capability` (D1's own named gap), `Interaction` has no variant, so it round-trips through
// a durable checkpoint trivially — 019 §1's "pending approvals/input requests" checkpoint-content
// item (`rt::AgentSessionRecord.open_interactions`, rt/agent_session.hpp), encoded via
// `rt/interaction_codec.hpp`'s own hand-written JSON codec.
//
// ADR-037: no longer `QUARK_SERIALIZE`'d — that macro's only real consumer was the old Quark-actor
// `agentengine::AgentSessionRecord` (core/agent_session.hpp, deleted) and `workflow/checkpoint.hpp`
// (also deleted, zero remaining consumers). `rt::` land has always encoded this type as JSON instead
// (`rt/interaction_codec.hpp`), never through Quark's wire codec — this header now has zero Quark
// dependency of any kind.

#include <cstdint>
#include <string>

namespace agentengine {

// 001 §2: "reason ∈ {input, auth}" — the tag that lets one mechanism serve both `InputRequired`
// and `AuthRequired` without a caller needing to guess from context which kind of "waiting" it is
// looking at. ADR-029 amendment (design → red-team → prove → judge, not an ad-hoc addition, per
// CLAUDE.md's governance for RFC-normative text): adds `approval` — a run suspended because a
// pending tool call needs real human sign-off (`AgentSession::handle()`'s suspend-for-approval
// path), the same "one mechanism, tagged by reason" shape `input`/`auth` already establish.
// ADR-057 amendment: adds `codeact_ask` — a run suspended because a script running inside
// `execute_code` called `agent.ask()` (026 §5, Design B: abort-and-replay). Deliberately NOT the
// same tag as plain `input`, even though `agent.ask` maps to `interaction_reason::input` in 026 §5's
// own table: ADR-057 §4 Finding A2 found that an ordinary `input`/`auth` interaction is
// proven-and-tested (ADR-029 finding #5) to legitimately coexist with a fresh `StartRun`, but a
// codeact ask must NOT permit that -- its replay state (the stored script source/language/answers)
// is keyed to one specific suspended round, and a second concurrent `StartRun` would race the same
// `history_`/`exec_state_` the suspended script is mid-replay against. `start_run()`'s admission
// check has a matching arm for this reason, distinct from the `input`/`auth` case it deliberately
// still allows through.
enum class interaction_reason { input, auth, approval, codeact_ask };  // ae-naming-lint: allow interaction_reason — 001 §2 names this concept normatively; 027 has not been updated to list it

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

} // namespace agentengine
