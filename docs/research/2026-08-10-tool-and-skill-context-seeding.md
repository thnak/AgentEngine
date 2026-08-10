# Research record — how tools and skills get seeded into inference context (Anthropic vs. MAF)

**Compiled:** 2026-08-10 · **Status:** dated snapshot, not a living document

Question: for a given inference call, how do tool definitions and skill content actually enter
the model's context — request shape, ordering relative to system prompt/history, and how large
tool/skill catalogs avoid blowing the context window? Two independent surveys (Anthropic/Claude
side, Microsoft Agent Framework side), synthesized here with a short note on relevance to
AgentEngine's own **009-Plugin-and-Extension-System.md §8** (Skills) and the
`HistoryAndSkillsProvider`/ADR-024 code already in the tree.

---

## 1. Anthropic (Claude API / Claude Code)

**Tools.** The Messages API takes a dedicated top-level `tools` array, separate from `system` and
`messages`. Anthropic folds the `system` text and the JSON-Schema tool definitions into one
system-prompt-shaped prefix the model sees *before* `messages` — tools are never interleaved into
conversation history, which is what makes them cache-stable across turns. `tool_choice`
(`auto`/`any`/`tool`/`none`) governs whether a call is forced. There is no silent truncation of
tool definitions.
([Tool use overview](https://platform.claude.com/docs/en/agents-and-tools/tool-use/overview.md),
[How tool use works](https://platform.claude.com/docs/en/agents-and-tools/tool-use/how-tool-use-works.md),
[Define tools](https://platform.claude.com/docs/en/agents-and-tools/tool-use/define-tools.md))

**Skills.** Live under `.claude/skills/` (or configured dirs), auto-discovered, no explicit
registration. Only the SKILL.md frontmatter (`name`, `description`, a `control: automatic|manual`
gate) is advertised up front; the markdown body loads lazily — only once the model selects the
skill as relevant, or the user explicitly triggers `/skill-name`. Templated placeholders
(`{{cwd}}`, `{{repo}}`, `{{branch}}`, `{{args}}`) resolve at invocation time, so session state
never has to be baked into the always-on advertisement.
([Skills guide](https://code.claude.com/docs/en/skills.md))

**Claude Code's own tool/skill seeding.** Built-in tools (Read/Edit/Bash/…) and configured
MCP-server tools are constructed once per session into the system-reminder context. Confirmed in
this very session's `<system-reminder>` blocks: large tool catalogs are deferred — a
`ToolSearch`-style tool stays resident and the rest are loaded by name/keyword search on demand
(matches the documented `tool_search_tool` mechanism below), rather than front-loading every MCP
tool's full schema on every turn.

**Scaling large tool sets (production pattern).** `defer_loading: true` plus a
`tool_search_tool` (regex- or BM25-backed) cuts upfront tool-definition tokens by 85%+, loading
only ~3-5 tools per turn on demand; keep the most-frequently-used tools non-deferred. Tool
descriptions are load-bearing for selection accuracy — Anthropic's own guidance pushes explicit
namespacing (`github_`, `slack_`) and multi-sentence descriptions of *what/when/parameters/caveats*.
Token strategy, in order of application: prompt-cache the stable tool-def prefix → tool search once
the catalog exceeds ~20 tools → context editing to prune stale `tool_result` blocks in long
conversations → programmatic tool calling (collapse chained calls into one sandbox script so
intermediate results never enter history).
([Manage tool context](https://platform.claude.com/docs/en/agents-and-tools/tool-use/manage-tool-context.md),
[Tool search tool](https://platform.claude.com/docs/en/agents-and-tools/tool-use/tool-search-tool.md),
[Writing tools for agents](https://www.anthropic.com/engineering/writing-tools-for-agents),
[Effective context engineering](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents))

---

## 2. Microsoft Agent Framework (MAF)

Repo: `github.com/microsoft/agent-framework` (Python `agent-framework-core` + provider packages,
.NET `Microsoft.Agents.AI` on top of `Microsoft.Extensions.AI`, Go `agent-framework-go`).
Successor to Semantic Kernel's agent layer.

**Tools.** A plain function becomes a tool via `AIFunctionFactory.Create(method)` (.NET) or a bare
Python callable / `@tool`-decorated function. `ChatClientAgent` merges agent-level and per-run
tools into `ChatOptions.Tools`; the underlying `IChatClient` serializes that into the wire schema.
Auto function-calling is the default the instant any tool is registered — SK's explicit
`FunctionChoiceBehavior.Auto()` step was deliberately dropped as unneeded ceremony; choice is still
controllable via `ChatOptions.ToolMode` (`Auto`/`None`/`Required[name]`).
([Function tools](https://learn.microsoft.com/en-us/agent-framework/agents/tools/function-tools),
[SK→MAF migration guide](https://learn.microsoft.com/en-us/agent-framework/migration-guide/from-semantic-kernel/))

**Skills.** SK's `Plugin`/`Kernel`/`[KernelFunction]` chain is gone entirely. MAF instead adopted
the open **agentskills.io** spec: a skill is a directory with a required `SKILL.md`
(YAML frontmatter: `name`, `description`, `allowed-tools`, …) plus `scripts/`/`references/`/`assets/`.
An `AgentSkillsProvider` (C#) / `SkillsProvider` (Python) context provider registers three tools on
the agent — `load_skill`, `read_skill_resource`, `run_skill_script` — with pluggable, composable
sources: file-based, code-defined (`AgentInlineSkill`), class-based (packageable as a NuGet/PyPI
unit), and **MCP-based** (`UseMcpSkills`), where a remote MCP server exposes an index at
`skill://index.json` and skills stream in as `skill-md` (per-file) or `archive` (size/file-count
capped download). A stated trust boundary: **archive-skill scripts fetched from a remote MCP
server are never auto-executed** — untrusted-input treatment for anything an external MCP server
controls.
([Agent Skills](https://learn.microsoft.com/en-us/agent-framework/agents/skills),
[MCP skills discovery](https://devblogs.microsoft.com/agent-framework/discover-agent-skills-from-mcp-servers-in-net/),
[agentskills.io spec](https://agentskills.io/specification))

**Context placement — four-stage progressive disclosure**, explicit in the docs:
1. **Advertise** (~100 tok/skill) — name+description in the system prompt on every run.
2. **Load** (<5000 tok recommended) — full SKILL.md body only once `load_skill` is called.
3. **Read resources** — `read_skill_resource` fetches reference files/templates on demand.
4. **Run scripts** — `run_skill_script` executes bundled code on demand.

`read_skill_resource`/`run_skill_script` are only advertised at all if some skill actually has
resources/scripts — trimming the always-on cost further. Plain (non-skill) tool lists have no
documented ordering/truncation policy beyond "everything goes in `ChatOptions.Tools`" — progressive
disclosure via skills is MAF's answer to the scaling problem, not a flat-tools-array feature.

**Other stated principles.** Side-effecting tools support `approval_mode` (human-in-the-loop gate
before execution); skills expose the same per-tool approval toggle, with an explicit warning to
disable it "only for skills and scripts from sources you trust." Any `AIAgent` can become a callable
tool for another agent via `Agent.AsAIFunction()` — explicitly contrasted with the Handoff pattern
(full control transfer) as a narrower "call and get control back" primitive.
([Tool approval](https://learn.microsoft.com/en-us/agent-framework/agents/tools/tool-approval))

---

## 3. Cross-cutting agreement

Both systems converge on the same shape despite unrelated implementations:

- **Tools/skill *advertisements* are system-prompt-prefix material, placed before conversation
  history** — never interleaved into `messages`. Both docs treat this as what makes the
  advertisement cache-stable turn over turn.
- **Full skill *content* is never front-loaded** — only a short name+description is unconditional;
  the body loads lazily behind an explicit tool call (`load_skill` in MAF; implicit
  selection-then-expand in Claude Skills).
- **Flat tool lists don't get this treatment** — progressive disclosure is specifically a
  skills-layer mechanism in both ecosystems; large flat tool catalogs are handled by a *different*
  mechanism (Anthropic's `tool_search_tool`/`defer_loading`; MAF has no stated equivalent for a
  large flat `ChatOptions.Tools` list beyond routing it through the skills layer instead).
- **Remote/external skill sources are explicitly untrusted input at the point they enter context**
  — MAF states it outright for MCP-sourced skills (no auto-exec of remote archive scripts);
  Anthropic's MCP connector tools carry the same treatment implicitly via its general external-content
  handling.

## 4. Relevance to AgentEngine

`009-Plugin-and-Extension-System.md` §8 already lands on the same interchange format both
surveyed systems converged on independently: **§8a fixes `SKILL.md` (agentskills.io)** as the
format, and **§8b ("Progressive disclosure comes free from the worktree")** is the same
advertise-then-lazy-load shape as MAF's four-stage model and Claude's frontmatter/body split — this
is corroborating evidence the RFC's choice, not new information changing it.

Two points worth carrying into open-question resolution, not automatic action:

- **§8d ("Skills over MCP: we specify nothing yet, deliberately")** — MAF has since shipped a
  concrete design for this exact gap (`UseMcpSkills`, `skill://index.json`, `skill-md`/`archive`
  transfer modes, no-auto-exec-of-remote-scripts trust boundary). If/when §8d is picked back up,
  this is a real prior-art data point, not a decision to adopt it wholesale — MCP-skills is still
  an open question in AgentEngine, and I2/I3 (no ambient authority, model output is never
  authority) would need their own read on whether MAF's approval-gated execution model satisfies
  them.
- The already-fixed `HistoryAndSkillsProvider` bug (skill advertisement was landing *after*
  conversation history, not before — `include/agentengine/core/history_and_skills_provider.hpp`)
  put the advertisement in the position both Anthropic and MAF treat as the only correct one
  (prefix, before history). The recent fix is validated by both external surveys, not merely by
  the e2e proof already in `tests/test_agent_session_skills_live_e2e.cpp`.

No RFC or ADR change is implied by this record on its own — it's background for whoever next
touches §8d or the skills-over-MCP open question.

**Sources:** see inline citations above; primary ones —
[Claude tool use overview](https://platform.claude.com/docs/en/agents-and-tools/tool-use/overview.md),
[Claude Code Skills guide](https://code.claude.com/docs/en/skills.md),
[Anthropic: writing tools for agents](https://www.anthropic.com/engineering/writing-tools-for-agents),
[MAF Agent Skills](https://learn.microsoft.com/en-us/agent-framework/agents/skills),
[MAF function tools](https://learn.microsoft.com/en-us/agent-framework/agents/tools/function-tools),
[agentskills.io](https://agentskills.io/specification).
