# `ToolOptimizerProvider` — on-demand tool loading over MCP/plugin/agent sources — design draft

**Status: designed, not implemented.** Matches this project's `design → red-team → prove → judge`
discipline (CLAUDE.md) applied honestly before any code lands, per `schedule-wakeup-standing-effect-
design-draft.md`'s own precedent for this class of document.

**Correction to this draft's own premise, found during verification — read this before anything
else below.** The originating conversation framed this as "reopening OQ-18." **That premise is
wrong, and the correction changes what this document actually is.**

## 0. OQ-18 is already resolved, already accounted for MAF's provenance stamping, and this design does not reopen it

`OpenQuestions.md`'s OQ-18 entry (`## Resolved`, not `## Open`) already asked exactly the question
re-raised this session — "should `ContextProvider` composition become a sequential pipeline, like
MAF's?" — and already read the exact same MAF source this session read
(`AIContextProvider.cs:174-176`, `WithAgentRequestMessageSource`, cited by line number in OQ-18's
own text). It drafted a concrete opt-in `ReactiveContextProvider` design, red-teamed it, and
rejected it for five reasons, of which "no provenance" is only the first — the other four survive
even if provenance stamping is added:

1. No provenance (the point this session rediscovered) — but MAF's stamping was already known and
   already weighed, not missed.
2. Reopens the exact cross-contributor coupling `context_assembly.hpp`'s budget rule already
   rejects, moved from the budget layer to the content layer.
3. "Zero-cost, opt-in" doesn't survive `ContextProviderDescriptor`'s `std::function` type-erasure
   boundary without either widening every descriptor's signature or adding a real per-iteration
   branch.
4. A no-code, principle-only entry would break this project's own ADR discipline (every existing
   ADR pairs a decision with real, implemented, tested code).
5. **The composite pattern already gives *better* provenance than message-stamping would** — a
   composite's own code calls each sub-provider directly, so it knows exactly which one produced
   what by construction (C++ type identity), with no string-tag reconstruction and no risk of a
   spoofed or mismatched stamp — at zero cost to every other provider that doesn't need it.

The decision's own stated bar for reopening: **"Revisit only if a *third* independent pairing shows
the composite pattern doesn't scale — and if so, build and prove it as a real ADR with the
provenance question answered as part of the implementation, not deferred again."**

**Does `ToolOptimizerProvider` clear that bar? No.** §2 below designs it as an ordinary composite —
the exact `HistoryAndSkillsProvider` idiom OQ-18 already prescribes — and it works. It is not
evidence the composite pattern fails to scale; it's the pattern's second real proof point (after
`HistoryAndSkillsProvider`), not a counterexample. **This document is therefore not a reopening of
OQ-18. It's the concrete design OQ-18's own resolution told a future reader to write** when a real
reactive need materialized. `assemble_context`/`ContextProviderDescriptor`/`ContextProvider` need
zero changes.

One separable, smaller idea survives from the original conversation, kept in §5 as its own
low-priority open question rather than folded into this design: whether stamping `ContextContribution`
with per-contributor provenance is worth doing **on its own merits**, for I4 audit/forensics, wholly
independent of chaining. Not designed here — flagged only.

## 1. What `ToolOptimizerProvider` is for

Two problems from this session's earlier research, both real and cited:
- `union_codeact_tools` (`include/agentengine/core/codeact_tool_union.hpp:24-37`) unions four tool
  sources — agent's own tools, skill-unlocked tools, **MCP-discovered tools, WASM-plugin tools** —
  and only the skill branch arrives pre-scoped (`skill_unlocked_tools`, already filtered by
  `scope_tools_to_mounted_skills`). `mcp_tools`/`wasm_tools` are raw `std::vector<ToolDescriptor>`,
  unioned unconditionally. A connected MCP server's full tool surface is declared the moment it's
  bound — no on-demand gate exists for it today, unlike skills.
- Large MCP ecosystems can exceed 200k tokens of tool-schema alone
  (`docs/research/2026-08-20-scaling-llm-capability-and-the-safety-surface.md` §6, MCP-Zero
  arXiv:2506.01056) — tool-selection accuracy measurably degrades well before that.

`ToolOptimizerProvider` is a composite `ContextProvider` that gates the MCP/plugin/agent-tool branch
the same way `mount_skill` (`009 §8c` Phase 3, `core/mounted_skills_state.hpp`) already gates the
skill branch: small default surface, three always-on management tools
(`search_tools`/`mount_tool`/`unmount_tool`) to grow it on demand, mid-task, agent-driven — matching
MCP-Zero's own finding that model-generated capability requests match tool documentation better than
requests inferred from raw user queries.

## 2. Design — an ordinary composite, not a new generic mechanism

```cpp
// core/tool_optimizer_provider.hpp (sketch, not implemented)
class ToolOptimizerProvider {
public:
    ToolOptimizerProvider(AgentToolSource agent_tools, McpToolSource mcp_tools,
                           PluginToolSource plugin_tools, std::vector<std::string> always_on = {});

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&);
    [[nodiscard]] task<std::monostate> on_turn_end(TurnView, EffectContext&);

private:
    AgentToolSource    agent_tools_;
    McpToolSource      mcp_tools_;
    PluginToolSource   plugin_tools_;
    std::vector<std::string> always_on_;
    std::vector<std::string> mounted_;   // real, persistent, per-run state — mounted_skills_state.hpp's
                                          // own property, re-consulted fresh every turn, never living
                                          // only inside a transcript message
};
```

`on_context` calls its three owned sources **directly, as plain function calls** — not through
`assemble_context`'s generic fan-out — builds the "universe" `ToolTable` (the same shape
`union_codeact_tools` already assembles), filters it to `always_on_ ∪ mounted_` (reusing
`scope_tools_to_mounted_skills`'s exact filter shape, generalized off "skills" to any named set), and
adds the three management tools unconditionally. This is precisely the `HistoryAndSkillsProvider`
idiom: `ToolOptimizerProvider` knows the concrete types of its three sources at compile time, calls
them itself, and the "reactivity" (deciding what's currently visible based on what came before) lives
entirely inside its own ordinary C++ code — never through the generic `ContextProviderDescriptor`
seam `assemble_context` iterates.

Once built, it plugs into the existing outer composition unchanged:
`ComposedContextProvider<HistoryProvider<...>, SkillsProvider, ToolOptimizerProvider>` — because at
that outer level it is genuinely independent of History/Skills (fan-out is correct there), exactly
matching how `HistoryAndSkillsProvider` already composes two independent contributors today.

**Three management tools, matching `mount_skill`'s own trust shape exactly:**
- `search_tools(query)` — read-only, ranks the full universe (name/description match) against
  `query`, returns candidate names as an ordinary tool result. Does not mount anything itself —
  keeps the agent, not a heuristic, deciding what to request next (the MCP-Zero finding).
- `mount_tool(name)` / `unmount_tool(name)` — mutate `mounted_` only. **Grant no new capability**:
  the underlying agent/MCP/plugin tool sources are already capability-bound at session/run start
  through the ordinary `007` grant flow; these two tools only move the visibility window over an
  already-authorized set — identical in shape to `mount_skill`'s own documented property
  (`009 §8c`: "grants no new capability... mounting only activates something already
  pre-authorized"). All three are zero-capability, always-available, same precedent as `mount_skill`.

## 3. The one integration point that must not be gotten wrong

`skill_tool_scoping.hpp`'s own file-top warning states the rule this design inherits verbatim:
filtering only what's *declared* to the model while leaving a broader table at the `invoke_tool(...)`
call site is **cosmetic, not a real restriction**. Today, `union_codeact_tools`'s `mcp_tools`/
`wasm_tools` parameters are raw and unscoped, and whatever `ToolTable` results feeds **both** the
model-visible declaration and the live `invoke_tool()` authorization check (per that file's own
comment, the two must never be independently constructed).

Adopting `ToolOptimizerProvider` means the call site currently feeding `union_codeact_tools`'s
`mcp_tools`/`wasm_tools` must instead feed **`ToolOptimizerProvider`'s own currently-scoped subset**,
recomputed from the same `mounted_` state at the same cadence for both the declaration path (this
provider's `on_context`, once per turn) and the invocation path (whatever builds the live table for
`invoke_tool`/`invoke_agent_tool`, once per round) — never computed once for one purpose and
separately, at a different point, for the other. This is the single highest-risk implementation
detail in the whole design; getting it wrong reintroduces exactly the "hidden but still callable"
defect class `009 §8c`'s Phase 2/3 amendments already had to close once for skills.

## 4. Self-red-team

Checked against the four specific failure classes named for this pass:

- **Can a chained provider inject tainted content into a later provider's declared instructions
  without a visible declassification point?** Does not apply to this design as scoped — unlike
  OQ-18's rejected `ReactiveContextProvider`, `ToolOptimizerProvider` never reads another
  `ContextProvider`'s `ContextContribution` (instructions or messages) at all. It only composes raw
  `ToolDescriptor` sources (MCP/plugin/agent), which is a narrower, already-covered case: a
  `ToolDescriptor`'s `name`/`description`/schema are already tainted, capability-gated, tool-pipeline
  content (`006 §7`), not new trust surface this design introduces.
- **Does provenance leak anything sensitive (026 §8 G3 "no leakage")?** Real risk, concretely
  named: if `search_tools`' results or `mount_tool`'s error messages ever surface which underlying
  MCP server/transport a tool came from using anything beyond an operator-configured, human-readable
  label — a raw connection string, internal hostname, or backing type name — that's a G3 violation.
  Must be scrubbed at the same boundary `026 §8 G3`'s hostile corpus already tests against, before
  implementation, not discovered after.
- **Does bounding to "tools only" actually close the gap MAF's docs warn about, or move it?**
  Closes it for this design specifically, because — see the first bullet — this was never message/
  instruction chaining to begin with; it's composition over tool *sources*, a narrower and
  already-safe shape. It would not generalize as a defense if someone later tried to extend
  `ToolOptimizerProvider` to also compose instructions/messages from other providers — that would be
  the rejected `ReactiveContextProvider` shape again, under a new name, and inherits all five of
  OQ-18's original objections.
- **Replay (I5) — if a chained decision depends on the same turn's earlier state, is it still
  deterministic?** Yes, by construction, and for a clean reason: `search_tools`/`mount_tool`/
  `unmount_tool` are ordinary tools, invoked through the normal ten-step pipeline (`006 §3`), so
  their calls already appear as `ToolCallStarted`/`ToolCallFinished` on the run's own event stream —
  replayable exactly like any other tool call. `mounted_` changes only as a **direct, recorded**
  consequence of those calls, never as an unlogged side effect the provider computes independently
  each turn. The one way to violate this: a future "preload based on history" heuristic (floated in
  the originating conversation, not designed here) must be a pure function of already-recorded
  transcript state (e.g. counting prior `ToolCallFinished` events) — if it instead calls out to an
  external, cross-run usage-stats service or a live embedding model *outside* the tool-call pipeline,
  that introduces new nondeterminism needing its own recorded seam, not covered by this draft.

**Net result of the red-team:** the design survives essentially intact, but for a narrower reason
than the originating conversation assumed — it's safe because it was never really "chaining" in the
OQ-18 sense, not because chaining itself was made safe. §3's declare/invoke-parity point is the one
real implementation risk, not a design-level one.

## 5. Separable, lower-priority open question — not designed here

Whether stamping `ContextContribution` (messages/tools) with per-contributor provenance is worth
doing **independent of chaining**, purely for I4 (every effect attributable) — right now, if
something bad lands in context, there's no way to attribute which contributor's output it was after
the fact, only by re-reading each contributor's own code. This is a real, separate, smaller
question — flagged here, not designed, and explicitly not a re-litigation of OQ-18's own
already-judged "chaining" question.

## 6. What this draft is not

Not an implementation. Not an ADR. Not a reopening of OQ-18 (§0). If this design is picked up for
real, it should land as ordinary M6/M7-track feature work — a new composite `ContextProvider`
conformer following an already-proven, already-judged pattern — not as a contested design requiring
its own `design → red-team → prove → judge` cycle the way a genuine OQ-18 reopening would, since it
changes nothing about `assemble_context`, `ContextProviderDescriptor`, or the `ContextProvider`
concept itself.
