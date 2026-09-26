# ADR-193 — When agent A hands work to B and B to C, does the text keep its author, the effects their owner, and the chain its budget?

- **Renumbered:** written as ADR-185; renumbered to ADR-193 on 2026-09-25 when this stack merged into `main`, where those numbers had been taken by other ADRs in the meantime. Commit messages, PR titles and ADR cross-references written before then use the old number.

- **Status:** Proposed — built, tested offline (§6), red-teamed twice (§7: no fatal; 4 major, all fixed; §8, round
  2: 3 major and 6 minor, all fixed, 2026-09-25; §9, red team round 3 against the round-2 fixes, 2026-09-26: 2 major
  (issue #113) plus 2 found while fixing, all fixed; not yet re-red-teamed). **Needs the project owner's judgement** (it touches I3's untainting rule, 003 §2, for every
  handoff).
- **Date:** 2026-09-25.
- **Scope:** `core/delegation.hpp` (new: `make_delegated_message`), `trust/principal.hpp` (`delegation_root`,
  `root_id()`), `core/effect_context.hpp` (`delegated_event_sink`, `charge_delegated_usage`), `rt/agent_session.hpp`
  (event forwarding, delegated usage folded and budget-checked, pinned context, root-scope lesson lookup),
  `rt/agent_spawn.hpp` / `rt/agent_spawn_child_run.hpp` (delegated input, root-keyed quota, `child_id`, whole-run
  usage, per-target settings, shared lessons), `rt/agent_workflow_executor.hpp` (delegated input for agent nodes),
  `core/approved_lessons.hpp` (`texts(scope)`), `core/tool_pipeline.hpp` (audit names the root), tests (§6); round 3
  (§9): `rt/delegated_run_guard.hpp` (new), `rt/workflow_supervisor.hpp` (`WorkflowResult::usage`),
  `rt/workflow_as_chat_client.hpp`, `rt/workflow_as_executor.hpp`.
- **Related:** 003 §2 (taint; untainting is explicit and logged) · 007 §2 / 018 §2 (delegation via `on_behalf_of`) ·
  ADR-163 (whole-run usage for workflow nodes) · ADR-173 (the fence) · ADR-191 (approved lessons) · ADR-192
  (unattended mode; its §5 residual on spawned children) · the investigation behind this ADR (a 3-hop offline probe;
  findings reproduced in `tests/test_delegation_provenance.cpp`).

## 1. The question

**Stated so it has a wrong answer:** after A → B → C, can anything downstream tell that the text C was given was
written by a model (and possibly copied from memory or a tool result A saw fenced as untrusted), that C's effects
belong to A's chain, and has the chain stayed within A's budget and quota?

**Before this ADR: no, on every count.** Measured with a 3-hop offline probe:

- `agent.spawn` built the child's input as `role::user`, `origin=user`, `tainted=false`. A's fenced memory lesson and
  an injected "SYSTEM OVERRIDE" reached B, and via B reached C, as untainted user text. That is an implicit, unlogged
  untainting (003 §2) with no host opt-in. The permission gates still held (C's tool calls stayed
  `arguments_tainted`), so this was model steering, not an authority bypass.
- A workflow agent node received the previous node's reply unchanged, as **its own assistant turn**.
- Attribution stopped after one hop: `on_behalf_of` named only the immediate parent, children had no event tap (A saw
  none of C's tool calls), and the spawn reply did not name the child.
- Budgets: the reply reported only the child's final model call. The child's tokens never reached A's budget, and the
  per-principal spawn quota restarted at every hop (each child is a fresh principal).
- Settings were never inherited, which is correct, but a child could not be given them either except through a
  hand-written always-yes decider, with no ADR-192 audit.

## 2. The rule

**A hop never changes who wrote the text, who is accountable, or what the chain may spend.** Every handoff applies
it the same way:

1. **Delegated text keeps its author.** `make_delegated_message(source, text)` builds the next agent's task as a
   `role::user` message of two parts: a host-authored line (untainted, `content_origin::system`) naming the kind of
   handoff, the delegator and the depth, and saying a model wrote it, not a human; then the text itself,
   `content_origin::external`, `tainted = true`. It is **not fenced**: the next agent is meant to carry it out, and a
   fenced task would be inert (ADR-191's measurement). The receiving agent's authority is unchanged (its grant is
   attenuated; its calls are already `arguments_tainted`). What changes is that recordings, summarizers, memory
   capture and every taint-aware consumer see it for what it is, and the next agent is told.
   - `agent.spawn`: from the caller's principal id, depth = the child's delegation depth.
   - Workflow agent node: decided per content item (`delegate_foreign_items`), not by the message's role, because a
     fan-in merges payloads onto the first one's role. Every item the host did not author becomes tainted and
     `external` with its value kept; an assistant-role message becomes a user one; a user-role one gets the host
     line. Host-authored input passes through unchanged. *(Corrected in §8.2: a message of any role that carries
     foreign items becomes a user-role delegated message -- host items, host line, foreign items.)*
   - The delegator's name is quoted, control characters are stripped and the length is capped before it enters the
     host line. *(§8.2: cut on a character boundary; Unicode line separators, NEL and bidi controls replaced too.)*
   - *(§8.2)* The host line ends in a paragraph break and says everything after it in the message is the request.
2. **Lineage.** `Principal::delegation_root` names the chain's root and is set by `derive_on_behalf_of`, so every hop
   carries it. `ToolInvocationAudit::principal_delegation_root` records it, and `AgentSpawnReply::child_id` names the
   child.
3. **Events flow up, wrapped.** `AgentSession` gives each tool call an `EffectContext::delegated_event_sink`, and a
   spawned child's run-event tap is set to it. The parent re-emits each child event as its own `delegated_event`
   (a new kind, appended last). The payload names the child run and the depth and carries the child's event
   unchanged. Recursively, the root's host sees every hop, including C's tool calls and ADR-192 audit lines. Protocol
   projectors never mistake a child's lifecycle for the parent's: A2A ignores the kind, and AG-UI labels it
   `ae:delegated_event`.
4. **Spending flows up.**
   - **What is reported and charged.** The stock child runner reports `run_usage()`, the whole run, not the final
     call. On every outcome, success or failure, it charges that plus its discarded-stream estimates (budget only)
     through `EffectContext::charge_delegated_usage`.
   - **Where the budget is checked.** The parent folds pending delegated usage into its run before each model call and
     checks its token budget there. `run_usage()`/`run_tokens_consumed()` include what is not yet folded, so a run
     that ends without another call still reports it.
   - **Capping each child.** A child's token budget is capped at what the caller has left
     (`EffectContext::remaining_token_budget`), and a spawn with nothing left is refused.
   - **Result.** The root's budget bounds the tree (I8). *(Not true as first built: within one batch every spawn saw
     the same pre-batch remainder, and it compounded with depth. Corrected, with the exact bound, in §8.1.)*
   - **Quota.** The spawn quota is keyed on (tenant, root), so a tree shares one quota.
   - **Detached copies.** A detached copy of a tool's context (`background_task`, a workflow's detached worker) drops
     both sinks, since they point at the session. *(§8.1: the session's usage closure no longer points at the session,
     and `WorkflowChatClient`'s worker now keeps it, to charge a failed inner run.)*
5. **Settings are never inherited, only named.** A parent's fence, lesson level and unattended mode never reach its
   children. A host may give a spawn target, explicitly and per target:
   - `unattended_operator` (and a `veto`);
   - `fence_disabled_by`;
   - `approved_lessons` with a `lesson_level`;
   - `share_lessons`.

   Each setting is applied through the child session's ordinary ADR-191/192 setters, so it is audited, and the audit
   reaches the root through (3). A target naming an empty operator is refused at registration.
6. **A chain can share knowledge, explicitly.** A delegated principal's approved lessons are matched against its
   chain's root (a child's own id is a fresh hash nobody could approve for). With `share_lessons`, every lesson the
   root has approved is placed in the child's context. `AgentSession::set_pinned_context` is the new, general,
   host-only seam for that. The child session's grant re-verifies each lesson exactly as for the root, so a lesson A's
   model merely *copied* into the input stays tainted delegated text, never approved.

## 3. What this is, stated plainly

- It **does not untaint** anything and **does not fence** delegated tasks. The next agent follows a delegated task
  about as readily as before (it is still a user-role request); the change is that it is told the request came from
  a model, and every consumer sees the taint. Telling it did **not** change how readily it follows, measured live
  (§6): 20/20 and 20/20 as a delegated task, the same as the old plain form. *(§8.3: that measurement used the
  round-1 host line, which the OpenAI wire glued to the task; the round-2 wording is not re-measured.)*
- (5) and (6) are host opt-ins under ADR-070's seam (explicit, off by default, audited, host code only). Sharing
  lessons down a chain extends ADR-191's approval scope from a principal to its delegation tree, which is the owner's
  call.

## 4. What this does NOT claim (residuals)

- **Custom child runners.** Only the stock `run_child_agent_session` applies the settings, the event tap and
  whole-run usage. A host's own runner gets the fields on `ChildSpawnRequest` and must honour them.
- **Workflow nodes** are told only "an upstream agent node", not which one; the adapter is not given the edge. Their
  principals are still independent (not derived on behalf of the workflow), and their usage already flows through the
  workflow (ADR-163), not through a parent session. *(Only on success, as first built: a failing node's spend
  vanished. Fixed in §8.1.)* Only agent nodes apply the rule: a plain function node that
  relays an agent's text passes it on as it received it.
- **`multi_agent::spawn`** (host-driven) still runs a child as the parent's principal by default. The host builds its
  `StartRun`, so the input is host-authored.
- **Memory keyed on delegated text:** a child with a `MemoryProvider` queries memory with the first text of the last
  user message, which is now the host line. That is harmless, but it makes the query less useful.
- **Budget timing:** delegated usage is checked before the parent's next model call, not mid-child. Each child is
  capped at what its caller had left when it was spawned, so siblings spawned in one parallel batch can together
  overspend by up to one batch. *(Wrong as written: `agent.spawn` batches are sequential, and every sibling saw the
  same stale remainder, so a batch overspent by up to (batch size - 1) x budget and that compounded with depth. See
  §8.1 for the fix and the bound that now holds.)*
- **The quota is lifetime-of-process** (`SpawnQuotaTracker` never resets), so a runaway tree spends its root's quota
  until restart.
- **`texts(scope)`** shares `find`'s existing key format: a principal id containing `\x1f` could reach another
  scope's texts. This is a pre-existing limit of the registry key; ids are host-assigned. *(Superseded: the registry
  key is now a structured (tenant, principal, text) key, from the round-3 lesson-scope fix; the spawn quota's own
  `\x1f`-joined key is fixed in §8.2.)*
- **Event ordering** across hops is by arrival; each child's own sequence is kept inside the wrapper.
- Measured live on one model (§6).

## 5. Checked against the invariants

- **I2:** no authority added; the quota is tightened (root-keyed); settings are per-target host opt-ins.
- **I3:** closes an implicit untainting.
- **I4:** lineage, child id and upward events.
- **I8:** usage charged up the tree, and the budget checked on it.

## 6. Evidence

`tests/test_delegation_provenance.cpp` (A → agent.spawn(B) → agent.spawn(C), scripted, offline):
- **P1-P2:** B and C receive delegated tasks: a host line (from, depth, "not a human"), then the tainted, external
  text carrying A's memory and the injection.
- **P3:** root lineage at every hop.
- **P4:** B's and C's events reach A's tap, including C's tool call.
- **P5:** A's usage counts B's and C's whole runs, and A's budget stops A when the tree overspends.
- **P6:** a quota of 1 bounds the tree.
- **P7:** A's unattended/fence-off settings are not inherited.
- **P8:** a target's unattended operator runs C's call, and the audit reaches A.
- **P9:** shared lessons are approved through the root, fenced or unfenced by level; without sharing there are none.
- **W1-W2:** a workflow agent node receives an upstream reply as a delegated task; the workflow's input passes
  through unchanged.
- **Round 1:**
  - **F1:** children that fail still charge their whole spend (20004 counted, where 4 used to be).
  - **F2:** a child is capped by its caller's remaining budget.
  - **E1:** an A2A projector over A's stream completes once and AG-UI starts one run.
  - **W3-W5:** user- and system-role fan-in merges are handled per item (the agent's text is tainted, and fenced in a
    system message), and non-text content is kept.
  - **P9b:** at depth 2 a lesson is approved through the root, and another principal's are never shared.
  - **P10:** a lesson A's model copies into the input stays unapproved.
  - **R1:** a target with an empty operator is refused at registration.
- **Live** (DeepSeek `deepseek-flash`, `tests/test_memory_lesson_label_live_e2e.cpp` arms D0/D1, one interleaved run,
  20 trials per cell; followed / asked / other). The same task, handed to an agent the old way (plain user text) and
  as a delegated message: alert channel 20/0/0 vs 20/0/0, deploy region 20/0/0 vs 20/0/0 (control with no task:
  0/0/20, 0/16/4). The host line costs nothing in task-following on this model.
- Planted mutants, each failing a test:
  - **Build:** input untainted; no root on derive; usage not charged; no event sink; final-call usage only; quota on
    the caller; no root-scope lesson lookup; no workflow rewrap; no budget check on fold; unattended setting ignored.
  - **Round 1:** charge only on success; raw forwarding; rewrap by role only; no budget cap; no registration check;
    lesson fallback to the parent; `texts` ignoring scope.
  - **Survivor:** one equivalent mutant (an early return that is only a shortcut).
- Writing the fan-in fix, the test caught a real bug in its first version: host-authored input returned with its
  items moved out (empty text).

## 7. Red team

**Round 1 (one reviewer).** No fatal.

| # | Sev | Finding | Disposition |
|---|---|---|---|
| M1 | major | A failing child charged nothing upward (and neither did its descendants): the root's budget did not bound the tree -- 30004 spent, 4 charged; each child also had its own full budget | The runner charges on every outcome; a child's budget is capped at the caller's remainder; a spawn with nothing left is refused (F1, F2; mutant-checked) |
| M2 | major | Forwarded child events broke protocol projection: the A2A task went `completed` three times mid-run, AG-UI saw nested runs | Wrapped as `delegated_event`; A2A ignores it, AG-UI labels it (E1; mutant-checked) |
| M3 | major | The workflow rewrap tested the message role only; a fan-in merge delivered an upstream model's text as untainted user text, or unfenced in the system channel | Decided per item (W3, W4; mutant-checked) |
| M4 | major (latent) | Detached context copies kept the session-capturing sinks (use-after-free for a backgrounded call) | Both sanitizers reset them |
| m | minor | Pending usage not folded at a run's end; discarded-stream estimates not charged; quota lifetime and tenant; non-text content dropped; misconfiguration found only at spawn time | Accessors include pending usage; estimates charged (budget only); quota keyed on (tenant, root), lifetime disclosed; content kept (W5); refused at registration (R1) |
| nits | nit | Unescaped delegator name in the host line; `delegation_root` is a public field; `texts` key collision; the event tap is always attached | Name quoted and sanitized; the rest disclosed (§4) |
| tests | — | No failing-child, fan-in or projector test; P9 could not tell root from parent; `texts` scope untested; the copied-lesson claim untested | F1, W3-W5, E1, P9b, P10 |

**Round 2 (one reviewer, an executed probe).** No fatal; 3 major, 6 minor. Fixes and corrected claims in §8.

| # | Sev | Finding | Disposition |
|---|---|---|---|
| M1 | major (I8) | Every spawn in one response saw the same pre-batch remaining budget (set once per batch), so neither the refusal nor the cap fired inside a batch, and it compounded with depth: budget 1000, 4 spawns of 900 -> 3602 charged; 3 x 3 two levels -> 8108 | Recomputed before every sequential call; parallel calls split the remainder (R2-B1, R2-B2, R2-B3) |
| M2 | major (I8) | A failing workflow agent node's spend vanished: the adapter returned the error without usage, the supervisor recorded zero for a failed body, and `WorkflowChatClient` failed its stream with none (node spent 1000; workflow 0; outer session 0) | Charged on the error path through the node's context, counted per attempt, charged to the calling session on a failed stream (R2-W6) |
| M3 | major (I8, ADR-178) | Cancelling the root never reached a running spawned child (8 child model calls after `A.cancel()`) | The caller's cancellation is bridged to the child; its partial spend is still charged (R2-C1); the same for workflow agent nodes (R2-C2) |
| m1 | minor | OpenAI joins a message's text parts with nothing between them, so delegated text could open with a forged host line that read as the real one's continuation | The host line ends in a paragraph break and claims everything after it (R2-H1) |
| m2 | minor | `quoted_label` cut a multibyte character at 120 bytes and passed U+2028/U+2029/U+0085 | Decoded as UTF-8, cut on a boundary, separators, NEL and bidi controls replaced (R2-L1) |
| m3 | minor | A fan-in whose first payload was a system-role failure marker kept an upstream agent's text in the system channel; with `fence_disabled_by` it would be sent as instructions | Any role -> user-role delegated message (W4, rewritten) |
| m4 | minor | The spent-budget refusal ran after the quota slot, the cost token and the worktree were taken | Refused first (R2-Q1) |
| m5 | minor | A child whose chat client threw charged nothing | Charged before rethrowing (R2-E1) |
| m6 | minor | The quota key joined tenant and root with `\x1f` and could collide | Length-prefixed (R2-Q2) |
| X5 | (major, shared) | `share_lessons` ignored the tenant | Fixed by the round-3 lesson-scope change (tenant + principal); regression R2-T1 |

## 8. Round-2 amendment (2026-09-25)

### 8.1 Budgets, failures and cancellation (M1-M3)

- **Per-call remainder.** `AgentSession::dispatch_tool_calls` now recomputes `EffectContext::remaining_token_budget`
  before every call in a sequential class. A child's charge lands in `run_tokens_consumed()` the moment it returns, so
  the next sibling sees what the earlier ones left. `agent.spawn` is sequential (it declares no `Parallelizable`), so
  this is the path every stock spawn takes; R2-B1 confirms the recompute is what bounds it.
- **Parallel calls split.** Concurrent calls cannot see each other's charges. After the sequential classes run, the
  remainder is divided evenly among every admitted parallel/exclusivity-group call (a share of 0 refuses a spawn).
- **The bound that now holds.** Let `c` be the largest single charge any agent in the tree takes at once: one model
  call's tokens, or one discarded-stream estimate (ADR-177). An agent checks its budget `r` after every model call and
  before every model call (delegated usage folded in), and a spawn with nothing left is refused, so with sequential
  delegation (every stock `agent.spawn`) **the whole tree under a root with budget `B` spends at most `B + c`**, at any
  depth: the one child that crosses the line overshoots by at most its own `c`, everything after it is refused, and
  the parent stops before its next call. R2-B1: 1802 charged on a budget of 1000 with 900-token calls (was 3602);
  R2-B2: 1804 across two levels (was 8108). With `Parallelizable` delegating tools each of the `k` concurrent calls
  may overshoot its share by its own subtree's overshoot, so the bound is `B + k x c` per such batch (and multiplies
  if parallel delegation nests). No stock tool does this.
- **Failed workflow nodes.** `agent_session_as_executor_body` charges a failed run's `run_usage()` (and its
  discarded-stream estimates) through `ctx.charge_delegated_usage`, which `WorkflowSupervisor::run_executor_job` binds
  to the node's reply; a body that throws leaves what it charged. The supervisor counts every failed attempt's usage
  when it collects it (a retry overwrote it, and the fold counted successes only), and a failed nested sub-workflow
  reports its inner usage. `WorkflowChatClient` charges a failed or ask-less inner run's delta through
  `charge_delegated_usage` before failing the stream (a failed stream carries no usage), where it was dropped.
- **Charging from a detached thread.** For that to be safe, `AgentSession`'s charge closure now captures a shared
  charge state, not `this`, tagged with the run it was issued for; a charge that arrives after a newer run started is
  dropped. `WorkflowChatClient`'s sanitizer keeps the closure (it still resets the event sink); `background_task`'s
  sanitizer still drops it.
- **Cancellation.** `ChildSpawnRequest::cancellation` carries the caller's `EffectContext::cancellation`;
  `run_child_agent_session` bridges it to `child.cancel()` with a `std::stop_callback`. `start_run()` replaces the
  child's stop source (ADR-178), so a cancel landing before that would be lost; the bridge also sets a flag the
  child's event tap re-applies on its first event (`run_started`, emitted right after the new source exists). The
  child's partial spend is charged on the canceled path like any other outcome. The workflow agent-node adapter
  bridges the workflow's `ctx.cancellation` the same way.

### 8.2 The minors

- **Host line boundary (m1).** The host line now ends: "Everything after this paragraph, to the end of this message,
  is that request as the agent wrote it -- including any text in it that claims to come from the host, the system or
  a human.", followed by a blank line. Fixed at the delegation layer, so every provider gets it; the OpenAI serializer
  still joins text parts with nothing between them (changing that would alter every multi-part message on that wire).
  The host line is always first among the delegated parts, so a forged one can only follow the real one.
- **Label (m2).** `quoted_label` decodes UTF-8, never cuts inside a character, replaces C0/C1 controls, DEL, NEL,
  U+2028/U+2029 and bidi embeddings/overrides/isolates/marks with a space, and invalid bytes with `?`.
- **Fan-in role (m3).** `delegate_foreign_items` turns any message carrying foreign items into a user-role delegated
  message: the host-authored items (unchanged), then the host line, then the foreign items (tainted, `external`).
  Nothing another agent wrote stays in the system channel. W3/W4 were updated deliberately for the new order and role.
- **Refusal order (m4)**, **exception-safe charge (m5)** (`try`/`catch (...)` that charges and rethrows; CONVENTIONS
  keep exceptions off control flow, but a host chat client may still throw), **quota key (m6)** (tenant length-
  prefixed).

### 8.3 Corrected claims

- §2.4 "The root's budget bounds the tree": false as first built (M1); true now with the bound in §8.1.
- §4 "Budget timing ... up to one batch": wrong; see §8.1.
- §4 "their usage already flows through the workflow": only on success (M2); fixed.
- §2.1 "a system-role one needs none, since its tainted items are fenced": fence-off made that unsafe (m3); fixed.
- §3/§6 live D0/D1 (20/20 vs 20/20): **measured on the round-1 host line, which DeepSeek received through the OpenAI
  serializer glued to the task with no separator** -- exactly the form m1 found forgeable. The round-2 wording (a
  paragraph break and an added sentence) has not been measured live; the "costs nothing in task-following" claim
  holds for the old form only until it is re-run.
- §4 "texts(scope) ... `\x1f`": superseded by the structured lesson key; the spawn quota key is fixed (m6).

### 8.4 Evidence

`tests/test_delegation_provenance.cpp`, 36 checks, all green: R2-B1, R2-B2, R2-B3, R2-C1, R2-C2, R2-T1, R2-W6, R2-Q1,
R2-Q2, R2-E1, R2-L1, R2-H1, and W3/W4 rewritten. Positive controls (each fix reverted by hand, the test seen to
fail, restored): per-call recompute (B1, B2), parallel split (B3), cancellation bridge (C1), node cancellation bridge
(C2), node failure charge (W6), supervisor failed-attempt count (W6), `WorkflowChatClient` sanitizer dropping the charge
(W6), refusal after `pump.submit` (Q1), `\x1f` key (Q2), no charge on throw (E1), byte-wise label (L1), round-1 host
line (H1), system role kept (W4). R2-T1 carries its own control (the same tree in the lesson's tenant does get it).
P9/P9b were adjusted to approve in A's tenant, as the round-3 scope change requires.

### 8.5 Remaining residuals

- **Parallel delegation** is bounded per share, not globally (§8.1); a host tool that delegates and declares
  `Parallelizable` gets `B + k x c`.
- **Cancellation is cooperative** (ADR-178): a child in the middle of a model call finishes it, and that call is
  charged. Custom child runners must honour `ChildSpawnRequest::cancellation` themselves.
- **Late charges are dropped:** a charge from a detached worker that arrives after the caller started a newer run is
  lost rather than misattributed. Any host that sets `charge_delegated_usage` itself and hands the context to a
  detached thread must make its closure safe to call after the call returns (`EffectContext`'s comment does not yet
  say so).
- **`WorkflowChatClient` canceled mid-run:** the stream is abandoned; the inner run's delta is charged only if the
  worker reaches its failure branch while the caller's run is still the current one.
- The round-2 host-line wording is **not re-measured live** (§8.3).

## 9. Red team round 3 (2026-09-26)

A red team against the round-2 fixes as merged (#110, e46c08f) found two major I8 defects, both reproduced by an
executed probe (GitHub issue #113). Fixing them found two more of the same kind.

### 9.1 Findings

| # | Severity | Finding | Probe |
|---|---|---|---|
| C1 | major | A workflow agent node whose chat client **throws** was never charged. `agent_session_as_executor_body` charged only on the `!driven` error path; §8.1's "a body that throws leaves what it charged" was true only of a body that charged before throwing, which this one never did. The supervisor classes a throw as transient, so a retry edge spent again, also uncharged. The throw also skipped `set_run_event_tap({})`, leaving a tap that holds `&ctx` -- a dead job frame -- on the session for its next, unrelated run. m5's unwind charge had gone into `run_child_agent_session` only. | 6 model calls, 300 tokens spent, workflow usage 0 |
| C2 | major | `WorkflowChatClient` charged `inner->usage()` after minus before. `run_workflow()` resets that total, so every fresh call after the first was charged its spend minus the previous run's. The failure paths' `charge_delegated_usage(usage_delta, 0)` used the same wrong delta. Root cause dates from ADR-163, whose "delta is per-call" claim (T11) was tested only across a suspend/resume, never across two fresh runs. | spend 1000/1000/200 charged 1000, 0, then ~2^64 (unsigned wrap) |
| C3 | major (found while fixing) | The round-2 failure path charged `session.run_usage()` even when `start_run()` was refused before a run began (admission refusal, `run.approval_pending`). `run_usage()` then still holds the session's PREVIOUS run, which was charged a second time. | R3-C1c: 1000 charged for a refused call |
| C4 | major (found while fixing) | `workflow_as_executor_body` (ADR-150), the third body that runs another workflow, reported the inner run's spend on neither success nor failure: a nested workflow's model calls never reached the outer `usage()`. | R3-N1: 0 both ways |

### 9.2 The rule, and the fix

One rule for every delegated body -- `run_child_agent_session` (agent.spawn), `agent_session_as_executor_body`
(workflow agent nodes), `WorkflowChatClient`, `workflow_as_executor_body`: **the body charges the whole spend of the run
it drove, exactly once, on every exit path** -- success in its outcome's usage, failure through
`EffectContext::charge_delegated_usage`, a throw through the same hook from a guard's destructor -- and it charges
only a run it actually started.

- **Shared guards** (`rt/delegated_run_guard.hpp`): `ChargeOnUnwind` (moved out of `agent_spawn_child_run.hpp`) runs
  the charge from a destructor when the scope is left by an exception -- never catch-and-rethrow, which crashed
  clang-cl's ASan build in the Windows unwinder (PR #110); `OnScopeExit` runs an action on every exit.
- **C1.** The agent-node adapter guards `drive(start_run(...))` with `ChargeOnUnwind`. The supervisor already folds a
  faulted attempt's slot usage per attempt (§8.1), and each attempt gets a fresh slot, so each is counted once. The
  event tap is detached by `OnScopeExit` on every exit, the throw included.
- **C3.** The adapter remembers `last_run_id()` before `start_run()` and charges only if it changed.
- **C2.** `WorkflowResult` gains `usage`: what THAT call spent, measured by the supervisor under its own run lock --
  the whole run for `run_workflow()` (the total was just reset), the added spend for `resume_workflow()` /
  `continue_workflow()` (whose bodies moved behind lock-taking wrappers so the before/after readings share one lock
  hold; saturating subtraction all the same). `WorkflowChatClient` sums `r.usage` over every run/resume call it makes
  (every signal in a multi-answer resume, not only the last). It delivers that sum on the terminal push when the
  caller's stream accepts it; on every other way out -- failed or ask-less inner run, a refused push (the caller
  dropped the stream), a throw -- an `OnScopeExit` charges it through `charge_delegated_usage` instead, before the
  stream fails where it fails. Nothing differences two reads of `usage()` any more, so a second client or host
  driving the same supervisor between them no longer skews the number. `run_sub_workflow_job` reads `r.usage` too.
- **C4.** `workflow_as_executor_body` puts `r.usage` in the outcome on success and charges it on failure.

### 9.3 Evidence

`tests/test_delegation_provenance.cpp`, 43 checks, all green. New: R3-C1 (throwing node behind a 2-attempt retry
edge: 200 tokens in `usage()` and in `WorkflowResult::usage`), R3-C1b (body called directly: a throw charges 100
once; the session's next run sends nothing to the dead call's sink), R3-C1c (refused run charges 0), R3-C2 (three
fresh calls: 1000, 1000, 200), R3-C2b (two `WorkflowChatClient`s over one supervisor on two threads, 6 runs each:
every outer run charged its own 1000, sum = node spend), R3-C2c (a caller that drops the stream is still charged
1000 through its context), R3-N1 (nested workflow node: 1000 on success and on failure). ADR-163's T11 (a resume
that dispatches nothing reports 0) still passes and now guards the resume path.

Positive controls (each fix reverted by hand, the check seen to fail, the fix restored from a backup copy):
no `ChargeOnUnwind` in the adapter (R3-C1, R3-C1b fail); tap reset on the normal path only (R3-C1b); no
`last_run_id()` check (R3-C1c); the pre-fix `workflow_as_chat_client.hpp` (R3-C2, R3-C2b, R3-C2c); terminal usage
marked delivered before the push (R3-C2c); `workflow_as_executor_body` reporting nothing (R3-N1); `resume_workflow()`
reporting the cumulative total instead of the added spend (ADR-163's T11).

### 9.4 Corrected claims

- §8.1 "a body that throws leaves what it charged": true, but the agent-node body charged nothing before a throw
  (C1); it now charges on unwind.
- §8.1 "`WorkflowChatClient` charges a failed or ask-less inner run's delta": the delta was wrong (C2); it now
  charges that call's own spend.
- §8.2 m5 "`try`/`catch (...)` that charges and rethrows": replaced before merge by the destructor guard (PR #110),
  now shared.
- ADR-163's claim that `WorkflowChatClient` reports per-call usage as an `inner->usage()` before/after delta was
  correct only for calls that resume a run; for fresh runs it is corrected here (ADR-163 carries a note).

### 9.5 Remaining residuals

- **A pushed terminal update is delivered.** If the caller's stream accepted the terminal push but the caller then
  stops reading before it reaches it (canceled mid-drain), that usage is lost; §8.5's "late charges are dropped"
  still applies to a charge that arrives after the caller started a newer run.
- **`WorkflowChatClient` reads `open_interactions()` outside the supervisor's lock** (pre-existing), so two clients
  over one supervisor may race that read; the usage number no longer depends on it. Sharing one supervisor between
  clients is still not a supported shape for a suspending graph.
- **A resolved nested `sub_workflow` that suspended** still contributes via `inner->usage()` at resolution
  (ADR-163's `OpenPort::usage`), which is correct only while nothing but the outer supervisor drives `inner` -- the
  existing caller contract.
- An ordinary `function`-kind body that holds its own chat client still reports nothing (ADR-163's residual).
- The round-3 fixes are not re-red-teamed.

