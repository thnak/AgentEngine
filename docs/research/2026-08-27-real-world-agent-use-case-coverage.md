# Real-world agent use-case coverage: what AgentEngine actually supports vs. what shipped products do

**Date**: 2026-08-27. **Scope**: functional/use-case validation, not security — the companion,
adversarial half of this exercise (CVE replay, TOCTOU races, attack simulation) lives in
`docs/planning/identity-native-sandbox-worktree-design.md` §38 and `decisions/ADR-014`. This
document asks a different question: **for the real problems shipped AI agent products actually
solve, does AgentEngine's real, current implementation solve them too — and where, concretely,
does it not?**

## Method

Two phases, deliberately separated so the external research isn't biased by knowing what
AgentEngine already does:

1. **Five context-free research agents**, each with zero knowledge of this codebase, surveyed one
   domain via WebSearch against real, currently-shipping products and frameworks — coding-agent
   sandboxes, long-running/crash-recoverable agents, multi-agent delegation, enterprise tool
   governance (+ the MCP spec itself), and computer-use/browser agents. Full agent reports are in
   the session transcript this doc was produced from; every claim below that isn't independently
   re-cited traces back to one of those five reports.
2. **One context-aware fork**, working from the external findings, cross-checked six concrete
   mechanisms against AgentEngine's *actual, current, production* code (`include/agentengine/`,
   `src/`, `tests/`) — explicitly excluding the not-yet-Judged prove-phase sketches under
   `docs/planning/proofs/` and the identity-native design doc, which are design artifacts, not
   shipped capability. Every AgentEngine claim below has a real `file:line` citation.

Findings are reported as **shipped and wired end-to-end** / **primitive exists but not wired into
a real call path** / **design-only, no production code** / **not found** — deliberately not
rounded up, per this project's own standing practice of never claiming a capability that isn't
real and executed.

## 1. Coding-agent sandbox/worktree isolation

**What real products do.** Every actively-developed coding agent researched — Claude Code, Cursor,
GitHub Copilot coding agent, and (for parallel/sub-agent work) OpenAI Codex CLI — uses a real git
worktree as the unit of isolation for an agent's attempt at a task: a real branch, a real separate
checkout, discardable or mergeable independent of the main tree. Cursor's `/worktree` and
`/best-of-n` (run N attempts across N isolated worktrees, keep the best), Claude Code's
`--worktree`/desktop-app-worktree-per-session, and GitHub's "every session runs in its own git
worktree" for local sessions, plus a `copilot/*`-branch-only push restriction for cloud sessions,
are all real, dated, documented mechanisms — not proposals. Devin and Replit take a different but
related approach (VM/CoW-filesystem snapshot per session rather than git-worktree per se), but the
underlying need — **isolate one attempt from the shared tree, decide later whether to keep it** —
is the universal shape.

Separately, and NOT the same mechanism, a **non-git checkpoint/rewind layer** exists in Claude Code
(`/rewind`, up to 100 checkpoints/session, 30-day retention) and Cursor (per-turn checkpoints,
click-to-restore) that restores file state (and, for Claude Code, optionally conversation state)
to a point in the session's own timeline — explicitly **not** a substitute for git, and both
products disclose the same class of gap: bash/shell-tool edits and background-subagent edits are
not tracked or restorable, only edits made through the product's own file-editing tools are.
OpenAI Codex CLI shipped and then **removed** an analogous `/undo`: it wrote dangling git objects,
corrupted the git index on restore, and produced storage bloat (one user reported 102GB of orphaned
objects on a 5.7GB project) — a real, disclosed, load-bearing lesson that a checkpoint layer built
*on top of* git rather than *independent from* it is a documented failure mode, not a
theoretical one.

**What AgentEngine has.** Extensive prove-phase design exists — `Ledger`/`BranchHandle`/
`spawn_child_branch`/case-fold-safe `commit()`/`merge_trees()` — under
`docs/planning/proofs/worktree_io/` and the identity-native design doc, all real, compiled, and
run standalone. **None of it is wired into `AgentSession`/`Tool<>` in production code.** Grep of
`include/agentengine/` and `src/` found no call path from a real session or tool invocation into
any worktree/branch primitive. This is the single largest, most consistently-validated gap this
research surfaced: it is not a niche feature — it is the *primary* isolation mechanism of every
actively-developed coding-agent product surveyed.

**STATUS: design-only, no production code.**

## 2. Long-running state, crash recovery, and session resume

**What real products do.** The two most architecturally serious systems researched — LangGraph and
OpenAI's Agents SDK — both converge on the same shape: a durable, structured state object (not a
raw transcript) keyed by a portable identifier (`thread_id` / a serialized `RunState`), persisted
externally (Postgres, your own DB), reloadable in a different process. Both explicitly document,
in their own words, the same landmine: **resuming the same session/thread concurrently from two
places is unsafe, and avoiding that is the caller's responsibility, not something the framework
enforces.** LangGraph's own GitHub issue tracker has a real, dated, corroborated incident
(langgraph.js#2040, 2026-03-09) where exactly this happened for real in production — a
statically-initialized async-context singleton leaked state between two concurrent invocations in
the same Node process, cross-contaminating two different customers' PII in a multi-tenant chatbot,
in one case escalating a $16,000 payment to the wrong customer. Anthropic's own Claude Agent SDK
goes further than most and states outright that cross-machine session resume is *not* a mechanism
they recommend relying on — "Don't rely on session resume... often more robust" to re-derive
results and feed them into a fresh session — a deliberately modest, disclosed position from a
vendor that could have oversold this.

**What AgentEngine has.** A real, shipped equivalent of the OpenAI `RunState` shape:
`AgentSession::to_record()`/`restore_from_record()`/`snapshot_record()`
(`include/agentengine/rt/agent_session.hpp`, locked under `session_mutex_`), free functions
`save_agent_session_snapshot`/`load_agent_session_snapshot`/`checkpoint_if_due`/`delete_session`
(same file, ~2605–2865), JSON-encoded, against a real `SessionStore` concept with a real,
path-injection-safe `FileSessionStore` (`rt/session_store.hpp:286-300`). Architecturally this is
the right shape and already matches the industry's own accepted pattern. Two honest, disclosed
gaps, found by direct inspection, not assumed:

- **History rehydration is not wired.** `load_turn_delta()` exists but its own comment states it is
  "not itself wired into `load_agent_session_snapshot()`... a separate, larger question this ADR
  does not answer" (`agent_session.hpp:2725`) — meaning a restored session's `history_` is not
  actually reconstructed from turn deltas today, unlike LangGraph/OpenAI, which do reconstruct full
  state on resume. A snapshot restores *something*, not yet the full conversation.
- **No cross-process concurrent-resume guard exists**, matching (not exceeding) the industry's own
  disclosed position: `session_mutex_` is an in-process `AsyncMutex` enforcing I1 ("one session,
  one executor") within a single process, same as LangGraph/OpenAI/Claude Agent SDK all do — none
  of them enforce this across processes either, and all three explicitly document it as the
  caller's job. AgentEngine is not behind the field here; it just hasn't stated the caveat as
  explicitly in its own docs as Claude Agent SDK does in its own.

**STATUS: primitive exists but not wired end-to-end** (snapshot/restore real and well-shaped;
history rehydration genuinely open; cross-process exclusivity is an industry-wide unsolved
problem, not an AgentEngine-specific gap).

## 3. Multi-agent delegation and capability-scoped handoff

**What real products do.** Every major multi-agent framework researched — OpenAI Agents SDK,
Microsoft Agent Framework, AutoGen, CrewAI, Google ADK — converges on two structurally distinct
delegation shapes: a full-context **"handoff"** (parent hands the whole conversation and control to
a peer, no guaranteed return — OpenAI's `Handoff`, MS AF's handoff orchestration, AutoGen's Swarm,
ADK's `transfer_to_agent`), and an isolated **"tool-call"** shape (child runs isolated, returns a
scoped result, control structurally guaranteed to return — OpenAI's `Agent.as_tool()`, MS AF's
"agent-as-tools", ADK's `AgentTool`, Anthropic's own lead/subagent research-system pattern).
**Across every single one of these frameworks, the research explicitly did not find a formal,
structural capability-*attenuation* object** — something that lets parent agent A hand child
agent B strictly *less* authority than A itself holds, enforced by the framework rather than by
convention. What exists everywhere instead is coarser: a fixed tool list set at agent-construction
time (MS AF, AutoGen, ADK), an `is_enabled`/`tool_filter` callback gating what's *offered* to the
model (OpenAI) rather than what's *authorized*, or converting peer agents into callable tools
(CrewAI) with the actual data-flow contract undocumented even in CrewAI's own community forum. ADK
itself has a real, named, open GitHub issue (`google/adk-python#1039`) where a sub-agent, once
handed full conversational control, never generates the call needed to hand it back — a concrete
symptom of "handoff" delegation's structural authority-return problem, not a hypothetical one.
Real production cost is disclosed too: Anthropic's own multi-agent research system write-up states
plainly that multi-agent delegation "use[s] about 15× more tokens than [single-agent] chats" and
that early versions had agents "spawning 50 subagents for simple queries" before explicit scaling
rules were added.

**What AgentEngine has** — and this is the one place this research found AgentEngine genuinely
*ahead* of the field, not behind it, and it directly corrects a stale memory from earlier in this
project's own history. `include/agentengine/rt/agent_spawn.hpp` (landed 2026-08-23) implements
exactly the missing primitive: the child's `CapabilitySet` is derived via
`ctx.capabilities->attenuate(target->metadata.capability_ceiling)` (line ~392-393) — the file's own
comment states the final child set is "never a copy of the caller's own." `AgentSpawnArgs`
deliberately carries no depth/budget field the *model* can influence (line ~98-100: authority
"comes from the CALLER's already-held CapabilitySet/SpawnBudget, never from the call's own args" —
I3, model output is never authority). `CapabilitySet::attenuate()` itself
(`include/agentengine/trust/capability.hpp:794-804`) is real, tested
(`tests/test_capability_enforcement.cpp`, `tests/test_agent_tool_invocation.cpp`,
`tests/test_native_exec_capability.cpp`), fails closed the instant any requested entry isn't
subsumed by the parent's own grant, and narrows concretely per capability type (e.g. `find_net_out()`
narrows a multi-host `NetOut` grant down to exactly one matched host, never the original set). This
is precisely the "scoped/attenuated capability as a formal object" every other framework surveyed
was found to lack. **A prior session's memory record (`project_adr029_and_tool_loop_backlog.md`)
stated "agent.spawn call path still unbuilt" as of 2026-08-13 — this is now stale; it shipped ten
days later and should be treated as done, not as an open backlog item.**

**STATUS: shipped and wired end-to-end** — a genuine, verified differentiator against every
production framework surveyed, not a self-assessment.

## 4. Enterprise tool governance, approval gates, and MCP

**What real products do.** The clearest, most load-bearing enterprise pattern is **resumable,
first-class human approval that actually blocks execution**, not just a logged warning: OpenAI's
Agents SDK returns an `interruptions` array plus a serializable `state` when a `needs_approval`
tool is hit, letting the app resume the *same run* later with an allow/deny decision (even across a
process boundary, since `state` round-trips through JSON); Anthropic's Managed Agents puts the
session itself into `session.status_idle` with `stop_reason.type: 'requires_action'` until a client
sends `user.tool_confirmation`; ServiceNow's documented pattern rejects out-of-policy calls
*before* they reach the target system, not after. The clearest disclosed governance failure is
Replit's July 2025 production database wipe: an explicit user-declared "code freeze" was violated
by the agent, whose own root cause (per Replit's own follow-up) was that "the box had no internal
walls" — preview, test, and production shared one database — fixed by Replit only *after* the
incident, by building forkable, isolated dev/prod databases and a "planning-only" mode. Separately,
the MCP specification's 2026-07-28 revision made the protocol core **stateless** (no
`initialize`/`initialized` handshake, no `Mcp-Session-Id` header — every request self-describing),
deprecated Dynamic Client Registration in favor of Client ID Metadata Documents, and added
mandatory RFC 8707 resource-parameter binding plus RFC 9207 issuer validation specifically to close
a real confused-deputy/authorization-server-mix-up class of vulnerability that existed in earlier
drafts. Notably, the spec's own human-in-the-loop language (for the now-deprecated Sampling
feature) is phrased as **"SHOULD," not "MUST"** — MCP itself does not mandate approval gates or
per-call budgets; every platform surveyed (OpenAI, Anthropic, Microsoft, ServiceNow) built that
enforcement on top of MCP, not from it.

**What AgentEngine has.** `ApprovalDecider` (`core/tool_pipeline.hpp:319`) is a synchronous
decision function, but when no in-process decider can answer, `resolve_approval_outcome()`
(`tool_pipeline.hpp:477-499`) returns `approval_outcome::needs_decider`, which
`AgentSession::run_rounds()` turns into a real, genuinely-suspended `Interaction`
(`interaction_reason::approval`, `agent_session.hpp:857-865`) — the session opens the interaction
and *rejects* a new `start_run()` while one is outstanding. Resume happens via
`resolve_interaction()` (line ~928), under the same `session_mutex_` that enforces I1.
`expired_interaction_ids()` (line ~770) gives timeout handling. A distinct, deliberately separate
`hook_decision` suspend reason exists for external-process dispatch
(`core/tool_call_hook.hpp`) — the codebase's own history records a real red-team-found fatal
finding that reusing the approval one-shot path for this would have been wrong (line ~1856-1868),
fixed before shipping. This is architecturally closer to Anthropic Managed Agents' `status_idle` +
`tool_confirmation` shape than to a bare synchronous callback, and is a real, tested,
already-shipped mechanism, not a gap.

On MCP itself: `include/agentengine/protocol/mcp/server.hpp:281` pins `protocolVersion` to the
literal `"2026-07-28"` — correctly the stateless revision, not an older stateful one — and no
`Mcp-Session-Id` handling was found in `client.hpp`, consistent with having moved past the old
session-header model. But no reference to RFC 8707 (`resource` parameter) or RFC 9207 (issuer
validation) exists anywhere in `protocol/mcp/` — the message-shape/transport layer tracks the
current spec; the OAuth 2.1 authorization layer that spec depends on for its confused-deputy
defenses does not exist. This is not a new finding — it matches prior session memory's own
ADR-061 result ("011 §10 G2 NOT met (auth 3/49, no OAuth machinery)") — but this research
independently confirms *why that gap matters in practice*: it is exactly the mechanism the current
MCP spec added specifically to prevent a real, named attack class, not an abstract compliance
checkbox.

**STATUS**: approval-as-resumable-primitive — **shipped and wired end-to-end**. MCP protocol
version — **shipped and current**. MCP OAuth/resource-binding authorization layer — **not found**
(confirmed pre-existing, now validated as real-world load-bearing, not merely a spec-compliance
gap).

## 5. Computer-use / browser agents

Every product surveyed (Anthropic Computer Use, OpenAI Operator/ChatGPT Agent, Google Mariner →
Gemini 2.5 Computer Use, Amazon Nova Act, Perplexity Comet) targets a GUI/browser action space
(click, type, screenshot, scroll) that AgentEngine's own design does not currently address at all —
AgentEngine's sandbox surface is code execution and mediated filesystem/network access, not visual
UI control. This is noted for completeness, not as a gap: it is simply outside the engine's stated
scope, and nothing in this research suggests it should be pulled in. One transferable pattern is
worth naming, though: Amazon Nova Act's default-*empty* `allowed_file_upload_paths`/
`allowed_file_open_paths` allowlists are structurally the same shape as AgentEngine's own
`Grant<FsRead>`/`Grant<FsWrite>` capability model (explicit, fail-closed, narrow-by-default) — an
independent, cross-domain validation that this design pattern (rather than a coarser sandboxed-root
model) is what real, current products converge on when they do bother to scope file access at all.

## 6. Summary

| Use case (industry-validated, not invented) | AgentEngine status | Real citation |
|---|---|---|
| Isolate one task/attempt on a branch/worktree, merge or discard | **Design-only** | prove-phase only; no `AgentSession`/`Tool<>` call path found |
| Non-git checkpoint restoring file+conversation state together | **Not found in production** (see gap above; prove-phase `Ledger` addresses the file half only, and isn't wired) | — |
| Durable, serializable session state, cross-process resumable | **Primitive exists, partially wired** | `agent_session.hpp` `to_record`/`restore_from_record`/`SessionStore` |
| History rehydration on session restore | **Open, disclosed** | `agent_session.hpp:2725` |
| Cross-process concurrent-resume exclusivity | **Not enforced** (industry-wide gap, not AgentEngine-specific) | `session_mutex_` is in-process only |
| Resumable, blocking human-approval gate on a tool call | **Shipped** | `tool_pipeline.hpp:477-499`, `agent_session.hpp:857-928` |
| Formal capability-attenuation object for sub-agent delegation | **Shipped — ahead of every framework surveyed** | `agent_spawn.hpp`, `capability.hpp:794-804` |
| MCP 2026-07-28 stateless transport | **Shipped, current** | `protocol/mcp/server.hpp:281` |
| MCP OAuth 2.1 / RFC 8707 / RFC 9207 authorization layer | **Not built** (pre-existing, confirmed real-world load-bearing) | matches ADR-061's own 011 §10 G2 finding |
| Fail-closed, narrow-by-default file capability grants | **Shipped, matches industry pattern (Nova Act)** | `Grant<FsRead>`/`Grant<FsWrite>` |
| GUI/browser computer-use action space | **Out of scope** (not a gap) | — |

## 7. What this changes going forward

1. **Correct a stale memory record**: `agent.spawn`/capability-scoped delegation is shipped
   (2026-08-23), contradicting the 2026-08-13 backlog note that called it unbuilt. Treat it as done.
2. **The single highest-value next step, by a wide margin, validated across every actively-developed
   coding-agent product researched**: wire the identity-native design's branch/worktree primitives
   (or a scoped-down first version of them) into a real `AgentSession`/`Tool<>` call path. This
   isn't a novel feature request — it is the *primary* isolation mechanism every real coding agent
   surveyed ships, and AgentEngine currently has the primitives proven standalone but not connected
   to anything a real session can call.
3. **History rehydration on session restore** (`agent_session.hpp:2725`) is a real, scoped, concrete
   gap against the industry's own accepted `RunState`-shaped pattern — worth closing on its own,
   independent of the worktree work above.
4. **The MCP OAuth/resource-binding layer** (already tracked via ADR-061) is now independently
   confirmed load-bearing by real spec analysis, not just an internal audit finding — the current
   spec added RFC 8707/9207 specifically to close a real vulnerability class, so this is a
   real-world-relevant gap, not a compliance formality.
5. **Do not over-invest in cross-process concurrent-resume locking** relative to the other items
   above — every serious framework researched (LangGraph, OpenAI Agents SDK, Claude Agent SDK
   itself) treats this as the caller's responsibility and documents it as such rather than
   enforcing it centrally; AgentEngine's current position is consistent with the field, not behind
   it. If it's ever addressed, LangGraph's real, dated production incident
   (langgraph.js#2040 — a statically-initialized singleton leaking state across concurrent
   invocations) is the concrete failure shape to design a regression test against, not a
   hypothetical one.
