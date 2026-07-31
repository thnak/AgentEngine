# 009 — Plugin and Extension System

**Status:** Draft · **Depends on:** 006, 007, 008 · **Gate:** §10 · **Contract of record:** [`wit/`](wit/)

## Goal

One extension mechanism for everything third parties contribute — tools, skills, model providers,
memory stores, content filters — built on the **WASM Component Model**, so that an extension is a
single signed artifact that runs identically on Windows, Linux, and macOS, holds only the
capabilities the host hands it, and cannot take the host down.

## 1. Why the component model, specifically

- **One artifact, three OSes.** No per-platform build matrix, no per-platform loader, no
  `dlopen`/`LoadLibrary` ABI hazard, and no native-plugin crash taking the engine with it.
- **Capability-based by construction.** A component has no ambient authority: it reaches the world
  only through imports the host supplies. This is **I2** enforced by the runtime rather than by
  discipline.
- **Typed interfaces, not `void*`.** WIT worlds give versioned, checkable contracts across the
  boundary, with generated bindings on both sides.
- **Language-agnostic — including the C/C++ ecosystem.** `wasi-sdk` (clang + wasi-libc) compiles
  ordinary C and C++, so the large permissively-licensed native library ecosystem becomes available
  as *sandboxed* plugins. This is the point that makes the plugin story expansive rather than
  limiting: **a heavy library is safer as a plugin than as a linked host dependency**, because a
  plugin holds only what it is granted (see §7).
- **Async without threads.** WASI 0.3 (2026-06-11) put `async func`, `stream<T>`, and `future<T>`
  in the Canonical ABI, which maps onto Quark's coroutine handlers instead of a thread per call.

Ecosystem risk here is fundamentally different from the Python-on-WASM risk that shaped 010: **we**
define the WIT world, and plugin authors compile *to* it from the language they already use. There
is no dependency on a third-party package index having WASM builds.

## 2. Plugin kinds (the WIT worlds)

| World | Implements | Typical authors |
|---|---|---|
| `ae:tool` | One or more tools (006): schema, invoke | Everyone |
| `ae:skill` | A named bundle of instructions + tools + resources, loadable on demand | Domain packagers |
| `ae:provider` | The model provider seam (004) | Exotic/local inference protocols |
| `ae:memory` | Memory/vector/retrieval store (005) | Store vendors |
| `ae:filter` | Content/safety filter over messages and tool results (017) | Safety vendors |
| `ae:codec` | Content transformation: parse, extract, transcode, tokenize (003) | The C/C++ library track (§7) |

Each world is versioned (`ae:tool@1.0.0`). The engine supports N-1 world versions with a declared
deprecation window mirroring MCP's 12-month policy (024).

## 3. Package format

```
plugin.aepkg  (zip)
  manifest.toml         identity, version, world, capabilities requested, limits, entry points
  component.wasm        the component binary
  schema/               tool schemas (generated; validated against the component's exports)
  assets/               static resources the plugin may read (mounted read-only)
  SIGNATURE             detached signature over a manifest digest that covers every file
```

**Manifest declares, the operator grants.** The manifest is a *request*, never a grant:

```toml
[plugin]
id = "org.example.pdf-tools"
version = "1.4.2"
world = "ae:tool@1.0.0"

[capabilities]
fs_read  = ["${input}"]                 # requested mounts, parameterized
net_out  = []                           # none requested
secrets  = []

[limits]
memory_bytes = 268435456
wall_ms      = 30000
```

Load fails closed if the component imports anything the manifest does not declare — the manifest
cannot under-declare its way past the operator's approval.

## 4. Lifecycle

```
discover → verify signature → parse manifest → operator approval (capabilities shown)
        → compile/AOT-cache → instantiate (pooled) → invoke → destroy
        → hot-unload on revocation (in-flight calls canceled)
```

- **Verification before parsing anything else.** Signature and publisher first (007 §7).
- **Approval is over capabilities, not over trust vibes.** The operator sees exactly what §3
  requests; a version bump that widens the request requires re-approval.
- **Digest pinning by default**; floating versions are opt-in.
- **AOT compilation is cached** by `{component digest, runtime version, target}` so instantiation is
  microseconds on the hot path, not a compile.
- **Instance pooling with pre-input snapshot reset** (008 §6) — pooling never becomes a
  cross-invocation channel.
- **Revocation is runtime**: unload cancels in-flight calls and prevents new ones.

## 5. Host imports — what a plugin can reach

A plugin's imports are materialized per invocation from the **intersection** of its manifest
request, the operator grant, and the caller's capability set (007 §3 attenuation-only):

`log` · `metrics` · `fs` (preopened dirs only) · `http` (host-mediated egress, allowlisted) ·
`tool-call` (invoke another tool — executing at the *plugin's* trust tier, not the agent's) ·
`secrets` (resolve one granted name at point of use) · `clock` / `random` (virtualizable) ·
`blob` (content-addressed read/write, 003 §3)

**Not available in any world:** raw sockets, subprocess, nested sandbox creation, arbitrary
filesystem, host environment, or any handle that outlives the invocation.

## 6. Execution and limits

Plugins run in the `wasm` profile (008) under the same contract as any sandbox: memory, fuel/epoch
CPU limits, wall clock, output size, and a bounded cancellation. A plugin trap is a structured
result, never a host fault. The host never blocks a session activation on a plugin call (001 §8).

## 7. The C/C++ library track (first-party plugins)

An explicit program, not a side effect: wrap high-value open-source native libraries as `ae:codec`
and `ae:tool` components so that capability-heavy functionality is available *without* host trust.
Candidates, in rough priority order:

| Plugin | Library | Capability it replaces |
|---|---|---|
| Document extraction | poppler / mupdf-class PDF, libxml2 | Parsing hostile files in-process |
| Structured code understanding | tree-sitter | Running a parser on untrusted source |
| Archive handling | libarchive, zlib/zstd | Zip-bomb exposure in the host |
| Media | image/audio codecs (libpng, libjpeg-turbo, libwebp, dr_libs) | Decoding hostile media in-process |
| Embedded data | SQLite, DuckDB | A database dependency in the host process |
| Tokenization | tokenizer libraries | Local token counting (004 Q3) |
| Local inference | ONNX Runtime (WASM build) | Embeddings/classification without a host ML dependency |

**Rule:** a proposed *host* dependency must be evaluated as a plugin first, and the RFC or ADR
adopting it as a host dependency must say why the plugin path was rejected.

## 8. Skills

A **skill** is a plugin kind (`ae:skill`) that bundles instructions, tools, and resources an agent
loads on demand — the "give the agent a new competence" unit. It is deliberately the same artifact
and the same trust pipeline as any other plugin: signed, capability-declaring, revocable.

Skill loading is dynamic but **snapshotted per run** (006 §6): a skill loaded mid-run does not
retroactively change what earlier turns were permitted to do.

Interop with external skill conventions (Anthropic's `SKILL.md` packaging, MAF's agent skills, and
anything MCP standardizes) is an **importer**, not a second mechanism: an external skill package is
converted into an `ae:skill` at load, subject to the same approval. The exact mapping is pending the
MCP/skills research feeding RFC 011.

## 9. Observability

Per invocation: plugin id + version + digest, world, instantiation (cold/warm/pooled), fuel/CPU,
memory peak, capabilities used, egress hosts, outcome. Plugin identity is a first-class metric
dimension so a misbehaving plugin is visible without code changes.

## 10. Promotion gate

- **G1** — the same `ae:tool` component binary loads and produces identical results on Windows,
  Linux, and macOS (byte-identical outputs for a deterministic fixture).
- **G2** — a component whose imports exceed its manifest fails to load; a component that requests a
  capability the operator did not grant fails to instantiate. Positive controls included.
- **G3** — a trapping/looping/allocating-forever/output-flooding plugin is contained within its
  declared limits, with measured kill time, and the host is unaffected.
- **G4** — warm invocation overhead (host→guest→host, trivial function) within the 023 budget;
  pooled instantiation p99 measured.
- **G5** — revocation cancels in-flight calls; a retained host handle is unusable afterwards.
- **G6** — one real C/C++ library from §7 ships as a plugin end-to-end, with the sandboxed build
  reproducible from source.

## 11. Open questions

- **Q1** — Whether `ae:provider` plugins are viable for latency-sensitive streaming, or whether
  providers should stay host-side seams.
- **Q2** — Distribution: a first-party registry, or reuse of an existing artifact registry (OCI)?
  OCI is tempting — content-addressed, signed, mirrorable, already deployed everywhere.
- **Q3** — Whether plugins should be able to declare *typed* WIT interfaces for their own tools
  rather than JSON Schema, with the schema generated from WIT (better typing, harder MCP interop).
- **Q4** — Pinning to a Wasmtime version that ships WASI 0.3 by default (46+) versus supporting a
  range; the async ABI difference is not a small compatibility surface.
