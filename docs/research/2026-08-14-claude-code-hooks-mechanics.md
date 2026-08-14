# How Claude Code's and the OpenAI Agents SDK's "Hooks" actually work — two different things sharing one name

**Date:** 2026-08-14. **Sources:** `https://code.claude.com/docs/en/hooks` (fetched 2026-08-14;
`https://docs.claude.com/en/docs/claude-code/hooks` 301-redirects here); `https://openai.github.io/
openai-agents-python/ref/lifecycle/` (fetched 2026-08-14, via search + WebFetch, per the OpenAI Agents
SDK's own published reference docs). Per `CLAUDE.md`'s research discipline: do not assert what a
product does from memory. **Why this doc exists:** the user proposed adding "Hooks" to AgentEngine,
naming Claude Code's own Hooks feature as the concrete gap — MAF has no equivalent (confirmed by
existing research, `docs/research/2026-08-11-maf-middleware-codeact-skills-deep-dive.md`). Checking a
second framework's "hooks" (OpenAI Agents SDK) turned up a real, load-bearing finding: **the word
"hooks" names two architecturally unrelated things across the industry**, not one feature with minor
variations — see §3. This grounds the AgentEngine proposal in both real mechanics before any design is
drafted — see the companion gap doc, `docs/planning/external-process-hooks-gap.md`.

## What a hook is

A hook is a **user-defined shell command** (or an HTTP call, an MCP tool call, a `prompt`, or an
`agent` invocation) that Claude Code runs automatically when a named lifecycle event fires, configured
declaratively in `settings.json` (or a plugin's `hooks/hooks.json`, or skill/subagent frontmatter).

## Event taxonomy — 24 named events, three shapes of blocking behavior

Grouped by cadence: once per session (`SessionStart`, `Setup`, `SessionEnd`), once per turn
(`UserPromptSubmit`, `UserPromptExpansion`, `Stop`, `StopFailure`), once per tool call
(`PreToolUse`, `PermissionRequest`, `PermissionDenied`, `PostToolUse`, `PostToolUseFailure`,
`PostToolBatch`), subagent/team events (`SubagentStart`, `SubagentStop`, `TeammateIdle`), task
management (`TaskCreated`, `TaskCompleted`), async standalone events that fire independently of the
turn loop (`WorktreeCreate`, `WorktreeRemove`, `CwdChanged`, `DirectoryAdded`, `FileChanged`,
`InstructionsLoaded`, `ConfigChange`, `PreCompact`, `PostCompact`), and MCP/display events
(`Elicitation`, `ElicitationResult`, `Notification`, `MessageDisplay`).

Typical tool-call ordering: `UserPromptSubmit → PreToolUse → PermissionRequest → (tool executes) →
PostToolUse (or PostToolUseFailure) → PostToolBatch → Stop/StopFailure`.

**Only a subset can actually block or change the outcome** — this is the load-bearing distinction for
any AgentEngine mapping:

| Can gate the action | Cannot — already happened / observation only |
|---|---|
| `PreToolUse`, `UserPromptSubmit`, `UserPromptExpansion`, `PermissionRequest`, `Stop`, `SubagentStop`, `Elicitation` | `PostToolUse`, `PostToolUseFailure`, `StopFailure`, `SessionStart`, `SessionEnd`, `PermissionDenied`, `Notification`, `MessageDisplay`, most async standalone events |

## The decision contract

**Input:** JSON on stdin (command hooks) or as an HTTP POST body. Common fields on every event:
`session_id`, `prompt_id`, `transcript_path`, `cwd`, `permission_mode`, `hook_event_name`, `effort`.
Event-specific fields add the relevant payload — e.g. `PreToolUse` adds `tool_name`, `tool_input`,
`tool_use_id`.

**Output:** exit code plus optional JSON on stdout. Exit code **2** is the only code that blocks an
action on its own (stderr becomes the shown reason); exit 1 and other non-zero codes are non-blocking
errors, action proceeds. A JSON object on stdout, independent of exit code, carries finer control:

```json
{
  "continue": true,
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "allow" | "deny" | "escalate",
    "permissionDecisionReason": "...",
    "updatedInput": { "...": "modified tool input" },
    "additionalContext": "..."
  }
}
```

**`updatedInput` is a direct content-rewrite point**: a `PreToolUse` hook is not limited to allow/deny
— it can hand back a *modified* tool call, which Claude Code then executes in place of the model's
original. This is the exact shape ADR-033 (`decisions/ADR-033-middleware-model-call-chain.md`) found
fatal for AgentEngine's own in-process `Middleware`: content a hook rewrites can carry a fabricated or
mutated tool call, and nothing in the contract shown here says what trust tier that rewritten call
gets relative to one the model itself produced.

## Execution authority — the finding that matters most for AgentEngine

Quoted directly from the docs: **"Hooks are user-defined shell commands that execute automatically.
Your hook commands run with the full permissions of the user running Claude Code."**

Concretely:

- The hook process runs as the same OS user as the Claude Code CLI itself — home directory, SSH keys,
  API tokens/credentials in the environment, and every file that user can read or write are reachable.
- **No sandboxing.** The docs state hooks "are not sandboxed or isolated" — they can read secrets,
  execute arbitrary commands, modify files, and fork subprocesses.
- The only gate before a hook can run at all is a one-time **workspace trust dialog** for
  project-level hook sources (`.claude/settings.json`); user-level (`~/.claude/settings.json`) hooks
  run with no trust prompt, and a managed/organization policy can force-enable hooks regardless of
  local trust.
- Hooks see the full JSON of every tool call they fire on — file contents, Bash commands, arguments —
  with no field-level redaction.

This is a **textbook ambient-authority grant**: the hook process's authority is "whatever the invoking
user's OS account can do," not a capability set scoped to the event it's reacting to. It is exactly
what AgentEngine's **I2** ("no ambient authority" — `AgentEngineSpecification.md` §4) exists to
prevent, and exactly what `009-Plugin-and-Extension-System.md` §5 names explicitly as unavailable to
any plugin world: *"Not available in any world: raw sockets, subprocess, nested sandbox creation,
arbitrary filesystem, host environment, or any handle that outlives the invocation."*

## Configuration shape (for reference — not a claim AgentEngine should copy the wire format)

`settings.json`'s `hooks` object maps an event name to an array of `{matcher, hooks: [...]}` entries;
each hook entry has a `type` (`command` | `http` | `mcp_tool` | `prompt` | `agent`) and type-specific
fields (`command`/`args` for `command`; `url`/`headers`/`allowedEnvVars` for `http`). Matchers filter
by tool name (exact, `|`-separated list, or a regex) or, for non-tool events, by an event-specific
discriminator (`SessionStart`'s `startup|resume|clear`, `FileChanged`'s literal filenames). Default
timeouts range from 10s (`MessageDisplay`) to 600s (`command`/`http`/`mcp_tool`), with `SessionEnd`
capped to a 1.5s shared budget (extendable to 60s). A hook that times out is canceled with no output
read and the action proceeds — except `WorktreeCreate`, which fails on timeout.

## 3. The OpenAI Agents SDK's "Hooks" are a completely different architecture

`RunHooksBase`/`AgentHooksBase` (`openai-agents-python`) are **in-process, plain async Python method
overrides** — `on_llm_start`/`on_llm_end` (around each model call), `on_agent_start`/`on_agent_end`
(when the active agent changes / produces a final output), `on_tool_start`/`on_tool_end` (around each
local tool invocation), `on_handoff` (when control passes between agents). A caller subclasses
`RunHooksBase` (run-wide) or `AgentHooksBase` (scoped to one agent), overrides the methods it wants,
and passes the instance to the runner or sets it on `agent.hooks`.

Three properties make this a genuinely different mechanism from Claude Code's, not a smaller version
of the same one:

1. **No subprocess, no config file, no wire contract.** A hook is an ordinary awaited method call in
   the same Python process, receiving the real in-memory `Agent`/`Tool`/`RunContextWrapper` objects
   directly — there is no serialization boundary at all, let alone one crossing into a separately
   authored external command.
2. **Observation-only — no return-value contract for blocking, denying, or rewriting.** The reference
   docs describe overriding "the methods you need" with no documented mechanism for a hook's return
   value to deny a tool call, short-circuit an LLM call, or rewrite arguments. This is the opposite of
   Claude Code's `PreToolUse`/`permissionDecision`/`updatedInput` contract (§2 above).
3. **Full, unscoped context access, but no privilege escalation beyond the process it already runs
   in.** A hook sees the real `Agent`/`Tool` objects (whatever the host process already holds) — there
   is no separate authority grant to reason about, because nothing crosses a trust or process boundary
   to reach it.

**This shape is architecturally identical to what AgentEngine already declares and partly
implements**: 002 §5's `Middleware<Ms...>` before/after hooks around the model call
(`include/agentengine/core/middleware.hpp`, ADR-033) are the same "in-process, typed-context,
subclass/override" idiom, except AgentEngine's version is already MORE capable than OpenAI's on the
one axis that matters most for policy enforcement — `before_model`/`after_model` CAN short-circuit
with a response or a denial (002 §5's own text: "middleware may inspect, annotate, rewrite content,
short-circuit with a result, or deny"), where OpenAI's hooks structurally cannot. AgentEngine is not
behind the OpenAI Agents SDK on in-process hooks; it is ahead on the one declared-but-unwired point
(model call) and behind only on wiring the other three (run/turn/tool-call) into a real consumer.

## 4. The real, three-way split this leaves

| | Execution | Can gate/deny/rewrite? | Authority model | AgentEngine's closest existing thing |
|---|---|---|---|---|
| **MAF** | — | — | — | No hooks concept at all (confirmed, `2026-08-11-maf-middleware-codeact-skills-deep-dive.md`) — only its own decorator-chain `AIAgentMiddleware`, the direct prior art 002 §5's `Middleware<Ms...>` was already built from |
| **OpenAI Agents SDK "Hooks"** | In-process, same call stack | No — observation only | N/A (no boundary crossed) | `Middleware<Ms...>`'s declared-but-unwired run/turn/tool-call points (002 §5) — AgentEngine's version is a strict superset (deny-capable) once wired |
| **Claude Code "Hooks"** | Out-of-process, subprocess/HTTP, host-configured | Yes — `permissionDecision`, `updatedInput`, exit code 2 | Full permissions of the invoking OS user, unsandboxed | Nothing yet — this is the genuine gap, and the one this project's own I2/009 §5 constraints rule out copying verbatim |

## What this means for AgentEngine, directly (full design question in the gap doc)

Claude Code's Hooks feature is real, well-specified, and genuinely useful — configurable interception
at well-named lifecycle points, with a documented JSON contract. But its actual authority model (full
user permissions, unsandboxed, workspace-trust-gated rather than capability-scoped) is not a shape
AgentEngine can adopt as-is: it is the specific thing I2 and 009 §5 were written to rule out. What
transfers is the **event taxonomy and the allow/deny/rewrite decision shape** — not the "engine execs
an arbitrary host command with the invoking user's full authority" execution model. See the design
draft for how the taxonomy maps onto AgentEngine's own existing seams (`RunEvent`/`emit_run_event` for
the non-blocking half, `Middleware`/`ApprovalDecider` for the blocking half) instead.
