# ADR-176 — Which sandbox image did a command actually run in, and which surfaces are allowed to answer?

- **Status**: Proposed — implemented, proven, and **red-teamed in one round (§11)**, pending
  project-owner sign-off.
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
  `tests/test_mandatory_sandbox_provider.cpp`, `tests/support/memory_cap.hpp`,
  `bench/docker_image_digest_resolution.cpp` (new), `tests/CMakeLists.txt`, `.github/workflows/ci.yml`,
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

### (e) Resolved once per surface — a measured decision, and a CORRECTED measurement

> **This section's original numbers were wrong.** It claimed `docker inspect` measured ~480 ms against a
> ~900 ms warm `docker run -d`, i.e. a **>50% regression on every `run_command`**, and §5 rejected an
> alternative on that figure's strength. §8 item 5 of the first draft flagged that those numbers were
> "not a benchmark harness's output and are not reproducible from anything checked in" — and then
> asserted the conclusion was "robust to a wide error bar". Writing the harness (§10) falsified both: the
> real figure is **9%** for the digest, and the direction reverses — `docker inspect` is *cheaper* than
> the `docker exec` a tool call already pays. **The ">50%" figure should not be repeated.** It is kept
> here, struck, so a reader who saw it elsewhere knows it was retracted rather than quietly reworded.

`SandboxRuntime::run()` calls `surface.reset(host_dir)` once per command, so anything spawned in
`reset()` is spawned once per tool call. Measured by `bench/docker_image_digest_resolution.cpp`, median
of 5, on this project's own Windows/Docker Desktop development machine:

| | |
|---|---|
| `docker exec` — what a tool call already pays | ~130 ms |
| `docker inspect {{.Image}}` — the digest | ~69 ms |
| `docker image inspect {{.Descriptor.mediaType}}` — the kind (§9) | ~131 ms |
| `reset()` = destroy + create + seed | ~674 ms |

So resolving both per reset would cost about **25% of a `reset()`+`exec`** — and that denominator is
itself an under-estimate of a real tool call, which also materializes the worktree, drains the container
through another `docker cp`, rescans the tree and commits to the Ledger. 25% is an **upper bound**.

The cache survives the correction, on a smaller and now-honest argument: 25% of every tool call, to
re-derive a value that only something *outside* this process can change, is still not worth paying. But
it is a 25% saving bought with a real staleness window (§6), not a catastrophe averted, and §5's
rejection of per-reset resolution is re-argued on that basis rather than on the retracted figure. Both
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
- **Resolve on every `reset()`.** Rejected on measured grounds (§3e) — but on **corrected** grounds,
  and the correction shrinks the margin a lot: ~25% added latency on every `run_command` (upper bound),
  not the >50% this ADR originally asserted. The staleness window it would close is one nothing
  *in-process* can open. This is the alternative most worth revisiting if the surfaces ever read identity
  through a daemon API instead of the CLI, because essentially all of the 25% is process startup and
  round trip rather than work — §10's harness exists so that re-examination is a re-run, not an argument.
  A reviewer who weighs a per-command-correct provenance record above 25% latency should overturn this;
  the ADR records the number rather than the preference.
- **Resolve once in the constructor.** Rejected: there is no container to read a binding off before the
  first `reset()`, so the Docker surface would have had to re-resolve the reference — i.e. adopt the
  containerd surface's weaker answer on the backend that does not need it.
- **Reach containerd's gRPC API to read the snapshot's parent chain.** Rejected for this change:
  `ContainerdCliBackend` is deliberately CLI-shaped (ADR-145), and adding a gRPC client to close a
  one-`ctr run`-wide window is a different ADR's worth of dependency.

## 6. Residuals, named not hidden

- **The accepted staleness case, and it now costs less to reject than this ADR first claimed.** `image_`
  never changes for a surface's life and `docker run` does not re-pull an image already present locally,
  so every container a surface creates runs the same image **unless something outside this process
  re-pulls the tag mid-session**. In that case the cached digest names the image the *first* container
  genuinely ran, not the one a later container ran. A real value that is stale, never a fabricated one —
  and strictly better than the nothing-at-all that preceded it — but it is a wrong answer in a provenance
  record, and under **I4** a digest attached to a command that ran in container N while naming container
  1's image is not "stale but real" *for that effect*: it is wrong for that effect. This ADR bought that
  with ">50%"; it actually costs ~25% (§3e), and a reviewer may reasonably decide that is too cheap a
  price to charge for a correct record. A host that cannot tolerate it today should pin a digest
  reference rather than a tag.
- **The KIND can be `unknown` for a real, correctly-resolved digest** (§9): a media type this project's
  closed list does not recognize, or a daemon with no `.Descriptor` field. `unknown` means "not
  established", never "not a manifest digest", and a consumer must decline to compare rather than guess.
  The Docker surface re-attempts the kind whenever the digest changes, so a transient failure is not
  permanent — but while the digest is unchanged the failed kind is *not* retried, which is a deliberate
  trade against spawning a CLI process per command for every image whose kind is genuinely unestablished.
- **`config` and `manifest` are both UNPROVEN kinds in this tree.** Every image the suite uses is
  multi-platform, so the executed checks exercise `index` and `unknown` only. `config` additionally needs
  a Docker daemon on the classic graph driver, and none is reachable from this machine — Docker Desktop
  shares one containerd-image-store daemon into WSL2. Both gaps are named in the test header too, so a
  reader of the checks is not left inferring coverage from which ones happen to exist.
- **containerd's answer is structurally weaker** (§3c), and nothing in the accessor names says so; only
  the function comment and this ADR do. A caller that treats the two surfaces' `image_digest()` as
  equally strong is not warned by the type system.
- **containerd matches the ref exactly.** A host that configures a short reference (`alpine:latest`)
  rather than the fully-qualified one `ctr` prints (`docker.io/library/alpine:latest`, the surface's
  own default) gets an **empty** digest — correct under the "empty means not known" rule, silent under
  every other reading. No normalization is attempted.
- ~~**The two digests are not reliably the same kind of digest.**~~ **CLOSED by §9**, which carries the
  kind alongside the digest so a consumer never has to infer comparability from the surface type. The
  history is kept because it cost two wrong claims: the draft asserted a fixed split — Docker a *config*
  digest, containerd a *manifest* digest — which was **measured false for Docker** on the machine this
  was written on: with the **containerd image store** (Docker Desktop's default, `io.containerd.snapshotter.v1`,
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

**`tests/test_execution_surface_image_identity.cpp`** (new) — **24 runtime checks** plus 3
compile-time assertions, against a **live Docker daemon**, reported as 24/24 on the branch. Memory-capped
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
| N9 | A provider over a no-image surface reports empty strings — including no kind. |
| N10 | `image_digest_kind()` is `unknown` before the first `reset()`, for the same reason N3 holds. |
| N11 | The kind is `index` — **not** `manifest` — for this multi-platform reference, and **CONTROL**: it equals what an independent `docker image inspect {{.Descriptor.mediaType}}` resolves the same image to. |
| N12 | **CONTROL.** `unknown` is reachable three ways: an unrecognized media type, a line that merely *contains* a known media type (the exact-match guard, §9), and a malformed image id. So no kind is a constant. |
| N13 | The kind travels with the digest across a move, in both directions. Unlike N8 this **cannot** pass by accident: an enum is copied intact by a move, so without the explicit reset the moved-from surface would still answer `index`. |

**`tests/test_mandatory_sandbox_provider.cpp`** — **4 new checks**: `bound_image()` reports the
configured reference and a well-formed digest, and the real `run_command` reply **JSON** carries both,
the digest cross-checked against the same surface's own accessor rather than a literal (a hardcoded
expectation would still pass if both were fabricated).

**`tests/test_containerd_execution_surface.cpp`** — **8 issue-#80/§9 checks**, and the first draft's
claim that these were "NEVER EXECUTED" was **wrong about CI**. It was true of the development machine,
whose containerd socket is root-only and whose `sudo` cannot authenticate non-interactively. It was never
true of the Linux CI leg, which pulls `alpine` and runs the three containerd tests under `sudo`:
`test_containerd_execution_surface` passes there, and its issue-#80 checks are unconditional — the test
returns 1 if `reset()` fails — so a pass means they ran against a real containerd 2.2.2 as root.

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
- ~~**Evidence gap: the containerd runtime checks were never executed.**~~ **Withdrawn — it was wrong
  about CI**, and is kept here because an overstated gap is as much a defect in an ADR as an overstated
  claim. The Linux CI leg pulls `alpine` and runs the three containerd tests under `sudo`; they pass, and
  the issue-#80 checks in them are unconditional, so a pass means they executed against a real containerd
  2.2.2 as root. What is true is narrower: they cannot run on the *development* machine, whose containerd
  socket is root-only and whose `sudo` cannot authenticate non-interactively, so they are not exercised
  in the local loop before a push.
- **The memory cap on the new test needed its own fix to work at all.** Added as `a3864e1`, it turned the
  Linux CI leg **red**: the cap used `RLIMIT_AS`, which is inherited across `fork`/`exec` and bounds
  reservations, and the `docker` CLI is a Go binary whose runtime reserves a large virtual arena before
  `main()` — the child died with "failed to reserve page summary memory". An address-space cap and
  shelling out to any container CLI are mutually exclusive at every cap size a test would tolerate.
  Fixed in `3d0f101` by switching the POSIX mechanism to `RLIMIT_DATA`, which does not count `PROT_NONE`
  reservations; CI is green on that commit. What `RLIMIT_DATA` actually bounds was then measured rather
  than assumed — see §11 findings 13–15.

## 8. What the first draft left unattacked

The first draft of this ADR shipped with **no red-team round**, and said so: PR #83 argued the work was
additive observability outside the `design → red-team → prove → judge` gate, and it shipped with
self-adversarial evidence instead — three controls, three planted mutants, and one mutant disclosed as
uncovered rather than argued away. §8 then listed five things a round *should* attack, so that its
absence was a known quantity rather than an assumption of soundness.

**That round has now been run (§11), and four of those five predictions were correct.** Keeping the list
is worth more than deleting it, because the accuracy is the point: the things a draft knows it has not
checked are, in fact, where its defects are.

| The draft predicted | Outcome |
|---|---|
| 1. The caching window is argued, not demonstrated | **Still open.** No probe re-pulls a tag between two `reset()`s. §6 carries it, now with a corrected price. |
| 2. The containerd path is unexecuted | **CLOSED.** It was never true of CI: the Linux leg pulls `alpine` and runs the three containerd tests under `sudo`. `test_containerd_execution_surface` passes there with the issue-#80 checks, which are unconditional — the test returns 1 if `reset()` fails, so a pass means they ran. What was unexecuted was the *developer machine*, whose containerd socket is root-only. §7's original claim overstated the gap. |
| 3. The digest-kind mismatch misleads a consumer | **CONFIRMED and fixed** — §9. The round went further and found the *first* fix still wrong (index and manifest collapsed into one kind). |
| 4. Nothing proves every future caller goes through `bound_image()` | **Still open.** N1 proves the concept discriminates; nothing stops a future caller reaching for a surface accessor directly. |
| 5. The measured numbers are not a harness's output | **CONFIRMED, and worse than predicted** — §10. The draft hedged that "the >50% conclusion is robust to a wide error bar". It was wrong by roughly 7×, and the direction reversed. |

---

## 9. The digest's KIND — closing §6's comparability residual

**Decision: `ImageIdentifiedSurface` gains `image_digest_kind()`, and every backend MEASURES it.**

A bare `sha256:<64 hex>` does not say what it digests, and two digests of different kinds for one image
never match. A consumer comparing them concludes "different image" and is wrong. §6 originally left this
as follow-on work and named the guessing as the residual — but a provenance field whose comparability a
reader has to infer is a field that will eventually be compared wrongly, so it is closed here.

```cpp
enum class ImageDigestKind { unknown, index, manifest, config };
```

- **`index`** — an image INDEX (manifest list): what a multi-platform reference resolves to.
- **`manifest`** — a single platform's image manifest.
- **`config`** — the image's config blob.
- **`unknown`** — no digest, or a kind that could not be **established**. It does *not* mean "not a
  manifest digest"; it means nothing was measured. A consumer must not compare an `unknown` against
  anything, including another `unknown`.

**`index` and `manifest` are deliberately separate, and collapsing them was this section's own first
defect** (§11, finding 2). They are digests of two *different objects* for one image — an index names the
manifests, so its digest can never equal any of theirs. Spelling both `manifest` made the rule "compare
only when the kinds match" license exactly the false "different image" the type exists to prevent, for
one image recorded from a host holding the index and a host holding one platform's manifest.

**Measured, never inferred, and that is the whole design.** Both backends read a MEDIA TYPE off the
daemon and map it through one shared closed list, `image_digest_kind_from_media_type()`:

| Backend | Where the media type comes from |
|---|---|
| Docker | `docker image inspect --format {{.Descriptor.mediaType}} <image id>` |
| containerd | the TYPE column of the **same `ctr images ls` row** the digest came from |

The tempting alternative — read the image store out of `docker info` and map containerd-store to
manifest, classic-graph-driver to config — is a restatement of Docker's documentation with a parsing step
in front of it. CLAUDE.md's "do not assert what a protocol does from memory" applies to a daemon's
behaviour as much as to a wire protocol, and this file has already been wrong about exactly this once.

**The match is EXACT against a closed list, which is a correctness requirement rather than tidiness.**
The CLI helpers in this tree merge stderr into the text they return, so the candidate string can be any
line a daemon chose to emit. A substring rule ("contains manifest") would let a warning mint a confident
kind for a digest nobody measured. An unrecognized string is `unknown` — the honest answer and the safe
one. §11 finding 4 is exactly this defect, in the version that shipped before the round.

**The kind is keyed by the digest it describes.** `DockerExecutionSurface` caches
`kind_resolved_for_`, the digest the kind was measured for; if the digest ever changes the kind is
re-resolved, and while it does not, nothing is spawned. That makes a kind describing a *different* digest
unrepresentable rather than merely unlikely, and it fixes §11 finding 6 — a first version guarded the
kind inside the digest's own "is it empty" block, so a digest that resolved while its kind lookup failed
pinned `unknown` for the surface's entire life from one transient CLI failure. `ContainerdExecutionSurface`
needs no key: `resolve_image_identity()` returns both from one call, so they are never resolved apart.

**Reply plumbing.** `RunCommandReply` and `TaskBranchRunReply` gain `image_digest_kind`, and
`BoundImage` gains `digest_kind` — the wire spelling, not the enum, so a caller copying it into a reply
or an audit record does not re-derive the mapping at each site. `unknown` serializes to the **empty
string**, matching the empty-digest convention: absence is absence, never the word "unknown"
masquerading as a value.

**I2/I3 are untouched, verified rather than assumed.** Docker's kind lookup re-validates its argument to
`sha256:`+64-hex before argv construction, so no leading-dash value can reach it; containerd's puts no
caller value in argv at all. Nothing model-derived reaches either, and nothing in the tree reads
`image_digest_kind` to make a permission decision.

---

## 10. The benchmark, and the measurement it falsified

`bench/docker_image_digest_resolution.cpp` (new) turns §3e's cost claim into a program anyone can re-run.
It exists because the first draft's numbers were prose in a code comment, and §8 item 5 knew it.

Running it the first time **falsified the claim it was written to document**: `docker inspect` is ~69 ms,
not ~480 ms, and is *cheaper* than the ~130 ms `docker exec` a tool call already pays. Cross-checked with
a shell loop outside the process, which agrees on the ordering and the order of magnitude.

Unlike `bench/rt_block_on_handoff.cpp`, this one ends in a **PASS/FAIL verdict**, which is what
`bench/README.md` says a benchmark in this tree is for. The verdict pins the corrected claim — identity
resolution must be below the 50% share this ADR originally asserted — so the retracted number cannot
quietly come back. Had the verdict existed when §3e was written, ">50%" would have been a failing build
rather than a sentence nobody could check.

Three things it is careful about, each because §11 found the first version was not:
- Every timed operation reports **success**, and a run where the daemon went away prints a failure line
  instead of a confident table of ~20 ms failure paths.
- `reset()` is labelled **destroy + create + seed**, because it is several processes and a tar build, not
  one CLI call like the other three rows.
- The denominator `reset()+exec` is stated as **less than a real tool call**, so the printed share is an
  upper bound — the direction that argues *against* the cache this bench defends.

Its probe container carries ADR-171's isolation flags and the `ae_`-prefixed name
`DockerCliBackend::reap_orphans()` looks for, so it is not a container this repo's own machinery would be
blind to.

**Not measured:** there is no containerd equivalent. `ctr` needs root, so it cannot run on the machine the
Docker numbers come from, and the containerd surface's `reset()` comment borrows the Docker ratio as an
**assumption of similarity** — disclosed there and here, not a second measurement.

---

## 11. Red-team round 1

Two independent adversarial passes, one on the digest-kind design and one on the caching, the cost claims
and the memory cap. Both were instructed to find defects that make shipped code wrong or a written claim
false, and to report bluntly when they found nothing. Findings and dispositions:

| # | Finding | Severity | Disposition |
|---|---|---|---|
| 1 | **The code cited ADR-176 §9/§10, which did not exist**, and the ADR still asserted the opposite of what the code did — §6 listed the digest-kind mismatch as open, §8 listed it as unbuilt, and `decisions/README.md` still carried both the "config digest" claim and the retracted ~480 ms numbers. CLAUDE.md: the spec wins, and the spec was stale. | FATAL | **FIXED.** §9, §10 and this section written; §3e, §5, §6, §8 and the README row corrected. Reported independently by both passes, which is why it is listed first. |
| 2 | **`manifest` conflated INDEX and per-platform MANIFEST**, so "compare when the kinds match" licensed comparing two digests that can never be equal for one image — the exact false "different image" the enum exists to prevent. | FATAL | **FIXED.** `index` is now its own kind (§9). The reported trigger (a `--platform` pull yielding a platform-manifest image ID) did **not** reproduce on Docker 29.7.2 — the ID stayed the index digest — so the *trigger* is unconfirmed while the *conflation* was real and is fixed on its own merits. |
| 3 | **Docker's `config` answer was reached by elimination**, not measurement, and the header claimed it was measured. | SERIOUS | **FIXED.** The RepoDigests heuristic is gone; `config` is now reported only when the media type *is* a config type. |
| 4 | **The heuristic's stated safety premise was false**: `run_argv()` merges stderr into `stdout_text`, so "the template emits that one field and nothing else" did not hold, and a warning containing `sha256:` could mint a kind. | SERIOUS | **FIXED** by the same change — the match is now exact against a closed list, and a check pins that a line merely *containing* a known media type resolves to `unknown`. |
| 5 | **containerd's "same row" guarantee did not exist**: two separate `ctr images ls` spawns matched by a mutable tag, and the kind function parsed the row's digest and discarded it. | SERIOUS | **FIXED.** `resolve_image_identity()` returns both from one call; `resolve_image_digest()` and `resolve_image_digest_kind()` are now thin wrappers over it, so there is one parsing path. |
| 6 | **The kind was never retried**: guarded inside the digest's `if (empty)` block, so one transient failure pinned `unknown` for the surface's life while the comment above promised a retry. | SERIOUS | **FIXED** by keying the cache on the digest (§9). |
| 7 | **`image_digest()`'s concept-level contract was false** after the first `reset()` — it said "the CURRENT execution environment ... at the moment that environment was created", which the caching makes untrue from command 2 onward. | SERIOUS | **FIXED.** The contract now says FIRST, explains why, and points at the residual. |
| 8 | **The bench was untracked and built by nothing**, so the replacement numbers had the same "not reproducible from anything checked in" property as the ones they replaced. | MINOR | **FIXED** (committed). Not CMake-built, which is the existing `bench/` convention (`bench/README.md`: no bench build yet). |
| 9 | **`reset()` was mislabelled** in the bench as one `docker run -d`; it is destroy + create + seed. And the `reset()+exec` denominator omits `drain_to()`, materialize, rescan and commit, so both headline ratios were overstated — in the direction that flatters the cache. | SERIOUS | **FIXED.** Row relabelled; the share is now stated as an upper bound. |
| 10 | **"a value that cannot change for this surface"** contradicted the staleness paragraph fourteen lines below it. | SERIOUS | **FIXED** in §3e and the code comment; §6 now states the I4 consequence plainly. |
| 11 | **The kind timing was unguarded**: with an empty digest it would print `0 ms` as a measurement. Nothing in any timed loop checked success. | SERIOUS | **FIXED.** Both guarded; a failed rep prints a failure line and fails the verdict. |
| 12 | **The bench's hand-rolled container** had no ADR-171 flags, no reapable name, and a `--rm` that could never fire. | MINOR→SERIOUS | **FIXED.** |
| 13 | **Two claims about RLIMIT_DATA were wrong**: it counts the *virtual* size of private writable mappings (not RSS), and private **file-backed** mappings *are* counted. | MINOR (load-bearing) | **CONFIRMED by direct measurement and FIXED.** Probed on the same kernel with 512 MiB mappings under a 256 MiB cap: private-writable-anon untouched → REFUSED; `PROT_NONE` → allowed; private file-backed → REFUSED; `MAP_SHARED` → allowed. The header now states this, with the probe results. |
| 14 | **The sanitizer skip was justified by an address-space argument** the same file said no longer applied. | SERIOUS | **RESOLVED — the skip stands, for a measured reason.** An ASan binary dies before `main()` under a 256 MiB RLIMIT_DATA *and* under a 1 GiB one (it asks for ~15 TB of shadow, which is a private writable mapping, not a `PROT_NONE` reservation). POSIX sanitizer legs therefore run **uncapped**, which is now disclosed as a real hole rather than implied to be a non-issue. |
| 15 | **Three files cite the cap as satisfying CLAUDE.md's machine-safety rule**, whose own example is a fork bomb — which neither rlimit bounds, both being per-process and inherited. | MINOR | **FIXED in the documentation.** The header now states what the cap does *not* do, and names `RLIMIT_NPROC` / `JOB_OBJECT_LIMIT_ACTIVE_PROCESS` as the mechanisms that would. |
| 16 | **`memory_cap()`'s return value is discarded by 3 of its 4 callers**, while their headers state the cap as fact. | MINOR | **PARTIALLY FIXED.** `test_execution_surface_image_identity` now prints a note when the cap is not in force. The other two are unchanged. |

**Raised and NOT acted on**, recorded so the decision is visible rather than silent:

- **Memoize the kind by digest and resolve the DIGEST per reset** (~9%), which would delete the staleness
  residual entirely. The kind half is implemented; the digest half is a behaviour change to a decision
  this PR already made, and it is left for sign-off with the numbers now measured rather than flipped
  unilaterally. §5 and §6 both point at it.
- **A pass/fail assertion for the memory cap in every capped test.** `memory_cap.hpp` says a caller
  "must" prove the cap; one of four does. Named, not fixed.

**What the round cleared, stated because a red team that finds nothing must say so:** argv injection on
both backends (I2/I3 intact — nothing model-derived reaches either argv, and no permission decision reads
the kind); `image_digest_kind_name()` lifetimes; containerd column misalignment; a config media type
falsely matching the manifest test; and the `unknown`/empty-string encoding being distinguishable from
"no image at all".
