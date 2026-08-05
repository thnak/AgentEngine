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
- **E2.** `register_agent<A>()` — the real metadata compiler: builds the agent metadata table, runs
  002 §6's 8 named validation checks. Checks needing machinery this milestone doesn't build
  (credentials/004, handoff-cycle/014) are stubbed to always-pass with a tracked comment, not
  silently skipped. **Size: L**
- **E3.** An agent declaring `Tools<TrivialNativeTool>` and a matching `Capabilities<...>` ceiling
  actually runs one tool call end-to-end through Phase B's pipeline — the headline exit-criterion
  sentence, made real. Mostly wiring; Phases A and B do the real work. **Size: M**
- **E4.** 002 §8 G3 miniature — validation rejects at least the capability-ceiling-mismatch and
  tool-name-collision defect classes with a specific diagnostic, negative test per class (full
  8-class suite deferred alongside E2's scoping). **Size: M**

## What's explicitly deferred past this phase (see breakdown doc's full list)

- 002's G1 (objdump zero-cost parity).
- 002's G2 (YAML/C++ metadata byte-identity — needs 015, the Declarative Agent Format milestone).
- 002's G4 (handoff/sub-agent/remote-agent compile-time indistinguishability — needs 014, Workflow
  and Orchestration).

## Exit criteria

- `Agent<Derived, Policies...>` has a real, tested metadata compiler (`register_agent<A>()`), not
  an empty CRTP base.
- A concrete agent declaring both `Tools<...>` and a covering `Capabilities<...>` ceiling actually
  invokes a native tool end-to-end through the Phase B pipeline, proven by a test — this is the
  milestone's own headline sentence, made real and measured.
- At least the capability-ceiling-mismatch and tool-name-collision validation defect classes are
  caught at `register_agent<A>()` time with a specific, actionable diagnostic (negative test per
  class).
- Full test suite green on Windows and Linux/gcc-14.
