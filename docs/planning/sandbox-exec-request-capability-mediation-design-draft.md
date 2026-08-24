# Design draft — closing the "`ExecRequest::source` is not yet Runner-mediated" gap

**Status:** draft, pre-red-team. Scopes the last remaining named `KataBackend` residual
(`kata_backend.hpp`'s "Still NOT done" list: "`ExecRequest::source` is... not yet Runner-mediated"),
generalized on inspection to all three `SandboxBackend` implementations — see §1 for why a Kata-only
fix would repeat the exact per-backend-divergence mistake
`decisions/ADR-087-sandbox-spec-capability-enforcement.md` already corrected once for `mounts`/`net`.

**Relates to:** `008-Sandbox-and-Isolation.md` §1b (the "sandbox is the whole execution environment"
layering), §2 (`SandboxSpec`'s "empty-by-default authority" contract), §4 (the capability-enforcement
table), `010-Python-Code-Interpreter.md` §1a (`Runner`), CLAUDE.md's locked decision ("the embedded
native CPython interpreter is the one mediated code-interpreter path, permanently... never a second
interpreter for `execute_code`").

## 1. What the residual bullet actually claims, and what's true

`kata_backend.hpp`'s residual list, and the identical bullets in `linux_native_jail_backend.hpp`/
`native_jail_backend.hpp`, all say the same thing: `ExecRequest::source` reaching a `SandboxBackend`'s
`exec()` is trusted, already-resolved input — "the caller is trusted to have already resolved from a
name," "not yet Runner-mediated." Read at face value this sounds like an unfinished wiring task
specific to Kata. It is not. `native_jail_backend.hpp:57-68` states the actual architecture directly:

> This backend does not itself interpret `language`; it exists on `ExecRequest` for forward
> compatibility with the Runner-routed shape 010 will need. A raw command line reaching this backend
> from anywhere an agent's own output could shape unmediated would violate I2/I3 -- **that mediation
> is the caller's job, not this backend's**, exactly as 008 §1b layer 2 already specifies for
> `subprocess`/`ToolCall` dispatch generally.

So all three backends' `exec()` are, **by design**, a low-level primitive that trusts its caller —
identical posture, not a Kata-specific gap. The question this draft investigates: *does anything today
actually route model-authored text into that trusted parameter without an intervening mediation step,
and if the answer is no, what — if anything — is still worth fixing?*

## 2. Current-state findings (traced through real source, not assumed)

1. **`execute_code`'s real production call path never reaches any `SandboxBackend::exec()` at all.**
   `tools/cli_chat.cpp:626-627`: `real_execute_code()` builds `ExecRequest{language, a.code,
   ctx.codeact_preseeded_answers}` (`a.code` is the raw, model-supplied argument) and calls
   `runner.run(req, exec_state_, ctx)`, where `runner` is a `native_jail::MediatedPythonRunner`
   reached through `CodeActRunnerBinding`. `MediatedPythonRunner` is an IPC client to a separately
   jailed worker process (`native_jail_backend.hpp`'s own 2026-08-23 "SUPERSEDED" correction) — it
   does not call `NativeJailBackend::exec()`, `LinuxNativeJailBackend::exec()`, or
   `KataBackend::exec()`. **No wiring exists anywhere in this codebase today that lets
   `execute_code`'s model-supplied `code` reach any `SandboxBackend`'s `exec()` directly.**
2. **The original plan for that wiring was tried and abandoned.**
   `docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md:67`: "`PythonRunner`/
   `ShellRunner` (010 §1a) become real `Runner`s that plug into this same `SandboxBackend` in M3,
   unchanged." `native_jail_backend.hpp`'s SUPERSEDED note (2026-08-23) records that this did **not**
   happen the way planned: `MediatedPythonRunner` was rebuilt as a standalone IPC-driven jailed
   worker instead, and explicitly states the shell case — whether `MediatedShellRunner` should ever
   route through the same `SandboxBackend`-hosted shape — is "explicitly NOT decided here... named as
   a residual." The M3 composition this residual bullet gestures at was never actually built for
   *any* backend, Kata included, and the project's own record shows it was reconsidered on purpose,
   not merely delayed.
3. **A locked decision already forecloses the obvious "build it for Kata" answer.** CLAUDE.md: "The
   embedded native CPython interpreter is the one mediated code-interpreter path, permanently --
   never WASM, never a second *interpreter* for `execute_code`." Kata can never host `execute_code`'s
   Python path, by standing project decision — so the only way `ExecRequest::source` could ever reach
   `KataBackend::exec()` from model output is a **shell**-shaped path, and no roadmap document, RFC,
   or ADR proposes one. There is no unbuilt Kata-specific consumer to design toward here; inventing
   one now would be exactly the "design for hypothetical future requirements" CLAUDE.md rejects.
4. **`cap::Exec` — CORRECTED after red-team (an earlier draft of this section misread it).** An
   earlier version of this finding claimed `cap::Exec` was scoped to *nested* exec only (008 §4's
   "`Exec` (nested)" table row) and therefore said nothing about top-level `SandboxBackend::create()`/
   `exec()` authorization. An independent red-team pass (`Agent` tool, this same design cycle) checked
   the actual authoritative source and found that claim wrong: `007-Capability-and-Trust-Model.md:59`
   —
   > `Exec<profile>` | Create a sandbox of a profile | profile, resource limits
   —
   is unambiguous: `cap::Exec` (`capability.hpp:120-130`, fields `profile_name`/`cpu_ms_cap`/
   `wall_ms_cap`/`memory_bytes_cap`) is the **top-level** capability gating whether a caller may
   create/use a `SandboxBackend` of a given profile at all, matching its own comment ("until Phase C
   reconciles the two when `SandboxBackend` needs to consume this directly") literally. 008 §4's
   "`Exec` (nested)" row is a **separate** concept — code already running *inside* a sandbox trying to
   spawn a *further* nested sandbox/subprocess, denied unconditionally by construction regardless of
   any grant (008 §4 line 327: "Nested sandbox creation is denied in every profile. A guest cannot
   create a guest") — the two are easy to conflate but are genuinely different questions. Corrected
   conclusion: `grep -rln "cap::Exec\b" include src` finding no enforcement site is **not** "nothing to
   enforce" — it is a real, live gap. `authorize_spec()` (`sandbox.hpp:210`) never reads
   `spec.capabilities`' `cap::Exec` grants; nothing anywhere checks whether a caller holds `Exec<profile>`
   before `create()` runs. This is the SAME shape of problem ADR-087 already fixed once for
   `mounts`/`net` — reproduced, unfixed, for the capability that was specifically designed to gate
   `create()` itself.

## 3. What this means — two separate findings, not one

**Finding 1 — the residual bullet itself ("not yet Runner-mediated") describes correct-by-design
behavior, currently duplicated three times.** Nothing today constructs an `ExecRequest` from
model-controlled text and hands it to any `SandboxBackend::exec()` without an intervening trusted
layer (§2.1-2.3) — `exec()` trusting its caller to have mediated `source` is the *correct*,
already-settled contract, exactly as 008 §1b layer 2 assigns that responsibility. It is also **not a
well-scoped, closeable Kata-specific gap** to "fix" by building new mediation — that would mean either
inventing a second interpreter (foreclosed by CLAUDE.md's locked decision) or a Kata-hosted
shell-grammar layer with no product consumer and no settled cross-backend precedent. What genuinely
IS a defect here: this one settled contract is stated **three separate times**, independently worded,
in `kata_backend.hpp`, `linux_native_jail_backend.hpp`, and `native_jail_backend.hpp` — the exact
"no per-backend divergence" drift risk ADR-087 already named for a *different* field.

**Finding 2 (new, surfaced by red-team, corrected from an earlier misreading in §2.4) — `cap::Exec`
is a real, live, currently-unenforced I2 gap, structurally identical to the one ADR-087 already fixed
for `mounts`/`net`, just one layer up (gating `create()` itself, not what a created sandbox may
touch).** This is NOT the same finding as #1 — it is not about `ExecRequest::source` mediation at all,
it is about whether a caller is authorized to create/use a given `SandboxBackend` profile in the first
place. It deserves its own resolution, not a bolt-on fix folded silently into #1's close-out (§4B
explains why rushing this into code now would itself be a patch, not a fresh design).

## 4. Finding 1 — options considered, and the resolution

**A. Build `KataShellRunner`/`KataPythonRunner` grammar-mediation, mirroring `ShellRunner`'s
AST-parse-then-dispatch shape.** Rejected. No consumer wants it (§2.3), CLAUDE.md's locked decision
forecloses the Python half outright, and the shell half would be solving a problem native-jail itself
has explicitly left undecided (§2.2) — building a Kata-specific answer to an unsettled cross-backend
question would be exactly the "per-backend divergence" ADR-087 already rejected once.

**B. Leave the three scattered comments exactly as they are; close the residual bullet as
"investigated, not a real gap," no other change.** Correct in substance but leaves the real
documentation-drift risk (§3 Finding 1) unaddressed — three independently-maintained descriptions of
one invariant, silently.

**C. (Recommended, fresh design rather than a patch onto either of the above) Promote the contract
to the actual spec, once, and have all three backends point to it instead of re-describing it.**
**Corrected placement, per red-team:** an earlier draft proposed a new `008-Sandbox-and-Isolation.md`
§1b subsection; red-team correctly flagged that §1b (129-181) is scoped narrowly to native-jail's own
composed mediation stack (Worktree/ShellRunner/PythonRunner/import-allowlist), not backend-agnostic
contract language, while **§2** ("The contract," 182-230) already IS the backend-agnostic
`SandboxBackend`/`SandboxSpec`/`ExecRequest` declaration site, with an existing "every backend must
provide" 7-item list this clause composes naturally alongside (as an 8th item, or an adjoining
paragraph — final wording decided at implementation time). The clause: `SandboxBackend::exec()`
receives `ExecRequest{language, source}` as fully-resolved, already-mediated input; a `SandboxBackend`
implementation is never responsible for interpreting `language` or safety-checking `source` itself;
that responsibility belongs to whatever constructs the `ExecRequest` (010 §1a's `Runner` layer, where
one exists, or trusted host/test code otherwise); a raw, unmediated model-output string reaching any
backend's `exec()` would be an I2/I3 violation **at the call site that constructed the `ExecRequest`**,
not inside the backend that trusted it. Then replace each of the three backends' own multi-sentence
restatements with a single line pointing at that clause — keeping each backend's genuinely
backend-specific residual bullets (Kata's GPU/`pids`/etc. items, native-jail's own M2-scope framing)
intact, replacing only the duplicated *general* claim.

This is not a patch onto any existing function — it adds no new runtime code, no new capability
check, nothing to red-team for a correctness bug. It is a genuine design correction: **the invariant
already exists and is already correct; the defect was that it existed in three independently-editable
copies instead of one authoritative one.**

## 5. Finding 2 — `cap::Exec`, disclosed as a real gap and deliberately NOT rushed into code this pass

§3 Finding 2 established this is real: no capability gates whether a caller may `create()` a
`SandboxBackend` of a given profile at all, despite `cap::Exec` existing specifically for that (007
§3). Implementing it properly (extending `authorize_spec()` with the same empty-skips-check,
opt-in shape ADR-087 already established for `mounts`/`net` — the "fresh design, not patch" answer
here is to *reuse* that one proven mechanism rather than invent a parallel one) hit a real, unresolved
design question mid-investigation, surfaced by reading `sandbox_backend_registry.hpp:151`'s
`register_hardware_isolation_backend(std::string name, ...)`: **backend identity in this codebase is a
host-chosen registration string, not a fixed enum** — no `sandbox_profile` enum type exists anywhere
in the tree today (`grep -rn "enum class sandbox_profile"` finds nothing), contradicting
`capability.hpp:123-125`'s own comment that `cap::Exec::profile_name` values "match `sandbox_profile`'s
enumerators... by convention." A host can register `KataBackend` under any name it likes via
`register_hardware_isolation_backend()`; `KataBackend::create()` itself has no fixed "I am called
kata" identity to compare a grant's `profile_name` against, and none of `authorize_spec()`'s four
current call sites thread the registry name through at all.

Implementing Finding 2 today would mean picking an answer to that question under time pressure —
hardcode a string per backend type (`"kata"`, `"native_jail"`), thread the registry's chosen name
through every `create()` call site, or something else — without it being settled whether backend-*type*
or backend-registration-*name* is the right granularity for this capability to gate. Shipping a check
against a guessed answer would be exactly the kind of decorative-not-real containment this project's
"no vacuous claims" posture already rejects once this session (`decisions/ADR-088-...md`'s rejected
OOM heuristic). **Decision: disclose Finding 2 explicitly as a real, open gap — not close it as
"investigated, not a real gap" (that would be false), and not implement a guessed mechanism either.**
This is recorded as its own follow-on item, not folded into Finding 1's close-out.

## 6. What this draft explicitly does NOT attempt, and why

- **No Runner-grammar mediation for Kata or anyone else** (Finding 1, Option A). No settled consumer
  exists (§2.3); adding one now would be speculative scope growth this project's own conventions
  reject.
- **No `cap::Exec` enforcement code this pass** (Finding 2, §5) — the backend-identity granularity
  question is real and unresolved; implementing against a guess would be worse than disclosing the
  gap honestly.
- **No resolution of native-jail's own open "does `MediatedShellRunner` route through
  `SandboxBackend`" question** (§2.2) — that predates this draft, is not Kata-specific, and deserves
  its own project-owner-scoped decision, not one folded silently into a Kata residual close-out.

## 7. Open questions

*For Finding 1 (resolved by red-team, recorded for the record):*
1. ~~Is §1b or §2 the right home?~~ **Resolved: §2** (red-team finding, §4 above).
2. Does stating this in 008 create tension with 010 §1a's own `Runner` concept comment
   (`runner.hpp:60-70`)? The new clause must describe the CONTRACT (`exec()` trusts its caller), not
   prescribe that a `Runner` must be the thing that satisfies it — `MediatedPythonRunner` satisfies
   the *spirit* of 008 §1b layers 1-2 today without ever calling `SandboxBackend::exec()` at all, and
   the new clause must not accidentally imply that's wrong.
3. Replace only the duplicated *general* sentences in each backend header, not the surrounding
   backend-specific framing — red-team flagged `linux_native_jail_backend.hpp:20-43` specifically as
   a file where the shared sentence (22-24) is interleaved with backend-specific containment prose in
   the same bullet, making a careless replace-all risky. Edit surgically, verify by reading the full
   diff before considering it done, not just a mechanical `Edit` replacement.

*For Finding 2 (open, for whoever picks up the follow-on):*
4. Is backend-*type* (`"kata"`, `"native_jail"`, `"wasm"`, `"remote"`) or backend-registration-*name*
   (the caller-chosen string passed to `register_hardware_isolation_backend()`/`register_backend()`)
   the right granularity for `cap::Exec::profile_name` to gate against? The registry supports multiple
   named instances of conceivably the same backend type with different configuration (e.g. two
   `KataBackend`s registered under different names with different `image_`/`cni_network_name_`) —
   would a real deployment ever need to distinguish those, or is backend-type coarse-enough authority
   for what `cap::Exec` is meant to express (007 §3's whole capability catalog is fairly coarse-grained
   elsewhere too — `NetOut<host>` is the finest-grained entry, most others are all-or-nothing per
   resource class)?
5. Should the check live in `authorize_spec()` itself (called at `create()`), or does `cap::Exec`'s own
   `cpu_ms_cap`/`wall_ms_cap`/`memory_bytes_cap` fields imply it should also narrow/validate
   `SandboxSpec::limits` at the same call, not just gate whether `create()` is allowed to proceed? (007
   §3's "attenuation-only" framing suggests a granted `cap::Exec` should be able to narrow a spec's
   limits, not just gate profile choice — but that's new enforcement surface beyond "is this call
   authorized at all," worth scoping separately.)

## 8. Verification plan, if this proceeds past red-team

**Finding 1 (safe to implement now):** No behavioral test is needed — pure documentation/spec
consolidation, zero runtime code change. Verification is a build check only (comments don't affect
compilation) plus a manual read of all three edited header comments to confirm each backend-specific
residual bullet survived the consolidation intact and only the duplicated general claim was replaced.

**Finding 2 (deferred, not implemented this pass):** Write up as a short "investigated and deferred"
ADR, matching this session's own `decisions/ADR-090-kata-backend-pids-limit-investigated-and-deferred.md`
/ `decisions/ADR-092-kata-backend-disk-net-bytes-investigated-and-deferred.md` precedent — the real
finding (a currently-unenforced, purpose-built capability gate) and the real open question (§7 items
4-5) recorded for whoever resolves it next, not silently dropped.
