# ADR-193 — When agent A hands work to B and B to C, does the text keep its author, the effects their owner, and the chain its budget?

- **Renumbered:** written as ADR-185; renumbered to ADR-193 on 2026-09-25 when this stack merged into `main`, where those numbers had been taken by other ADRs in the meantime. Commit messages, PR titles and ADR cross-references written before then use the old number.

- **Status:** Proposed — built, tested offline (§6), red-teamed once (§7: no fatal; 4 major, all fixed; the fixes are
  not yet re-red-teamed). **Needs the project owner's judgement** (it touches I3's untainting rule, 003 §2, for every
  handoff).
- **Date:** 2026-09-25.
- **Scope:** `core/delegation.hpp` (new: `make_delegated_message`), `trust/principal.hpp` (`delegation_root`,
  `root_id()`), `core/effect_context.hpp` (`delegated_event_sink`, `charge_delegated_usage`), `rt/agent_session.hpp`
  (event forwarding, delegated usage folded and budget-checked, pinned context, root-scope lesson lookup),
  `rt/agent_spawn.hpp` / `rt/agent_spawn_child_run.hpp` (delegated input, root-keyed quota, `child_id`, whole-run
  usage, per-target settings, shared lessons), `rt/agent_workflow_executor.hpp` (delegated input for agent nodes),
  `core/approved_lessons.hpp` (`texts(scope)`), `core/tool_pipeline.hpp` (audit names the root), tests (§6).
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
     line. Host-authored input passes through unchanged.
   - The delegator's name is quoted, control characters are stripped and the length is capped before it enters the
     host line.
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
   - **Result.** The root's budget bounds the tree (I8).
   - **Quota.** The spawn quota is keyed on (tenant, root), so a tree shares one quota.
   - **Detached copies.** A detached copy of a tool's context (`background_task`, a workflow's detached worker) drops
     both sinks, since they point at the session.
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
  (§6): 20/20 and 20/20 as a delegated task, the same as the old plain form.
- (5) and (6) are host opt-ins under ADR-070's seam (explicit, off by default, audited, host code only). Sharing
  lessons down a chain extends ADR-191's approval scope from a principal to its delegation tree, which is the owner's
  call.

## 4. What this does NOT claim (residuals)

- **Custom child runners.** Only the stock `run_child_agent_session` applies the settings, the event tap and
  whole-run usage. A host's own runner gets the fields on `ChildSpawnRequest` and must honour them.
- **Workflow nodes** are told only "an upstream agent node", not which one; the adapter is not given the edge. Their
  principals are still independent (not derived on behalf of the workflow), and their usage already flows through the
  workflow (ADR-163), not through a parent session. Only agent nodes apply the rule: a plain function node that
  relays an agent's text passes it on as it received it.
- **`multi_agent::spawn`** (host-driven) still runs a child as the parent's principal by default. The host builds its
  `StartRun`, so the input is host-authored.
- **Memory keyed on delegated text:** a child with a `MemoryProvider` queries memory with the first text of the last
  user message, which is now the host line. That is harmless, but it makes the query less useful.
- **Budget timing:** delegated usage is checked before the parent's next model call, not mid-child. Each child is
  capped at what its caller had left when it was spawned, so siblings spawned in one parallel batch can together
  overspend by up to one batch.
- **The quota is lifetime-of-process** (`SpawnQuotaTracker` never resets), so a runaway tree spends its root's quota
  until restart.
- **`texts(scope)`** shares `find`'s existing key format: a principal id containing `\x1f` could reach another
  scope's texts. This is a pre-existing limit of the registry key; ids are host-assigned.
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

