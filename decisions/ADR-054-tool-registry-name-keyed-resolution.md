# ADR-054 — `ToolRegistry`, a host-curated name-keyed tool resolution seam

**Status:** Judged (2026-08-14, project owner sign-off). Designed (inherited from
`docs/planning/tool-capability-registry-design-draft.md`'s own already-self-red-teamed sketch),
implemented, and proven (real code + tests, §4). Re-verified at sign-off review:
`tests/test_tool_registry.cpp` (T1-T8) and `tests/test_agent_yaml_compiler.cpp` (F-6) both still pass
in full, unchanged since commit `51083eb`.

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #4 (the finding this ADR
closes) and gap #5's name-keyed `ToolTable`-construction half, which this ADR also closes as a direct
consequence. `docs/planning/tool-capability-registry-design-draft.md` (the design this implements,
unchanged from the draft — this pass found no correction needed, unlike ADR-053's own §2).
`006-Tool-and-Function-Plane.md` (spec.tools' own normative "references tools by name from the
registry" line, previously placeholder text with no mechanics). `015-Declarative-Agent-Format.md` §1/§2
(the declarative surface this closes name resolution for). `include/agentengine/core/agent_yaml_
compiler.hpp` (the one real call site wired this pass).

## 1. The question

**Stated so it has a wrong answer:** given a bare string (`"web_search"`) in a declarative document's
`spec.tools`, does this codebase have any way to resolve it to a real `ToolDescriptor`?

**Before this ADR: no.** 006 has zero occurrences of "registry"; 015 §1 asserts the reference exists
("the declarative form references tools by name from the registry — native, plugin, MCP, A2A") with no
mechanics behind it. `compile_agent_document()`'s own top comment named this explicitly as a real,
blocking gap rather than working around it — `tools`/`capability_ceiling` stayed honestly empty for
every declarative agent, regardless of what `spec.tools` listed.

## 2. What re-grounding against current code found (design draft §0, unchanged by this implementation
pass)

- **`ToolTable` already had a real, tested, DESCRIPTOR-keyed runtime construction path**
  (`from_descriptors()`, `tool_pipeline.hpp`) — what was missing was narrower than gap #5's original
  framing suggested: not "zero runtime construction API," specifically the name → `ToolDescriptor`
  half.
- **`ToolDescriptor` already carried a per-tool `capability_ceiling` field** — the data shape a
  registry entry needs already existed; what was missing was the TRUST RULE for what populates that
  field when the source isn't host-authored C++.
- **Native capability-ceiling binding is a compile-time property** (`capability_ceiling_of<Policies
  ...>()`), trustworthy only because a human wrote the C++ that produced it — that assumption is what
  breaks for a name-resolved (self-reported) tool, not the checking mechanism's own shape.
- **A real, narrower "resolve by name" precedent already existed**: `WasmBackend::list_tools()`
  (ADR-010) self-reports `ToolDescriptor`s from a loaded component, with capabilities bound at the
  manifest/INSTANCE level (`requested_capabilities`), not per-tool — the load-bearing precedent this
  design reuses (§3 below), not a solved instance of this gap (it never had to reconcile a
  self-reported PER-TOOL ceiling against anything).

## 3. The design (as implemented)

Full writeup lives in `core/tool_registry.hpp`'s own top comment; summarized here.

### 3a. `ToolRegistry` — host-curated, never auto-discovered

```cpp
enum class tool_provenance { native, wasm_plugin, mcp_server, a2a_agent };

class ToolRegistry {
public:
    [[nodiscard]] result<void> register_tool(std::string name, ToolDescriptor descriptor,
                                              tool_provenance provenance,
                                              std::vector<Capability> const& outer_grant = {});
    [[nodiscard]] std::optional<ToolDescriptor> find(std::string_view name) const;
    [[nodiscard]] std::optional<std::string> exclusion_reason(std::string_view name) const;
};
```

Mirrors `ChatClientRegistry`'s own shape (`register_*`/`find`, an `unordered_map` behind it) —
`register_agent<A>()` already takes an analogous registry as an optional pointer parameter for the
identical job (resolving a declared identifier against a host-curated table); `compile_agent_document()`
gains the same parameter shape, defaulting `nullptr`.

**Namespace squatting is answered structurally, not by an added check**: nothing is ever
auto-discovered into the registry — a host that wants a WASM plugin's tools available writes code that
calls `register_tool()` for each one, explicitly, after whatever provenance validation (§3b) applies.
Two sources wanting the same name is a HOST-TIME registration conflict, caught by `register_tool()`'s
own fail-closed duplicate check exactly where a human is already looking (host wiring code) — the same
place `check_tool_name_collision()` already catches it for one agent's own native tool list.

**The `nullptr` case preserves today's exact behavior, not a new fail-closed default** — checked
directly, not assumed: `test_agent_yaml_compiler.cpp`'s F-1 (015 §2's own worked example, which DOES
list `web_search`/`code_interpreter`/a handoff) still asserts `compiled_meta.tools.descriptors().empty()`
when no registry is supplied, unmodified, still passing (F-6a re-proves it explicitly under the new
parameter's default). `registry == nullptr` means "no registry machinery available" — `tools` stays
honestly empty, byte-identical to before this ADR. `registry` supplied but a specific named tool absent
from it is the only case that fails closed (F-6c) — a genuinely different situation (a host that bothered
to wire a registry made a specific, checkable claim about what's in it).

### 3b. Capability-ceiling trust for non-native provenance

`check_capability_ceiling()`'s existing shape (`agent_registry.hpp`) — for each tool, walk its declared
`capability_ceiling`, verify a governing `CapabilitySet.contains()` every entry — is reused, not
reinvented, called again at a different layer:

- **Native**: unchanged, no `outer_grant` check runs — the compiler is the trust boundary, a
  hand-authored `ToolDescriptor` is trustworthy by construction.
- **`wasm_plugin`/`mcp_server`/`a2a_agent`**: a tool's self-reported `capability_ceiling` is validated,
  entry-by-entry via the same `.contains()` shape, against the CALLER-supplied `outer_grant` — that
  provenance's own already-host-decided authority (a WASM instance's ADR-010 `requested_capabilities`,
  an MCP connection's configured grant, an A2A delegation's configured grant). A tool whose self-report
  claims more than its own provenance's outer grant covers is EXCLUDED — this ONE tool, never the whole
  batch, never silently widened or clamped (clamping was considered and rejected in the draft: it would
  answer a DIFFERENT question than what the source asked for, failing confusingly later at invocation
  instead of clearly at registration).

### 3c. `ToolTable::from_names()` (gap #5's own remaining half)

```cpp
[[nodiscard]] static result<ToolTable> from_names(std::vector<std::string> const& names,
                                                    ToolRegistry const& registry);
```

Declared in `tool_pipeline.hpp` (next to `from_tools`/`from_descriptors`, where a caller looks for it,
with `ToolRegistry` forward-declared since that header cannot depend on `tool_registry.hpp` — the
dependency runs one direction only), defined out-of-line in `tool_registry.hpp`. For each name:
`registry.find(name)`; if absent, fail closed with `agent.tool_not_found_in_registry`, naming the
specific missing tool; delegate the resolved descriptors to the already-real, already-tested
`from_descriptors()` — no new `ToolTable` machinery.

### 3d. `compile_agent_document()` wiring

Gains an additive `ToolRegistry const* registry = nullptr` parameter (Milestone 5 Phase B6's own
`ChatClientRegistry const*` precedent, same shape). With a registry supplied, each plain-string
`spec.tools` entry resolves via `from_names()`; a `{handoff: ...}` entry (014's workflow/handoff
vocabulary, 015 §2's own worked example has one) is correctly skipped, not mistaken for a tool name —
unchanged scope from before this parameter existed. `spec.capabilities` (YAML capability parsing into
real `cap::` variants) stays a separate, still-open gap named explicitly in the draft's own §4 —
`capability_ceiling` stays empty even with a registry supplied.

## 4. Self-red-team findings (design draft §4, verified still correct against the real implementation)

**MUST-NAME 1 — a real diagnostics requirement, closed with `exclusion_reason()`.** A WASM plugin
advertising 10 tools where 1 fails ceiling validation still registers the other 9 (T4's sibling check,
§4 below, proves this directly — a rejected tool does not fail its whole batch). If a later document
references the excluded 10th by name, `find()` alone returns `nullopt`, indistinguishable from "never
existed." `exclusion_reason(name)` closes this: `std::nullopt` for a name never passed to
`register_tool()` at all (T5); a real, specific string for a name that WAS considered and rejected,
whether by duplicate collision or capability mismatch (T2, T4).

**Checked, not assumed: does per-provenance validation reintroduce a widening bug `.contains()` doesn't
have?** No — `subsumes()` (`trust/capability.hpp`) is already fail-closed by construction (a capped
parent rejects an uncapped or broader request); reusing it against a different outer `CapabilitySet`
depending on provenance inherits that same property rather than introducing a new one to get wrong.
T6 additionally proves an EMPTY `outer_grant` (the parameter's own default) rejects a non-native tool
with ANY declared capability — not a provenance-specific carve-out that could accidentally admit an
unvetted grant.

**A real, deliberately out-of-scope gap, named explicitly: `spec.capabilities` YAML parsing.** Turning
YAML capability descriptions into real `cap::` variant values is unbuilt, its own separately-scoped
follow-up (`compile_agent_document()`'s own file-top comment already named this for a different,
unrelated reason before this ADR). This ADR closes tool-NAME resolution only; a compiled agent still
cannot declare a non-empty capability ceiling from YAML until that separate gap also closes.

**Considered and rejected: resolving tool names lazily, at first invocation.** Would let a document
reference a tool that only becomes available later, but trades a fast, loud, compile-time failure for a
slow, runtime one — rejected for the same reason 002 §6 chose fail-fast for the native path.

## 5. Evidence

`tests/test_tool_registry.cpp` (T1-T8, new): native registration with no outer_grant needed (T1);
duplicate-name rejection leaves the first registration untouched and is diagnosable via
`exclusion_reason()` (T2); a covered non-native ceiling registers (T3); an uncovered one is excluded —
per-tool, not per-batch, and distinguishable from "never offered" (T4, T5); `mcp_server`/`a2a_agent`
get the identical treatment as `wasm_plugin`, including the empty-grant case (T6); `from_names()`
resolves a real `ToolTable` when every name is present (T7) and fails closed with the specific missing
name when one isn't (T8).

`tests/test_agent_yaml_compiler.cpp` (F-6, extended): `registry == nullptr` preserves F-1's own
already-passing "honestly empty" assertion byte-for-byte (F-6a); a supplied registry resolves exactly
the plain-string `spec.tools` entries, correctly skipping a `{handoff: ...}` entry (F-6b); a name absent
from the supplied registry fails closed with the real, specific error code (F-6c).

Full suite: green (this pass), zero regressions.

## 6. What this ADR does not claim

- **Does not build any provider-specific ingestion path.** Calling `tools/list` against a live MCP
  connection, or a WASM component's `list-tools` export, or an A2A delegation's skill listing, and
  turning the response into `register_tool()` calls, is real, separate implementation work this ADR
  specifies the CONTRACT for (validate against a provenance-appropriate outer grant, then register),
  not the integration code itself.
- **Does not resolve `spec.capabilities` YAML parsing** — named explicitly in §4, a separate gap.
- **Does not provide a live-update/re-resolution mechanism.** A `ToolRegistry` snapshot is frozen at the
  moment a document compiles (002 §6's own philosophy, matching the native path); MCP's
  `tools/list_changed` notification and an analogous WASM hot-reload case are not wired to invalidate an
  already-compiled `AgentMetadata`.
- **Does not decide registry scope/lifetime** (one process-wide `ToolRegistry` vs. one per
  session/tenant) — this ADR assumes "whatever the host constructs and passes to
  `compile_agent_document()`," the same way `ChatClientRegistry` already defers that question today.
