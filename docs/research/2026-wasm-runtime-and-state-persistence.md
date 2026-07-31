# Research record — WASM runtime selection and live-session state persistence

**Compiled:** 2026-07-31 · **Status:** dated snapshot · **Decides:** 008 §1 runtime requirement, 008 §6 lifetime, 010 §2

**Note (later same day):** §2's framing of the wasm/native trade as an operator choice, and the
`microvm` profile referenced throughout, are **superseded** by 008 §1/§1b and 010 §2 — the Python
interpreter is embedded native CPython permanently (no WASM Python backend, regardless of this
section's snapshot-portability finding) and `microvm` is dropped as a profile entirely. The
technical findings below are not wrong and are left as compiled; the *trade-off framing* built on
top of them no longer reflects the current decision.

Triggered by two questions: *"can we use WASM3 because it has more features"*, and *"keep the WASM
sandbox live with the session so files and state can be kept/restored"*.

---

## 1. "WASM3" is ambiguous, and the two readings point opposite ways

### 1a. `wasm3` — the runtime

[`wasm3/wasm3`](https://github.com/wasm3/wasm3) is a fast **interpreter**-based WebAssembly runtime,
notable for tiny footprint, extreme portability (down to microcontrollers), and **the best cold
start of any runtime** — as of January 2026 it is the cold-start winner, while Wasmtime keeps the
edge on steady-state execution.

But on the axis that matters for our plugin ABI, it is **behind, not ahead**: wasm3 shows only
**partial support for WASI Preview 2 and the Component Model**. It passes the core spec testsuite
and runs many WASI apps, but it does not implement the component model that RFC 009's entire plugin
design is built on — WIT worlds, typed imports/exports, and WASI 0.3 async (`async func`,
`stream<T>`, `future<T>`).

**Verdict: wasm3 cannot host the plugin ABI.** "More features" is the wrong way round here — it has
fewer of the features this design depends on. It remains genuinely attractive for a *different*
job (see §3).

### 1b. **WebAssembly 3.0** — the specification

[Wasm 3.0 was completed on 2025-09-17](https://webassembly.org/news/2025-09-17-wasm-3.0/), three
years after 2.0, and it *does* have substantially more features:

| Feature | Why it matters here |
|---|---|
| **Garbage collection** | Managed languages (Java, Kotlin, Dart, C#) compile to Wasm without shipping their own GC — widens who can write plugins |
| **Exception handling** | Native Wasm exceptions with typed tags. **C++ plugins can use exceptions**, which matters for the C/C++ library track (009 §7) — many real libraries assume them |
| **Memory64** | `i64` addressing, 4 GB → 16 EB. Removes the 4 GB ceiling for data-heavy interpreter and codec workloads |
| **Tail calls** | Functional languages and compiler-generated code stop blowing the stack |
| **Relaxed SIMD, multiple memories** | Throughput; cleaner separation of guest arenas |

It is shipping in major browsers, and standalone engines including **Wasmtime** are completing their
implementations.

**Verdict: yes — and this is orthogonal to runtime choice.** Wasm 3.0 is a specification level we
should *require of whichever runtime we pick*, not an alternative to Wasmtime.

### 1c. The resulting requirement

The plugin host requires a runtime that provides **all** of: the Component Model, **WASI 0.3**
(async, `stream<T>`, `future<T>`), and **Wasm 3.0** core features (at minimum exception handling and
memory64, for the C/C++ track). Today that is **Wasmtime 46+**, which ships WASI 0.3 with Component
Model Async enabled by default. wasm3 satisfies none of the three.

## 2. Live-session state: what actually persists, and how

The goal — a conversation whose sandbox stays alive so files and state survive across turns,
passivation, and restart — decomposes into **two different problems** with two different answers.

### 2a. Files → the worktree, not the sandbox

Files must **not** depend on sandbox survival. They belong to a durable, content-addressed virtual
filesystem the engine owns (RFC 025), backed by Quark's `Store` seam and checkpointed with the
session. A sandbox mounts a subtree; destroying the sandbox loses nothing.

This is the single most important structural decision here: it makes file persistence independent of
runtime, profile, OS, and process lifetime. Every profile gets identical file semantics.

### 2b. Interpreter memory → profile-dependent, and honestly so

In-memory interpreter state (variables, imported modules, open handles) is a different matter.

**Wasmtime has no built-in snapshot/restore.** The feature requests
([#3017](https://github.com/bytecodealliance/wasmtime/issues/3017),
[#4002](https://github.com/bytecodealliance/wasmtime/issues/4002)) date to 2021–2022 and remain
unimplemented in 2026. The upstream guidance is to externalize state rather than snapshot it.

However, the same discussions confirm the mechanically important part: **dumping linear memory to a
byte array is sufficient to capture the memory itself** — what is hard is capturing *execution*
state (the stack, in-flight host calls). That distinction is what makes a restricted form viable:

> **Snapshot only at quiescent points** — between executions, when no guest stack exists. At that
> moment a WASM Python interpreter's entire heap *is* its linear memory, so a snapshot is a memory
> dump plus the store's table/global state, and a restore is the inverse.

That is a real capability we would implement ourselves (Wasmtime does not hand it to us), and it is
portable across Windows, Linux, and macOS because linear memory is linear memory everywhere.

**For native processes, the equivalent is CRIU** — and it does not generalize: Linux-only, and by its
own documentation active network connections often do not survive, complex socket states may not
restore cleanly, and GPU/device-mapped workloads frequently fail. It is not a cross-platform
mechanism, so it cannot back a portable contract.

### 2c. Consequence

| | `wasm` profile | `native-jail` / `microvm` |
|---|---|---|
| Files across turns | Worktree — durable, always | Worktree — durable, always |
| Variables across turns *while session is active* | Live instance retained | Live process retained |
| Variables across **passivation / restart** | **Snapshot + restore at quiescent points** (we implement) | **Lost** — session resumes with a clean interpreter; worktree carries the rest |
| Portability of that guarantee | All three OSes | CRIU: Linux-only, fragile |

**This is a genuine argument for the `wasm` profile that RFC 010 §2 did not weigh.** It does not
overturn the package-ecosystem finding — a WASI Python still cannot `import numpy` — but it converts
the interpreter backend from "obvious default" into a real trade the operator makes:

> **Full package ecosystem, clean interpreter after passivation** (`native-jail` / `microvm`)
> **versus portable, restorable in-memory state with a stdlib-only Python** (`wasm`).

Both are legitimate; they suit different products. The seam is what lets the choice be made per
deployment rather than per framework.

## 3. Where wasm3 could still earn a place

Not as the plugin host, but its cold-start and footprint advantages are real. Two candidate niches,
neither adopted without a gate:

- **Ultra-short, high-frequency guest calls** where instantiation dominates and the guest needs no
  component-model imports — e.g. a content filter (`ae:filter`) evaluated on every message.
- **Constrained or embedded deployments** where Wasmtime's footprint is prohibitive and the plugin
  set is fixed and simple.

Both would mean a **second runtime behind the same host interface**, which is a real maintenance and
security cost (two sandbox escape surfaces, two conformance stories). Recorded as an open question,
not a plan.

## 4. Not verified

- Wasmtime's exact completion status per Wasm 3.0 proposal (GC / EH / memory64 / tail calls) at a
  specific version — "on track to completion" is what the source says, and we should pin a version
  and test the features we depend on rather than trust a roadmap.
- Whether any runtime now offers production snapshot/restore of component instances.
- wasm3's current WASI P2 coverage in detail (reported as partial; not enumerated).
- Real cold-start and steady-state numbers on our own workload — the cited comparison is generic and
  must be re-measured against 023 budgets before any second-runtime decision.

**Sources:** [Wasm 3.0 completed](https://webassembly.org/news/2025-09-17-wasm-3.0/) ·
[wasm3](https://github.com/wasm3/wasm3) · [wasm3 runtime feature matrix](https://wasmruntime.com/en/runtimes/wasm3) ·
[WASI 0.3](https://wasi.dev/releases/wasi-p3) ·
[Wasmtime issue 3017](https://github.com/bytecodealliance/wasmtime/issues/3017) ·
[Wasmtime issue 4002](https://github.com/bytecodealliance/wasmtime/issues/4002) ·
[CRIU](https://criu.org/Main_Page) · [WebAssembly ecosystem 2026](https://zylos.ai/research/2026-02-05-webassembly-ecosystem-2026/)
