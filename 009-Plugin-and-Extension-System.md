# 009 — Plugin and Extension System

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 006, 007, 008 · **Gate:** §10 · **Contract of record:** [`wit/`](wit/)

## Goal

One extension mechanism for everything third parties contribute — tools, skills, model providers,
memory stores, content filters — built on the **WASM Component Model**, so that an extension is a
single signed artifact that runs identically across the target platform set (021 §2), holds only the
capabilities the host hands it, and cannot take the host down.

## 1. Why the component model, specifically

- **One artifact, every targeted OS.** No per-platform build matrix, no per-platform loader, no
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

## 3a. Distribution: OCI artifacts, no first-party registry (resolves OQ-6)

**Plugins distribute as OCI artifacts, pushed to and pulled from any OCI-conformant registry** —
not through a first-party AgentEngine registry. Every property §3/§4 already specifies is something
OCI registries already provide, for free, at ecosystem scale:

- **Content addressing** — an OCI reference resolves to a digest; §4's "digest pinning by default"
  is simply what pulling `@sha256:...` already means, not a mechanism this project has to build.
- **Signing** — `plugin.aepkg`'s `SIGNATURE` file duplicates what Sigstore/cosign or Notation
  already do natively for OCI artifacts (keyless or key-based signing, verifiable by any consumer
  with standard tooling); a future package-format revision can attach the signature as an OCI
  referrer instead of a bundled file, though that is a format detail, not part of this decision.
- **Mirroring** — private registries, air-gapped copies (`oras cp` / `skopeo`), and pull-through
  caches are ordinary OCI operations, already deployed in every environment that would run this
  engine (011 §9 made exactly this argument about the MCP registry's own OCI-labels option).

**Why not a first-party registry:** it would mean standing up and operating infrastructure —
hosting, uptime, auth, quota, abuse moderation — to reinvent what OCI already does, and it would
recreate the exact hazard 011 §9 already documents for the MCP registry: **presence in a registry
reads as an implicit trust signal to users even when the registry explicitly disclaims one.** A
first-party registry we operate would face the identical choice — moderate content (expensive,
ongoing) or state plainly that presence proves nothing (which raises "then why build it"). OCI
carries none of that operational or trust-signaling burden, because nobody mistakes pushing to a
registry *we don't run* for our endorsement.

**What OCI does not solve — discovery — gets the lightest fix that works:** a curated, versioned
index (a git-hosted list of `{id, oci-ref, description}` entries, PR-reviewable, not a live service)
rather than a registry-side search feature. An index entry is a pointer, never an endorsement beyond
"known to exist" — keeping the same posture 011 §9 insists on for the MCP registry, applied to our
own plugin ecosystem instead of exempting it.

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

## 7. The C/C++ library track (first-party plugins) — the generic tool catalog

**This table is the generic tool catalog**, resolving `OpenQuestions.md` OQ-17: every candidate below
is source-agnostic and first-party, ships as a plugin under §6's contract, and exists so applications
stop reinventing the same dozen capability-crossing operations. The split that decides what belongs
here versus needing no `Tool` at all: an operation that crosses a capability boundary (network,
filesystem, hostile-input decode) belongs here; pure computation (math, JSON transforms, unit
conversion) needs no `Tool` declaration — the code interpreter (026 §1) already gives the model an
ordinary environment for that, and wrapping it as a tool would be a capability boundary with nothing
on the other side of it.

An explicit program, not a side effect: wrap high-value open-source native libraries as `ae:codec`
and `ae:tool` components so that capability-heavy functionality is available *without* host trust.
Candidates, in rough priority order:

| Plugin | Library | Capability it replaces |
|---|---|---|
| **Content reading** | Engine-native, not a wrapped library | A generic `read_content`-class tool for text/bytes from any granted source (worktree path, mounted input, or a URL under `NetOut`) — returns a bounded preview plus a `BlobRef` (003 §3) above the 006 §7 threshold, never raw materialized content, so "read this file" can never by itself exhaust a run's token budget (006 §7, 028 §2). First on this list because every application needs it on day one and it is the candidate most exposed to that hazard — it ships built-in rather than deferred to an operator plugin |
| Document extraction (text layer) | poppler / mupdf-class PDF, libxml2, and a DOCX/PPTX/XLSX structural parser (Docling/MarkItDown-class: one structural representation — layout, reading order, table cells — across formats) | Parsing hostile files in-process; deterministic default tier for 029 §5b's Extract stage |
| OCR | Tesseract-class | Reading text from scanned/image-only pages no text layer covers — not a `ChatClient` call, but imperfect; deterministic-tier alongside text-layer extraction, not attributed like tier 3 below |
| Layout-aware document extraction (VLM tier) | A vision-capable `ChatClient` call (004) rather than a wrapped library | Recovering table/form structure that tiers 1-2 can't — the attributed, budgeted, determinism-waived tier (029 §5b/§5d); a hallucinating extractor is model output, not document content, and must be provenanced accordingly (I3) |
| Structured code understanding | tree-sitter | Running a parser on untrusted source |
| Archive handling | libarchive, zlib/zstd | Zip-bomb exposure in the host |
| Media | image/audio codecs (libpng, libjpeg-turbo, libwebp, dr_libs) | Decoding hostile media in-process |
| Embedded data | SQLite, DuckDB | A database dependency in the host process |
| Tokenization | tokenizer libraries | Local token counting (004 Q3) |
| Document chunking | tree-sitter (code-aware) / a markdown-and-plain-text structural splitter, token-bounded via the tokenization candidate above | Multiple swappable `ae:codec` strategies (recursive/structure-aware default, fixed-size, semantic/contextual as opt-in upgrades) feeding knowledge-corpus ingestion (029 §5b) — no single strategy is fixed by spec, evidence disagrees on which wins per corpus |
| Local inference | ONNX Runtime (WASM build) | Embeddings/classification without a host ML dependency |
| Graph extraction & community detection | An entity/relationship extractor (LLM-backed, 029 §5c) + a Leiden-class clustering library | Knowledge-graph-shaped retrieval (029 §5c) as an alternative to flat chunk retrieval — clustering runs deterministically in-process; extraction/summarization are attributed `ChatClient` calls, not host-side ML plumbing |
| CLI-style tools | `git`, `ripgrep`, `fd`, and similar, wrapped as `ae:tool` components | Named commands `ShellRunner` (010 §1a) can dispatch to, without ever resolving or exec'ing an arbitrary host binary — see 010 §2 for why the shell itself is engine-native, not a wrapped binary |
| Remote execution (SSH) | An SSH client library (e.g. libssh2), wrapped as an `ae:tool` bundled with a `SKILL.md` teaching its use (§8c) | An unmediated `ssh` subprocess call agents would otherwise reach for. Gated by `NetOut<host>` + `Secret<name>` (007 §3, 018 §4), resolved fresh per invocation — no session handle persists across calls (006 §8 G3); any underlying connection reuse is host-side plumbing, invisible to the capability system, the same as HTTP keep-alive under an ordinary `NetOut` client |
| Real-time voice (speech-to-speech) | An `ae:provider`-shaped WebSocket/WebRTC client over a vendor realtime API | **Not yet a spec-ready candidate.** `docs/research/2026-08-05-voice-and-computer-use.md` §1: turn interruption is the common case (not exceptional), and no vendor supports deterministic session replay — a direct tension with **I5** unresolved by any mechanism this spec already has, not a plugin-shaped gap |
| Computer use (screen capture + input injection) | A vision-driven `click`/`type`/`scroll`/`screenshot` action set, `remote`-profile-only (008 §3, matching CLAUDE.md's hardware-isolation rule) | **Not yet a spec-ready candidate.** Needs new parameterized capabilities (`ScreenCapture<display>` / `InputInject<display>`, 007 §3-shaped). Two real, documented attacks (`docs/research/2026-08-05-voice-and-computer-use.md` §2: ZombAIs, HiddenLayer, both Oct. 2024) defeat 017's text-delimiting defense because the injected instruction arrives as pixels, not text the engine frames — an actual gap in 017's model, not an extension of it |

**Rule:** a proposed *host* dependency must be evaluated as a plugin first, and the RFC or ADR
adopting it as a host dependency must say why the plugin path was rejected.

## 8. Skills

A **skill** bundles instructions, resources, and optional scripts that give an agent a competence on
demand. Four different things in this ecosystem share the word, so this section states precisely
which one we implement.

### 8a. The interchange format is `SKILL.md` (agentskills.io)

**Adopted as the skill format of record.** It is a genuine open standard — published at
`agentskills.io/specification`, Apache-2.0, originally authored by Anthropic and released as an open
standard in December 2025 — and it is implemented by both Anthropic's products and Microsoft Agent
Framework with the *identical* directory layout and frontmatter. Adopting anything else would mean
being incompatible with the entire existing skill corpus for no benefit.

```
skill-name/
├── SKILL.md          # YAML frontmatter + Markdown instructions (the frontmatter IS the manifest)
├── scripts/          # optional executable code
├── references/       # optional documentation
└── assets/           # optional templates and resources
```

Frontmatter: **`name`** (required, 1–64 chars, `a-z0-9` and hyphens, no leading/trailing hyphen, no
`--`, **must match the directory name**), **`description`** (required, 1–1024 chars, stating what it
does *and when to use it*), `license`, `compatibility`, `metadata` (string→string), `allowed-tools`
(space-separated, marked experimental upstream).

Two properties of the format we must handle rather than assume away:

- **There is no `version` field.** Convention places it in `metadata.version`. Our loader records
  the package digest as the real identity and treats `metadata.version` as a label.
- **The spec is prose plus a constraints table** — no JSON Schema, no RFC-2119 language, and no
  version identifier on the specification itself. Our importer therefore validates against the
  constraints explicitly and rejects rather than guessing.

### 8b. Progressive disclosure comes free from the worktree

The format's three-level disclosure — ~100 tokens of `name` + `description` for every skill, the
`SKILL.md` body on activation (target < 5 000 tokens), bundled files only on demand — is exactly the
shape an ordinary filesystem affords. So skills are **mounted read-only at `/skills/<name>`**
(025 §3) and the agent reads them with ordinary file operations (026 §6). We do not introduce
`load_skill` / `read_skill_resource` tool wrappers; the mount is the mechanism. This is a deliberate
divergence from MAF's own implementation, which uses exactly those two tools (plus `run_skill_script`)
as a fixed three-tool surface regardless of catalog size, rather than a filesystem mount — confirmed
by reading its source, not assumed (`docs/research/2026-maf-provider-concepts.md` §1). Neither MAF
nor anything else surveyed does vector/semantic search over skills or tools; "many skills" is solved
by advertising cheaply and loading lazily, not by search, in both designs.

The token property that makes skills worth having is preserved: a bundled script's **stdout enters
the context, its source does not**.

### 8c. Executable skills are plugins

A skill that ships **code** is packaged as an `ae:skill` plugin (§3) and inherits the whole trust
pipeline: signed, capability-declaring, operator-approved, digest-pinned, revocable. A skill that is
pure instructions and references needs no component and is mounted directly.

**`allowed-tools` is advisory, never a grant.** Upstream marks it experimental and its support
varies by runtime — Anthropic's own Agent SDK documents that it is not honoured through the SDK and
that skill filtering "is a context filter, not a sandbox". We treat it as a *request* to be
reconciled against the operator's grant, exactly like a plugin manifest (§3): the manifest declares,
the operator grants, capabilities enforce (007 §3).

**Skill names are labels, not identifiers.** Skills are namespaced per origin so a skill fetched
from a remote source can never shadow a local one — a shadowing attack is otherwise trivial.

**Amendment (Phase 1 implementation, `core/skill_provider.hpp`).** The mount path itself stays the
flat `/skills/<name>` §8b already specifies — namespacing every path by origin was considered and
rejected, since it would both deviate from §8b's own text and make the common single-source case
needlessly verbose. Anti-shadowing is enforced instead by **reject-on-collision**: if two declared
sources would produce the same skill name, loading fails closed for the whole set (no partial
mount — not even the skill that was processed first survives) with a named error
(`skill.name_collision_across_sources`) identifying both origins. A silent last-source-wins is never
reachable; the operator sees a real, specific load failure and must resolve the collision (rename,
drop a source) rather than have it resolved for them.

Skill loading is dynamic but **snapshotted per run** (006 §6): a skill loaded mid-run does not
retroactively change what earlier turns were permitted to do.

### 8d. Skills over MCP: we specify nothing yet, deliberately

Verified against the specification, the schema, and the extension registry: **there is no skill
primitive in MCP at `2026-07-28`, and no official MCP extension for skills.** The string does not
appear in the normative schema.

What exists is a **Skills Over MCP Working Group** (formed February 2026, promoted to a WG in April)
whose current direction is **SEP-2640, an unmerged Draft** on the Extensions Track that would add
`io.modelcontextprotocol/skills` with `skills/list` and `skills/get` over a `skill://` URI scheme.
Its design **has already changed once**: the earlier `skill://index.json` discovery approach was
superseded — and Microsoft's shipped, experimental `UseMcpSkills` / `MCPSkillsSource` implements the
*superseded* design. The MCP roadmap places skills under "On the Horizon", explicitly not a priority.

**Therefore we implement no skills-over-MCP conformance.** What we do instead is cheap and
sufficient: keep extension negotiation (011 §3.6) general enough that
`io.modelcontextprotocol/skills` slots in as configuration when and if it lands. Building against a
Draft whose wire shape has already turned over once would be building a migration.

One clause from that draft is worth adopting *now*, on its merits, whatever happens to the SEP:

> Hosts **MUST NOT** treat a digest match as a security boundary — digests are unsigned and
> server-supplied.

That is exactly why §3 requires signatures and §4 verifies them before parsing anything.

### 8e. What "skill" does *not* mean here

- **A2A `AgentSkill`** (012) is a discovery record inside an Agent Card — `{id, name, description,
  tags, examples}`. No `SKILL.md`, no instructions body, no bundled files, no progressive
  disclosure. Functionally it is closer to an MCP `Tool` listing than to a skill in this section's
  sense. Pure vocabulary collision; the two must never be conflated in code or documentation.
- **Semantic Kernel "skills"** were renamed to *plugins* years ago and mean a group of functions —
  i.e. our tools, not our skills.

Details and sources: [`docs/research/2026-mcp-ecosystem.md`](docs/research/2026-mcp-ecosystem.md).

### 8f. Generic skills (the other half of OQ-17)

First-party `SKILL.md` bundles, mounted exactly like any other skill (§8b) — no special loader, no
special trust tier — teaching the model how to use surfaces the engine itself provides. These earn
their place precisely because a model has *not* seen a million examples of them, unlike ordinary
Python (026 §1's whole point):

| Skill | Teaches |
|---|---|
| `using-the-code-interpreter` | Idioms for `execute_code` (010 §1); when a single call suffices versus when CodeAct's multi-step form pays for itself |
| `using-codeact` | Worked `agent.*` examples (026 §5) — filtering large results in-process instead of round-tripping every row through the model |
| `reading-large-content` | When to use §7's content-reading tool's preview-then-page pattern instead of asking for a whole file, tying directly to 006 §7's token-budget rule |
| `producing-structured-output` | Shaping a final response against a declared schema (003 §5) reliably |
| `shell-pipelines` | `ShellRunner`'s grammar (010 §2) — composing pipes/redirects idiomatically within its documented subset |

These ship in this repo, not as a separate download, and are mounted by default subject to the same
per-session grant model as any skill (§8c) — "built-in" means "shipped and trusted by default," not
"ungoverned."

## 9. Observability

Per invocation: plugin id + version + digest, world, instantiation (cold/warm/pooled), fuel/CPU,
memory peak, capabilities used, egress hosts, outcome. Plugin identity is a first-class metric
dimension so a misbehaving plugin is visible without code changes.

## 10. Promotion gate

- **G1** — the same `ae:tool` component binary loads and produces identical results on every
  platform in the current target set (021 §2 — byte-identical outputs for a deterministic fixture).
- **G2** — a component whose imports exceed its manifest fails to load; a component that requests a
  capability the operator did not grant fails to instantiate. Positive controls included.
- **G3** — a trapping/looping/allocating-forever/output-flooding plugin is contained within its
  declared limits, with measured kill time, and the host is unaffected.
- **G4** — warm invocation overhead (host→guest→host, trivial function) within the 023 budget;
  pooled instantiation p99 measured.
- **G4a** — a real streaming fixture (§11 Q1) run against an `ae:provider` plugin, measuring
  per-chunk host↔guest crossing cost against 004 §7 G4's existing host-side streaming budget;
  determines whether `ae:provider` stays viable for latency-sensitive streaming or falls back to a
  direct 004 §3 backend addition.
- **G5** — revocation cancels in-flight calls; a retained host handle is unusable afterwards.
- **G6** — one real C/C++ library from §7 ships as a plugin end-to-end, with the sandboxed build
  reproducible from source.

## 11. Open questions

- ~~**Q1** — Whether `ae:provider` plugins are viable for latency-sensitive streaming, or whether
  providers should stay host-side seams.~~ **Resolved, conditionally viable, gated on a measurement
  rather than asserted (2026-08-04):** the architectural question is already answered — WASI 0.3's
  `stream<T>`/`future<T>` map onto Quark coroutines (§1) specifically so guest streaming doesn't need
  a thread per call, so there's no structural reason `ae:provider` can't stream. What's unproven, with
  no implementation to measure (design phase), is the marginal per-chunk host↔guest crossing cost
  against a real streaming provider, which 004 §7 G4's existing host-side budget doesn't include.
  Resolved as: **default stays host-side `ChatClient` seams** (004 §3) for the two first-class
  backends — `ae:provider` remains the stated extensibility path for exotic/local backends (already
  in 004 §3's table), provisional on a promotion-gate measurement (this RFC's §10 G4a) rather than a
  blanket verdict.
  If that measurement shows per-chunk overhead materially worse than the host-side budget, a
  latency-sensitive exotic backend is documented as belonging as a direct 004 §3 backend addition
  instead of an `ae:provider` plugin — a fallback stated now so the decision isn't reopened from
  scratch if the measurement is unfavorable.
- ~~**Q2** — Distribution: a first-party registry, or reuse of an existing artifact registry (OCI)?
  OCI is tempting — content-addressed, signed, mirrorable, already deployed everywhere.~~
  **Resolved, OCI, pull-only, no first-party registry (OQ-6, 2026-08-03/04; see also §3a):** a real,
  executed pull round trip against a public OCI registry (Docker Hub: anonymous bearer-token
  exchange → image-index manifest → platform manifest → content-addressed config blob, the blob
  fetch redirecting to separate CDN storage) needed nothing beyond plain HTTPS and a SHA-256 check —
  every returned digest verified byte-for-byte against an independently computed hash. §3's
  distribution needs (content-addressed identity, digest pinning, signature carriage) are already
  what the protocol gives for free; building a first-party registry means also building auth,
  storage, GC, and mirroring that OCI registries already operate at the scale this project would
  otherwise have to reach on its own. **What AgentEngine builds is a minimal pull-only client**
  (token exchange, manifest fetch, blob fetch by digest, SHA-256 verify — a few hundred lines against
  an existing HTTP capability, the same "system-API, not a third-party dependency" framing as
  Windows CNG/BCrypt in `decisions/ADR-005-capability-bearer-tokens-cross-process.md`) — push,
  garbage collection, and chunked resumable upload are out of scope entirely, because AgentEngine is
  never the publisher: whoever authors a plugin publishes it with existing standard tooling (`oras`,
  `docker`, `skopeo`). **One concrete detail this decision surfaces, not solved here**: the blob fetch
  redirected to a host distinct from the registry API host, so an egress allowlist (007 §3 `NetOut`,
  008 §4) for the plugin-pull path must cover the registry's blob-storage host(s) too, not just the
  registry hostname — a per-provider detail for whoever implements §4's host imports. **Also not
  solved here**: how `plugin.aepkg` (§3) maps onto an OCI artifact (one layer blob plus
  `manifest.toml` fields projected to annotations, `SIGNATURE` moved to an OCI referrer per the
  Notation/cosign convention rather than bundled in the zip) — named as follow-on design work, not
  this pass's job.
- ~~**Q3** — Whether plugins should be able to declare *typed* WIT interfaces for their own tools
  rather than JSON Schema, with the schema generated from WIT (better typing, harder MCP interop).~~
  **Resolved, Yes — WIT is the source of truth, JSON Schema is generated from it (2026-08-04):** this
  is the same single-source-of-truth discipline the project already applies everywhere a schema
  exists (011 §4: "Tools are generated from tool metadata — one schema source, so a listing cannot
  drift from the tool's actual contract") — §3's package format already ships a `schema/` directory
  described as "generated," and this makes explicit what it's generated *from*: the component's own
  WIT-typed exports, mechanically, never a separately hand-authored JSON Schema merely checked for
  consistency against them (which is how the two would silently drift). The "harder MCP interop"
  concern turns out narrower than it sounds: MCP still receives ordinary JSON Schema 2020-12 at the
  wire boundary either way (011 §3.1), so interop doesn't get harder at the protocol level — what's
  harder is only authoring a JSON-Schema-idiomatic shape WIT's simpler type system can't naturally
  express (some `oneOf`/`anyOf` compositions). For that narrow case, a plugin author uses a typed
  escape hatch (a WIT `json-value`-shaped type for that one parameter) rather than the tool's whole
  schema falling back to hand-authored — the same "declared shape plus an escape hatch for what
  doesn't fit" pattern already used elsewhere (003 §1's `Custom` content kind).
- ~~**Q4** — Pinning to a Wasmtime version that ships WASI 0.3 by default (46+) versus supporting a
  range; the async ABI difference is not a small compatibility surface.~~ **Resolved 2026-08-03/04
  (see OpenQuestions.md OQ-7): pin to a single version, currently 47.0.3** (the latest release as of
  this date) — no 0.2/0.3-RC compatibility range. Unlike MCP/A2A, Wasmtime is a build-time embedded
  dependency, not a wire peer whose independently-released versions we must interoperate with
  regardless of our own choice — nothing external forces us to support more than one at a time. Pin
  to one specific version, the same discipline CONVENTIONS already applies to Quark's submodule
  commit, CMake's floor, and the compiler versions in 021 §5: a bump is a deliberate, single-commit
  change gated on the full 008/009 hostile and conformance suites passing clean against the new
  version before the pin moves, never an ongoing dual-version support burden. Doubling conformance
  and sandbox-escape surface to track two Wasmtime majors concurrently — real cost the "async ABI is
  not a small compatibility surface" framing was right to flag — buys nothing when no external actor
  requires it. Verified against the real, downloaded Windows x64 C API release: Component Model
  headers (`wasmtime/component/*`) and WASI 0.3's async primitives (`stream`/`future`, referenced in
  `component/func.h`, `component/linker.h`, `component/types/val.h`) are present in the shipped
  headers, not just documented; a real engine/store/module/instance/call round trip against this
  exact release succeeds under MSVC 19.51.36252 (the same toolset the rest of this project uses).
