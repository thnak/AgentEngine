M2 Phase E — Agent CRTP surface (002), now that there's something to author

## Context

Milestone 2's headline exit-criterion sentence (`docs/planning/v1-implementation-roadmap.md`):
*"...an agent declares `Tools<...>`, a capability-gated native tool call is enforced end to
end..."* Phases A (ADR-009 capability enforcement) and B (the 006 §3 tool pipeline,
`core/tool_pipeline.hpp`) already do the real work here; Phase E is mostly wiring `Agent<Derived,
Policies...>` (`core/agent.hpp`) up to what A and B already built, plus the remaining declaration
surface 002 §3 needs for API completeness.

Current state of `core/agent.hpp` (as of Phase B's completion): `Capabilities<Cs...>` and
`Approval<M>` are now shared with `Tool`'s declaration site (`core/policy_tags.hpp`,
`core/tool.hpp`) per Phase B's decision 7 execution. 8 of 002 §3's 13 policy tags are still
empty/stub types (`Concurrency`, `Retry`, `Memory`, `Middleware`, `Stateless`, `OutputSchema`
missing entirely). `Agent` itself does nothing yet — no metadata compiler, no `register_agent`.

## Tasks

- **E1.** (done) Remaining policy tags (`Concurrency`, `Retry`, `Memory`, `Middleware`, `Stateless`,
  `OutputSchema`) added as empty/near-empty stub types for API completeness, matching 002 §3's
  table — real behavior for most is out of scope until the milestone that owns it (document per
  tag which milestone that is). **Size: M** — see the breakdown doc's own E1 entry (Phase E
  section) for the full writeup; `include/agentengine/core/agent.hpp` and
  `tests/smoke_vocabulary.cpp`.
- **E2.** (done) `register_agent<A>()` — the real metadata compiler: builds the agent metadata
  table, runs 002 §6's 8 named validation checks. Checks needing machinery this milestone doesn't
  build (credentials/004, handoff-cycle/014, and — a real finding, not anticipated by this task's
  original scope note — `SandboxProfile<P>`'s two checks, whose template-parameter kind 002 §2 and
  008 §2a disagree about) are stubbed to always-pass with a tracked comment, not silently skipped.
  **Size: L** — see the breakdown doc's own E2 entry for the full writeup;
  `include/agentengine/core/agent_registry.hpp`, `tests/test_agent_registry.cpp`.
- **E3.** (done) An agent declaring `Tools<TrivialNativeTool>` and a matching `Capabilities<...>`
  ceiling actually runs one tool call end-to-end through Phase B's pipeline — the headline
  exit-criterion sentence, made real. Pure wiring, as scoped: `invoke_agent_tool()`
  (`core/agent_registry.hpp`) is the one glue function connecting `register_agent<A>()`'s compiled
  `AgentMetadata` (E2) to `core/tool_pipeline.hpp`'s real `invoke_tool()` (Phase B) via a fresh
  `CapabilitySet::grant_root(meta.capability_ceiling)` per call — no new enforcement logic.
  **Size: M** — `include/agentengine/core/agent_registry.hpp`, `tests/test_agent_tool_invocation.cpp`.
- **E4.** (done) 002 §8 G3 miniature — validation rejects at least the capability-ceiling-mismatch
  and tool-name-collision defect classes with a specific diagnostic, negative test per class (full
  8-class suite deferred alongside E2's scoping). **Size: M** — already satisfied by E2's own
  `tests/test_agent_registry.cpp` (its `NameCollisionAgent`/`CapabilityGapAgent` negative cases each
  assert a specific `error.code`, not just pass/fail); this task added no new test file, only
  explicit `002 §8 G3` cross-references at those two cases and the file's top comment so the gate's
  satisfaction is traceable by name rather than implicit. A G3 gate is about the proof existing, not
  about which task's commit happened to add it.

## What's explicitly deferred past this phase (see breakdown doc's full list)

- 002's G1 (objdump zero-cost parity).
- 002's G2 (YAML/C++ metadata byte-identity — needs 015, the Declarative Agent Format milestone).
- 002's G4 (handoff/sub-agent/remote-agent compile-time indistinguishability — needs 014, Workflow
  and Orchestration).
- 002's own full G3 (all 8 §6 defect classes, not just the 2 this milestone's real machinery can
  prove) — the other 6 need the same missing infrastructure E2's stubbed checks named (004's
  ChatClient registry, SandboxProfile's RFC-conflict resolution, 014's handoff graph).

## Exit criteria

- `Agent<Derived, Policies...>` has a real, tested metadata compiler (`register_agent<A>()`), not
  an empty CRTP base. **Met (E2).**
- A concrete agent declaring both `Tools<...>` and a covering `Capabilities<...>` ceiling actually
  invokes a native tool end-to-end through the Phase B pipeline, proven by a test — this is the
  milestone's own headline sentence, made real and measured. **Met (E3).**
- At least the capability-ceiling-mismatch and tool-name-collision validation defect classes are
  caught at `register_agent<A>()` time with a specific, actionable diagnostic (negative test per
  class). **Met (E2/E4).**
- Full test suite green on Windows and Linux/gcc-14. **Met** — 30/31 on Windows and 22/22 on Linux
  as of E3's own verification pass, the one Windows exception being the pre-existing, unrelated
  `test_native_jail_backend_windows` OOM-detection flake tracked in Phase C's own issue doc.

**Phase E is complete.** All four tasks (E1-E4) done; every exit criterion met.
