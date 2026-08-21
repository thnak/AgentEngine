# ADR-071 — Should AgentEngine ship native, unsandboxed host-process-execution providers, and how
# do they stay compatible with I2/I3 and worktree confinement without a sandbox jail?

**Status:** Proposed (design → red-team → prove phases complete; implemented and tested; awaiting
project-owner "Judged" sign-off).

**Relates to:** `decisions/ADR-070-host-configurable-responsibility-boundary.md` (the Delegated
Decision Seam pattern this ADR is a bounded instance of — see §4), `decisions/ADR-059-invoke-agent-
tool-capability-attenuation.md` (the per-invocation capability-narrowing precedent §5a's design
follows), `decisions/ADR-014-worktree-mount-path-canonicalization.md` (the handle-anchored
containment primitive this ADR's worktree confinement is built on and does not re-implement),
`decisions/ADR-001-shellrunner-grammar-and-dispatch.md` / `decisions/ADR-004-appcontainer-native-
jail-windows-backend.md` (the existing mediated-execution machinery this ADR is a deliberately
distinct, new capability class from, not a variant of), `decisions/ADR-037-remove-quark-as-core-
runtime.md` (the precedent for how a CLAUDE.md-locked decision gets narrowly amended, §7), `007-
Capability-and-Trust-Model.md` (I2/I3, checked against and left intact), `008-Sandbox-and-Isolation.
md` §1 (the "never a second [Python] runtime" locked paragraph this ADR narrows, §7), `AgentEngine-
Specification.md` §5 D2/D5 (the "PATH-based name resolution is ambient authority" passage this
ADR's whole design exists to stay inside, not override).

## 1. The question

**Stated so it has a wrong answer:** the project owner asked for four new `ContextProvider`
conformers — `NativeShellProvider`, `NativeBashProvider`, `NativePythonProvider`,
`NativeNodeProvider` — that run **natively on the host, with no sandbox jail**, each able to detect
what's actually installed and pre-scan it so an LLM can call a discovered executable by its short
name instead of a full path. Read literally, "scan PATH and let the model call what it finds" is
the exact ambient-authority pattern `AgentEngineSpecification.md` §5 names by name as the reason
`ShellRunner` was built as a closed dispatch table instead of wrapping a real shell — building it
anyway would reopen I2. Getting this wrong in the permissive direction means shipping the ambient-
authority hole 007 exists to close, under a different file name. Getting it wrong in the
conservative direction — refusing to build any of this because "PATH resolution is forbidden" —
fails the project owner's own explicit, ADR-070-Judged decision to trade some engine-enforced
safety for a broader feature surface, and leaves a legitimate SDK-extensibility need unmet.

The actual design question is narrower: **can PATH-scanning be discovery only, with a host-granted
capability as the sole source of invocation authority, such that scanning can never itself become
the reason a program becomes callable** — and if so, what closes the two gaps that answer leaves
open: (a) worktree confinement, which the sandbox jail normally provides and this design
deliberately omits, and (b) the CLAUDE.md-locked "embedded CPython, never a second runtime"
decision, which `NativePythonProvider` (a second, unmediated, host-installed Python) directly
implicates.

## 2. Prior art surveyed, and what it already proves

**ADR-070** (Judged 2026-08-21, same day as this ADR's design phase) already establishes the
mechanism this ADR needs: the *Delegated Decision Seam* — five required properties (explicit
opt-in, fails closed/safe when unset, narrows or decides among already-possessed authority only,
host code never model output, always audited) that let a host be handed real responsibility without
reopening I2/I3's own mechanisms. ADR-070 §1 names "capability/sandbox defaults" and "the
`ContextProvider` chain" as explicitly in scope for this kind of work; it implemented four bounded
instances of its own but did not implement a native-execution provider — this ADR is the next
instance of that same pattern, not a new one invented from scratch.

**ADR-059** already proves the per-invocation capability-narrowing shape this design's `real_run()`
step reuses: `invoke_agent_tool()`'s fix attenuates a caller's held ceiling down to a specific
callee's declared ceiling, per call, fail-closed if not subsumed. `cap::NativeExec`'s
`native_exec_pattern_covers()` narrowing (§5) is the identical shape applied to "one held pattern
grant narrowed to one concrete, resolved program name."

**ADR-014** already proves the handle-anchored, TOCTOU-safe containment primitive
(`open_within_mount_root`) this design's worktree-confinement half is built on. It was surveyed,
not re-implemented: this ADR's own red-team (§6) found and documents the real, honest limit of
reusing it for argv validation rather than file opens (a genuine TOCTOU gap a real unsandboxed
child process's own subsequent syscall reintroduces) — a gap `open_within_mount_root`'s own callers
inside this codebase don't have, because they use the verified HANDLE directly.

**Existing `ShellRunner`/`MediatedPythonRunner`** are confirmed, by reading both, to be a
*different* mechanism entirely — engine-native interpretation with a statically-proven absence of
any process-creation symbol (ADR-001 Sh-S1), not a wrapped-and-mediated real shell/interpreter. This
ADR's providers are not an extension of that mechanism; they are new, genuinely-process-spawning
code, kept structurally separate (a new Tier-2 backend, `src/backends/native_process/`, never
linked into `agentengine_shell_runner`/`agentengine_python_runner`) so Sh-S1's static proof for the
*existing* mechanism stays meaningful regardless of whether this new one is even built.

## 3. The competing designs

**Design A — open PATH scan, capability-gated invoke only.** Advertise every executable found on
PATH to the model (so it can call anything by short name), but still require a capability check
before actual execution. Steelman: maximizes the "call things by short name" ergonomics the
original request asked for. Rejected: discovery ITSELF becomes an oracle for "what's installed on
this machine" regardless of what the host actually authorized, which is a real information
disclosure even if invocation stays gated — and it is the specific pattern `AgentEngineSpecification.
md` §5 names as unacceptable, not merely undesirable.

**Design B (chosen) — capability-first discovery: scan filters against an already-held grant,
never the reverse.** A `cap::NativeExec{program_pattern, worktree_mount_id, ...caps}` capability,
host-granted via `CapabilitySet::grant_root()` before a session starts, is the ONLY thing that
determines what `scan_path()` may return or what a tool call may invoke. PATH-scanning is real
(it looks at the real filesystem, not a static allowlist), but it can only ever narrow "what's on
PATH" down to "what's on PATH AND already authorized" — never the reverse. Steelman: satisfies
ADR-070 property 3 exactly (narrows already-possessed authority, never mints it), stays inside
`AgentEngineSpecification.md` §5's own constraint, and still delivers the actual ergonomic win
requested (the model calls `python` instead of `C:\Python314\python.exe`) because the SHORT NAME,
not the full path, is what a granted pattern and a scanned candidate are matched on.

**Design C — fully open, no gating beyond "sandbox is off."** Scan and expose everything, no
allowlist, minimal capability ceremony. Rejected outright, named for the record: this is Design A
without even the invocation-time capability check, i.e. a direct reopening of I2 with no mitigation
at all. Not implemented, not prototyped — rejected at the design phase on the same grounds as
Design A, more severely.

## 4. Required properties (ADR-070 §4, applied to this instance)

1. **Explicit opt-in.** A `cap::NativeExec` grant is never ambient — it exists only via
   `CapabilitySet::grant_root()`, host-authored, attached before `session.start_run()` (mirrors the
   real `examples/06_capabilities_and_denial.cpp` pattern). A `Native*Provider` additionally
   requires explicit host construction with `owned_patterns`/`mount_root`/`worktree_mount_id` — it
   is never reachable by default; a session with no provider wired has zero native-exec surface.
2. **Fails closed/safe when unset.** Proven in `tests/test_native_providers.cpp` T1: `on_context()`
   with no held grant contributes zero tools and zero instructions — byte-for-byte the same as a
   session that never heard of this ADR at all.
3. **Narrows or decides among already-possessed authority only.** The load-bearing property.
   `scan_path()` (§5, `native_path_scan.hpp`) filters PATH candidates against already-granted
   patterns; it never returns an ungranted-but-real executable (proven: `test_native_path_scan.cpp`
   R-P3, `test_native_providers.cpp` R-T3). `real_run()` re-verifies the grant fresh, per
   invocation, against the LIVE `EffectContext` — never trusting a prior turn's scan (proven:
   `test_native_providers.cpp` R-S6, an invocation with the grant removed after discovery is
   denied).
4. **Host code, never model output.** `owned_patterns`/`mount_root`/`worktree_mount_id` are
   constructor parameters supplied by host/operator code; nothing in the discovery or dispatch path
   accepts a `Tainted<T>`. The model-facing `NativeProcessRunArgs{program, args}` is ordinary tool-
   call input (gated by capability + approval like any other tool call), not an authority channel.
5. **Always audited (I4).** Every invocation traverses the ordinary `ToolDescriptor::invoke`
   closure reached only through ADR-066's attribution-stamped `ContextContribution.tools` path —
   no separate, unaudited execution channel exists.

## 5. Falsifiable claims, evidence, and per-claim verdicts

### 5a. `cap::NativeExec` — the capability kind (`trust/capability.hpp`)

Added the 8 required extension points (`capability_kind::native_exec`, the `cap::NativeExec`
struct, the `Capability` variant arm, `capability_kind_of()`, `capability_from_kind()`,
`is_inert_for_text_derived_declassification()` → `false`, `capability_detail::subsumes_payload()`
via a new `native_exec_pattern_covers()` helper, `cap::decl::NativeExec<Pattern, Mount>` +
`to_capability()`), plus `CapabilitySet::native_exec_grants()` (a plain lookup, matching
`find_background()`/`find_schedule()`'s own established shape).

| Claim | Disproving experiment | Verdict |
|---|---|---|
| An exact-name grant covers only that exact request. | Grant "node", request "python3"; assert denied. | **CORRECT** — `test_native_exec_capability.cpp` N1. |
| A prefix grant ("python\*") covers matching concrete names and nothing else. | Grant "python\*", request "python3.11" (allow) and "node" (deny). | **CORRECT** — N2. |
| PATH-scan discovery output can never itself grant reach (ADR-070 property 3). | Simulate a 5-candidate scan against a 1-entry grant; assert exactly 1 is invocable. | **CORRECT** — R-N3. |
| A REQUESTED pattern can never itself carry a wildcard (no re-widening). | Request "\*" or "py\*" against a "python\*" grant; assert denied. Positive control: re-requesting the identical "python\*" succeeds. | **CORRECT** — R-N4. |
| `attenuate()` narrows a wildcard grant to one concrete invocation, including scalar caps. | Narrow "python\*"/30000ms to "python3.11"/5000ms; assert success and that a FURTHER over-budget narrowing is rejected. | **CORRECT** — N5. |
| A mismatched `worktree_mount_id` is rejected even with a matching program name. | Grant "node"@"trusted", request "node"@"different"; assert denied. | **CORRECT** — R-N6. |
| `text_derived` can never auto-declassify a native-exec call. | `is_inert_for_text_derived_declassification(native_exec)` must be `false`; positive control: `fs_read` stays `true`. | **CORRECT** — R-N7. |
| Kind derivation and the compile-time declaration tag round-trip correctly. | `capability_kind_of()`, `contains_kind()`, `cap::decl::NativeExec<"node","workdir">` → `to_capability()` → covers the matching runtime grant, not an undeclared one. | **CORRECT** — N8, N9. |

`tests/test_native_exec_capability.cpp`: 20/20 checks pass. Zero regressions in the pre-existing
`test_capability_enforcement`/`test_capability_declaration_tags`/`test_policy_reachability`/
`test_capability_token_proof` suites (all still 100% pass after this header's changes).

### 5b. `native_process_spawn.hpp` — the real, unsandboxed spawn primitive

Windows `CreateProcessW`, deliberately with NO `SECURITY_CAPABILITIES` attribute (no AppContainer)
— the isolation this ADR omits — but WITH the existing `native_jail::JobObjectLimits` reused
(without its AppContainer half) purely for best-effort CPU/wall/memory accounting, because CLAUDE.md's
"Machine safety" rule ("a test proving a fork bomb is contained must not be able to take the
machine with it") applies to a genuinely new process-spawning surface regardless of isolation
strength. A fixed, minimal environment block (no ambient host env leak, same rule
`native_jail_backend.cpp` already enforces for its own child processes) and the documented
Microsoft C runtime argv-quoting algorithm (a real injection vector if implemented wrong).

| Claim | Disproving experiment | Verdict |
|---|---|---|
| Argv quoting matches known-correct Microsoft vectors, including the trailing-backslash-before-quote injection case. | Compare against documented examples; build a 3-argument command line with a space-containing, backslash-terminated argument next to a flag, confirm it differs from the naive (vulnerable) quoting and keeps the flag a genuinely separate argument. | **CORRECT** — `test_native_process_spawn.cpp` Q1/Q2/R-Q3. |
| An empty `cwd` is rejected, never silently defaulting to the host process's own directory. | Spawn with `cwd=""`; assert denial. | **CORRECT** — S1. |
| A real spawn captures stdout and exit code faithfully, including a nonzero exit. | Spawn `cmd.exe /c echo ...` (assert stdout/exit 0) and `cmd.exe /c exit 3` (assert exit 3). | **CORRECT** — S2, S3. |
| A runaway/hung process is still terminated by a wall-clock safety ceiling, even if the caller sets a short one. | Spawn `ping -n 60 127.0.0.1` (≈59s) with `wall_ms_cap=500`; assert termination within seconds, classified `timeout`. | **CORRECT** — R-S4. |

`tests/test_native_process_spawn.cpp`: 20/20 checks pass (Windows; no POSIX implementation yet — a
named, honest gap, matching this project's existing "no Linux env available" posture for other
Milestone-3+ Linux items).

### 5c. `native_path_scan.hpp` — capability-filtered PATH discovery

| Claim | Disproving experiment | Verdict |
|---|---|---|
| An exact-name grant finds exactly that real file on a real, controlled PATH. | Real scratch directory with `node.exe`/`python3.11.exe`/`rm.exe`/`readme.txt`; grant `{"node"}`; assert `node` found, nothing else. | **CORRECT** — `test_native_path_scan.cpp` P1. |
| A prefix grant finds the matching real file. | Grant `{"python*"}`; assert `python3.11` found. | **CORRECT** — P2. |
| A REAL, present executable that matches no grant is NEVER surfaced (the load-bearing claim). | `rm.exe` genuinely exists in the scanned directory (proven by its siblings being found); assert it never appears for `{"node"}` or `{"node","python*"}`, while the actually-granted siblings STILL do in the same call. | **CORRECT** — R-P3, with an explicit positive control proving the denial is real filtering, not `scan_path` failing outright. |
| No grants → nothing discovered, ever, regardless of what exists. | `scan_path({})`; assert empty. | **CORRECT** — P4. |
| A non-executable-extension file is never surfaced. | `readme.txt` with a matching-name grant; assert empty. | **CORRECT** — P5. |

`tests/test_native_path_scan.cpp`: 8/8 checks pass. **Found-during-implementation gotcha,
documented for the record:** the first version of this test used Win32 `SetEnvironmentVariableA` to
control `PATH`, and failed non-deterministically depending on what was actually installed on the
test machine — `agentengine::pal::env_var()` is CRT-backed (`_dupenv_s` on MSVC), which does **not**
observe writes made through the separate Win32 process-environment-block API. Fixed by using
`_putenv_s` (the CRT-side write) instead; this is a real, easy-to-hit divergence worth naming rather
than silently correcting.

### 5d. `native_worktree_bridge.hpp` — worktree-confined argv validation

| Claim | Disproving experiment | Verdict |
|---|---|---|
| A bare filename (no separator) is always accepted. | `validate_argv_path(root, "output.txt")`. | **CORRECT** — `test_native_worktree_bridge.cpp` W1. |
| An existing nested path resolving inside the mount is accepted, both '/' and '\\' forms. | Real nested file under a real mount root. | **CORRECT** — W2. |
| A not-yet-existing intermediate directory is REJECTED (named scope limit), not silently allowed. | `"does_not_exist_yet/file.txt"`. | **CORRECT** — R-W3. |
| Absolute/drive/UNC forms are rejected outright. | `"C:\...\cmd.exe"`, `"\\\\server\\share\\..."`, `"\\rooted"`. | **CORRECT** — R-W4. |
| `..` traversal is rejected. | Leading and embedded `..`. | **CORRECT** — R-W5. |
| A leading `/` WITH a further separator (genuinely absolute-shaped) is rejected. | `"/etc/passwd"`. | **CORRECT** — R-W6. |
| **A CLI flag is never misread as a path escape attempt** (a REAL bug found live-wiring `NativeShellProvider` to `cmd.exe`: `"/c"` was rejected as an absolute path before this fix). | `"/c"`, `"/v:on"`, `"-n"`, `"--flag=value"` accepted; `"/etc/shadow"` (leading `/` WITH a further separator) still rejected — the narrow-carve-out negative control. | **CORRECT after a fix** — W7; see §6 item 4 for the full account. |

`tests/test_native_worktree_bridge.cpp`: 15/15 checks pass (10 before the flag fix, 5 added with
it).

### 5e. `native_providers.hpp` — the four `ContextProvider` conformers, end to end

| Claim | Disproving experiment | Verdict |
|---|---|---|
| No grant → contributes nothing (ADR-070 property 2). | `on_context()` with a null `EffectContext::capabilities`; assert empty tools/instructions, `is_available()` false. | **CORRECT** — `test_native_providers.cpp` T1. |
| A held, matching grant seeds instructions AND exactly one tool. | Real grant, real PATH scan finds "cmd"; assert 1 tool named `native_shell_run`, instructions mention "cmd". | **CORRECT** — T2. |
| An UNOWNED pattern (a real, held grant this provider instance was never configured to recognize) is never used. | Provider owns `{"cmd"}`; grant `"node"` instead; assert not available. | **CORRECT** — R-T3. |
| A MISMATCHED `worktree_mount_id` is never used, even with a matching program name (defense in depth against a wiring mistake). | Grant "cmd"@"OTHER_MOUNT" vs. provider's own "workdir". | **CORRECT** — R-T4. |
| A real, end-to-end invoke through the tool's own closure actually spawns and returns real output. | Invoke `native_shell_run{program:"cmd", args:["/c","echo","hello-native-provider"]}`; assert exit 0, stdout contains the text, reply round-trips the declared JSON schema. | **CORRECT** — S5. |
| The SAME tool closure, called with the grant absent, is denied — a fresh per-invocation check, not a cached one. | Build the `ContextContribution` while a grant IS held; invoke with a DIFFERENT `EffectContext` holding none. | **CORRECT** — R-S6. |
| A worktree-escaping argv entry is rejected end to end, before ever reaching the spawned child. | Invoke with `args: ["/c","type","../../escape.txt"]`; assert denial. | **CORRECT** — R-S7. |

`tests/test_native_providers.cpp`: 21/21 checks pass.

### 5f. `native_capability_announcer.hpp` — composing multiple families into one seeded block

Reuses the EXISTING `ComposedContextProvider<Ms...>` (`core/composed_context_provider.hpp`, already
proven elsewhere, e.g. `test_tool_optimizer_provider.cpp` R6) rather than a new, bespoke aggregator
— a deliberate simplification made during implementation: an initial sketch of a custom variadic
fold-over-coroutines aggregator was abandoned before being written, once `ComposedContextProvider`
was confirmed to already merge N providers' instructions/tools/messages through the same,
already-tested `assemble_context()` every other multi-provider composition in this codebase uses.

| Claim | Disproving experiment | Verdict |
|---|---|---|
| Composing all 4 families merges their tools and instructions into one contribution. | Real host machine, real `cmd`/`bash`/`python`/`node` all genuinely installed; compose all 4, assert 4 tools present and the combined instructions text mentions all 4 families. | **CORRECT** — `test_native_capability_announcer.cpp` A1-A3. |
| A provider with no matching grant contributes nothing, while its siblings still do (no fail-all). | Compose Shell+Python, grant only `cmd`; assert exactly 1 tool present, the ungranted one absent. | **CORRECT** — R-A4. |

`tests/test_native_capability_announcer.cpp`: 15/15 checks pass, run against this development
machine's REAL installed `cmd`/`bash`/`python`/`node` (not synthetic fixtures) — a realistic
end-to-end demonstration of the four-family design intent, not merely a unit test.

### 5g. `NativeCapabilityAnnouncer<Ps...>` — family-distinctness, compile-time

Found-during-review requirement (not in the original implementation pass): if `NativePythonProvider`
is available, no SECOND python-family provider may exist in the same composed tree — mandatory
because an LLM handed two functionally-identical tools (e.g. two independently-scoped
`NativePythonProvider` instances both contributing a `native_python_run` tool) has no principled way
to choose between them. Confirmed as a REAL gap first: `core/context_assembly.hpp`'s
`assemble_context()` concatenates every contributor's `ContextContribution.tools` unconditionally
(`out.combined.tools.insert(...)`, no name-collision check anywhere in that function) — so this was
not already prevented by existing machinery. Closed with a COMPILE-TIME check
(`detail::native_provider_families_distinct<Ps...>()`, a `requires`-clause constraint on the
`NativeCapabilityAnnouncer` alias itself): every `Ps::name` in the pack must be pairwise distinct,
which is both necessary and sufficient because two instances of the SAME `Traits` type (e.g. two
`NativeProcessProvider<traits::Python>`s) always share the identical declared name regardless of
construction-time arguments.

| Claim | Disproving experiment | Verdict |
|---|---|---|
| `NativeCapabilityAnnouncer<NativePythonProvider, NativePythonProvider>` fails to compile. | `try_compile()` gate against `compile_fail/native_capability_announcer_rejects_duplicate_family.cpp`. | **CORRECT** — build FATAL_ERRORs if this ever compiles; confirmed rejected on this pass. |
| Four DISTINCT families still compose fine (the rejection above is real filtering, not the header/alias failing outright). | `try_compile()` positive control against all 4 real families. | **CORRECT** — `compile_fail/native_capability_announcer_distinct_families_positive_control.cpp` compiles cleanly. |

Both proofs run as configure-time `try_compile()` gates (`tests/CMakeLists.txt`, WIN32-gated,
matching this whole backend's current platform scope, independent of whether
`AGENTENGINE_WITH_NATIVE_PROCESS` happens to be ON for a given configure) — the same idiom this
project already uses for `Tainted<T>`'s no-implicit-conversion proof and `CapabilitySet`'s
no-direct-construction proof. Zero runtime cost: the mis-wiring this guards against is caught at
BUILD time, before any session ever runs. Named scope limit: this only sees the direct `Ps...` pack
passed to ONE `NativeCapabilityAnnouncer` call — it cannot see through a caller manually nesting
composition (e.g. a second, separately-built announcer wired in elsewhere), which stays the host's
own responsibility, the same "the mechanism only bounds what a host's own misconfiguration can
reach" shape ADR-070 §7 already names for `PolicyDecider`.

**Full-tree evidence.** Windows/MSVC, `AGENTENGINE_WITH_NATIVE_PROCESS=ON`: full configure shows
`ADR-071 compile-fail proof: OK` (§5g's two `try_compile()` gates, alongside this project's five
pre-existing compile-fail proofs, all still `OK`); full `ninja` (all targets) zero compile errors;
full `ctest -j1` (sequential): **227/227 passed, 0 failed** (`Total Test time (real) = 256.29 sec`),
including all 6 new test executables this ADR adds — 99 checks total (20 capability + 20 spawn + 8
path-scan + 15 worktree-bridge + 21 providers + 15 announcer) — and zero regressions anywhere in the
pre-existing suite. (`ctest -j4` showed one unrelated, load-sensitive flake,
`test_rt_spawn_cost_budget` — a pre-existing concurrency test untouched by this ADR, whose own file
comment documents it depends on genuine thread contention under a real budget race; confirmed
non-regressive by re-running it standalone, ALL PASS, and by the clean `-j1` run above.) Default
build (`AGENTENGINE_WITH_NATIVE_PROCESS=OFF`, unset): confirmed the new backend/tests are entirely
absent from the build graph — a host that never opts in sees zero footprint from this ADR at all.

## 6. The red-team attack

1. **Can `scan_path()` ever expose something not already capability-granted?** No — proven with a
   real filesystem and a real ungranted-but-present executable (`rm.exe`), not a synthetic string
   comparison (§5c R-P3, §5e R-T3).
2. **Can a `text_derived` (model-influenced) tool call auto-invoke a native-exec tool with no
   human/policy decision?** No — `is_inert_for_text_derived_declassification(native_exec)` is
   `false` with a positive control proving the function still discriminates (§5a R-N7); every
   `native_*_run` tool defaults to `approval_mode::always_require`.
3. **Can a spawned child escape the worktree?** **Partially mitigated, not eliminated — the
   honestly-named residual.** cwd confinement + argv pre-validation via the same handle-anchored
   primitive ADR-014 uses for the containing-directory chain narrows the attack surface
   meaningfully, but a real unsandboxed child process's OWN subsequent raw syscall on a
   string-passed path argument is a genuine TOCTOU window this mechanism cannot close (unlike
   `open_within_mount_root`'s own callers, which use the verified HANDLE directly, never a
   re-derived path) — named explicitly in `native_worktree_bridge.hpp`'s own header comment and
   here, matching this project's disclosure norm for ADR-041's accepted Windows ACE-leak residual
   rather than overclaiming a hard boundary.
4. **Did the argv-shape heuristic itself introduce a false positive that would have blocked
   legitimate use, or a false negative that would have let something through?** A real false
   positive was found live-wiring `NativeShellProvider` to `cmd.exe`: `"/c"` (an ordinary Windows
   flag) was rejected as an absolute-path escape attempt, because the original classifier treated
   ANY argument containing a separator character as path-shaped. Fixed by adding an explicit,
   narrowly-scoped flag carve-out (`looks_like_flag()`: a leading `-`, or a leading `/` with no
   FURTHER separator) — proven not to reopen the negative case with `"/etc/shadow"` (leading `/`
   WITH a further separator) still rejected in the same test (§5d W7). This is exactly the kind of
   finding this ADR's own §5 evidence process exists to surface: caught by a real end-to-end spawn
   test against `cmd.exe`, not by reasoning about the code.
5. **Does `NativePythonProvider` create two divergent "python" behaviors the agent (or a human
   verifying its output) must reason about?** Yes, by design and unavoidably — mitigated, not
   eliminated, by keeping `MediatedPythonRunner`/the embedded CPython interpreter as *the*
   code-interpreter path (untouched by this ADR) and `NativePythonProvider` as a distinctly-named
   (`native_python_run`, not `execute_code`), separately-instructed native tool family whose own
   seeded instructions explicitly say "Distinct from the engine's own embedded, mediated code
   interpreter" (`native_providers.hpp`'s `traits::Python::tool_description`) — never silently
   substituted for the interpreter at any call site.
6. **Does leaving `ToolDescriptor::capability_ceiling` empty on the native-exec tools (matching
   `MemoryProvider::make_recall_tool_descriptor()`'s precedent) create a gap the generic pipeline's
   step 4/7 would otherwise have closed?** No — the authorization decision that field would
   otherwise express (an AND-of-all list, which cannot express "one of N alternative grants") is
   performed INSIDE `real_run()` against the LIVE `EffectContext`, proven fresh per invocation
   (§5e R-S6) — strictly more precise than a static ceiling would be for this shape, not a weaker
   substitute for it.

7. **Can two providers of the same native-execution family (e.g. two `NativePythonProvider`
   instances) end up composed into one tree, handing the LLM two functionally-identical tools with
   no principled way to choose between them?** No, for the direct composition point
   (`NativeCapabilityAnnouncer<Ps...>`) — rejected at COMPILE TIME via a `requires`-clause
   distinctness check over `Ps::name`, proven with a real `try_compile()` negative case plus a
   positive control (§5g). Confirmed this was a genuine, previously-unclosed gap:
   `assemble_context()` itself concatenates contributions with no tool-name collision check at all.
   Named scope limit: a host manually nesting composition outside one `NativeCapabilityAnnouncer`
   call is not covered — the same residual class as item 3 above and ADR-070 §7's own precedent.

No FATAL finding beyond the flag-classification bug (item 4) and the family-distinctness gap
(item 7), both found and fixed before this ADR's own evidence was finalized, not left as known
residuals.

## 7. The decision

**Design B is adopted and implemented**, per §5's evidence. It binds:
- `trust/capability.hpp` — `cap::NativeExec` added, `is_inert_for_text_derived_declassification`
  extended (must stay `false` for this kind), `CapabilitySet::native_exec_grants()` added.
- `src/backends/native_process/` — new Tier-2 seam (`native_process_spawn`, `native_path_scan`,
  `native_worktree_bridge`, `native_providers`, `native_capability_announcer`), gated behind
  `AGENTENGINE_WITH_NATIVE_PROCESS` (OFF by default), never linked into
  `agentengine_shell_runner`/`agentengine_python_runner`. `NativeCapabilityAnnouncer<Ps...>`
  additionally requires every composed `Ps::name` be pairwise distinct (§5g) — a host cannot compose
  two providers of the same native-execution family into one tree, mandatory so an LLM is never
  handed two functionally-identical tools.
- `CLAUDE.md`'s Python locked-decision bullet and `008-Sandbox-and-Isolation.md` §1 — **amended**,
  narrowly, following `decisions/ADR-037-remove-quark-as-core-runtime.md`'s own precedent for
  editing a locked decision (replace the bullet with a new locked statement + a `(Historical: ...)`
  clause naming this ADR + a re-lock sentence): the "embedded CPython, never a second runtime" rule
  is confirmed to be scoped, by its own original evidentiary case (WASM-vs-native tradeoffs for the
  *code interpreter*), to `MediatedPythonRunner`'s mediated, in-process embedding — `NativePythonProvider`
  is a distinct, explicitly-scoped, host-opt-in capability for a different job (native host-installed-
  package automation, not the CodeAct interpreter), not a second interpreter for the same one. See
  the companion `CLAUDE.md`/`008-Sandbox-and-Isolation.md` edits landed alongside this ADR.

**Explicitly out of scope, named rather than left implied:**
- A POSIX (`posix_spawn`) implementation of `native_process_spawn`/`native_worktree_bridge` —
  design-only for non-Windows today, matching this project's existing "no Linux env available to
  build/verify against" posture for other Milestone-3+ Linux gaps (`native_path_scan.cpp` alone is
  portable and builds on both platforms).
- Real engine-side harvesting of a native process's worktree writes back into the tracked worktree
  Tree (`materialize_mount()`/`harvest_mount()`, `src/backends/native_jail/worktree_mount_sync.hpp`)
  — this ADR's providers consume an ALREADY-MATERIALIZED real directory the host supplies; the host
  remains responsible for materializing before and harvesting after, using that existing, unmodified
  machinery, exactly as documented in `native_providers.hpp`'s own file-top comment.
- A general "validate a not-yet-existing multi-segment path whose intermediate directories also
  don't exist yet" mode for argv validation — rejected outright today (§5d R-W3), not silently
  worked around.
- Real resolution of the argv-validation TOCTOU residual named in §6 item 3 — would require handing
  a spawned child a pre-opened handle instead of a path string for every path-shaped argument,
  which most real host executables (cmd.exe, bash, python, node) have no calling convention for.

**Residual risks:**
- The TOCTOU gap named in §6 item 3 — real, honestly disclosed, not eliminated.
- A host that wires a `Native*Provider` with an overly broad `owned_patterns` (e.g. `"*"` — though
  `native_exec_pattern_covers` treats a bare `"*"` grant identically to any other prefix grant, it
  is a deliberately wide one the HOST chose) has, by its own configuration, made an intentionally
  broad capability decision — the same "the host can misconfigure its own policy, the mechanism only
  bounds what that misconfiguration can reach" shape ADR-070 §7 already named for `PolicyDecider`.
- `Job Object` resource capping (§5b) inherits the SAME "job-time-limit is unreliable, wall-clock is
  the trustworthy enforcement point" finding `job_object_limits.hpp`'s own header comment already
  documents from ADR-004's measurement — `cpu_ms_cap` is best-effort only; a caller that needs a
  dependable bound must set `wall_ms_cap`.
- The family-distinctness guarantee (§5g) covers exactly one `NativeCapabilityAnnouncer` call's own
  `Ps...` pack — a host that manually composes two SEPARATE announcers (or hand-builds a
  `ComposedContextProvider` bypassing this alias entirely) into some outer, further-nested tree can
  still reintroduce duplicate-family tools; this is the host's own construction choice, outside what
  a compile-time constraint on one alias can reach.
