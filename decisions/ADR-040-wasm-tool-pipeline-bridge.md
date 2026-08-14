# ADR-040 — How does a loaded `WasmBackend` component's tools become reachable through the real 006 §3 tool pipeline, without opening a new authority-crossing hole between an agent's capability ceiling and a plugin's own capability grant?

**Status: Proposed** — self-graded against the falsifiable claims below (§4-§7), same posture as
ADR-037/ADR-039's own status line: per this project's governance, only the project owner marks an
ADR "Judged." Treat §7's verdicts as evidence for that sign-off, not as one.

**Resolves:** `src/backends/wasm/wasm_backend.hpp`'s own long-standing note that `WasmBackend`
(Milestone 2 Phase D, `decisions/ADR-010-wasm-component-host-manifest-capability-binding.md`) is
"tested standalone... not wired into `core/tool_pipeline.hpp`'s `ToolTable`/`invoke_tool`." RFC
006 §2 already lists "WASM plugin" as an expected tool source alongside native/MCP/A2A under the
same "one tool list" uniformity rule — this ADR is what makes that real, mechanically.

## 1. The question

`WasmBackend::invoke_tool()` already enforces a real, proven authority boundary (ADR-010 §3.2/§3.3):
a plugin's guest code may only reach the capabilities its manifest requested AND the host operator
granted at `create()` time. That answers "what may the plugin's own code touch." It does not answer
a different question this ADR must: "may THIS agent, in THIS run, call this specific plugin tool at
all" — the same I2 question `core/tool_pipeline.hpp`'s `invoke_tool()` already answers for every
other tool source via `ToolDescriptor::capability_ceiling` checked against the run's own `held`
`CapabilitySet`. Wiring a wasm-discovered tool into that pipeline for the first time means answering
it without accidentally treating the plugin's own internal capability grant as if it were the same
thing, or silently skipping the agent-level check because the plugin-level one already exists.

## 2. Design (accepted)

Precedent, not invention: `protocol/mcp/mcp_tool_bridge.hpp`'s `mcp_tools_as_descriptors_from()`
already solved the identical shape of problem for MCP-discovered tools — gate each adapted
descriptor behind exactly one `cap::ToolCall{name}`, independent of whatever the external source's
own internal authority model is. `backends/wasm/wasm_tool_bridge.hpp`'s
`wasm_tools_as_descriptors_from()` does the same:

- `capability_ceiling = {cap::ToolCall{plugin_id + "::" + tool_name}}` — the agent-level gate. This
  is deliberately **not** the plugin's `manifest.requested_capabilities` (FsRead/NetOut/etc.) —
  those stay entirely inside `WasmBackend::invoke_tool()`'s own, already-proven, separate check
  against `operator_grant`. Two independent authority layers, checked at two different points by two
  different mechanisms, is not a double-binding conflict: the outer pipeline's step 4/7 (`held.bind`)
  answers "may this run call this tool," and only if that succeeds does execution ever reach
  `WasmBackend::invoke_tool()`'s own internal bind against `operator_grant`, which answers "what may
  the guest code touch while it runs."
- **Delimiter-collision guard, fail closed.** `PluginManifest.id` is plugin-author-supplied, not
  host-assigned, so naive `plugin_id + tool_name` concatenation would let `(id="x", tool="y::z")`
  and `(id="x::y", tool="z")` collide onto the same `cap::ToolCall` name — a real spoofing path one
  plugin could use to impersonate another's capability grant. `wasm_tools_as_descriptors_from()`
  rejects either input containing the `"::"` delimiter (`wasm.tool_name_ambiguous`) before building
  any descriptor, rather than escaping — simpler, and a plugin/tool id needing `::` in its own name
  has no legitimate reason to.
- **`captures_session_state = true` on every produced descriptor.** The adapted `invoke` closure
  holds a `shared_ptr<WasmBackend>` plus a handle into live, mutable `WasmBackend::instances_` state
  — exactly the condition ADR-028 introduced this field to flag, so `background_task()`
  (`tool_pipeline.hpp`) structurally refuses to detach a thread holding this reference with no
  synchronization against `WasmBackend::destroy()`.
- **Output-content scope, a fresh decision** (distinct from ADR-010 §3.1's *input*-direction
  blob/tool-call residual): `tool_pipeline.hpp`'s step 9 wraps `ToolDescriptor::invoke`'s entire
  return into exactly one `Data` `ContentItem` — there is no path today for a tool's success reply to
  become a `Media`/blob item. `wasm_tool_result_to_json()` fails closed
  (`wasm.tool_result_unsupported_content`) if a wasm tool's result contains a `Media` item anywhere,
  rather than silently dropping it; a `Data` item's `.json` is decoded back to `json::Value`; a
  `Text` item wraps as `{"text": ...}`; empty content maps to `null`.
- **Taint is inherited for free**, same as the MCP/native_jail bridges already document: the wasm
  guest's own WIT `text-item.tainted` bit is never consulted — `invoke_tool()`'s step 9
  unconditionally taints every successful result's content regardless (006 §7).

**Explicitly out of scope, named not silently dropped:**
- **No production loader.** This bridge takes an already-`create()`d, already-`load_component()`d
  `handle` — it never calls either itself, matching `mcp_tools_as_descriptors()`'s identical
  assumption toward an already-connected `McpClient`. Confirmed by direct search: `load_component()`
  has exactly one caller anywhere in this repo today, `tests/test_wasm_backend.cpp`. No
  session/registry/CLI loader exists yet.
- **`PluginManifest.id` uniqueness is unenforced.** Nothing today stops two different loaded
  components from being given the same `plugin_id` string by whatever future loader constructs their
  manifests; the delimiter guard above prevents *ambiguous concatenation*, not *duplicate ids*. A
  real loader must assign or validate uniqueness — this ADR does not build one.
- **Manifest approval/signature verification is unchanged** — ADR-010 §5 finding F6's residual
  ("the caller of `create()` ... IS the trusted operator-grant boundary by construction") applies
  identically here; this bridge adds no new trust in that direction.

## 3. Competing design considered and rejected

**Gate wasm tools behind their own `manifest.requested_capabilities`, re-exposed on the outer
`capability_ceiling`.** Rejected: it would require the agent's own declared ceiling to enumerate a
plugin's internal capability needs (FsRead, NetOut, ...) just to call one tool, coupling agent
authoring to plugin internals and widening what an agent-level grant has to say — the opposite of
I2's attenuation-only posture. It would also let two different capability *models* (agent-ceiling
kinds vs. plugin-manifest kinds) drift apart the moment a plugin's manifest changes without the
agent's declaration changing to match. The `cap::ToolCall`-only gate, already proven for MCP, keeps
the two models independent by construction.

## 4. Falsifiable claims

| # | Claim | Disproven by |
|---|---|---|
| W1 | An agent-level `held` `CapabilitySet` missing the specific `cap::ToolCall{plugin_id::tool_name}` denies the call, regardless of what the wasm instance's own `operator_grant` holds. | The call succeeding, or denying for the wrong reason, when `operator_grant` is fully granted but `held` lacks the `ToolCall` entry. |
| W2 | A `plugin_id` or tool name containing `"::"` is rejected before the backend is ever touched. | The delimiter check running after `list_tools()`, or two colliding `(plugin_id, tool_name)` pairs producing the same descriptor name. |
| W3 | A wasm tool result's `is_error: true` maps to a real pipeline error, never a silently successful empty result. | `is_error: true` producing a successful `ToolResult`. |
| W4 | A `Media` content item anywhere in a wasm tool's result fails the adapted call closed, never silently dropped. | A result containing a `Media` item returning successfully with that item missing, unremarked. |
| W5 | A wasm-hosted tool, adapted and unioned via `union_codeact_tools()`, is reachable through the real `agentengine::invoke_tool()` end-to-end — the reply is the real guest's own computed output, not a stub. | The call failing for a structural reason, or the reply not matching what the real component actually computed. |
| W6 | `union_codeact_tools()`'s cross-source collision check fires against a wasm-sourced name clash, the same as it already does for agent/skill/MCP. | A wasm-sourced name colliding with another source's tool silently resolving by precedence instead of being rejected. |

## 5. Red-team

- **Name-collision spoofing (W2)** — the actual attack the delimiter guard closes: without it, a
  malicious or careless plugin loader could construct `(plugin_id, tool_name)` pairs that collide
  onto an already-granted `cap::ToolCall` name belonging to a different, legitimately-trusted plugin,
  letting an agent's grant for one tool silently authorize a different one. Closed structurally
  (reject the delimiter in either input) rather than by trusting callers to escape correctly.
- **A hostile plugin claiming `tainted: false`** — moot by construction: `invoke_tool()`'s step 9
  taints every successful result unconditionally, so nothing this bridge's own mapping does to the
  wasm guest's self-declared taint bit (it's never read) can affect the outer result's real taint.
- **`WasmBackend::destroy()` racing a live `ToolTable` built from this bridge** — named, not solved:
  if a caller destroys the handle while a `ToolTable` built from `wasm_tools_as_descriptors_from()`
  is still reachable, subsequent calls fail (the handle no longer resolves inside `WasmBackend`),
  not silently succeed against stale state — but no synchronization prevents the race itself. Same
  posture as the pre-existing `create()`-before-`load_component()`-before-`list_tools()` ordering
  contract `WasmBackend` already places entirely on its caller; this bridge does not strengthen it.
- **Double-authority-layer confusion** — considered directly: could the outer `cap::ToolCall` check
  and the inner `operator_grant` check ever disagree in a way that's exploitable? No — they gate
  different things (call permission vs. guest-code capability) and neither can be satisfied by
  forging the other: `cap::ToolCall`'s payload is a plain string compared by exact equality
  (`capability.hpp`'s `subsumes_payload`), and `operator_grant` is host-configured at `create()`,
  entirely outside this bridge's own code path.

## 6. Evidence

`tests/test_wasm_tool_bridge.cpp`:
- W1 — "I2 gate" block: `held` without the `ToolCall` entry, `operator_grant` fully granted, denies
  with `tool.capability_not_held`.
- W2 — "delimiter guard" block, against a never-created backend/handle (proves the check precedes
  any backend call); adaptation-time check for a tool name containing `"::"`.
- W3, W4 — `wasm_tool_result_to_json()` unit tests against hand-built `ToolResult` values (the real
  fixture has no tool producing `is_error: true` or a `Media` item, so these are proven directly
  against the pure mapping function both the real bridge and these tests call — single source of
  truth, not two implementations that could drift).
- W5 — "e2e" block: the adapted `echo` tool, unioned into a real `ToolTable`, invoked through the
  real `agentengine::invoke_tool()`; the returned text is asserted equal to the real dumped JSON
  arguments sent, round-tripped through the real compiled `ae:tool` component
  (`tests/fixtures/wasm_ae_tool_fixture`), not a stub. The `now` tool separately exercises the
  Data-content mapping path.
- W6 — "union collision" block: a synthetic MCP-sourced descriptor colliding with a wasm-adapted
  descriptor's name is rejected with `codeact.tool_name_collision_across_sources`.

SKIPs (CTest `SKIP_RETURN_CODE 77`) when the `cargo-component` toolchain is unavailable, same posture
as `test_wasm_backend.cpp` — except the pure-logic W3/W4 unit tests and the W2 delimiter guard, which
run unconditionally regardless of fixture availability.

## 7. Verdicts

Self-graded pending owner sign-off (see Status above). All six claims (W1-W6) hold against the
evidence in §6 as implemented; no claim required a design change once written (unlike ADR-010's own
§7.5 findings, which were caught only during proving) — the design in §2 was validated against a
dedicated pre-implementation review pass before any code was written, which is where the delimiter
guard and `captures_session_state` requirement were caught and folded into the design rather than
found afterward.

## 8. Residuals carried forward

- No production loader exists yet (§2) — the next real gap whoever wires this into an actual agent
  session or CLI will hit first.
- `PluginManifest.id` uniqueness is unenforced (§2).
- `WasmBackend::destroy()` vs. a live `ToolTable` built from this bridge is a named, not solved, race
  (§5) — the same caller-responsibility posture `WasmBackend` already places on `create`/
  `load_component`/`list_tools` ordering.
- Manifest approval/signature verification remains ADR-010 §5 F6's unresolved residual, unchanged.
- `Media`/blob wasm tool results are not supported — fail closed, not silently dropped (§2, §4 W4) —
  a real follow-on decision (e.g. base64-embedding into the `Data` JSON payload) if a plugin ever
  needs to return binary output through this path.
