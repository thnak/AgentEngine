# ADR-082 — Does the `native-jail` promotion gate (008 §9, G1-G8) actually hold for the real,
# shipped M2 Phase C implementation, gate by gate, on the evidence that exists today — and where it
# doesn't, what specifically is missing?

**Status:** Judged (2026-08-23) — a synthesis-and-verification pass over already-shipped work, not a
fresh design decision. Formalizes M2 Phase C (`docs/planning/milestone-2-tools-capabilities-sandbox-
breakdown.md` Phase C, tasks C1-C6, all marked "(done)" there) against `decisions/README.md`'s own
rule that an isolation boundary is security-critical and needs an ADR — none existed for this gate as
a whole before this one. Re-verifies the Windows-side evidence directly, this pass; the Linux-side
evidence is read and reported honestly, not re-executed (no Linux build/test environment available in
this session — see §4's disclosure on each gate this affects).

**Relates to:** `decisions/ADR-004-appcontainer-native-jail-windows-backend.md` (Spiked, not
Judged — the Windows AppContainer/Job-Object primitives task C2's real backend uses, superseding that
ADR's own prove-phase spike code without a follow-on ADR ever being written until now),
`decisions/ADR-041-appcontainer-ace-leak-accepted-residual.md` (Judged — the one narrow Windows
residual this gate already had its own ADR for), `decisions/ADR-081-jailed-python-worker-process-
slice-1.md` (Judged — closes 008 §1b/§3's kernel-jail requirement for the *interpreter specifically*,
Windows-only; this ADR is broader — the whole `SandboxBackend` promotion gate, not just the
interpreter's own additive worker-process surface), `docs/planning/linux-native-jail-pivot-root-
containment-design-draft.md` and `docs/planning/linux-jailed-python-worker-design-draft.md` (both
already name, and this ADR does not re-litigate, the one root-cause Linux filesystem/process-
visibility gap several of the findings below trace back to), `docs/issues/m2-phase-c-native-jail-
sandbox.md` (the original phase tracker; its own "Exit criteria" are what this ADR checks against).

## 1. The question

`docs/planning/v1-implementation-roadmap.md`'s Milestone 2 exit criterion states, by name: "`native-
jail` sandbox parity (008 §9 G1) holds on Windows and Linux." `008-Sandbox-and-Isolation.md` §9
defines eight gates (G1-G8) a sandbox profile must clear to be considered promoted. M2 Phase C's own
tracker (`docs/issues/m2-phase-c-native-jail-sandbox.md`) lists six tasks (C1-C6) targeting G1-G4
specifically, all six marked done in the breakdown doc with real code and real tests. **No ADR was
ever written for any of it** — `decisions/README.md` shows exactly one native-jail-adjacent Judged
entry before this pass (ADR-041, one narrow residual) and one Spiked-not-Judged entry (ADR-004, a
prove-phase spike the real C2 backend explicitly does not extend). Per CLAUDE.md's own rule ("any
security-critical choice: capability representation, isolation boundary... needs an ADR"), this is a
real process gap independent of whether the underlying code is actually sound — the question this ADR
answers is whether it is, gate by gate, on the evidence that actually exists, not on the roadmap
document's own unqualified "(done)"/"holds" language.

## 2. Method

Not a design → red-team → prove cycle (nothing new is being designed) — a **verification** pass: for
each of G1-G8, locate the test(s) claiming to satisfy it, read them directly (not summarized from
memory of the breakdown doc), and — for the Windows side, where a build/test environment exists in
this session — re-run them directly rather than trust their last-known status. Where a gate's own
claim is scoped narrower than "fully holds" (several are, honestly, in the tests' own header
comments), that scoping is preserved here, not smoothed over into a blanket "Judged."

## 3. Re-verified evidence (this session, Windows)

Built and ran directly, this pass (the same build already used for `decisions/ADR-081-jailed-python-
worker-process-slice-1.md`'s own evidence section):

```
test_native_jail_runner_stubs .................. Passed  0.01s
test_native_jail_backend_windows ................ Passed  5.11s
test_native_jail_abuse_corpus_windows ........... Passed  5.24s
test_native_jail_parity_windows ................. Passed 10.13s
test_native_jail_ambient_authority_windows ...... Passed  3.58s
test_native_jail_teardown_cycles_windows ........ Passed  4.73s
100% tests passed, 0 tests failed out of 6
```

The Linux counterparts (`test_native_jail_backend_linux.cpp`, `_abuse_corpus_linux.cpp`,
`_parity_linux.cpp`, `_ambient_authority_linux.cpp`, `_teardown_cycles_linux.cpp`) were read in full
for this ADR but not executed — reported below strictly from what their own code and header comments
claim, with that limitation stated at each gate it affects, matching this project's established
disclosure norm for Windows-only sessions (`decisions/ADR-071-...md`'s POSIX residual, both design
drafts referenced above).

## 4. Gate by gate

### G1 (parity) — Judged, correctly scoped, not a blanket claim

`tests/helpers/abuse_case_corpus.hpp` is **one array**, included verbatim by both
`test_native_jail_parity_windows.cpp` and `_parity_linux.cpp` — structural parity (the same table
compiles into two different binaries against two different `SandboxBackend` implementations) rather
than two independently-written tests kept in sync by hand. Five cases (`well_behaved`,
`ordinary_failure`, `infinite_loop`, `oom`, `unbounded_output`) each reduce to one
`exec_outcome_class`; re-run directly for this ADR on Windows, all five pass (`test_native_jail_
parity_windows`, above).

**The corpus itself discloses, in its own header comment, exactly what it does NOT claim**: fork-bomb
and fs-escape are deliberately excluded from this shared table because their outcomes don't reduce to
a single `exec_outcome_class` the same way (fork-bomb is a succeeded-count; fs-escape is platform-
asymmetric — real on Windows, absent on Linux, see G2/G3 below) — each is handled instead by its own
per-platform test with its own G2 positive control, not silently folded into a parity claim that
would be false for fs-escape specifically. **This is the correct call, not a gap**: claiming G1 parity
for a case one platform can't actually contain would be the kind of unreviewed overclaim this
project's own audit tracker exists to catch elsewhere.

**Verdict: G1 holds, for the five cases the shared corpus actually covers.** Fork-bomb parity is a
separate, real claim (both platforms contain it via `pids`/`pids.max`, per-platform tests, not
re-verified on Linux this pass) — not part of G1's own outcome-classification claim as scoped by this
corpus.

### G2 (containment) — Judged on Windows in full; Judged on Linux minus one disclosed case

`test_native_jail_abuse_corpus_windows.cpp`: re-run this pass, passes; covers fork-bomb, OOM, infinite
loop, unbounded output, **and fs-escape** (ADR-041 already cites this file's Case 4 —
`ESCAPE_DENIED` for an arbitrary secret vs. `ESCAPE_OK`, documented, for `win.ini`), each with its own
positive control (limits disabled, demonstrably fails) — the gate's own non-vacuousness requirement.

`test_native_jail_abuse_corpus_linux.cpp`: read in full for this ADR. Its own header comment states
plainly: **"fs-escape attempt is DELIBERATELY NOT covered here: `LinuxNativeJailBackend`'s
`CLONE_NEWNS` gives the guest a private mount namespace but nothing populates it with a restricted
view... the guest can read/write anything the invoking user can, anywhere on the host."** This is not
an untested-but-working case — it is a genuinely uncontained one, named as a real, tracked gap
(GitHub issue #5) in the test file's own comment, not discovered by this ADR. Fork-bomb, OOM,
infinite-loop, and unbounded-output cases are covered with their own positive controls on Linux, per
direct reading of the file — not re-run this pass (no Linux environment).

**Verdict: G2 holds on Windows for the full §7 abuse-case set. On Linux, G2 holds for four of five
cases; fs-escape containment does not exist, disclosed honestly at the point it was scoped out, not
discovered here.** This is the single root cause behind this same finding recurring in G3 below.

### G3 (no ambient authority) — Judged on Windows in full; a real, disclosed gap on Linux

`test_native_jail_ambient_authority_windows.cpp`: re-run this pass, passes — env, network, and
process-enumeration probes each find exactly the granted set, each proven non-vacuous by a real
canary value planted, confirmed reachable unsandboxed (positive control), confirmed unreachable
sandboxed.

`test_native_jail_ambient_authority_linux.cpp`: read in full for this ADR. Its own header states:
**"Filesystem is DELIBERATELY NOT covered here, for the same already-tracked reason C3's Linux corpus
skips the fs-escape case... Process enumeration below turns out to be the SAME root cause (no `/proc`
remount inside the new pid namespace either) — both are one tracked gap (GitHub issue #5, scope
widened by this task), not two."** So on Linux, G3 is unproven for two of its four named axes
(filesystem, process) — env and network are covered (not re-run this pass).

**Verdict: G3 holds on Windows for all four axes. On Linux, G3 holds for env/network only; filesystem
and process-visibility containment do not exist, tied to the identical root cause as G2's Linux
fs-escape gap** — one real defect (no mount-namespace population / no `/proc` remount), not two
independent ones, and already the explicit subject of `docs/planning/linux-native-jail-pivot-root-
containment-design-draft.md`. This ADR does not re-design that fix; it confirms the gap is real,
current (not stale — read directly against today's code, matching that design draft's own §0
re-grounding), and correctly scoped as one root cause across both gates.

### G4 (teardown) — Judged, at the bounded scope 008 §9 G4's own text already accepts as a M2
substitute

`test_native_jail_teardown_cycles_windows.cpp`: re-run this pass, passes. 300 create/exec/destroy
cycles (not the RFC's literal 10⁵ — CLAUDE.md Machine Safety, the same posture M1 used for its own
10⁴-session gate), censusing four real resources (process handle count, private-bytes usage,
AppContainer DACL entry count, no orphaned hostile-child process by name), each validated
non-vacuous by a positive control run through the SAME measurement path that deliberately produces a
known leak first. 300 was chosen for its own sensitivity margin (a single-HANDLE-per-cycle leak would
overshoot the test's slack 15×), not merely for speed — read directly, not taken on the breakdown
doc's word. The file's own comment discloses the one real scope limit itself: **no ASan build is
configured in this project** (`CMakeLists.txt`/`tests/CMakeLists.txt` have no sanitizer flag anywhere),
so the census substitutes for ASan's own half of G4's "ASan + handle/pid census" text — an explicit,
carried-forward gap, not silently assumed covered.

`test_native_jail_teardown_cycles_linux.cpp`: exists, not re-run this pass (no Linux environment);
reported here as unverified-this-pass rather than claimed equivalent without having read its own
disclosed scope in the same depth as the Windows file.

**Verdict: G4 holds on Windows at the bounded, disclosed scope 008 §9 G4's own text already
anticipates for this milestone. Linux side exists but is unverified by this ADR specifically** (a
narrower claim than "Judged," stated as such).

### G5 (cold start) — Correctly out of scope, not a gap

008 §9 G5 requires measuring cold-start p50/p99 against the 023 budgets. `023-Performance-Targets-
and-Budgets.md` stays `TBD-baselined` until M8 per this project's own roadmap — there is nothing to
measure against yet. M2 Phase C's own tracker names this deferral explicitly. **Not evaluated here as
a gap**, carried forward accurately rather than re-litigated.

### G6 (downgrade visibility) — Open, deliberately deferred, still unclosed

"An unavailable profile with no fallback fails startup; with a fallback, the downgrade appears in
diagnostics, trace, and metrics. Proven by a negative test." No test matching this description exists
anywhere under `tests/` for `native-jail` (checked directly — no file, no test name, no comment
referencing G6 or "downgrade" in the native-jail test set). M2 Phase C's own tracker names this
deferral explicitly ("What's explicitly deferred past this phase... G6 (downgrade visibility)") — so
this is a known, disclosed gap, not a silent one, but it remains genuinely open: no negative test, no
diagnostics/trace/metrics wiring for this specific claim has been verified either way.

**Verdict: G6 does not hold. Correctly, honestly disclosed as deferred at the time M2 Phase C was
scoped — but still open today, with no follow-on work tracked to close it.**

### G7 (session boundary) — Open, and NOT among the gates M2 Phase C's own tracker named as deferred

"A guest instance is never reused across sessions or principals; a canary written in session A is
unreachable from session B on every profile, including under pooling and snapshot-restore. Positive
control: a deliberately shared pool is caught." Checked directly: no test file, test name, or comment
anywhere under `tests/` references G7 or a native-jail session-boundary/pooling canary. **Unlike G5
and G6, this gate is absent from M2 Phase C's own "what's explicitly deferred" list** — it was not
weighed and consciously deferred; it appears to have been missed entirely when that tracker's scope
was written, a distinct kind of gap from G5/G6's honest deferrals.

**What the code itself suggests, stated as an argument, not a proven claim**: reading `NativeJailBackend::create()`/`create_python_worker()` (`native_jail_backend.cpp`) directly, every call
mints a brand-new `Instance` (fresh Job Object, fresh per-call AppContainer grants for `create()`;
fresh worker process for `create_python_worker()`) with no pooling or instance-reuse code path
anywhere in either method — `instances_.emplace(id, ...)` against a monotonically increasing counter,
every time. If true, G7's own concern (a guest instance surviving into a second session) may be
structurally impossible today simply because nothing pools instances yet — the same "sound by
code-level construction, not proven by test" shape `decisions/ADR-079-...md` disclosed as INCONCLUSIVE
rather than CORRECT for its own `SpawnPump` concurrency claim. **This ADR makes the same call**: this
is a plausible argument, not a verdict — CLAUDE.md's own rule that "a test that cannot fail proves
nothing" cuts the other way here too: an argument that cannot be run proves nothing either.

**Verdict: G7 does not hold, as a proven claim. INCONCLUSIVE by code-level construction (no pooling
exists to reuse an instance across), not CORRECT — no positive control, no canary test, ever run.**
Recommend this be tracked explicitly (unlike G6, it currently isn't tracked anywhere), given the day
this gate would start mattering is exactly the day someone adds instance pooling for a performance
reason and forgets this property was never actually gated on a test.

### G8 (snapshot fidelity) — Not applicable to `native-jail`, by the RFC's own text

008 §9 G8 is scoped to `wasm` explicitly: "Does not apply to `native-jail`'s interpreter/shell state,
which §6a states plainly does not survive passivation." **Not a gap** — carried forward accurately.

## 5. The decision

**This ADR formalizes, retroactively, what M2 Phase C already built and tested**, closing the process
gap CLAUDE.md's own rule identifies (no prior ADR existed for this isolation boundary as a whole).
It binds no new code — no implementation changed as part of this pass.

| Gate | Verdict |
|---|---|
| G1 (parity) | **Judged**, correctly scoped to the 5-case shared-corpus claim; fork-bomb/fs-escape parity is a separate claim (see G2). |
| G2 (containment) | **Judged on Windows** (full §7 corpus). **Judged on Linux minus fs-escape** (real, disclosed, tracked gap). |
| G3 (no ambient authority) | **Judged on Windows** (all 4 axes). **Judged on Linux minus filesystem/process** (same root cause as G2's Linux gap). |
| G4 (teardown) | **Judged on Windows** at the bounded, disclosed 300-cycle/no-ASan scope. **Unverified this pass on Linux** (file exists, not re-run). |
| G5 (cold start) | Correctly out of scope pending 023's M8 baseline — not evaluated. |
| G6 (downgrade visibility) | **Open.** Deliberately, honestly deferred by M2 Phase C; still unclosed, no follow-on tracked. |
| G7 (session boundary) | **Open, INCONCLUSIVE.** Not even named as deferred by M2 Phase C — a tracking gap, not just an implementation one. Plausible by code-level construction (no pooling exists), never proven by a test. |
| G8 (snapshot fidelity) | N/A to `native-jail` by 008's own text — not a gap. |

**The roadmap's own exit-criterion language — "`native-jail` sandbox parity (008 §9 G1) holds on
Windows and Linux" — is accurate as literally written** (G1 specifically, scoped to its own shared
corpus, does hold on both platforms) **but should not be read as "the native-jail gate is fully
closed."** G2/G3's shared Linux root cause, G6, and G7 remain real, open items this ADR surfaces
precisely so the next reader doesn't have to re-derive them from six separate test-file header
comments.

**Residual risks and follow-on work, named rather than left implied:**
- **The single highest-leverage fix available**: closing `docs/planning/linux-native-jail-pivot-root-
  containment-design-draft.md` (mount-namespace population + `/proc` remount) closes G2's Linux
  fs-escape gap and G3's Linux filesystem/process gap simultaneously — one fix, two gates, already
  designed (self-red-teamed, not built) in that document.
- **G6 needs real work**: a negative test (unavailable profile, no fallback, startup fails) and,
  separately, diagnostics/trace/metrics wiring for the fallback case — neither exists today.
- **G7 needs a real positive control**: a canary-write-in-session-A / assert-unreachable-in-session-B
  test, run today (it would almost certainly pass, per the code-level argument in §4) — turning an
  argument into a fact before anyone adds pooling and quietly invalidates the argument's own premise.
- **G4's Linux teardown claim is unverified by this ADR specifically** — re-run it directly the next
  time a Linux environment is available, rather than carrying this ADR's Windows-only re-verification
  forward as if it covered both platforms.
- **This ADR itself inherits every residual its cited ADRs already disclosed** (ADR-004's `cpu_ms`
  best-effort finding, ADR-041's accepted ACE-leak residual, ADR-081's Slice 2/Linux-parity
  residuals) — not repeated in full here, cited by reference rather than re-litigated.
