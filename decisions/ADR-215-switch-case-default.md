# ADR-215: A default case for switch/case routing

**Status:** **Proposed (2026-10-01).** Design, one self red-team pass (§4), implemented and proven (§6).
The project owner judges it.

**Relates to:** GitHub issue #34. `014-Workflow-and-Orchestration.md` §1 (edge kinds), §4 (request
ports, issue #155's route check). `015-Declarative-Agent-Format.md` §3 (workflow document).
`027-Vocabulary-and-Naming.md` (the `edge_kind` row). I6 (declarative/native equivalence).
ADR-152 (workflow event stream). ADR-214 (written in the same series).

Files: `include/agentengine/workflow/graph.hpp`, `src/rt/workflow_supervisor.cpp`,
`include/agentengine/workflow/workflow_event.hpp`, `include/agentengine/workflow/introspection.hpp`,
`include/agentengine/workflow/yaml_compiler.hpp`, `tools/test_driver/test_driver.hpp`.

## 1. The question

A `switch_case` source fires exactly one of its cases. When the reply matches no case, the run
ends `routing_failed`. The same happens when the source is an executor whose reply carries no route
at all. Issue #34 asked for a default case, so a router can say "anything else goes here" without
enumerating every label.

There are three open points:

- how a default is represented;
- what counts as "no case matched";
- whether a default changes the rule that more than one match is a hard failure (RF-1/RF-2 in
  `test_rt_workflow_supervisor_patterns.cpp`).

## 2. Decision

1. **A new edge kind, `edge_kind::switch_default`.** It is an unlabelled edge. It fires **iff**
   the source has at least one `switch_case` edge and **zero** of them matched the reply's routes.
   The routes are matched exactly as before: a route equal to a case label selects that case.
2. **More than one match is still `routing_failed`, default or not.** A default answers "nothing
   was chosen". It does not resolve an ambiguous choice. RF-1 and RF-2 are unchanged, and DF-3 pins
   this rule with a default present.
3. **Exactly one match never takes the default** (DF-2).
4. **`validate_workflow` gets two new rules.**
   - `workflow.duplicate_switch_default`: a source may have at most one default.
   - `workflow.switch_default_without_cases`: a default needs at least one `switch_case` sibling.
     Without one, it would be a `direct` edge spelled as a fallback.
   - A default that carries a label is rejected by the existing `workflow.unexpected_case_label`
     rule.
   - The default edge takes part in every existing per-source and per-edge check: type match, and
     agreement of the edge-failure policy across a source's edges.
5. **Request ports.** An answer that names no switch case is valid when the port has a default; the
   run takes the default. An answer naming an **undeclared** label is still refused with
   `invalid_routes` (issue #155), even with a default. A default catches "no case chosen". It does
   not catch a typo, which the answerer can correct by answering again (IQ9).
6. **Observability.**
   - Taking the default emits a `message_routed` event with `edge_kind::switch_default`.
   - The `RouteSelected` payload gains `took_default` (appended last), so the stream says why the
     default ran.
   - The test driver's JSON includes `"took_default": true` only when it is set. Scenarios exported
     before this change still compare equal.
   - Mermaid renders the default as `== "(default)" ==>`, and DOT as `[label="(default)", style=bold]`.
7. **Native ergonomics.**
   - `WorkflowBuilder::connect_case(from, to, label, on_failure = {})`
   - `WorkflowBuilder::connect_default(from, to, on_failure = {})`

   Both are `connect()` underneath, so the compile-time type check and the shared validator apply
   unchanged.
8. **Declarative parity (I6).**
   - In the YAML/JSON compiler, `case: <label>` on a `to` edge produces a `switch_case` edge, and
     `default: true` produces a `switch_default` edge.
   - W-7 asserts that the compiled `Workflow` is `==` to the one the native builder produces.
   - The compiler refuses, with distinct error codes:
     - both keys on one edge;
     - either key on a `fan_out_to`/`fan_in_to` edge;
     - a non-string `case`;
     - a `default` other than `true`.

### Why a separate enumerator and not a flag

An earlier option was `bool is_default` on a `switch_case` edge. It was rejected for two reasons.

- **The compiler forces review.** With `-Wswitch -Werror`, a new enumerator makes every exhaustive
  switch over `edge_kind` fail to build until someone decides what a default means there. That
  covers the supervisor's `edge_fires`, the renderers and the test driver's name table. A flag would
  be silently counted as one more case wherever the code tests `kind == switch_case`. In
  `route_from`, that would turn "zero matched" into "one matched", which is wrong.
- **An existing rule does the label check.** An unlabelled default falls naturally under the
  existing rule that only labelled kinds carry a label.

### Comparison with MAF

MAF's `SwitchBuilder` (`Microsoft.Agents.AI.Workflows/SwitchBuilder.cs`) has `AddCase(predicate, ...)`
and `WithDefault(...)`. It reduces to a fan-out edge whose selector returns the **first** matching
case, or the default set when nothing matches.

AgentEngine keeps the default concept, with two deliberate differences:

- **Several matches are not resolved by declaration order.** In MAF the first match wins. Here,
  more than one match is a hard failure (014 §1's "exactly one"). Making the outcome depend on edge
  order would make reordering a graph's edges a behavior change.
- **One default target, not a set.** A default that needs several targets points at an executor
  that fans out.

## 3. What "matched" means

Only `switch_case` edges count. A `multi_selection` label that a reply names on the same source
fires its own edge as before. It does not count as a switch match, so the default also fires when
no switch case matched. This follows the issue's wording ("zero sibling switch_case edges") and
keeps the two edge kinds independent, as they were before this change.

## 4. Self red-team (adversarial pass before landing)

1. **A default turns a model's garbage route into a silent success.** A function or agent executor
   that emits a misspelled label used to end the run `routing_failed`. With a default it now takes
   the default.
   - *Accepted, by design.* This is what a default is for, and the author opted in per source.
   - *Not an I3 issue.* The default is an author-declared edge. Model output can only reach an edge
     the graph already contains, and this widens no authority.
   - *Request ports differ deliberately* (§2.5). A human answer naming an undeclared label is
     refused, because the person can answer again; an executor cannot.
   - *Residual R1:* `RouteSelected` carries the matched cases and the available labels, but not the
     route labels the reply asked for that matched nothing. A host cannot see *what* was misspelled.
2. **`took_default` and `switch_fired` interaction.**
   - Could the default fire alongside a matched case? No: `took_default` requires
     `switch_fired == 0` (DF-2).
   - Could a default rescue more than one match? No: the failure test is
     `switch_fired != 1 && !took_default`, and `took_default` is false whenever `switch_fired > 0`
     (DF-3).
3. **A default on a source with no cases**, which would bypass the "exactly one" rule, is rejected
   at validation (A5). Two defaults are also rejected, so "the" default is never ambiguous.
4. **Port answers.** `port_routes_valid` accepts zero fired cases only with a default. The
   undeclared-label check runs first and is unchanged (IQ9 asserts both halves).
5. **Agent-kind nodes.** `agent_session_as_executor_body` never proposes routes, so a switch out of
   an agent node with a default **always** takes the default. That is correct, since no case can
   match. The header comment now says so, so nobody mistakes it for model-driven routing.
6. **Declarative documents change meaning.** The YAML compiler did not reject unknown edge keys, so
   before this change a stray `case:` key was silently ignored and the edge compiled as `direct`.
   That same document now compiles to a `switch_case` edge.
   - *Accepted.* The previous behavior was itself the defect: 015 §4 requires unknown fields to be
     errors.
   - *Residual R2:* the compiler still does not reject unknown keys in general. That gap predates
     this ADR and is not fixed here.
7. **Fan-in barriers.** `seed_fan_in_holds` looks only at `fan_in` edges, and a default edge is
   never one. A default target that feeds a fan-in is held like any other source.

## 5. Residuals (named, not fixed)

- **R1:** routes that matched nothing are not reported (§4.1).
- **R2:** the YAML compiler does not reject unknown fields in general (§4.6). `multi_selection`,
  `chain` and per-edge `on_failure` remain unbuilt in the declarative form, as before.
- **R3:** `multi_selection` has no default of its own. An empty selection still fires nothing, as
  before. No issue has asked for one.
- **R4:** the marketing site's API reference (`web/marketing/src/data/apiContent.ts`) still lists six
  edge kinds.

## 6. Proof

The tests below, by file:

- **`tests/workflow/test_rt_workflow_supervisor_patterns.cpp`, DF-1 to DF-4**
  - DF-1: zero matches, and no route at all, take the default; only the default's target runs.
  - DF-2: exactly one match fires that case, not the default.
  - DF-3: more than one match is still `routing_failed` with a default present.
  - DF-4: the event stream records `message_routed(switch_default)` and
    `RouteSelected.took_default`.
  - SW-1, RF-1 and RF-2 are unchanged and still pass.
  - **Positive control:** with `took_default` forced to `false` in `route_from`, DF-1, DF-4 and IQ9
    fail. The source was then restored from a backup copy, not with git.
- **`tests/workflow/test_rt_workflow_supervisor_request_port.cpp`, IQ9:** at a port, an empty answer
  takes the default, and an undeclared label is refused with the port left open.
- **`tests/workflow/test_workflow_graph_validation.cpp`, A5 (ADR-215 cases):**
  - one default validates;
  - a duplicate default, a default without cases, and a labelled default are each rejected;
  - defaults are scoped per source.
- **`tests/workflow/test_workflow_yaml_compiler.cpp`, W-7:** the YAML switch with a default is `==`
  to the native `connect_case`/`connect_default` build, and the refusals listed in §2.8 hold.
- **`tests/workflow/test_workflow_introspection.cpp`, G0:** the default renders distinctly in
  Mermaid and in DOT.
