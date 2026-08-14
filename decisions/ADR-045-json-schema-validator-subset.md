# ADR-045 — A scoped JSON Schema validator, closing 006 §8 G2 and 015 §7 G2

**Status:** Proposed (2026-08-14). Designed, self-red-teamed, implemented, and proven (real code +
new test file, full suite green); awaiting the project owner's explicit "Judged" sign-off per this
project's governance (`decisions/README.md`; `OpenQuestions.md` OQ-11).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #3 (the finding this
ADR closes). `006-Tool-and-Function-Plane.md` §8 G2 and `015-Declarative-Agent-Format.md` §7 G2 (the
two gates this validator exists to satisfy). `docs/planning/tool-capability-registry-design-draft.md`
(this session's gap-4 design draft — the future non-native tool-invocation path that design names
would be this validator's real consumer, once built; see §5).

## 1. The question

**Stated so it has a wrong answer:** does this codebase have any way to check an arbitrary JSON
instance against an arbitrary JSON Schema document, or is `core/json_schema.hpp`'s existing
"generate a schema from a known C++ type" machinery the only schema-shaped thing that exists?

**Before this fix: only generation existed, confirmed against current code.** `json_schema_of<T>()`
only ever emits a schema string *from* a C++ type. The only runtime "validation" in the tree was
`schema::from_json<T>()` — a compile-time-typed, reject-not-coerce decoder walking a *known C++
struct's* fields, used directly by `invoke_tool()`'s `InvokeFn` closure. No generic
instance-against-a-schema-*document* check existed anywhere.

## 2. What re-grounding against current code found (scoping the real, reachable value, not just
   confirming the gap)

- **No regex-DoS bug exists to fix — the codebase has no regex engine anywhere.** `std::regex` has
  zero hits in `include/`. The audit's "regex-DoS budget gap" is prospective design guidance for a
  *not-yet-built* validator, not an existing defect — this ADR resolves it by **not building a regex
  engine at all** (§3), structurally avoiding the whole risk category rather than trying to bound one.
- **`invoke_tool()`'s existing type-directed decode already protects native tools; a generic
  validator adds nothing there.** For a native `Tool<Derived,...>`, the C++ `Args` type *is* the
  schema — `schema::from_json<Args>()` already rejects malformed input with full type safety, stronger
  than a generic JSON-Schema check could be. This validator's real, reachable value is the path that
  has **no compile-time `Args` type at all**: a tool resolved by *name* (WASM/MCP/A2A), where
  `args_schema_json` is the only schema information that exists — exactly the same provenance gap
  #4's tool registry design draft (`docs/planning/tool-capability-registry-design-draft.md`, this
  session) already had to reason about. **That invocation path is not built yet** (gap #4 is itself
  design-only) — so this ADR ships the validator core, real and tested against its own governing
  gates, without a production call site to wire it to today (§5).
- **015 §7 G2's own choice — reject a cyclic `$ref`, don't support recursive schemas — is confirmed,
  not assumed.** This matters because standard JSON Schema explicitly *permits* self-referential
  schemas (e.g. a tree-shaped `Node` whose `children` property is `$ref`s to itself) as a normal
  pattern; a naive reading might expect this validator to *support* recursion, not reject it. 015 §7
  G2's own text is unambiguous: a negative corpus including cyclic `$ref` must be *rejected with
  precise diagnostics*. This ADR follows the RFC's actual, narrower decision rather than the JSON
  Schema spec's more permissive default.

## 3. The design

`include/agentengine/core/json_schema_validator.hpp`, two entry points:

- **`validate_schema_well_formed(schema, budget)`** — checks a schema *document* for structural
  validity, specifically 015 §7 G2's own gate: real DFS cycle detection over every `$ref` the schema
  transitively contains, tracking pointers on the **current resolution path** (not a global visited
  set — a DAG-shaped schema referencing the same `$defs` entry from two independent places is
  legitimate and must not be flagged; only a pointer already active on the path is a real cycle).
- **`validate_instance(schema, instance, budget)`** — validates a JSON instance against a schema,
  collecting structured `ValidationViolation`s (006 §8 G2). Deliberately does **not** re-derive cycle
  detection: `$ref` is only ever resolved when also stepping into a *new instance node*, so a finite
  instance structurally cannot drive this into an infinite loop even against a schema that
  `validate_schema_well_formed` would flag as cyclic — proven directly (§6, BUDGET-1), not merely
  asserted.

**Deliberately a subset, not the full 2020-12 keyword vocabulary**: `type`, `enum`, `const`,
`required`, `properties`, `additionalProperties` (boolean form only), `items` (single-schema form,
not tuple/`prefixItems`), `minItems`/`maxItems`, `minLength`/`maxLength`,
`minimum`/`maximum`/`exclusiveMinimum`/`exclusiveMaximum`, and local `$ref`/`$defs` (a JSON Pointer
into the *same* document — resolving a `$ref` to an *external* file is 015's own separate resolution
step, gap #6, which must already have happened before a schema reaches this validator). **Not
implemented, by deliberate choice, not oversight**: `pattern` (would need a regex engine — the whole
risk category this ADR avoids, §2), `format`, and schema boolean composition
(`oneOf`/`anyOf`/`allOf`/`not`) — no gate in 006/015 currently requires any of them.

**A three-dimensional resource budget** (`ValidationBudget`), because this validator's whole job is
walking untrusted, potentially-adversarial JSON: `max_depth` (schema+instance co-descent bound),
`max_ref_hops` (longest allowed `$ref` chain, cyclic or not — a real backstop independent of cycle
detection, proven separately, §6 BUDGET-4), and `max_violations` (bounded output list). Every bound
fails closed with a real, structured violation the moment it's exceeded, never a silent truncation
that reports "valid."

## 4. Self-red-team findings

**A real, caught-before-shipping DoS gap: `max_violations` alone does not bound total work.** A huge
but entirely *valid* instance (a 10-million-element array with zero violations) never trips a
violation-count budget, yet the naive first draft still walked every element. Added a fourth budget
dimension, `max_nodes_visited`, bounding total (schema-node, instance-node) visits directly,
independent of whether any violation is ever found — proven directly against a huge valid instance
under a tight budget, with a positive control proving the same instance validates cleanly under the
default generous budget (§6, BUDGET-3).

**A real ordering bug the validator's own test caught, not spotted by inspection.** The
`max_nodes_visited` fix's first draft set the "budget exceeded" latch flag *before* calling the
helper that records the violation — but that helper itself checks the same latch before pushing,
so the very violation reporting the exceeded budget was silently swallowed by the flag it was trying
to report. Caught by BUDGET-3 failing on first run, not by re-reading the code; fixed by reordering
(record the violation, then latch). Left in as a fresh, concrete instance of this project's own
recurring "verify by running, not by re-reading" lesson.

**Checked, not assumed: undefined behavior on adversarial numeric input.** An early draft of the
`"integer"` type check round-tripped through `static_cast<std::int64_t>` — casting a `double` outside
`int64_t`'s range is undefined behavior in C++, and nothing bounds an adversarial instance's numeric
values beforehand. Fixed to a range-safe check (`std::isfinite(n) && n == std::floor(n)`) before this
ever reached a test, not found by one.

## 5. What this ADR does not claim

- **No production call site is wired yet.** Native tools don't need this validator (their `Args` type
  already provides stronger checking); the non-native, name-resolved tool-invocation path that *would*
  need it isn't built — it depends on gap #4's tool registry, itself still a design draft, not real
  code. This ADR ships a real, tested, gate-satisfying validator core; wiring it to a production call
  site is real, separate follow-up work once gap #4 lands as code.
- **Does not implement `pattern`/`format`/schema boolean composition** (§3) — named, not silently
  dropped; no current gate requires them.
- **Does not resolve `$ref` to an external file or URL** — that is 015's own separate resolution step
  (gap #6), a distinct, not-yet-closed gap this ADR does not attempt.
- **Not a claim of full JSON Schema 2020-12 conformance** — an explicitly named subset, scoped to
  what 006 §8 G2 and 015 §7 G2 actually require.

## 6. Evidence

`tests/test_json_schema_validator.cpp` (new file, 22 checks):

- **V-1**: a genuinely valid instance produces zero violations — the positive control every negative
  check below needs (022 §5).
- **G2-1 through G2-6** (006 §8 G2): missing-required-property, top-level type mismatch,
  `additionalProperties:false` rejection, `minItems` violation, `enum` rejection, and a violation
  nested two levels deep (object → array → object) all produce real, correctly-located, structured
  diagnostics — not thrown exceptions, not generic failures.
- **REF-1/REF-2** (015 §7 G2): a direct self-cycle and an indirect two-hop cycle (`A → B → A`) are
  both detected with an explicit "cyclic" diagnostic. **REF-3** (positive control): the same `$ref`
  target reached from two independent, non-overlapping paths is correctly *not* flagged — proves
  REF-1/REF-2 detect real cycles, not merely repeated visits. **REF-4**: a `$ref` to a nonexistent
  target is flagged, not silently ignored.
- **BUDGET-1**: `validate_instance()` terminates and correctly validates a *finite* instance against
  a genuinely cyclic schema — the "instance bounds it, not a separate cycle check" design claim,
  proven directly. **BUDGET-2**: an instance deeper than `max_depth` fails closed. **BUDGET-3**: a
  huge but entirely valid instance still fails closed once `max_nodes_visited` is exceeded, with a
  positive control proving the same instance validates cleanly under a generous budget. **BUDGET-4**:
  a long acyclic `$ref` chain fails closed on `max_ref_hops` alone, independent of cycle detection.

Full suite: green (`ctest`, this pass), zero regressions.
