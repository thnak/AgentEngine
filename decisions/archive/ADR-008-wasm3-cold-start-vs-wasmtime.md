# ADR-008 — Does wasm3's cold-start advantage over Wasmtime, measured on our own workload, clear the 023-budget bar OQ-12 set before it is even a candidate?

**Resolves:** OpenQuestions.md OQ-12 (008 §1a). **Scope, deliberately narrow** (small prove, matching
this backlog's established scale): this ADR measures cold-start latency only — parse/compile +
instantiate + call + teardown for one trivial exported function, on this machine, single-threaded.
It does not re-litigate 008 §1a's Component-Model/WASI-0.3 rejection of wasm3 (unaffected by
anything measured here), does not propose adopting wasm3 as a second runtime, and does not run the
full 023 §7 G1 reference-machine baseline (which needs its own dedicated pass across every §3 cell).

## 1. The question

008 §1a rejects `wasm3` as the primary plugin runtime because it has only partial Component
Model/WASI 0.3 support — the one axis the plugin ABI (009) depends on. Its real advantage, cold
start, is named but never measured against Wasmtime on AgentEngine's own workload: "the cited
cold-start comparison is generic, not measured on our workload. Blocked on a 023-budget measurement
before it is even a candidate" (OpenQuestions.md OQ-12). This ADR runs that measurement.

## 2. The competing designs, steelmanned

**Design A — Wasmtime only (status quo, 008 §1a).** One runtime, full Component Model + WASI 0.3 +
WebAssembly 3.0, already proven end-to-end (`decisions/README.md` ADR-... — see OQ-7's resolution,
009 §11 Q4). Steelman: one sandbox-escape surface, one conformance story, matches 009's "typed
interfaces... versioned, checkable contracts" requirement in full.

**Design B — wasm3 behind the same host interface, scoped to ultra-short/high-frequency guest calls
or constrained deployments (008 §1a's own framing).** Steelman: if its cold-start advantage is real
and large on our workload, a content filter invoked per message (017 §4) or a footprint-constrained
deployment gets a materially cheaper path, at the cost of "two sandbox-escape surfaces and two
conformance stories" 008 §1a already names as the price.

## 3. Falsifiable claims

| # | Claim | Disproven by |
|---|---|---|
| W1 | wasm3's cold-start (parse+instantiate+call+teardown) for a trivial exported function is faster than Wasmtime's equivalent, on this machine, by a wide, not marginal, margin. | wasm3 measured slower, or faster only within measurement noise (single-digit percent). |
| W2 | The gap, if real, is explained by a structural difference (interpreter vs. JIT compilation), not a measurement artifact favoring one side (e.g. asymmetric object reuse). | The two host programs are not actually symmetric, or the same gap disappears under a corrected symmetric measurement. |
| W3 | Wasmtime's measured cold-start numbers are informative against 023 §3's provisional WASM budgets even though those budgets are not yet baselined. | The measured numbers are not comparable to any 023 §3 cell because the operations don't match what those cells actually measure. |

## 4. The red-team attack

The adversarial angle here isn't sandbox escape (neither runtime executes anything hostile in this
test) — it's **"is the comparison rigged to favor one side without saying so,"** since a cold-start
number is trivially gameable by what each host program is allowed to reuse across iterations.
Red-team checks applied to the measurement itself:

- **Symmetric object reuse, verified by reading both host programs, not just trusting a summary.**
  Both `wasm3_host.cpp` and `wasmtime_host.cpp` (scratchpad,
  `wasm3-check/wasm3_host.cpp`/`wasmtime_host.cpp`) reuse exactly one long-lived object across all
  1000 iterations — wasm3's `IM3Environment`, Wasmtime's `wasm_engine_t*` — and recreate everything
  else (runtime/module vs. store/module/instance) fresh every iteration, with the timer started
  before that per-iteration creation and stopped after per-iteration teardown. Confirmed by direct
  source inspection, not asserted.
- **Correctness-checked, not just timed.** Both hosts assert the call result equals 42 on every
  iteration and abort with a `FAIL` message on any parse/instantiate/call error — a fast wrong answer
  would not silently pass as a fast right one.
- **Identical guest bytes.** Both hosts load the same `add.wasm` file from disk — compiled once via
  `wasmtime_wat2wasm` from `(module (func $add (export "add") (param i32 i32) (result i32) local.get
  0 local.get 1 i32.add))` — so neither side gets an easier or harder module to parse.
- **Independent re-execution, not just trusting the first run.** Both executables were re-run
  directly (not re-measured by a fresh build) after the initial three-run report, on the same
  machine: `min_us=0.600 p50_us=0.700 p99_us=0.700 max_us=23.200` (wasm3) and `min_us=95.200
  p50_us=127.000 p99_us=268.700 max_us=1132.000` (wasmtime) — consistent with the original report's
  range across all four runs now on record.
- **wasm3 build provenance.** Cloned directly from `github.com/wasm3/wasm3` (not vendored/assumed),
  compiled from its own `source/*.c` with MSVC — no substitute or mocked library.

## 5. Executed evidence

Machine: this dev box, single-threaded, no parallel builds (`/MP` not used), MSVC 19.51.36252
(toolset 14.51.36231), Windows x64. `std::chrono::steady_clock`, 1000 iterations per run, percentiles
computed by sorting samples and indexing (no parametric estimate), min/max also reported.

| Run | Runtime  | n    | min (µs) | p50 (µs) | p99 (µs) | max (µs) |
|---|---|---|---|---|---|---|
| 1 | wasm3    | 1000 | 0.600 | 0.700 | 1.000 | 43.200 |
| 1 | wasmtime | 1000 | 97.200 | 111.000 | 240.800 | 1821.800 |
| 2 | wasm3    | 1000 | 0.600 | 0.700 | 0.700 | 23.700 |
| 2 | wasmtime | 1000 | 91.200 | 128.600 | 288.400 | 1117.300 |
| 3 | wasm3    | 1000 | 0.600 | 0.700 | 0.700 | 38.100 |
| 3 | wasmtime | 1000 | 95.900 | 112.800 | 253.300 | 1277.900 |
| 4 (independent re-run, this ADR) | wasm3    | 1000 | 0.600 | 0.700 | 0.700 | 23.200 |
| 4 (independent re-run, this ADR) | wasmtime | 1000 | 95.200 | 127.000 | 268.700 | 1132.000 |

Four runs, consistent: wasm3 p50 ≈ 0.7 µs, wasmtime p50 ≈ 111–128 µs — **a factor of roughly
160–180×**. p99: wasm3 ≈ 0.7–1.0 µs, wasmtime ≈ 241–289 µs — **roughly 250–400×**. Raw per-iteration
samples for all runs are on disk at `wasm3-check/wasm3_samples.csv` /
`wasm3-check/wasmtime_samples.csv` (1000 lines each, verified present).

Exact build commands (scratchpad `wasm3-check/`, all MSVC, vcvars64.bat sourced first):

```
git clone --depth 1 https://github.com/wasm3/wasm3 wasm3-check/wasm3
cl.exe /c /nologo /O2 /std:c11 /I wasm3\source  m3_bind.c m3_code.c m3_compile.c m3_core.c m3_env.c m3_exec.c m3_function.c m3_info.c m3_module.c m3_parse.c
lib.exe /nologo /OUT:m3.lib m3_bind.obj m3_code.obj m3_compile.obj m3_core.obj m3_env.obj m3_exec.obj m3_function.obj m3_info.obj m3_module.obj m3_parse.obj
cl.exe /nologo /std:c++20 /EHsc /O2 /I wasm3\source wasm3_host.cpp /link /LIBPATH:build m3.lib /OUT:wasm3_host.exe
cl.exe /nologo /std:c++20 /EHsc /O2 /I <wasmtime-c-api>\include wasmtime_host.cpp /link /LIBPATH:<wasmtime-c-api>\lib wasmtime.dll.lib /OUT:wasmtime_host.exe
```

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| W1 | **CORRECT** | Four consistent runs, 160–180× at p50, 250–400× at p99 — a wide margin, not measurement noise |
| W2 | **CORRECT** | wasm3 is a pure bytecode interpreter (`m3_ParseModule` walks/validates structure, no codegen); Wasmtime's `wasmtime_module_new` runs full Cranelift compilation to native code every iteration — this is the actual, structural reason for the gap, confirmed by what each API call does, not inferred from the timing alone |
| W3 | **PARTIALLY CORRECT, with a real caveat** | Wasmtime's measured cold-start (~91–129 µs p50, ~241–289 µs p99) sits comfortably under the provisional "Sandbox `wasm` create+exec+destroy p50/p99 ≤ 5 ms / ≤ 20 ms" goal (023 §3) for this trivial module — but this test never isolates a **warm** call, so it says nothing about the "Plugin call, warm invocation, p99 ≤ 100 µs" cell, and a real (non-trivial) plugin module would compile slower, which could threaten the "Plugin instantiate, pooled acquire, p99 ≤ 1 ms" cell if compilation isn't cached (023's own budgets are provisional/unbaselined regardless — see §7) |

## 7. The decision

**wasm3's cold-start advantage is real, large, and now measured on our own workload — it clears
OQ-12's bar to become a candidate.** It is **not** promoted to an actual second runtime by this ADR;
that remains future work gated on the items in §9, most importantly that Wasmtime's realistic
deployment shape (compile once, cache/reuse, instantiate cheaply per call — not recompile from
scratch every call, which is what this test deliberately measured for a fair cold-start comparison)
was never measured here and could substantially narrow the practical gap for AgentEngine's actual
architecture.

Concretely: **008 §1a's rejection of wasm3 as the *primary* plugin runtime is unchanged and
unaffected** — this measurement says nothing about Component Model/WASI 0.3 support, which is why
that rejection stands regardless of cold-start numbers. What changes is OQ-12's own gate: "blocked on
a 023-budget measurement before it is even a candidate" is satisfied — the measurement exists, is
real, and favors wasm3 by a wide margin for the specific shape of workload named (ultra-short,
high-frequency, cold-start-dominated guest calls). Whether to actually build a second runtime behind
008's `SandboxBackend` contract for that narrow niche is a **separate, larger decision** — it carries
008 §1a's own named cost ("two sandbox-escape surfaces and two conformance stories") that this ADR
does not evaluate, and should not be taken on this measurement alone.

## 8. Residual risks and deferred gates

- **Wasmtime's AOT/precompiled-module path was not measured.** `wasmtime_module_serialize` /
  `Engine::precompile`-class caching would remove per-call recompilation from the hot path in a
  realistic deployment; not measuring it here was deliberate (this ADR measures true cold start
  symmetrically for both sides) but means the practical gap for AgentEngine's actual architecture is
  still unknown, not narrowed by this result.
- **Trivial, memory-less, import-free module only.** No linear memory setup, no host-function
  imports, no WASI — real plugins (009 §5's host imports: `log`, `fs`, `http`, `tool-call`, `secrets`,
  `clock`/`random`, `blob`) would add instantiation cost to both sides, not necessarily
  proportionally.
- **023's own budgets are provisional, not baselined** (023 §3: "TBD-baselined until [G1] runs") —
  §6's comparison against those cells is context, not a pass/fail gate; a real budget baseline is a
  separate, larger undertaking (023 §7 G1) this ADR does not attempt.
- **No decision is made here on whether to actually build a wasm3 backend.** That decision needs its
  own design→red-team→prove→judge pass evaluating the "two sandbox-escape surfaces" cost 008 §1a
  names, which this ADR explicitly does not do.
- Evidence lives in scratchpad (`wasm3-check/`), not the repository — consistent with the OQ-7
  Wasmtime-smoke-test precedent for evidence that informs a decision without shipping as product
  code, since this ADR does not add a production wasm3 backend.
