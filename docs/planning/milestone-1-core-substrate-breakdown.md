# Milestone 1 — Core substrate: the walking skeleton — work breakdown and kick-off

**Status:** Work breakdown (stage 4 of [the review-signoff workflow](v1-review-signoff-workflow.md)),
written just-in-time as this milestone starts, per that doc's §4. Scoped to
[the roadmap's](v1-implementation-roadmap.md) Milestone 1 exit criterion: *"a single hard-coded
agent runs one turn against a mock `ChatClient`, on a Quark `AgentSession` actor, with
`Message`/`Content` as the wire shape and `Tainted<T>` compiling correctly (007 §9 G2's compile-fail
proof, provable even with nothing else built) — no tools, no sandbox, no real provider, in-memory
session only. This is 001 §9 G1/G2 in miniature."*

**RFCs:** 001 (Execution Model), 003 (Message and Content Model), 007 (Capability and Trust
Model — core types only: `Capability`, `Principal`, `EffectContext`, the taint mechanism). All three
are Reviewed (2026-08-05). This is explicitly narrower than any of their own full promotion gates
(001 §9 G1's 10⁴-session benchmark, 019-dependent checkpoint/replay, etc.) — those need RFCs not yet
in scope (008/009 for real sandboxing, 019 for durability) and stay with the milestones that own
them.

## Current state (verified 2026-08-05, after M0)

| Item | State |
|---|---|
| `Message`/`Content` wire-shape vocabulary (`content.hpp`) | Exists, header-only, matches 003 §1's kind list |
| `AgentSession` | Exists as a **plain data struct** — `session_id`, `Principal`, `history`, timestamps. Not a Quark actor; no protocol, no handler, no turn loop (001 §1/§3 not yet real) |
| `EffectContext`, `Capability`/`CapabilitySet`, `Principal` | Exist as vocabulary structs (007 §2/§3's shape only — no enforcement, correctly out of scope until M2) |
| `TaintedText` | Exists as its own hand-rolled class in `content.hpp`. **`Tainted<T>`, the general mechanism 003 §2 and 007 §4 both name normatively** ("`TaintedText` is `Tainted<T>` specialized for the text/bytes case, not a separate mechanism") **does not exist** — the M1 exit criterion names `Tainted<T>` by that literal name |
| `ChatClient` concept, `ChatRequest`/`ChatResponse` | Exist (`chat_client.hpp`), concept-shaped per 004 §1, synchronous (`ae::task<T>` not wired in yet — matches this milestone's own scope) |
| `AgentResponse`/`AgentResponseUpdate` | Named in 027 §2 as the canonical run-result type; **not yet defined anywhere in code** |
| 007 §9 G2 compile-fail proof harness | Does not exist — no compile-fail test infrastructure exists in this repo at all yet |
| A real Quark actor anywhere in AgentEngine code | **None** — `grep -rl "quark::" --include=*.hpp --include=*.cpp` outside `third_party/` returns nothing. This milestone is the first to actually touch Quark's actor API |

So M1 is a real implementation milestone, not a continuation of M0's scaffolding-audit work: the
vocabulary types mostly exist, but none of 001's actual behavior (a turn loop running as a Quark
actor) or 007's taint mechanism (as `Tainted<T>`, not just `TaintedText`) does yet.

## Design decisions made while breaking this down

Two things the RFCs name descriptively rather than by exact final identifier, resolved here rather
than guessed at implementation time and left undocumented:

1. **001 §1's table says "An `Ask<StartRun, RunResponse>` to the session actor."** `StartRun` is
   taken as the literal message type name (001 names nothing else it could be). `RunResponse` is
   *not* taken literally — 027 §2 already canonicalizes the actual reply-shape name as
   `AgentResponse` ("A run's result / streamed increment"), and 027 is normative for identifiers
   where 001 is describing behavior. Treated as one concept, one name: 001's "RunResponse" is prose,
   `AgentResponse` is the type. Neither `StartRun` nor `AgentResponse` (nor `Tainted`) has a 027 §2-4
   row yet — same pre-existing gap category the naming-lint's M0 backlog already tracks, added to
   with an inline suppression tagged to the RFC that actually names each one, not "M0 scaffolding."
2. **`AgentSession` becomes the actor itself**, not a separate `AgentSessionActor` wrapper type —
   001 §1's own table says "One Quark actor instance, key = `session_id`" for `AgentSession`
   directly, and 027 gives no second name for a wrapper. It becomes a template over its `ChatClient`
   backend (`AgentSession<ChatClientT>`), following this project's established CRTP-policy idiom
   (CONVENTIONS' own `Sandbox<Strict>`/`MaxTurns<12>` examples) rather than type-erasing a seam this
   early — 004's real `ChatClient` seam (and any type-erasure decision for it) isn't due until M5.

## Tasks

1. **`Tainted<T>`** (`include/agentengine/core/tainted.hpp`) — the general mechanism 003 §2/007 §4
   name: an explicit-construction wrapper with an `unsafe_view()` accessor and *no* implicit
   conversion to `T`/`T const&`/`std::string_view`. `content.hpp`'s `TaintedText` becomes
   `using TaintedText = Tainted<std::string>;`, consolidating the two names the RFCs already say are
   one mechanism. **S.**
2. **007 §9 G2 compile-fail proof** — no compile-fail test infrastructure exists in this repo yet, so
   this is new machinery, not just a new test: a `try_compile()`-based configure-time gate (CMake's
   standard idiom for this — a `FATAL_ERROR` if a snippet that must not compile does, or one that
   must compile doesn't) proving (a) `Tainted<T>` has no implicit conversion to the untainted
   accessor surface, and (b) the explicit `unsafe_view()` declassification path does compile — the
   positive control a fail-only suite can't do without. **M** — the mechanism is new, the specific
   proof is small.
3. **`AgentSession<ChatClientT>` as a real Quark actor** — turn the existing plain struct into
   `class AgentSession : public quark::Actor<AgentSession<ChatClientT>, quark::Sequential>`, protocol
   `Protocol<Ask<StartRun, AgentResponse>>`, and a `handle()` implementing 001 §3's turn loop in
   miniature (append input, call the templated `ChatClientT`, append the response, respond) — no
   tool calls, no policy resolution, no sandbox (001 §3 steps 3a-3c don't apply with nothing to call).
   **L** — first real contact with Quark's actor/dispatch API in this codebase; `quark::TestKit<A>`
   (`third_party/quark/include/quark/core/testkit.hpp`) is the right harness (single-actor,
   deterministic, no cluster/engine bring-up), not a full `Engine`.
4. **Hard-coded mock `ChatClient` + end-to-end proof** — a fixed, canned-response mock (test-local,
   not core — mirrors `tests/test_recorded_chat_client.cpp`'s existing precedent of keeping fixture
   clients in `tests/`) satisfying the `ChatClient` concept, and a test driving one full turn through
   `TestKit<AgentSession<MockChatClient>>::ask<AgentResponse>(StartRun{...})`, asserting the session
   history grew by the user turn + the assistant reply and the response content matches. This is
   001 §9 G1/G2 "in miniature," per the roadmap's own framing — not the real G1/G2 (10⁴ sessions,
   checkpoint/restart), which need 019 and stay with M4. **M.**

## Handover & kick-off

Milestone 1 starts 2026-08-05, immediately following M0. No deviation from the roadmap's assumed
order.
