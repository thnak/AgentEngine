# ADR-029 — Suspending a run mid-tool-loop for real human approval

**Status:** Judged, accepted (2026-08-13). Designed, red-teamed, implemented, and proven (real code
+ deterministic tests, §5); accepted by the project owner per this project's governance
(`decisions/README.md`; `OpenQuestions.md` OQ-11's resolution that the project owner is the ADR
judge).

**Relates to:** `decisions/ADR-027-agent-session-tool-call-loop.md` (this ADR closes its second named
out-of-scope residual — session-scoped stateful tools was ADR-028's; "Approval is synchronous-decider
only... suspending the run to wait for a real human answer... is out of scope this pass" is the exact
sentence this ADR revisits), `include/agentengine/core/interaction.hpp` and `open_interaction()`/
`resolve_interaction()` (`agent_session.hpp`) — the "existing but unwired" primitives 001 §2 already
specified and Milestone 4 Phase E1 already built standalone, this ADR is the first real caller of.

## 1. The question

ADR-027's tool-call loop (`AgentSession::run_rounds()`, née `handle()`) evaluates every approval-needing
tool call synchronously, through a caller-supplied `ApprovalDecider` (`std::function<bool(...)>`).
With no decider configured, every such call is denied every round — a session with an
`Approval<always_require>` tool and no decider cannot converge at all; it always fails
`run.max_turns_exceeded`. A REAL human approval workflow (a UI showing the pending call and waiting
for a click) cannot be expressed this way: a `std::function` cannot suspend and resume later, and
`AgentSession::handle()`'s own turn loop, before this ADR, made exactly one uninterruptible pass from
`StartRun` to either a converged response or a `run_failed` event — with no seam for a real,
multi-second (or multi-hour) wait in between.

**Stated so it has a wrong answer:** can a round genuinely suspend mid-loop — leaving `history_` and
`open_interactions_` in a coherent, resumable state, the `StartRun` ask correctly unresolved (never a
hang) — and later resume from a SEPARATE ask, continuing the SAME run/turn, without either widening
`StartRun` past Quark's fixed message-pool cap or duplicating the entire round-loop body a second
time for the resume path?

## 2. Prior art surveyed

`interaction.hpp`'s own header comment and `open_interaction()`/`resolve_interaction()`
(`agent_session.hpp`) already implement 001 §2's `Interaction{interaction_id, run_id, reason,
opened_at, expires_at?}` vocabulary and lifecycle — described as "Host-callable, like
fork/redact/delete — NOT wired into the synchronous turn loop" before this ADR. `run_event.hpp`
already declares `input_required`/`input_resolved`/`approval_requested`/`approval_resolved` kinds
with zero real producers. This ADR's job is wiring, not inventing new vocabulary — matching this
project's own established pattern (ADR-014's `open_within_mount_root` reused Quark's existing handle
machinery rather than inventing a new resolution primitive; ADR-028 reused `HistoryProviderT`'s
existing per-instance-member shape rather than widening `ContextProvider`).

## 3. The design considered and rejected

**First draft:** extend `StartRun` itself with an additive `std::optional<std::string>
resolving_interaction_id` field, reusing the one existing entry-point message rather than adding a
second one.

**Rejected by red-team, on hard evidence, not preference.** The red-team measured (a real compiled
probe against this project's own toolchain, not an estimate) that adding this field pushes
`sizeof(quark::Ask<StartRun, AgentResponse>)` from 160 to 200 bytes — over Quark's hard 192-byte
`quark::detail::MessagePool::kMaxPayload` `static_assert` ceiling (verified directly:
`third_party/quark/include/quark/core/actor_ref.hpp:264-265`, `testkit.hpp:207`). `StartRun` was
ALREADY measured close to this limit specifically because `SessionCaller` was deliberately kept
narrower than a full `Principal` for exactly this reason (`StartRun`'s own comment) — this design
would not compile, full stop, not merely "run over budget."

## 4. The accepted design

A **separate** message, `ResolveInteraction{std::string interaction_id; bool approved; std::optional
<SessionCaller> caller;}`, added to `AgentSession::protocol` alongside (not instead of) `StartRun`.
Independently re-measured via a real compiled probe (`agentengine::AgentResponse` reply, same as
`StartRun`): `sizeof(quark::Ask<ResolveInteraction, AgentResponse>) == 136` bytes — safely under the
192-byte cap, with room to spare.

The round loop itself (`handle(Ask<StartRun,...>)`'s former inline body) is factored into a shared,
private `template <class AskT> quark::task<std::monostate> run_rounds(AskT const& m)` — genuinely
awaitable per ADR-047 (`quark::task<T>` for `T != void`; a bare `quark::task<>` is the
executor-only, `detach()`-only top-level handler-frame type and has no `await_resume()` at all — this
codebase's own `context_provider.hpp` already documents this exact rule for `on_turn_end`). Both
`handle(Ask<StartRun,...>)` (a fresh run) and the new `handle(Ask<ResolveInteraction,...>)` (a
resumed continuation) `co_await run_rounds(m)` — one control-flow body, not two drifting copies,
matching this project's own "consolidate, don't duplicate" precedent from ADR-027's own
`tool_call_extraction.hpp`.

`run_rounds()` gains one new opt-in branch, inserted after a round's model call returns tool calls
and before any of them is invoked: if `suspend_for_approval_` (new, default `false`) is set and no
synchronous `approval_decider_` is configured, and ANY pending call in the round needs approval
(`tool_call_requires_approval()`, the exact same step-5 predicate `invoke_tool()` itself calls — a
new public re-export from `tool_pipeline.hpp` so the two can never drift apart), the WHOLE round
suspends atomically: a real `Interaction{run_id, interaction_reason::approval}` opens,
`input_required`/`approval_requested` events fire, and the coroutine returns without ever calling
`m.respond()` — the same "never fabricate a response, never hang" shape every other fail-closed
branch in this function already uses. With either condition false, behavior is byte-for-byte what
ADR-027 shipped.

`handle(Ask<ResolveInteraction,...>)` resumes it: **validates before resolving** (the interaction id
must be open, AND `history_`'s tail must still be exactly the suspended assistant tool-call message —
the shape `run_rounds()`'s suspend branch always leaves behind), only THEN calls
`resolve_interaction()`, continues the SAME `run_id`/`turn_index` (never resets the run counter — an
approval resume is a continuation, not a new run, for I4 attributability), and either folds a denial
`ToolResult` per pending call (new `make_denial_result()`, `tool_pipeline.hpp`) or invokes every
pending call for real through the ordinary `invoke_tool()` pipeline (capability/taint/idempotency
checks unchanged — I3: a human's decision drives this, but nothing bypasses the rest of the pipeline)
via a one-shot decider that approves unconditionally, safe specifically because validation already
confirmed the calls are the exact, unchanged set a human was shown. Either way it then
`co_await run_rounds(m)` again for any further rounds.

### Fixed during red-team (findings #1-#6, all closed before this design was accepted)

| # | Finding | Fix |
|---|---|---|
| 1 (fatal) | Extending `StartRun` overflows Quark's 192-byte message cap (measured 200 bytes). | Separate `ResolveInteraction` message, measured 136 bytes. |
| 2 | `run_rounds()` returning bare `quark::task<>` cannot be `co_await`ed by a sibling handler (ADR-047: only `task<T>`, `T != void`, is a genuinely awaitable nested coroutine). | Returns `quark::task<std::monostate>`, this codebase's own established idiom (`context_provider.hpp`'s `on_turn_end`). |
| 3 | Should a resume mint a fresh run, or continue the suspended one? | Continues the SAME `run_id`/`turn_index` — a fresh run would misattribute the resumed tool calls (I4) and let a resume dodge the per-run token budget (I8) by restarting the accumulator. |
| 4 | Resolving the `Interaction` before validating `history_`'s tail risks resolving against a round that isn't the one a human actually reviewed (a second resolver, or unrelated state mutation, in between). | Validate — unknown id, empty history, non-assistant tail, or no pending tool call — BEFORE calling `resolve_interaction()`; any failure leaves `open_interactions_`/`history_` completely untouched. |
| 5 | A second, ordinary `StartRun` sent while a round is suspended would start a concurrent second run over the same `history_`/`open_interactions_` this `quark::Sequential` actor was never designed to interleave. | `handle(Ask<StartRun,...>)` now rejects (fails closed, mints no run_id) whenever an open `Interaction` with `reason == interaction_reason::approval` exists — narrowed to that reason specifically, not `has_open_interactions()` in general: `test_agent_session_suspend_resume.cpp`/`test_suspended_zero_resources_e2e.cpp` already proved (pre-ADR-029) that an open `input`/`auth` Interaction legitimately coexists with an ordinary fresh `StartRun` (a host's own passivate/reactivate bookkeeping); the first cut of this fix used `has_open_interactions()` and broke both of those pre-existing tests — caught by running the FULL suite, not just this ADR's own new test, before considering the fix done. |
| 6 | An unrelated principal could resolve another principal's pending approval by guessing (or being told) an `interaction_id`, since `interaction_id` alone carries no ownership check. | `ResolveInteraction::caller`, checked via the exact same `principal_admitted_for` shape `StartRun::caller` already uses. |

## 5. Falsifiable claims and verdicts

All six proven in `tests/test_agent_session_suspend_approval.cpp`, a deterministic, offline suite (no
live model) matching ADR-027/028's own proof style.

| # | Claim | Evidence | Verdict |
|---|---|---|---|
| SU1 | An approval-needing call with `suspend_for_approval_` on and no decider suspends the round instead of denying it; no call is invoked before suspension. | SU1: the `StartRun` ask never resolves; `gated_tool`'s `invoke()` never fires; a real `Interaction{reason=approval}` opens naming the exact suspended `run_id`; `input_required`/`approval_requested` events fire. | **CORRECT** |
| SU2 | A fresh `StartRun` while suspended is rejected, not run as a second concurrent run. | SU2: the second ask never resolves; `last_run_id()` is unchanged (no new run_id minted); still exactly one open interaction. | **CORRECT** |
| SU3 | `ResolveInteraction{approved=true}` resumes the SAME run and invokes the pending call for real. | SU3: the ask resolves with the scripted post-resume text; `gated_tool`'s `invoke()` DID fire; the interaction closed; `last_run_id()` is unchanged from before the resume (continuation, not a fresh run). | **CORRECT** |
| SU4 | `ResolveInteraction{approved=false}` feeds back a denial without ever invoking the gated tool. | SU4: the ask still resolves (denial is an ordinary tool error, not a run failure); `gated_tool`'s `invoke()` never fired; the interaction closed. | **CORRECT** |
| SU5 | An unknown `interaction_id` fails closed and mutates nothing (finding #4's ordering fix). | SU5: the ask never resolves; `gated_tool`'s `invoke()` never fired; the REAL open interaction is still there, untouched, `size() == 1`. | **CORRECT** |
| SU6 | A `ResolveInteraction::caller` that doesn't own the session is denied at admission before the interaction lookup even runs (finding #6). | SU6: the ask never resolves; `gated_tool`'s `invoke()` never fired; `admission_denied_count()` incremented by exactly one; the real interaction stays open (a denied resolver must not consume it). | **CORRECT** |

## 6. What this ADR does not claim

- **Expiry is still unwired** (as originally written by this ADR; see the amendment immediately
  below for what changed). `Interaction::expires_at_ns` exists (001 §2's own optional field) but
  nothing in this ADR checks it — a suspended run with no human ever answering stays suspended
  forever (or until a host-level timeout/kill, outside this actor). Named in `interaction.hpp`
  already; unchanged by this ADR.

**Amendment (decisions/ADR-070-host-configurable-responsibility-boundary.md):** the gap above is
closed, as a Delegated Decision Seam — a host-driven QUERY, not an engine-internal timer.
`interaction.hpp`'s own comment is why: "no real wall-clock source wired in anywhere in this project
yet (Clock is not a wired capability)" — an engine-internal poll would have to invent exactly the
untracked nondeterminism I5 forbids. `AgentSession::set_interaction_expiry(interaction_id,
expires_at_ns)` lets a host that learns of a new suspension (the `input_required` event already
names the id) opt it into a timeout, in the host's own wall-clock terms;
`AgentSession::expired_interaction_ids(now_ns)` lets the host later ask, in that same host-supplied
"now," which open interactions have passed their configured `expires_at_ns`. Deciding what to do
about an expired interaction is entirely the host's job — typically the ALREADY-EXISTING
`resolve_interaction({id, approved: false})`, which is already race-free against a concurrently-
arriving real human answer via `session_mutex_` (I1) — this amendment adds no new resolution
mechanism, only the query and the setter that were missing. An interaction nobody calls
`set_interaction_expiry` for keeps `expires_at_ns == 0` ("no expiry") exactly as before this
amendment — every existing caller is unaffected. Proven: `tests/test_rt_agent_session_suspend_approval.cpp`'s
SU9.
- **`AgentSessionRecord`'s checkpoint already includes `open_interactions` (Phase D1) but the
  suspended round's OWN state — the pending assistant tool-call message in `history_` — is not
  durably checkpointed**, the same pre-existing gap ADR-027/028 both already name (`Message`/
  `ContentItem` have no `QUARK_SERIALIZE` yet). A process restart mid-suspension loses the resumable
  round even though the `Interaction` record itself would survive a real checkpoint/restore cycle.
- **A `ResolveInteraction` sent after the approved-branch's own `on_context()` call fails
  (`run.context_unavailable`) leaves the interaction already resolved but no tool-result folded into
  `history_`.** A narrow, rare failure window (matches the shape of every other "fails run closed,
  no respond()" branch in this loop) — a subsequent ordinary `StartRun` would then push a new user
  turn on top of an unresolved assistant tool-call message. Named, not specially handled; the same
  class of residual this project already accepts for `run.context_unavailable` elsewhere in
  `run_rounds()`.
- **No cross-call partial approval.** A round suspends and resumes as one atomic unit — approving or
  denying ALL of a round's pending calls together, never a subset. Matches MT-2's own proof that a
  real round can carry more than one call and this loop already treats a round as one unit (the
  single `tool_results_message` fold). A finer-grained per-call approval UI is a separate,
  not-yet-designed extension.
- **Real approval UI/transport wiring** (an actual human-facing surface that calls
  `ResolveInteraction` — AG-UI's `InputRequired` projection, an MCP progress notification, a CLI
  prompt) is not built here; this ADR proves the session-actor-side mechanism only, the same scoping
  ADR-028 used for the general stateful-tool mechanism versus real CodeAct wiring.

## 7. Files changed

- `include/agentengine/core/agent_session.hpp` — `ResolveInteraction` struct; `protocol` extended;
  `suspend_for_approval_` member + accessors; `handle(Ask<StartRun,...>)` shortened to setup +
  `has_open_interactions()` rejection + delegation; new `run_rounds<AskT>()` (the factored-out,
  awaitable round loop, with the new suspend branch); new `handle(Ask<ResolveInteraction,...>)`.
- `include/agentengine/core/tool_pipeline.hpp` — `tool_call_requires_approval()` and
  `make_denial_result()` extracted as public functions (single source of truth with `invoke_tool()`'s
  own step 5, which now calls the same predicate instead of a duplicated inline one).
- `include/agentengine/core/interaction.hpp` — `interaction_reason` gains `approval`.
- `include/agentengine/core/run_event.hpp` — `ApprovalRequested`/`ApprovalResolved` gain
  `interaction_id` (previously `call_id`-only, with zero real producers before this ADR).
- `tests/test_agent_session_suspend_approval.cpp` (new) — this ADR's §5 evidence.
- `tests/CMakeLists.txt` — registers the new test target.

Full regression suite: no new failures introduced by this ADR (verified against the pre-existing
178/179-179/180-pass baseline ADR-027/028 left — the one known failure,
`test_mediated_python_runner_hostile_corpus`, is the same pre-existing, unrelated failure, untouched
by any file this ADR changes). **Corrected 2026-08-11**: it was two real, deterministic
test-authoring bugs, not a flake; see ADR-024 §6's own corrected note.
