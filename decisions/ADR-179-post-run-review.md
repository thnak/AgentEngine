# ADR-179 — Post-run review: a session-owned capture, a bounded reviewer, and an approval queue

- **Status**: **Proposed — DESIGN ONLY, third draft. Two red-team rounds so far; round 1 killed the first
  draft, round 2 (six fatal/serious findings each in two independent reviews) forced this one. Nothing here is
  implemented, built, or executed.** Every claim in §5 is a claim to be proven.
- **Date**: 2026-09-21
- **Origin**: a question about self-improving agents. Hermes Agent's loop (a background pass after a task
  distils the trajectory into a reusable skill) is the reference; its documented form has no evaluation or
  approval before a lesson persists.
- **Reuses**: ADR-168, ADR-159/178, `agent.spawn` / `run_child_agent_session`, 029, ADR-173/180.
- **Touches invariants**: I1, I2, I3, I4, I5, I8.

## 1. What this is, and the honest bottom line

**It is** a way to capture a finished run faithfully, hand it to a reviewer, and put any lessons in a queue a
human or host approves. **It is not self-improvement**, and two preconditions for that name are still missing:

1. **An evaluation harness** (held-out tasks, scored on a sandbox branch; candidate **ADR-181**).
2. **Lessons that reliably change behaviour *and* cannot be weaponised.** ADR-180 §4b measured one model: an
   unfenced lesson is followed (23/23) and is an injection channel; through the safe route a *convention*
   was applied 8/8 while two *directives* were mostly ignored. The applied ones are exactly the shape an
   attacker would use (§6 T5). Not measured: other models, or whether lessons improve task outcomes.

**Recommendation from the red-teams, adopted here:** the *capture* is independently valuable (audit, replay,
debugging) and should be built first and alone; the reviewer, queue and promotion should not be built until
the evaluation harness exists. §7 stages it.

## 2. What exists (checked in source, 2026-09-21)

| Fact | Where |
|---|---|
| `start_run()` holds `session_mutex_` for the whole run; `AsyncMutex` is not re-entrant, has no owner check on `lock()`, but exposes `is_held_by_current_thread()` (unused) | `agent_session.hpp:932`; `async_mutex.hpp:209` |
| `history()` returns `const&`, unlocked. `redact()` (`:1394`) and `fork_from` on `*this` (`:1355`) mutate `history_` **without** the lock | `:845`, `:1394`, `:1374` |
| A `history()` read after `start_run()` races an overlapping run — a **named, unfixed residual** | `:3358-3368` |
| `run_rounds()` has ~20 `co_return`s; resume paths (`resolve_interaction`, `resolve_codeact_ask`, `resolve_hook_decision`) add ~10 more that never pass through it; three end a run with **no terminal event** (`:2103`, `:2199`, `:2233`) | `:2459-3036`, `:1217-2428` |
| Non-ending exits (admission, `unknown_id`, `stale`, `nothing_pending`, validation) exist in the same functions | `:1067-1112`, `:2085`, `:2337-2342` |
| A suspended run can suspend **repeatedly** (chained ask `:2222`, hook→approval `:2428`); `resolve_hook_decision` erases its pending state **before** validating the answers (`:2346` vs `:2359`), stranding the run | as cited |
| `AgentResponse.usage` is the **last call's**; `run_usage_` is cumulative; `discarded_tokens_estimate_` (ADR-177) is separate | `:2741`, `:2686`, `:2654` |
| Every tool result is `tainted=true`; an error result is `tainted=false` while embedding the message | `tool_pipeline.hpp:244`, `:214` |
| Retrieved memory / RAG never enters `history_`; providers have **no** taint-reporting interface (grep: 0 hits) | `agent_session_trust.hpp:58` |
| `list_memory_items` parses **every** blob under the mount and fails the call on the first non-`MemoryItem` blob. **This is a known, accepted design constraint, not a bug:** `memory_notes_materializer.hpp:12-25` records that a foreign blob in the memory ref broke retrieval (`json.malformed_number`, observed) and the project's answer was a **dedicated separate ref** for foreign data, "impossible by construction". A provenance store must therefore follow the same rule (§3.4) | `memory.hpp:388-403`; `memory_provider.hpp:198` |
| `write_memory_item` is read-modify-write: it predicts `write_seq = last_seq+1`, then `mount_write` reads the ref's tree, adds the blob and appends a `RefMoved`. `commit_ref` is a plain append (no compare-and-swap), so **two concurrent writers to one ref lose an update**: each builds a tree holding only its own item and the later append wins. The residual comment says "no other writer", but memory is **per principal** and a principal can have **many sessions**. `InMemoryWorktreeObjectStore` also has no mutex | `memory.hpp:296-305`; `worktree_ref_store.hpp:64-73`; `worktree_types.hpp:127` |
| The model client resolves its API key with `cap::Secret` at the point of use and fails `secret.not_granted` otherwise; the client has no `NetOut` check | `openai/chat_client.hpp:1145`; `secret.hpp:255` |
| `ChildSpawnRequest` has `token_budget`, `max_turns`; **no deadline**, no event tap; driven by blocking `block_on` | `agent_spawn_child_run.hpp:63-111` |
| `MemoryProvider::on_turn_end` already writes an **ungated** episodic `model_inferred` item from the turn (tool results included) every turn | `memory_provider.hpp:338-355` |

## 3. Decision

### 3.1 Capture: a session-owned mailbox, filled under the lock, drained through the lock

The first two drafts put a *callback* under `session_mutex_`. Round 2 showed that is the wrong shape: a sink
that throws loses the run's finished response, one that re-enters hangs forever, one that blocks stalls every
queued run, and "must not call back" is only a comment. Replace it:

- The session builds a `RunCapture` **under the lock** at a run's end and stores it in a **bounded per-session
  ring keyed by run id** (data only; no user code runs under the lock). A host reads it with
  `co_await session.take_capture(run_id)`, which takes `session_mutex_` normally — race-free by the same I1
  the run uses. An optional `set_run_capture_callback` may be layered on top, invoked **outside** the lock.
- **Single choke point.** `run_rounds()` becomes a thin wrapper over `run_rounds_impl()`; each resume path is
  split into a *validate* part (never captures) and a `*_locked_body` (captures on every return). The wrapper
  classifies the result — `finished`, `failed(code)`, `canceled` (code `run.canceled`), `suspended`,
  `abandoned` — and wraps the copy in `try/catch`, counting a failure rather than propagating it. A test/lint
  asserts no bare `co_return` remains in an impl body.
- **Re-entry fails loudly, not silently:** `start_run`, `resolve_interaction` and `snapshot_record` return
  `session.reentrant_call` when `is_held_by_current_thread()` is true.
- **Start index** is a member (`run_first_index_`) set in `start_run`, surviving `resolve_interaction()` calls,
  invalidated by `fork_from`, `clear_in_process_state` and `restore_from_record` (which today restores neither
  `history_` nor `run_id`).
- **Suspension:** each pause stores a `suspended` capture for the range since the last one; the run's end
  stores one terminal capture over the whole range. The driver dedupes by run id. A run never resumed is
  `abandoned` when the session is cleared, and otherwise never captured — stated, not hidden.
- **Content:** deep-copied messages (per-item `tainted` and `origin` preserved), `run_usage_` (cumulative) plus
  `discarded_tokens_estimate_`, the outcome and code, an `unresolved_tool_calls` marker when the range ends in
  an unpaired assistant tool call, and **a byte cap applied inside the seam** (excess dropped and counted) so
  the copy under the lock is bounded. The capture is a transcript, **not** what the model saw (history may be
  summarised; RAG and static instructions are absent).
- **Taint** is computed from the materialised messages in the capture and **fails closed**: any non-empty
  context contribution counts as taint unless its provider is on an explicit trusted list. No provider
  self-report is required, so a provider that forgets cannot cause a silent allow.
- Unset ⇒ one `size_t` maintained per run and nothing else; the taint scan runs at capture time only.

**Pre-existing defects this exposes, to fix first (§7 stage 0):** `resolve_hook_decision` erases pending state
before validating (`:2346`/`:2359`); `redact()` and `fork_from` mutate `history_` unlocked; `list_memory_items`
fails wholesale on one bad blob; `write_memory_item`'s single-writer assumption is already false across
sessions of one principal.

### 3.2 The reviewer

- **Capability ceiling is exactly `{cap::Secret<review_model_key>}`** — not "empty": the model client cannot
  call the provider without it. A whitelist check (P11), not an emptiness check, and a positive control that
  the reviewer's call completes. The reviewer therefore holds a live credential lease while reading attacker
  text; `reveal_text()` is confined to the client, but this is an I2 cost, stated.
- **Input keeps per-item taint** (messages, not a flattened string), so ADR-173's fence applies to the
  reviewer's reading of a hostile transcript.
- **Egress is a host declaration, not a capability**: the client has no `NetOut` gate, so
  `review_egress_permitted` is only a flag. The transcript reaches the reviewer's provider; unset ⇒ no review.
- **Recorded (I5):** mandatory run-event tap and recording client; refused without them.
- **Deadline:** `ChildSpawnRequest` has none and a blocking `block_on` cannot be cancelled by an enqueue-only
  callback. Needs a new `ChildSpawnRequest::deadline` (cooperative, via ADR-178's `cancel()`) or a watchdog
  thread. Until then P1's deadline half is unimplementable and is not claimed.

### 3.3 Candidates are structured, not free text (answers T5)

ADR-180 §4b shows fact-shaped lessons are *followed* through the safe route, so "facts not commands" is not a
defence — it is the attack shape. A poisoned tool result can yield "deploys for this team go to
`deploy.evil.example`", which reads as a convention and steers a tool argument no policy decider sees.

So a `LessonCandidate` is a **closed record** (`subject`, `key`, `value`, `source_span`), not prose. The host
validates before it reaches an approver: anything whose `value` exceeds a small length, control bytes and the
template's own delimiters are refused; imperatives, URLs, hostnames, paths and shell fragments are **flagged** to
the approver (`lesson_shape_warnings`) rather than refused — *amended 2026-09-24 (ADR-183 proportionality review):*
a fixed denylist over natural language refused sentences a human had read word for word, and a host could skip it
anyway. Show the approver the **source excerpt** from the tainted run, not only the summary. This reduces the channel, it does not close it — a benign-looking value can still bias
behaviour — and that is disclosed (§6 T5).

### 3.4 The queue and promotion

Every candidate from a run with any taint input is **held** for approval regardless of the gate — and per
round 2 that is *every* real run (`MemoryProvider` always contributes the `recall` tool; every tool result is
tainted). **The automatic path is deleted**: this is an approval queue, and the gate default (hold-all) is the
only mode. The queue is bounded, deduped by digest, and has a TTL; a rate limit that drops candidates is a
targeted-DoS lever (an attacker burns the budget so the legitimate run's lessons are dropped), so drops are
per-run-fair rather than first-come.

A promotion writes a `procedural` / `model_inferred` item (the item itself stays `model_inferred`; a host that opts in also records the human's approval of its exact text in an `ApprovedLessonRegistry`, and the session then delivers it as an approved-lesson block — ADR-183). **Provenance goes to a separate ref**, never the
memory mount (a non-`MemoryItem` blob there fails retrieval for that principal, and would be parsed as, or
injected as, a lesson). Records are keyed by `digest` + a **monotonic sequence**, `run_id` segments are
allow-listed (a re-created session reuses `:run:1`, and a `/` in a session id is a path), and the write goes
through the same `FsWrite` grant.

**Writers.** Memory is per principal; sessions are many; `session_mutex_` is per session, so "between runs on
the session's own executor" does not protect against another session's `on_turn_end`. The fix is a
**per-ref write lock taken inside `write_memory_item`** (also fixing the existing residual for all writers).
Until it exists, promotion is refused for any principal with more than one live session.

`MemoryProvider::on_turn_end`'s ungated write is the same risk and is **out of this ADR's scope but must be
gated or tagged the same way** (§7 stage 0), or the queue is audit-only.

### 3.5 Bounds (I8)

Byte cap inside the capture; `max_view_bytes` and a pre-send token estimate at review; reviewer
`token_budget`; `max_candidates`; `max_candidate_bytes`; the deadline (§3.2); a per-principal review rate limit
(concurrency-safe); a per-window promotion cap; queue size and TTL. The token estimate is advisory (tokenisers
differ, and `token_budget_` is checked after the response); the deadline is the real bound.

## 4. Competing designs (steelmanned)

- **Outer driver copying `history()` after `start_run()`** (draft 1). Racy; the source documents the race.
- **Callback under the lock** (draft 2). Throws, re-entry, and blocking all break the session; only a comment
  prevents them. Replaced by the mailbox in §3.1.
- **`AgentResponse` carries the capture.** Race-free for `finished` (built under the lock, returned by value)
  but failed/canceled/suspended are errors and cannot carry it. Incomplete alone.
- **Derive from `run_event_tap`.** Terminal events are emitted under the lock, but carry no messages and three
  ends emit none. Reduces to the same seam.
- **Extend `MemoryProvider::on_turn_end`.** Per turn, inline, ungated. Kept; not a substitute.

## 5. Falsifiable claims (each needs a control and a planted mutant)

**Capture (the part worth building first):**

| # | Claim | Mutant |
|---|---|---|
| C1 | Exactly one terminal capture per run and one `suspended` capture per pause, at **every** exit — a table test, one row per exit in §2 (~25) | An exit missing; a bare `co_return` |
| C2 | Validation/admission rejections produce **no** capture | A rejection captures |
| C3 | With two runs queued on one session, A's capture contains none of B's messages, **and the mutant's capture does contain B's** (deterministic barrier: a stub that parks A's continuation after the guard drops). Runs under a real TSAN build (Linux/WSL — MSVC ASAN does not detect races) | Capture taken after unlock |
| C4 | A sink/callback that throws leaves the run's `AgentResponse` and usage unchanged | Exception propagates |
| C5 | A re-entrant call returns `session.reentrant_call` and does not hang | No owner check |
| C6 | Outcome classification: `canceled` iff `run.canceled`; `failed` carries the code; an exception unwinds to `failed` | Misclassified |
| C7 | The capture range is exact across suspend/resume, `fork_from`, `clear`, `restore_from_record`; `run_id` is never empty | Stale start index |
| C8 | `redact()` concurrent with a capture is race-free | Unlocked `redact` |
| C9 | The capture's byte cap is enforced inside the seam and counted | Cap after copy |
| C10 | A range ending in an unpaired tool call is marked `unresolved_tool_calls` | Marker dropped |
| C11 | A provider absent from the trusted list makes taint non-zero (fails closed) | Provider self-report required |

**Review and queue (build only after the evaluation harness):**

| # | Claim | Mutant |
|---|---|---|
| R1 | A reviewer that errors or throws does not change the run's result | Error propagates |
| R2 | The reviewer's ceiling is exactly `{Secret<review_key>}` **and** its call completes | Emptiness check; no positive control |
| R3 | A reviewer without an event tap / `review_egress_permitted` is refused | Any check removed |
| R4 | Per-item taint survives into the reviewer's request | Input flattened |
| R5 | A candidate failing the closed-schema validator (URL, hostname, imperative, over-long) never reaches the queue; hostile facts are in the test corpus | Validator removed |
| R6 | Nothing is written without an explicit approval; unset gate ⇒ nothing written | Promoter skips approval |
| R7 | A promoted lesson stating a **destination or parameter** changes the tool arguments in a way the approval/policy layer evaluates **on the argument**, not only the tool name (this, not "approve all tool calls", is the real steering risk) | Argument path invisible to policy |
| R8 | A poisoned/foreign blob in the memory mount cannot fail `on_context`; provenance lives in a separate ref | Provenance in the mount |
| R9 | Two sessions of one principal promoting/`on_turn_end`-writing concurrently never yield a duplicate `write_seq` | No per-ref lock |
| R10 | Re-creating a session with the same id does not overwrite a provenance record; a `/` in an id cannot escape its path | No monotonic suffix / allow-list |
| R11 | Each bound in §3.5 is enforced **individually** (separate tests, not one bundle) and counted | Any one removed |
| R12 | The queue is bounded, deduped, expires; an attacker filling it cannot cause the legitimate run's lessons to be dropped first-come | First-come drop |

**Not claimed by any test:** that a lesson improves anything (§1).

## 6. Red-team dispositions

**Round 1** (first draft): capture race — redesigned; `touched_untrusted` wrong both ways — redesigned;
safe-route/no-op premise — accepted and reframed; second writer, suspension, unbounded input, egress and
authority, digest-dedupe overwriting provenance — addressed in §3; the rest acknowledged.

**Round 2** (second draft), capture seam (S) and premise (T):

| # | Sev | Finding | Disposition |
|---|---|---|---|
| S1 | fatal | "every exit" is ~30 sites, three end with no terminal event, some exits are not run ends, exceptions and destroyed frames uncovered | **§3.1** single wrapper, validate/body split, `abandoned`, lint; C1, C2, C6 |
| S2 | fatal | "captured once" contradicts repeated suspension; start index has no home; zombie runs; restore loses run_id | **§3.1** per-pause + terminal capture, member index, invalidation; **stage 0** fix the erase-before-validate bug; C7 |
| S3 | serious | Callback under the lock: throw, re-entry, blocking; enforced only by comment | **Replaced** by the mailbox; C4, C5 |
| S4 | serious | Race-free only against `start_run`/`resolve_interaction`; `redact`/`fork_from` unlocked | Stage 0; C8 |
| S5 | serious | Usage semantics, dangling tool calls, capture ≠ model context | §3.1; C10 |
| S6 | serious | "Zero cost" not literal; unbounded copy under the lock | §3.1 byte cap inside the seam; C9 |
| S7 | serious | P2/P9/P13 cannot fail as written (needs a deterministic barrier; MSVC ASAN misses races) | C3 rewritten; C1 split; TSAN on Linux |
| S8 | — | The mailbox dominates the callback | **Adopted** |
| T1 | fatal | Provenance blob in the memory mount breaks retrieval for that principal (**verified**, `memory.hpp:388`) | §3.4 separate ref; R8. **Not** a standalone bug — an accepted design constraint the project already handles the same way (§2) |
| T2 | fatal | Reviewer with an empty ceiling cannot call the model (needs `cap::Secret`) | §3.2 exact ceiling `{Secret<key>}`; R2 |
| T3 | fatal | Taint rule fails open; no reporting interface exists | §3.1 fail-closed; C11 |
| T4 | serious | Auto-promotion population ≈ empty | §3.4 **deleted** (*ADR-184 reintroduces an automatic approval as an explicit host opt-in for full-automation hosts, never a default; see its §5*) |
| T5 | serious | The approval queue is an attack surface; fact-shaped lessons are followed; no validator; `on_turn_end` ungated | §3.3 closed schema + validator + source excerpt; R5, R7; **partly open** (a benign-looking value can still bias) |
| T6 | serious | Per-session lock doesn't protect a per-principal ref; "between runs" not computable | §3.4 per-ref write lock; R9 |
| T7 | serious | `run_id` collision on session re-creation; path injection | §3.4; R10 |
| T8 | serious | The sink cannot enforce a deadline; the child has none | §3.2; deadline claim withdrawn until a field exists |
| T9 | serious | Falsifiability gaps (P3, P13, bundled P10; no claim for flooding, accumulation, growth) | §5 rewritten; R11, R12; **lesson accumulation/contradiction still has no claim** |
| T10 | serious | P8 tests deciders, not argument steering | R7 |
| T11 | minor | Over-claiming: the date lesson quoted its own answer | **Fixed and re-run** (ADR-180 §4b: result survived, confounds restated); "facts not commands" rule **withdrawn** |

## 7. Staging (adopted from round 2)

0. **Fix pre-existing defects first**, each with its own test. Status as of this revision:
   - `resolve_hook_decision` erased pending state before validating the answers, stranding the run —
     **FIXED**, test `H4d`/`H4e` in `test_rt_agent_session_tool_call_hook.cpp` (red before, green after).
   - Concurrent writers to one memory ref lose updates — **FIXED**: a per-ref mutex inside
     `write_memory_item` (`memory_detail::ref_write_mutex`). Test `test_memory_write_concurrency` (6 threads
     x 40 items): **red before** — only 54-56 of 240 items survived, `write_seq` duplicated and non-
     consecutive, and one run died outright (the in-memory object store has no mutex); **green after** — 240
     of 240, 10 runs out of 10. **Scope, not over-read:** serializes writers within one process going through
     `write_memory_item`; not another process, not a direct `mount_write` caller, and not
     `InMemoryWorktreeObjectStore` across *different* refs.
   - Unlocked `redact()` — **needs a decision**, not a mechanical fix: it is synchronous, so locking it means
     either an async signature (an API change) or a `session.busy` failure while a run is in flight.
   - `fork_from` mutating `*this` unlocked — **left**: it targets a fresh session, so this is a misuse hazard,
     not a live race; documented rather than changed.
   - `MemoryProvider::on_turn_end`'s ungated write — **needs a design decision** (gate vs tag), out of a
     mechanical pass.
   - ~~`list_memory_items` fails wholesale on a bad blob~~ — **retracted as a bug** (see §2): the accepted
     design is a separate ref for foreign data; ADR-179's provenance store already follows it.
1. **Capture mailbox** (§3.1, C1–C11). Independently useful for audit and replay; no review, no promotion.
2. **ADR-181, the evaluation harness**, plus measurement across models. Without it, do not call this
   self-improvement.
3. **Reviewer, closed-schema validator, approval queue, provenance ref** (§3.2–3.5, R1–R12).

## 8. Residuals

- The third draft is **not re-red-teamed**; the largest untested claim is that the mailbox wrapper really
  reaches every exit without touching the hot path of the largest, most heavily tested file in the tree.
- **Lesson accumulation and contradiction** have no claim; 029 §7 consolidation is the only hygiene.
- The closed schema **reduces** the injection channel; it does not close it (T5).
- Abandoned (never-resumed) runs are captured only if the session is cleared.
- The reviewer holds a live model-key lease while reading attacker text (I2 cost, §3.2).
- Egress is a flag, not a capability; the client has no `NetOut` gate.
- Names needing `tools/naming_lint.py`: `RunCapture`, `take_capture`, `LessonCandidate`, `PostRunOutcome`,
  `ReviewPolicy`, `run_with_post_run_review`.
- 029 §4 and 002 §5 amendments still to write.
