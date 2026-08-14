# Design draft: a name-keyed tool/capability registry

**Status:** Design draft, self-red-teamed once (below) — **not an ADR, no code written**. Per
project-owner direction (2026-08-14): document what this needs, do not implement yet, matching this
pass's own precedent for `docs/planning/linux-native-jail-pivot-root-containment-design-draft.md`.
Seeds a future ADR once a milestone claims this work. Companion:
`docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #4 (the finding this document
addresses) and gap #5 (the name-keyed half of `ToolTable`'s construction API, which this design
directly unblocks — see §5).

## 0. Re-grounding the question, not just restating the audit

Gap #4's one-line framing: *"No tool/capability name-keyed registry... a real design must resolve I2
capability-ceiling binding, I3 resolution timing, namespace squatting, and WASM-ABI conformance from
scratch."* Re-verified directly against current code (2026-08-14), not carried over from the audit's
own 4-day-old snapshot:

- **006/015 really do just assume a registry exists.** 006-Tool-and-Function-Plane.md has zero
  occurrences of "registry." 015 §1 says only: *"the declarative form references tools by name from
  the registry (native, plugin, MCP, A2A)"* — no mechanics anywhere. The audit's "placeholder text
  with no real content" characterization holds up; this is a genuine, currently-unresolved design gap,
  not a stale artifact.
- **`ToolTable` already has a real, tested, *descriptor*-keyed runtime construction path** —
  `ToolTable::from_descriptors(std::vector<ToolDescriptor>)` (`include/agentengine/core/
  tool_pipeline.hpp:164`), added 2026-08-10, used in five real call sites. What's actually missing is
  narrower than gap #5's original one-line summary suggested: not "zero runtime construction API," but
  specifically the **name → `ToolDescriptor`** half. This design closes exactly that half.
- **`ToolDescriptor` already carries a per-tool `capability_ceiling` field**
  (`tool_pipeline.hpp:55`, *"from the tool's declared `Capabilities<...>`"*) — the data shape a
  registry entry needs already exists. What's missing is not a field to add; it's the *trust rule*
  for what populates that field when the source isn't host-authored C++ (§2).
- **Native capability-ceiling binding is a compile-time property, not a runtime lookup.**
  `capability_ceiling_of<Policies...>()` reads a `Capabilities<...>` policy tag at compile time;
  `check_capability_ceiling()` (`agent_registry.hpp:339`) then verifies, per tool, that the agent's own
  derived `CapabilitySet` `.contains()` that tool's declared ceiling. This check's implicit assumption
  — that `tool.capability_ceiling` is already trustworthy — holds for native tools only because a
  human wrote the C++ that produced it. That assumption is the actual thing that breaks for a
  name-resolved tool, not the check's *shape* (§2).
- **A real, working "resolve by name" precedent already exists, at a narrower scope than gap #4
  asks for.** `WasmBackend::list_tools()` (ADR-010) calls a loaded component's own `list-tools` WIT
  export and returns self-reported `ToolDescriptor`s. Capabilities, though, are bound at the
  **manifest/instance** level (`requested_capabilities`, fixed by the *host* at load time, enforced by
  the sandbox) — not per-tool. This is the load-bearing precedent this design reuses (§2), not a
  solved instance of gap #4 (it never had to reconcile a self-reported per-tool ceiling against
  anything, because WASM tools weren't previously resolved by a cross-provider name registry at all).

## 1. What a "tool registry" actually has to answer

Given a bare string (`"web_search"`) in a declarative document's `spec.tools`, three questions, in
order:

1. What `ToolDescriptor` does this name resolve to?
2. Is that descriptor's own `capability_ceiling` *trustworthy* — safe to check the agent's granted
   ceiling against, the same way `check_capability_ceiling()` already does for native tools?
3. If two different sources both want to claim the same name, what happens?

## 2. The design

### 2a. `ToolRegistry` — host-curated, never auto-discovered

```cpp
enum class tool_provenance { native, wasm_plugin, mcp_server, a2a_agent };

class ToolRegistry {
public:
    // Fails closed on a duplicate name -- the SAME "two declared tools share a name" shape
    // check_tool_name_collision() already enforces for a single native agent's own tool set
    // (agent_registry.hpp:325), extended here to the whole registry rather than one agent's list.
    [[nodiscard]] result<void> register_tool(std::string name, ToolDescriptor descriptor,
                                              tool_provenance provenance);

    [[nodiscard]] std::optional<ToolDescriptor> find(std::string_view name) const;

private:
    struct Entry { ToolDescriptor descriptor; tool_provenance provenance; };
    std::unordered_map<std::string, Entry> entries_;
};
```

Deliberately mirrors `ChatClientRegistry` (`core/chat_client.hpp:244`) — `register_*`/`find`, an
`unordered_map` behind it, no other machinery. `register_agent<A>()` already takes this exact kind of
registry as an *optional* pointer parameter for an equivalent job (resolving a declared identifier
against a host-curated table, `ChatClientId` → `ChatClientCapabilities`); `compile_agent_document()`
gains the identical parameter shape for `ToolRegistry const*`, defaulting to `nullptr`.

**The `nullptr` case must preserve today's exact behavior, not fail closed — a real, checked
constraint, not an assumption.** `tests/test_agent_yaml_compiler.cpp`'s F-1 already asserts, against
015 §2's own worked example (which DOES list `web_search`/`code_interpreter`/a handoff), that
`compiled_meta.tools.descriptors().empty()` holds when no registry machinery exists —
`compile_agent_document()`'s own file-top comment calls this "honestly empty rather than fabricated."
A first-pass design that makes an absent registry fail closed on any `spec.tools` entry would flip
that currently-passing, currently-correct test into a failure — the exact risk the audit's own gap #5
row already named. The right split: **`registry == nullptr`** means "no registry machinery available
at all" — `tools` stays honestly empty, byte-identical to today, no error. **`registry` supplied but a
specific named tool is absent from it** is the only case that fails closed, with a real diagnostic
(§3) — a genuinely different situation (a host that bothered to wire a registry made a specific,
checkable claim about what's in it) from "no registry exists yet." Both cases are additive to the
zero-arg call sites the same way `ChatClientRegistry`'s own default-`nullptr` convention already is.

**This structurally answers namespace squatting**, not as an added check but by construction: nothing
is ever *auto-discovered* into the registry from whatever happens to be loaded. A host that wants a
WASM plugin's tools available writes code that calls `register_tool()` for each one, explicitly, after
whatever validation §2b requires. Two plugins wanting the same name is a **host-time** registration
conflict — `register_tool()`'s own fail-closed duplicate check catches it exactly where a human is
already looking (host wiring code), the same place `check_tool_name_collision` already catches it for
one agent's own native tool list.

### 2b. The actual new part: capability-ceiling trust for non-native provenance

`check_capability_ceiling()`'s existing shape (`agent_registry.hpp:339-348`) — for each tool, walk
its declared `capability_ceiling`, verify a governing `CapabilitySet` `.contains()` every entry — is
right and reusable. What's missing is *what governs* when the descriptor's own `capability_ceiling`
isn't host-authored C++:

- **Native**: unchanged. The compiler is the trust boundary; a hand-authored `ToolDescriptor` is
  trustworthy by construction.
- **WASM plugin**: a tool's self-reported `capability_ceiling` (from its own `list-tools` response)
  is validated — via the *same* `.contains()`-per-entry shape `check_capability_ceiling()` already
  uses, called again here at a different layer — against that plugin's own **instance-level
  `requested_capabilities`** (ADR-010, already host-decided at load time, already sandbox-enforced).
  A tool whose self-report claims more than the instance's own outer grant covers **fails to
  register** — excluded from the registry, not silently widened, not silently clamped. (Clamping was
  considered and rejected: silently narrowing a self-report to fit is a *different* answer than what
  the plugin asked for, and would fail with a confusing "denied" only much later, at actual invocation,
  instead of at registration where the mismatch is easiest to diagnose.)
- **MCP server / A2A agent**: identical shape — a self-reported schema/skill is validated against
  whatever outer capability grant the host already decided when it configured that MCP connection or
  A2A delegation (both already real concepts elsewhere in this codebase — 011/012's own connection
  configs), not a new authority source.

This is the concrete answer to "resolve I2 capability-ceiling binding... from scratch": **it doesn't
need a new mechanism, it needs the existing per-item `.contains()` check applied one layer earlier,
against a provenance-appropriate outer grant instead of always the agent's own.** No new capability
representation, no new enforcement primitive — reuse, not invention.

### 2c. Resolution timing: document-compile-time, matching 002 §6's own philosophy

002 §6: *"at `register_agent<A>()` the engine compiles metadata and validates, failing fast."*
`compile_agent_document()` should hold to the identical bar: a `spec.tools` entry naming a tool not
present in the supplied `ToolRegistry` fails **at compile time**, not silently deferred to first
invocation (which would surface a missing/renamed tool deep into an already-running agent — a worse,
later failure than a fast, loud one at compile time). `ToolTable::from_names()` (§5) is the mechanical
consequence of this choice: a pure lookup function, not a lazy/deferred resolver.

**Named, not solved here**: this timing choice means a `ToolRegistry` snapshot is frozen at the moment
a document compiles. MCP's own `tools/list_changed` notification (011) and an analogous WASM
hot-reload case are real, live-update mechanisms this design does not wire up — a document compiled
against a registry snapshot from time T does not automatically re-resolve if the underlying server's
tools change at T+1. Scoped out explicitly (§6), not silently assumed away.

## 3. `ToolTable`'s name-keyed half (gap #5), now a thin consequence of §2

```cpp
[[nodiscard]] static result<ToolTable> from_names(std::vector<std::string> const& names,
                                                    ToolRegistry const& registry);
```

For each name: `registry.find(name)`; if absent, fail closed with a real diagnostic (`agent.
tool_not_found_in_registry`) naming the specific missing tool, not a generic parse error; collect the
resolved descriptors; delegate to the already-real, already-tested `from_descriptors()`. No new
`ToolTable` machinery — the descriptor-keyed half already does the actual work.

## 4. Self-red-team findings

**MUST-NAME 1 — "reject the one bad tool, not the whole plugin" needs a real diagnostics
requirement, not just a policy decision.** §2b's per-tool rejection means a WASM plugin advertising 10
tools where 1 fails ceiling validation still registers the other 9. If a *later* document references
the excluded 10th by name, `ToolRegistry::find()` returns `nullopt` indistinguishable from "never
existed." A real implementation must keep enough of a record to turn that into "tool `x` was
considered and excluded at registration: `<reason>`," not a bare not-found — named as a concrete
requirement for whoever builds this, not assumed to fall out of the design above for free.

**Checked, not assumed: does §2b's per-provenance validation reintroduce a widening bug the
already-proven `.contains()` check doesn't have?** No — `.contains()` (via `subsumes()`,
`trust/capability.hpp`) is already fail-closed by construction (a capped parent rejects an uncapped or
broader request). Reusing it here, against a different outer `CapabilitySet` depending on provenance,
inherits that same fail-closed property rather than introducing a new one to get wrong.

**A real, deliberately out-of-scope gap named explicitly: `spec.capabilities` YAML parsing.**
`compile_agent_document()` already leaves `capability_ceiling` empty for a different, already-known
reason — turning YAML capability descriptions into real `cap::` variant values is unbuilt (its own,
separately-scoped follow-up, per the compiler's own file-top comment). This design closes tool-name
resolution; a compiled agent still can't declare a *non-empty* capability ceiling from YAML until
that separate gap also closes. Naming this explicitly matters because the two gaps look similar
("declarative capability stuff is missing") but are genuinely different pieces of work.

**Considered and rejected: resolving tool names lazily, at first invocation, instead of at
compile-time.** Would let a document reference a tool that only becomes available later (a plugin
loaded after the document compiled) — but trades a fast, loud, compile-time failure for a slow,
runtime one, surfacing mid-conversation instead of at authoring time. Rejected for the same reason
002 §6 chose fail-fast for the native path; not reopened here without a concrete need shown first.

## 5. What this design draft does not claim

No code exists. Not a decision about *how* a host actually populates a `ToolRegistry` from a live MCP
connection or A2A delegation — those provider-specific ingestion paths (call `tools/list`, validate,
register) are real, separate implementation work this draft specifies the *contract* for (validate
against a provenance-appropriate outer grant, then register), not the integration code itself. Not a
resolution of `spec.capabilities` YAML parsing (§4). Not a live-update/re-resolution mechanism for a
registry snapshot that's gone stale relative to a running MCP/WASM source (§2c). Not a decision about
whether a *single* process-wide `ToolRegistry` is the right scope, versus one per session/tenant —
this draft assumes "whatever the host constructs and passes to `compile_agent_document()`," deferring
multi-tenancy/isolation questions to whoever actually wires this in, the same way `ChatClientRegistry`
already defers them today.
