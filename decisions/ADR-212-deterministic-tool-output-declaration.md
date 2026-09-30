# ADR-212 — `Deterministic`: a tool's declared output equality, separate from `EffectClass`

**Status:** **Judged (2026-09-30, project-owner sign-off).** Design (§3), corrected by one independent
red-team pass (§4), implemented and proven (§6).

**Relates to:** GitHub issue #81. `006-Tool-and-Function-Plane.md` §1 (tool declaration).
`019-Durability-and-Long-Running-Agents.md` §3/§6 (`EffectClass`, re-execution on rewind).
`027-Vocabulary-and-Naming.md` §3. `include/agentengine/core/tool.hpp`,
`include/agentengine/core/tool_descriptor.hpp`. ADR-158 (the policy-tag + compile-fail precedent this
follows). ADR-176 (`ImageIdentifiedSurface`, which lets a host pin the environment a comparison ran
in). Research: `docs/research/2026-09-30-mcp-tool-annotations-determinism.md`.

## 1. The question

`EffectClass<C>` answers "is it safe to run this call again?". Nothing answers "will running it again
return the same thing?". The two are independent: a `pure` tool can sample, read a clock, or iterate
an unordered container, and an `idempotent` tool can return a fresh server timestamp on every call.

A host that re-executes a share of calls elsewhere and compares the outputs (the motivating case is
AeroCoWorker's sampled cross-node verification — an AeroCoWorker RFC, not an AgentEngine one; ADR-176
§1 says the same of #80) only learns something from the comparison when the tool's author has claimed
the output is a function of its inputs. Today that claim can live only in the host's own metadata,
vouched for by whoever deployed the tool, not by whoever wrote it.

## 2. What exists (verified on `main` at `0aec09e`)

- `EffectClass<C>` / `effect_class { pure, idempotent, at_most_once }`, default `at_most_once`
  (`tool.hpp:73-76`, `:235-241`). `ToolDescriptor::effect_class` carries it (`tool_descriptor.hpp:74`).
- `Parallelizable`, `Backgroundable`, `ExclusivityGroup<Name>`: tags folded by `Tool<Derived,
  Policies...>`, copied onto `ToolDescriptor` by `make_tool_descriptor<T>()` and
  `make_tool_descriptor_with_invoke<T>()` (`tool_descriptor.hpp:128-192`).
- "Deterministic" appears in the headers only in comments.
- **Declarative (015):** `spec.tools` names tools; `ToolTable::from_names()` resolves each to a copy
  of the registered `ToolDescriptor` (`agent_yaml_compiler.hpp:35-41`, `tool_registry.hpp:86`). A
  declarative agent never declares a policy on a tool it names, so it inherits the descriptor's.
- **WASM (009):** `ae:tool@1.0.0`'s `tool-descriptor` record has `name`, `description`, the two
  schemas and `parallelizable` (`wit/ae-tool.wit:252-263`). No effect class, no determinism.
- **MCP client:** `tools/list` entries become hand-built descriptors (`mcp_tool_bridge.hpp:30-38`).
  MCP 2026-07-28's `ToolAnnotations` has `title`, `readOnlyHint`, `destructiveHint`,
  `idempotentHint`, `openWorldHint` and nothing about output equality; clients MUST treat annotations
  as untrusted unless the server is trusted (research note).
- **MCP server:** `McpToolListEntry` emits no `annotations` at all (`protocol/mcp/server.hpp:107-112`).
- **Idempotency keys:** the foreground `invoke_tool()` path does not fill
  `EffectContext::idempotency_key` (`effect_context.hpp:260-266`), and the key is derived from
  `run_id` (`tool_pipeline.hpp:119-123`), which a verifying node does not share.
- `json_value_equal()` (`json_schema_validator.hpp`) is the codebase's one structural JSON equality:
  kind-exact, key-order-insensitive for objects, order-sensitive for arrays. It was in
  `schema::detail`; this ADR exposes it as `schema::json_value_equal`.

## 3. Design (as corrected by §4)

### 3.1 The declaration

```cpp
struct Deterministic {};  // core/tool.hpp, next to Backgroundable
```

Absent by default. `Tool<Derived, Policies...>::declared_deterministic()` is `constexpr`, folded
like `kHasParallelizable`. `ToolDescriptor::deterministic` (default `false`) carries it and is set by
`make_tool_descriptor<T>()` only (§3.4).

### 3.2 What the claim means

A tool declaring `Deterministic` promises: **two successful invocations with equal `Args`, which read
equal content through their granted capabilities, return `Reply` values whose `schema::to_json` values
are `schema::json_value_equal`.**

- **The point of definition is the tool's own `schema::to_json(Reply)`**, which is what
  `make_tool_descriptor<T>()`'s `invoke` returns — before pipeline step 9 turns it into
  `Data{json::dump(v)}` or, above `ctx.tool_result_byte_threshold`, into a `Media` blob reference.
  A host that only holds the `ToolResult` recovers the value with `json::parse` of the `Data` text or
  of the fetched blob. For finite numbers that round trip is exact (`json_value.hpp:470-476`). One
  side being `Data` and the other `Media` is a threshold difference, not a mismatch.
- **Equality is structural, not bytes.** Object key order does not count; array order does.
- **Only successes are covered.** Either side failing (deadline, budget, sandbox crash, transient
  network error) makes the pair "not comparable", never "match" and never "mismatch".
- **Inputs include what the tool reads.** A deterministic `read_file` is still deterministic when
  the file changed between runs; making the observed inputs equal (same worktree snapshot, same image
  — ADR-176's `ImageIdentifiedSurface` reports it) is the host's job, not the tool's.
- **Not covered, so a tool relying on any of them must not declare it:** clocks, randomness,
  network responses, the environment outside the capability ceiling, and vectors filled from
  unordered iteration. (`std::map`/`std::unordered_map` have no `to_json` mapping, so that case can
  only arise through a vector.)
- **Non-finite numbers are outside the claim.** NaN never equals itself and `json::dump` writes
  `nan`/`inf`, which does not parse. A reply containing one is "not comparable". `-0.0` and `0.0`
  are equal and both dump as `0`.

### 3.3 `Deterministic` requires an explicit `EffectClass<pure>`

The claim is only ever checked by running the call again on another node:

- `at_most_once` must not be re-run without operator acknowledgement (019 §6).
- `idempotent` is safe to re-run only under the original idempotency key. A verifying node gets no key
  (§2), so the effect would happen twice. And if it did get the key, a correct idempotent backend
  would replay its stored first reply, so a match would prove nothing.

`Tool<>` therefore rejects, at compile time, `Deterministic` with anything but an explicit
`EffectClass<effect_class::pure>`, including with no `EffectClass` (the default is `at_most_once`).
As with `declared_effect_class()`, the last declared `EffectClass` wins.

Hand-built descriptors never pass through `Tool<>`, so the same rule is the runtime query hosts use:

```cpp
[[nodiscard]] inline bool rerun_comparable(ToolDescriptor const& d) noexcept {
    return d.deterministic && d.effect_class == effect_class::pure && !d.captures_session_state;
}
```

The engine itself reads neither `deterministic` nor `rerun_comparable()`. It carries the
declaration; the host decides what to do with it.

### 3.4 Surfaces (I6)

| Surface | `deterministic` | Why |
|---|---|---|
| Native CRTP, `make_tool_descriptor<T>()` | from the tag | §3.1 |
| Native CRTP, `make_tool_descriptor_with_invoke<T>()` | `false` | the closure is not the code the tag describes, and it reads session state a verifying node lacks (§4 F2) |
| Declarative YAML/JSON naming a tool | inherited from the descriptor | §2: `spec.tools` references by name |
| Declarative-defined tool (`sandboxed_script`, `composite`) | `false` | no field to declare it — residual R3 |
| WASM plugin | `false` | `ae:tool@1.0.0` has no field — residual R1 |
| MCP client | `false` | no MCP field; `idempotentHint` is about effects, and annotations are untrusted |
| MCP server | not emitted | no standard field; inventing an `annotations` key is not conformance |

### 3.5 Failure direction (I2, I3)

The claim is written by the tool's author in host code. Model output never sets it; the MCP bridge,
WASM bridge and providers build descriptors with it `false`; nothing in the engine reads it for a
permission decision. I2 and I3 are untouched.

- **A false claim** makes a verifier see a mismatch and mark the result unverified. Fails safe.
- **A missing claim** makes a verifier skip a call it could have checked. Fails safe.
- **A false match** — different outputs comparing equal — fails **unsafe**: the verifier reports
  "verified". Known sources, and what this ADR does about each:
  - `schema::to_json` stores integers as `double` (`json_schema.hpp:333`). Two distinct 64-bit
    integers above 2^53 become equal. **Residual R4**, stated here.
  - A wrapper that replaces `invoke` and transforms the reply. `cli_chat`'s output cap
    (`tools/cli_chat.cpp`) can make replies that differ past the cap equal; the test driver's
    `double_tool` serves a recording instead of running the tool. **Both now clear
    `deterministic`.** The rule for any such wrapper, stated on `ToolDescriptor::deterministic`:
    clear it unless successes pass through unchanged. `recording_tool` qualifies — it passes a
    success through or turns it into an error — so it keeps the flag.
- **Using the claim for something other than comparison** — for example serving a cached reply
  instead of running the call — also fails unsafe on a false claim. That use is not what this ADR
  defines, and a host doing it owns the risk (ADR-070's seam discipline applies if it is ever brought
  into the engine).

## 4. Red-team (one independent pass, 2026-09-30)

Against the first draft of §3, which allowed `pure` or `idempotent` and copied the tag through both
factories.

| # | Finding | Severity | Disposition |
|---|---|---|---|
| F1 | `idempotent` is not safe to re-run on another node: no key reaches the foreground path, the key embeds `run_id`, and with the key a correct backend replays its first reply | MUST-FIX | **Fixed.** §3.3 now requires `pure`; a third compile-fail gate rejects `idempotent` |
| F2 | `make_tool_descriptor_with_invoke` copied the tag onto a closure over session state (`ToolT::invoke` is often a poison stub, e.g. `secret_quarantine.hpp:258`, `tool_optimizer_provider.hpp:80`) | MUST-FIX | **Fixed.** That factory sets `false`; `rerun_comparable()` also excludes `captures_session_state` for hand-built descriptors |
| F3 | §3.5 argued only the spurious-mismatch direction; false matches exist (int64 → double, `cli_chat`'s cap, `double_tool`) | MUST-FIX | **Fixed / recorded.** The two wrappers clear the flag; the wrapper rule is on the field; int64 precision is R4 |
| F4 | The claim's point of definition is before step 9, which a host never sees | SHOULD-FIX | **Fixed** in §3.2: recover by parsing `Data`/blob; exact for finite numbers; `Data` vs `Media` is not a mismatch. A recovery helper is R5 |
| F5 | `json_value_equal` edge cases: NaN, `-0.0`, duplicate keys in a hand-built value (equality is then asymmetric), vectors from unordered iteration | SHOULD-FIX | **Fixed** in §3.2. Duplicate keys: `to_json` of a reflected struct cannot produce them and the parser rejects them, so out of scope |
| F6 | A deterministic `at_most_once` tool could be compared against a journal without a re-run | NOTE | Recorded as R2; forbidding it fails safe |
| F7 | Declarative-defined tools cannot declare the claim | NOTE | Recorded as R3 |
| F8 | I2/I3 hold | NOTE | Agreed |
| F9 | §2's line references are accurate at `0aec09e` | NOTE | — |

## 5. Residuals

- **R1 — WASM.** `ae:tool@1.0.0` cannot express the claim (nor an effect class). Both belong in a
  versioned `ae:tool@1.1.0`, not a silent change to 1.0.0.
- **R2 — non-`pure` deterministic tools.** A deterministic `at_most_once` or `idempotent` tool cannot
  declare the claim, though a host could compare it against a journal rather than a re-run. Relax only
  with a consumer that needs it and an ADR.
- **R3 — declarative-defined tools** have no field for the claim. Add one when 015 grows per-tool
  policy fields (it has no `effect_class` field either).
- **R4 — integers above 2^53** compare equal after `to_json`. Fixing it means an integer kind in
  `json::Value`, a wider change than this ADR.
- **R5 — no host helper** recovers the comparable value from a `ToolResult`. §3.2 says how; ship a
  helper when a host needs one.
- **R6 — MCP.** No standard field. If MCP adds one, the client bridge may map it only for a
  host-trusted server and only together with a `pure` effect class.

## 6. Evidence

Built and run on Windows, MSVC (Visual Studio 18), Release, 2026-09-30.

- `tests/core/tools/test_tool_deterministic.cpp`, all checks pass:
  - **D1:** `declared_deterministic()` is true for the declared tools, including with reversed tag
    order and a later `EffectClass` overriding an earlier one. It is false for an undeclared tool and
    for an empty-policy tool (the MSVC C3520 case in `tool.hpp`).
  - **D2:** `make_tool_descriptor` copies the flag; `make_tool_descriptor_with_invoke` drops it.
  - **D3:** a default-constructed descriptor is `false`.
  - **D4:** `rerun_comparable()` is false for `idempotent`, `at_most_once`, `captures_session_state`,
    and a `pure` tool without the claim.
  - **D5:** `ToolTable::from_names()` carries the flag, true and false, through the registry.
  - **D6:** a deterministic tool re-runs to a `json_value_equal` reply. Positive control: a `pure`
    counter tool under the same procedure mismatches.
- **Compile-fail gates** (`ctest -L compile_fail`), all pass, each with `test_tool_deterministic` as
  its control: `tool_rejects_deterministic_without_effect_class`,
  `tool_rejects_deterministic_at_most_once`, `tool_rejects_deterministic_idempotent`.
- **Mutation checks,** reverted after running:
  - Weakening the `static_assert` to "not `at_most_once`" makes `tool_rejects_deterministic_idempotent`
    fail.
  - Reducing `rerun_comparable()` to `d.deterministic` fails three D4 checks.
- **Neighbouring suites pass:** `test_effect_reexecution`, `test_tool_registry`, `test_tool_pipeline`,
  `test_json_schema_validator`, `test_agentengine_test_driver`, and the ADR-158 compile-fail pair.
  `agentengine_cli_chat` and `agentengine_test_driver` build.
- **Lints pass:** the 027 naming lint (`Deterministic` is listed in 027 §3, no suppression), the
  layering lint and the milestone-status lint.

## 7. What this ADR does not claim

- That any declared tool is actually deterministic. Nothing checks the claim against behaviour; D6
  proves the comparison procedure on one input, not the property.
- Any verifier, cache, or replay feature in the engine.
