# ADR-065 — `ToolOptimizerProvider`: on-demand MCP/plugin/agent tool gating

**Status:** Judged — 2026-08-20. As-built: the decision, the real implementation, and its tests all
land together (matching ADR-024's own role for `mount_skill`), not a pre-implementation
contested-design cycle — issue #15's own design draft (§6) already established this doesn't need one,
since it changes nothing about `assemble_context`/`ContextProviderDescriptor`/`ContextProvider`
themselves.
**Relates to:** issue #15, `docs/planning/tool-optimizer-provider-design-draft.md` (the design this
records the build-out of), `OpenQuestions.md` OQ-18 (already resolved; this is not a reopening — see
§1), 009 §8b/§8c (`mount_skill`'s own precedent, and its own no-search finding), ADR-024 §8 (the
mount-visibility cadence invariant this class inherits, and the unmount gap it left open), 006 §3 (the
ten-step tool pipeline), `core/skill_tool_scoping.hpp` (the declare/invoke-parity warning this class's
own file-top comment quotes verbatim), `core/codeact_tool_union.hpp` (the cross-source collision
machinery this class reuses rather than reimplements).

## 1. The question, and why this is not OQ-18 reopened

Large MCP ecosystems can exceed 200k tokens of tool-schema alone before tool-selection accuracy
degrades (MCP-Zero, arXiv:2506.01056, cited in
`docs/research/2026-08-20-scaling-llm-capability-and-the-safety-surface.md` §6), and
`union_codeact_tools` (`core/codeact_tool_union.hpp`) unions a connected MCP server's or loaded WASM
plugin's *entire* tool surface unconditionally the moment it's bound — no on-demand gate exists for
either source today, unlike skills (`mount_skill`, 009 §8c).

The originating conversation framed this as reopening `OpenQuestions.md`'s OQ-18 ("should
`ContextProvider` composition become a sequential, MAF-shaped pipeline?"). It doesn't: OQ-18 already
read the same MAF source this session re-verified directly against the local checkout
(`D:\GitSrc\agent-framework`'s `AIContextProvider.cs:174-176`, `WithAgentRequestMessageSource`, and its
own `InvokingContext.AIContext` doc comment confirming MAF really does chain providers sequentially),
and rejected a generic reactive/chained-provider seam for five reasons — only one of which was "no
provenance." `ToolOptimizerProvider` is an ordinary composite `ContextProvider`, the exact
`HistoryAndSkillsProvider` idiom OQ-18's own resolution prescribes for a real reactive need: it calls
its three tool sources directly, as plain function calls inside its own `on_context()`, and never reads
another `ContextProvider`'s `ContextContribution`. `assemble_context`/`ContextProviderDescriptor`/
`ContextProvider` needed, and got, zero changes.

## 2. What was built

`include/agentengine/core/tool_optimizer_provider.hpp` — `ToolOptimizerProvider`, a composite
`ContextProvider` gating an agent-tools `ToolTable` plus two `ToolSourceFetch` closures (MCP/plugin)
behind three always-on, zero-capability management tools (`search_tools`/`mount_tool`/`unmount_tool`),
the same trust shape as `MountSkillTool` (`tools/cli_chat.cpp:311-330`): `Tool<T,
EffectClass<pure>>`, no `Capabilities<...>`, a dead-poison `invoke()`, real logic reached through
`make_tool_descriptor_with_invoke()`'s closure into the provider's own state.

Two design-draft sketch types didn't exist and needed real shapes: `AgentToolSource` became a plain
`ToolTable` (`union_codeact_tools`'s own first-source type); `McpToolSource`/`PluginToolSource` became
one shared `ToolSourceFetch = std::function<result<std::vector<ToolDescriptor>>(EffectContext&)>`,
re-invoked fresh every `on_context()` call (never cached) — a caller wires
`protocol/mcp/mcp_tool_bridge.hpp`'s `mcp_tools_as_descriptors(client)` or
`src/backends/wasm/wasm_tool_bridge.hpp`'s `wasm_tools_as_descriptors_from(backend, handle, id, ctx)`
in directly; `no_tool_source()` covers "not connected." The "universe" is built by reusing
`union_codeact_tools(agent_tools, ToolTable::from_tools<>(), mcp, plugin)` (empty skill-unlocked-tools
slot — skills stay `SkillsProvider`'s own concern) — its already-tested cross-source collision
rejection, not reimplemented — plus one additional check rejecting any source tool that shadows one of
the three reserved management-tool names. Visibility is `always_on ∪ mounted_`, filtered via
`scope_tools_to_mounted_skills` (already name-generic, no generalization needed).

## 3. Two real divergences from `mount_skill`'s own precedent

Named plainly, not left implicit — the design draft's own §2 claim of matching `mount_skill`'s trust
shape "exactly" is true only for the "grants no new capability" property; two of the three management
tools are genuinely new territory:

- **`search_tools` has no precedent anywhere in this codebase.** 009 §8b states outright: "Neither MAF
  nor anything else surveyed does vector/semantic search over skills or tools; 'many skills' is solved
  by advertising cheaply and loading lazily, not by search, in both designs." Implemented here as a
  cheap, deliberately non-semantic substring/keyword match over `name`+`description` — matching that
  spirit (no new ML dependency for a ranking need this small), not contradicting it: MCP-Zero's own
  finding is that a large MCP surface specifically needs *some* discovery mechanism once "advertise
  everything" stops being cheap, which is exactly the regime `mount_skill`'s own skill counts never
  reached.
- **`unmount_tool` has no precedent.** `mount_skill` has no unmount counterpart at all, and ADR-024 §8
  names "`MountedSkillsState` has no expiry/unmount within a run" as an explicit, never-closed residual.
  Closed here, for this mechanism only — `core/mounted_skills_state.hpp` is untouched; `mounted_` is a
  plain private `std::vector<std::string>` member of `ToolOptimizerProvider` itself, not a
  generalization of `MountedSkillsState`.

## 4. Properties verified, and by which test

`tests/test_tool_optimizer_provider.cpp` (36 checks, all passing):

- **Declare/invoke cadence** (skill_tool_scoping.hpp's own warning: the model-declared table and the
  `invoke_tool()`-authorized table must be the same table, recomputed from the same state, same
  cadence) — R2, mirroring `test_on_demand_skill_mount.cpp`'s own R2 exactly: a not-yet-mounted tool is
  genuinely rejected by the real `invoke_tool()` (`tool.unknown_name`, not merely undeclared), and the
  identical call succeeds once mounted, through the identical pipeline. This holds for free because
  `agent_session.hpp`'s `run_rounds()` already builds exactly one `ToolTable` per turn from
  `on_context()`'s own output and reuses it for every `invoke_tool()` call that turn — confirmed by
  reading `agent_session.hpp:1645/1749/1793` directly, and re-proven end to end through a real
  `rt::AgentSession` run in R6 (a scripted `mount_tool` call; the newly-mounted fixture tool is absent
  from the first outbound `ChatRequest.tools` and present on the second).
- **No leakage (026 §8 G3)** — R5: structurally, `ToolDescriptor` carries no connection-string/
  hostname/backend-type field for anything to leak from; verified that `mount_tool`'s error text for an
  unrelated unknown name never echoes another tool's operator-authored description.
- **Replay (I5)** — not independently tested; holds by construction, not by a dedicated test, because
  `search_tools`/`mount_tool`/`unmount_tool` are ordinary tools reached only through the normal
  ten-step pipeline (already replay-proven elsewhere), and `mounted_` changes only as a direct,
  recorded consequence of those calls.
- **Composition** — R6 Part A/B: `ComposedContextProvider<HistoryProvider<Window<0>>, SkillsProvider<>,
  ToolOptimizerProvider>` is the second real (non-test-fixture) 3-provider production composition in
  the tree (the first, and previously only, proof of `ComposedContextProvider<Ms...>` past two
  providers was `test_composed_context_provider.cpp`'s own synthetic fixture types). Confirms skills
  and tool-optimizer state stay independent, and the composite occupies `AgentSession`'s provider slot
  and runs a real turn end to end.

## 5. Residual/deferred — explicitly out of scope here

- **cli_chat.cpp live wiring.** This lands as a library feature plus tests only, per an explicit scope
  decision when this issue was picked up — `cli_chat.cpp`'s own single `union_codeact_tools` call site
  doesn't wire MCP/WASM sources in at all today (both default to `{}`), so adopting
  `ToolOptimizerProvider` there is greenfield exposure, not a refactor of something already live, and
  is real, separately-scoped follow-up work, not attempted in this pass. `currently_scoped_tools()` is
  exposed on the class specifically so that future call site can source a CodeAct-bridge union's
  `mcp_tools`/`wasm_tools` arguments from the same instance's own state, per this file's own
  integration-invariant comment (§3 of the design draft) — not sourced independently.
- **Real semantic/embedding-based `search_tools` ranking.** Deliberately out of scope (§3 above) —
  substring/keyword match only.
- **Cross-run mount persistence.** `mounted_` is per-instance, per-run state — matches
  `MountedSkillsState`'s own "snapshotted per run" framing, not attempted to be widened here.
- **Per-contributor `ContextContribution` provenance stamping (I4).** The design draft's own §5 —
  separable, not designed there, not touched here; tracked as its own open question (see
  `OpenQuestions.md`).
