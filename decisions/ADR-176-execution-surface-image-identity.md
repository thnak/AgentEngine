# ADR-176 — Which sandbox image did a command actually run in, and which surfaces are allowed to answer?

- **Status**: Proposed — implemented and proven, pending project-owner sign-off.
- **Date**: 2026-09-18
- **Closes**: GitHub issue #80 (PR #83, commit `5404f1e`, plus an uncommitted follow-up adding a memory
  cap to the new test — see §7).
- **Refines** ADR-102 Phase 3's `ExecutionSurface` concept without widening it, and extends
  ADR-145's `ContainerdExecutionSurface`. **Reopens nothing** in ADR-099 §7 (the disclosed
  `ExecutionSurface`-is-not-a-`SandboxBackend` boundary is untouched), ADR-171/ADR-172 (isolation),
  or ADR-174 (seed ownership).
- **Files**: `include/agentengine/sandbox/execution_surface.hpp`,
  `include/agentengine/sandbox/docker_execution_surface.hpp`,
  `include/agentengine/sandbox/containerd_execution_surface.hpp`,
  `include/agentengine/sandbox/mandatory_sandbox_provider.hpp`,
  `tests/test_execution_surface_image_identity.cpp` (new), `tests/test_containerd_execution_surface.cpp`,
  `tests/test_mandatory_sandbox_provider.cpp`, `tests/CMakeLists.txt`, `.github/workflows/ci.yml`,
  `027-Vocabulary-and-Naming.md`.
- **Why an ADR at all.** PR #83 argued none was needed: additive observability, no capability decision
  touched. That is true of the surface area (§2, last paragraph) and false of two things underneath it.
  First, the resolution sits on the per-command hot path — `SandboxRuntime::run()` calls `reset()` once
  per tool call — and the shipped design is a **measured** cost trade with an accepted staleness case
  (§3e, §4, §6). Second, "empty means *not known* and is never backfilled from the configured
  reference" is a provenance-integrity rule, not an implementation detail: it is one convenient-looking
  edit away from being broken, and the mutant table in §7 exists because of exactly that. A hot-path
  trade and an integrity rule are both things this repo writes down.

---

## 1. The question

A host could already pin a surface's image — `DockerExecutionSurface("alpine:3.20")`,
`ContainerdExecutionSurface("docker.io/library/alpine:latest")`, digest references accepted — but
nothing reported back what the pin *resolved to*. `image_` was private with no accessor on either
surface, and `RunCommandReply` carried `ok`, `exit_code`, `stdout_text`, `tree_digest` and
`turn_index` and no image identity at all.

So a consumer assembling a provenance manifest over many sandboxed tool runs could only ever restate
the reference it had itself configured. Restating a **tag** is worth nothing: `alpine:latest` names a
different set of bytes on two machines, or on one machine a week apart. `tree_digest` already named
the bytes a command *produced*; nothing named the environment that produced them.

Issue #80 cites AeroCoWorker's RFC 015 §4c.6 (a per-piece manifest: node, tool version, sandbox image)
as the consuming use case. **That RFC is not in this repository** — AgentEngine's own
`015-Declarative-Agent-Format.md` has sections 1-8 and no §4c — so it is named here as the requester's
stated motivation, not as an AgentEngine spec obligation this ADR discharges.

Two questions, not one:

1. What is the answer, and what does "I don't know" look like?
2. **Who is allowed to be asked?** Every in-tree `ExecutionSurface` test double runs commands
   somewhere with no image identity whatsoever, and a future process- or chroot-backed conformer would
   too. A concept that obliges every conformer to answer obliges the ones with nothing to say to invent
   something.

## 2. Decision

**`ImageIdentifiedSurface` is an opt-in *refinement* of `ExecutionSurface`, not a widening of it.**

```cpp
template <class T>
concept ImageIdentifiedSurface = ExecutionSurface<T> && requires(T const& t) {
    { t.image() }        -> std::convertible_to<std::string_view>;
    { t.image_digest() } -> std::convertible_to<std::string_view>;
};
```

The reason is load-bearing and is the whole of question 2: a surface with no image identity is a
**legitimate** `ExecutionSurface`, and forcing it to answer would put a fabricated value into a
provenance record — the one failure mode a provenance record cannot survive. Callers dispatch with
`if constexpr` and record *nothing* when the answer does not exist. `MandatorySandboxProvider::
bound_image()` is that dispatch, done once, in one place, so a host is not left re-deriving it.

**Two fields, and they mean different things.**

- `image()` — the reference the host **configured**, verbatim. Never empty for a conformer; it is the
  constructor argument.
- `image_digest()` — the **resolved** identity, as the backend reported it. **Empty means "not known".
  It is never backfilled from `image()`**, because a tag is precisely the thing a digest exists to
  replace: a record that silently substitutes the one for the other is worse than a record with a hole
  in it, since a hole is visible.

**This touches no capability decision, on any path.** Both accessors are `const` reads of a string the
host set or the daemon reported; `bound_image()` is called *after* the run and its result flows only
into two reply structs. No gate consults it, no `Capabilities<…>` ceiling changes, and no argv built
here takes model-derived input: the Docker lookup's only variable is `inst.container_id`, minted by
`create()` and already through `docker_cli_reject_leading_dash()`, and the containerd lookup's only
variable is the host-configured `image`, already through `reject_unsafe_token()` at `create()`. **I2
and I3 are untouched** — verified by reading the diff, not assumed from its shape.

**The invariant this serves is I4 — every effect is attributable.** A sandboxed exec is an effect. I4
requires the attribution to be carried *at* the effect, not reconstructed afterwards; before this
change the environment half of "what ran, and where" could only be reconstructed from host
configuration, which is a reconstruction and not an attribution. `image`/`image_digest` on the reply
are the environment half of that record, standing next to `tree_digest`'s output half. 008 §8 already
lists backend and profile among the per-exec observability fields; this is the same class of fact for
the `ExecutionSurface` family, which is deliberately not a `SandboxBackend` family (ADR-099 §7,
008 §2a) and therefore gets none of §8's fields by inheritance.

## 3. The mechanism

### (a) The concept (`sandbox/execution_surface.hpp`)

As above. `static_assert(ImageIdentifiedSurface<DockerExecutionSurface>)` and the same for
`ContainerdExecutionSurface` sit next to the existing `ExecutionSurface` assertions, so a member
silently dropped from either surface's hand-enumerated move operations is a compile error, not a
quietly empty provenance field.

### (b) Docker — read the binding off the live container

`DockerCliBackend::resolve_image_digest(Instance const&)` runs
`docker inspect --format {{.Image}} <container_id>`. That is a query about **this container**, not
about the reference: a container's image binding is fixed at creation and cannot drift, whereas asking
`docker image inspect alpine:latest` again can answer with a different image if the tag moved or a pull
happened in between. The value is Docker's image **config** digest — its immutable local identity —
not a registry repo digest, which a locally built or never-pushed image does not have.

### (c) containerd — re-resolve the reference, and say plainly that it is weaker

`ContainerdCliBackend::resolve_image_digest(std::string const&)` runs `ctr images ls` and matches the
**exact** ref against its REF column, taking the DIGEST column — containerd's **manifest** digest, the
value that makes `repo@sha256:…` portable. The exact match is deliberate: `alpine:3.20` is never
answered with `alpine:3.20.1`'s digest.

**This is weaker than the Docker answer and the ADR says so rather than letting the shared accessor
name imply parity.** It re-resolves the *reference*; it does not read a binding off the running
container. `ctr run` pulls and unpacks at creation, and a pull that moved the tag between this
container's creation and this call would be reported as this container's image when it is not. There is
no `ctr` single-command equivalent of Docker's container-level image field — a containerd container
record holds the ref, not a resolved digest — and reaching the snapshot's parent chain means the gRPC
API this deliberately CLI-shaped backend does not speak. It is called immediately after `create()`, so
the window is the width of one `ctr run`, and the failure mode is a stale-but-real digest, never a
fabricated one.

### (d) Failure is empty, never an error; shape is validated before reporting

Both lookups return an **empty string** on any failure, never a `result<>` error. Provenance
enrichment must not be able to fail a command: a daemon that declines to describe its own container is
not a reason for the tool call to fail. Both then check the shape before reporting — `sha256:` prefix,
exactly 71 characters, 64 lowercase hex digits — so a daemon that answered with a warning line, a Go
template error, or `<no value>` produces "not known" rather than a provenance record naming a string
that is not an image. The Docker path additionally takes the **last** non-empty line, because
`run_argv()` merges stdout and stderr by this file's own convention (the same reason and the same shape
as `create()`'s own id extraction).

### (e) Resolved once per surface — a measured decision, not an assumed-cheap one

`SandboxRuntime::run()` calls `surface.reset(host_dir)` once per command, so anything spawned in
`reset()` is spawned once per tool call. Measured on this project's own Windows/Docker Desktop
development machine:

| | |
|---|---|
| `docker inspect` | ~480 ms |
| warm `docker run -d` | ~900 ms |

Resolving per reset would therefore have been a **>50% regression on every `run_command`**. Both
surfaces instead resolve **once per surface**, guarded by `if (resolved_digest_.empty())` on the first
`reset()` that produces something to read it from — which also means a failed lookup leaves the field
empty and is **retried on the next `reset()`**, so one daemon hiccup does not blind the surface for its
whole life. The cached digest travels with `instance_` through both move operations (explicitly
enumerated, and explicitly `clear()`ed on the moved-from object — see §7's disclosed uncovered mutant
for what that clear does and does not buy).

### (f) The reply plumbing

`MandatorySandboxProvider::bound_image()` returns `BoundImage{reference, digest}` — two empty strings
when `Surface` is not an `ImageIdentifiedSurface`, or when no surface is bound. It is **public**,
because a host assembling its own manifest should get the same answer the tools put in their replies
from the same place rather than reaching for the surface and re-deriving the `if constexpr` itself.
`RunCommandReply` and `TaskBranchRunReply` each gain `image` and `image_digest`, both in the
`AE_JSON_SCHEMA` field list. Both call sites read `bound_image()` **after** the run, not before:
`SandboxRuntime::run()` may `reset()` the surface, and a value captured beforehand would name the
previous container's image.

## 4. Behaviour changes, and what they cost

- **`RunCommandReply` and `TaskBranchRunReply` grew two fields each, and both are in the JSON schema**,
  so every `run_command` / `run_in_task_branch` tool reply is now longer and carries the reference and
  digest into the model's context. That is a real per-call token cost against 023's budgets, paid on
  every sandboxed tool call, not only on ones whose provenance anyone reads.
- **A surface with no image identity reports absence, not a default.** `bound_image()` returns two
  empty strings; nothing was invented on its behalf.
- **The digest is stable across resets of the same surface.** A second `reset()` destroys the first
  container and creates another; the field is not blanked and not re-resolved.
- **`test_composed_sandbox_providers_live` needed `/bigobj` for a second, compounding reason.** ADR-175
  grew the `rt` headers that TU pulls in enough to tip its single `.obj` past MSVC's COFF section
  limit (C1128) **on the ASan leg only**; that half already landed on `main` as `ce80d2e`. Issue #80
  then pushed the plain legs over too: the TU instantiates three whole sandbox-provider stacks, and the
  two new fields cost another `AE_JSON_SCHEMA` codec instantiation per reply type per stack. The flag
  was already there; this change extended the comment explaining why it is. A mechanical compiler limit
  either way, not a code defect. `test_execution_surface_image_identity` gets its own `/bigobj` for the
  same reason — it instantiates `MandatorySandboxProvider` too.
- **CI exclusion lists are hand-maintained here**, so `test_execution_surface_image_identity` was added
  by name to the `ctest -E` lists of both Windows jobs (`windows-msvc`, `windows-clang-cl`), which have
  no Docker daemon. Its ctest entry also takes the `docker_daemon` `RESOURCE_LOCK` every other
  daemon-backed test in this suite takes.

## 5. Rejected alternatives

- **Add `image()`/`image_digest()` to `ExecutionSurface` itself.** Rejected as the central decision
  (§2): it would oblige every conformer with no image to answer, and the only answers available to such
  a conformer are a fabricated one or an empty one that the concept now claims is meaningful. The
  `if constexpr` dispatch exists precisely so the absence is typed rather than encoded as a sentinel.
- **Backfill `image_digest()` from `image()` when unresolved.** Rejected: a tag is what a digest exists
  to replace, so this makes the strongest-looking field the least trustworthy one, invisibly. It is
  also the mutant the test suite catches four ways (§7).
- **Return a `result<std::string>` and let resolution failure surface as an error.** Rejected: it lets
  provenance enrichment fail a command. An unavailable digest is a normal, meaningful outcome, not an
  error — the same posture `DockerCliBackend::exec()` already takes toward a non-zero exit code.
- **Resolve on every `reset()`.** Rejected on measured grounds (§3e): >50% added latency on every
  `run_command`, to close a staleness window nothing in-process can open.
- **Resolve once in the constructor.** Rejected: there is no container to read a binding off before the
  first `reset()`, so the Docker surface would have had to re-resolve the reference — i.e. adopt the
  containerd surface's weaker answer on the backend that does not need it.
- **Reach containerd's gRPC API to read the snapshot's parent chain.** Rejected for this change:
  `ContainerdCliBackend` is deliberately CLI-shaped (ADR-145), and adding a gRPC client to close a
  one-`ctr run`-wide window is a different ADR's worth of dependency.

## 6. Residuals, named not hidden

- **The accepted staleness case.** `image_` never changes for a surface's life and `docker run` does
  not re-pull an image already present locally, so every container a surface creates runs the same
  image **unless something outside this process re-pulls the tag mid-session**. In that case the cached
  digest names the image the *first* container genuinely ran, not the one a later container ran. A real
  value that is stale, never a fabricated one — and strictly better than the nothing-at-all that
  preceded it — but it is a wrong answer in a provenance record, and a host that cannot tolerate that
  should pin a digest reference rather than a tag.
- **containerd's answer is structurally weaker** (§3c), and nothing in the accessor names says so; only
  the function comment and this ADR do. A caller that treats the two surfaces' `image_digest()` as
  equally strong is not warned by the type system.
- **containerd matches the ref exactly.** A host that configures a short reference (`alpine:latest`)
  rather than the fully-qualified one `ctr` prints (`docker.io/library/alpine:latest`, the surface's
  own default) gets an **empty** digest — correct under the "empty means not known" rule, silent under
  every other reading. No normalization is attempted.
- **The two digests are not reliably the same kind of digest, and for Docker the kind depends on the
  OPERATOR'S daemon configuration.** This is the one residual the ADR draft got wrong and is corrected
  here rather than quietly reworded. The draft asserted a fixed split — Docker a *config* digest,
  containerd a *manifest* digest. That was **measured false for Docker** on the machine this was written
  on: with the **containerd image store** (Docker Desktop's default, `io.containerd.snapshotter.v1`,
  confirmed via `docker info`), `{{.Image}}`/`{{.Id}}` **is** the manifest digest — verified across three
  images, each `{{.Id}}` byte-identical to its own `RepoDigests[0]` digest. With the classic graph driver
  Docker documents the image ID as the config digest instead; **that half is NOT verified here**, because
  no classic-store daemon was reachable from this machine (the WSL2 `docker` is the same Docker Desktop
  daemon, same store). So the honest statement is: `ContainerdExecutionSurface` always reports a manifest
  digest; `DockerExecutionSurface` reports the container's image ID, which is a manifest digest under one
  store and documented to be a config digest under the other. Both are `sha256:<64 hex>`, nothing in the
  field name or the JSON says which, and **whether two records of one image compare equal is decided by
  how an operator configured their daemon** — which is worse than a fixed divergence, since a consumer
  cannot tell from the value. Compare digests only within one surface type's own records. Carrying the
  digest's kind in the value or a sibling field is the real fix and is deliberately **not** smuggled into
  the change that introduced the field; it needs its own decision about the reply schema. The concept's
  own header (`sandbox/execution_surface.hpp`) and `DockerCliBackend::resolve_image_digest()` both state
  this at the point of use, so a reader is not dependent on finding this ADR.
- **Nothing pins the cross-surface contract, and N5 cannot catch it.** The control compares the reported
  digest against an independent `docker image inspect` on the SAME host, so it is self-consistent by
  construction and would pass identically under either image store. No check compares a Docker-sourced
  digest with a containerd-sourced one for the same image — and none can, until the two surfaces can run
  against one host in one test.
- **This covers the `ExecutionSurface` family only.** `SandboxBackend` conformers — `native-jail`,
  `wasm`, `kata` — are untouched, and `ExecOutcome` gained nothing. That is the ADR-099 §7 /
  008 §2a boundary, carried forward unchanged, not an oversight.
- **The reference and digest enter the model's context.** They are serialized into the tool reply the
  model reads. Nothing consumes them back as authority today, and I3 is not weakened by this change —
  but a host must take provenance from `bound_image()` or the reply struct, **never** from a digest
  restated in model output, which is an ordinary untrusted string.
- **`image()` is only as meaningful as the host's own honesty.** It is the constructor argument echoed
  back; it proves what was configured, not what was intended.
- **No `SandboxSpec`/profile plumbing, and no span or audit field.** A host that wants the image in a
  trace or an audit record still has to put it there itself; this change makes the value *available* to
  I4's consumers, it does not emit it.

## 7. Evidence

> **Correction, recorded rather than silently fixed.** The first draft of this ADR asserted that Docker
> reports an image config digest. That is measured false on the machine the change was developed on (see
> §6). The parent session caught it while checking the draft's own derived residuals, and both the code
> comment that made the same claim and §6 were corrected before this ADR was committed to the branch. The
> claim had been restated from Docker's documentation rather than measured — the exact failure mode
> CLAUDE.md's "do not assert what a protocol does from memory" rule names.

**`tests/test_execution_surface_image_identity.cpp`** (new) — **13 runtime checks** plus 3
compile-time assertions, against a **live Docker daemon**, reported as 13/13 on the branch. Memory-capped
at 256 MiB (1 GiB under a sanitizer) via `tests/support/memory_cap.hpp`, per CLAUDE.md's machine-safety
rule — the cap bounds a mutant planted in a parsing loop that reads daemon stdout, not a growth path the
test itself has.

| Check | Property it pins |
|---|---|
| N1 (3 × `static_assert`) | **CONTROL.** A no-image double satisfies `ExecutionSurface` and is **rejected** by `ImageIdentifiedSurface`; the real Docker surface is accepted. Without the rejection half, the `if constexpr` dispatch is dead code. |
| N2 | `image()` is the configured reference verbatim, before and after `reset()`. |
| N3 | `image_digest()` is empty before the first `reset()` — never backfilled. |
| N4 | After `reset()` it is a well-formed `sha256:<64 hex>`. |
| N5 | **CONTROL.** That digest equals what an **independent** `docker image inspect --format {{.Id}}` resolves the same reference to. A hardcoded, fabricated or accidentally-constant value cannot pass. |
| N6 | **CONTROL.** `resolve_image_digest()` against a container that does not exist returns empty — the "not known" path is reachable, so N4/N5 are not a branch that always yields a digest. |
| N7 | A second `reset()` (destroy-then-create) leaves the same well-formed digest — the caching contract of §3e. |
| N8 | The digest travels to the moved-to surface, and the moved-from one claims none. |
| N9 | A provider over a no-image surface reports two empty strings. |

**`tests/test_mandatory_sandbox_provider.cpp`** — **4 new checks**: `bound_image()` reports the
configured reference and a well-formed digest, and the real `run_command` reply **JSON** carries both,
the digest cross-checked against the same surface's own accessor rather than a literal (a hardcoded
expectation would still pass if both were fabricated).

**`tests/test_containerd_execution_surface.cpp`** — **5 new checks**: reference before `reset()`, empty
digest before `reset()`, well-formed digest after, a match against an independent `ctr images ls`
lookup, and an unheld reference resolving to empty. **These were NOT EXECUTED** — see below.

**Planted mutants, each caught:**

| Mutant | Caught by |
|---|---|
| `image_digest()` backfills from the configured tag | N3, N4, N5, N7 |
| `resolve_image_digest()` answers with a plausible constant | N5 |
| `bound_image()` reports nothing | 3 of the 4 provider checks |

**One disclosed UNCOVERED mutant.** Deleting the explicit `other.resolved_digest_.clear()` in
`DockerExecutionSurface`'s move constructor **fails nothing** on MSVC: a 71-character digest is past the
SSO buffer, so moving the string steals the pointer and empties the source anyway. The standard leaves a
moved-from `std::string` "valid but unspecified", so the explicit clear turns an implementation's habit
into this type's guarantee; N8 pins the guarantee and would only catch its removal on a library that
behaves differently. Recorded in the test file itself, not only here.

**Builds and runs:**

- Windows MSVC Release: **366/366 green**, recorded on the branch before and after the rebase onto
  `ce80d2e`.
- Linux g++-14: both touched test targets **compile**.
- `python tools/naming_lint.py`: OK — `ImageIdentifiedSurface` has a real row in
  `027-Vocabulary-and-Naming.md`, not a suppression.
- **Evidence gap, stated rather than glossed: the containerd runtime checks were never executed.** They
  compile on Linux; the machine available for this work has a containerd socket that needs root, and
  `sudo` could not authenticate non-interactively in that session. The containerd half of §3c is
  therefore proven by construction and by code reading only. Run
  `sudo ~/aebuild/tests/test_containerd_execution_surface` to close it.
- **The memory cap in `tests/test_execution_surface_image_identity.cpp` is a follow-up that was still
  uncommitted in the shared checkout when this ADR was written** — it is not part of commit `5404f1e`.

## 8. Red-team rounds

**None were run.** This is the honest state of the change, not a formality skipped: PR #83 explicitly
argued the work was additive observability outside the `design → red-team → prove → judge` gate, and it
shipped with self-adversarial evidence instead — three controls (N1's rejection half, N5's independent
oracle, N6's reachable failure path), three planted mutants, and one mutant disclosed as uncovered
rather than argued away.

What a real round should attack, listed so its absence is a known quantity rather than an assumption of
soundness:

1. **The caching window.** §6's first residual is argued, not demonstrated. An executed probe — re-pull
   the tag between two `reset()`s and show the reply still names the first image — would turn it from a
   claim into a measured fact, and would settle whether "stale but real" is tolerable for the manifest
   use case that asked for this.
2. **The containerd path end to end**, which no run has touched (§7). Its exact-ref match, its
   stale-window claim, and its empty-on-unheld-reference behaviour are all unexecuted on a live daemon.
3. **The digest-kind mismatch** (§6): whether a consumer comparing a Docker record against a containerd
   record for the same image is misled by two fields with the same name and incomparable values.
4. **The `if constexpr` dispatch's reachability.** N1 proves the concept discriminates; nothing proves
   that every future caller will actually go through `bound_image()` rather than reaching for a surface
   accessor directly and reintroducing the fabrication risk at a new site.
5. **The measured numbers themselves.** ~480 ms / ~900 ms come from one developer machine with Docker
   Desktop on Windows. The >50% conclusion is robust to a wide error bar, but the numbers are not a
   benchmark harness's output and are not reproducible from anything checked in.
