# ADR-089 — KataBackend: close the output-cap guest-process-orphan gap ADR-088 disclosed

Status: Proposed (design → independent red-team → fix → implement → local verification complete,
2026-08-24; awaiting project-owner Judged sign-off)

## 1. The question

`decisions/ADR-088-kata-backend-abuse-corpus.md` §4 fixed a real gap where a `wall_ms` timeout only
killed the host-side `ctr` CLI wrapper process, not the guest-side process it was attached to — and
that same section explicitly disclosed, but deliberately did **not** fix, a structurally identical
risk on the `output_bytes` cap path: when a stream hits its cap, `run_ctr()` force-closes that
stream's pipe and stops reading it, then falls through to an **unconditional blocking**
`waitpid(pid, &status, 0)` on the host-side process — no guest-side kill, no bound on that wait at
all. This ADR closes it.

## 2. What was attempted first, and what the red-team pass found wrong with it

A design was drafted before any code was written: fold the output-cap case into the same
SIGKILL-and-bounded-reap path the timeout case already uses (`force_terminate = timed_out ||
output_truncated`), and extend `exec()`'s existing guest-side `ctr tasks kill --exec-id` call to fire
on that same condition, leaving classification (`ok`/`crash` from `exit_code`) unchanged.

An independent adversarial red-team pass against this draft, run before any code was written, found
it unsound in two blocking ways:

1. **The proposed trigger signal (`output_truncated`) is not exhaustive.** `output_truncated` was
   only ever set when a `read()` call was *partially* dropped (`take < n`) — a read landing exactly
   on the cap boundary (`take == n`) still force-closes the stream on the next line, but drops no
   bytes on that particular read, so `output_truncated` stays `false`. This is not a rare corner
   case: it happens whenever the configured cap size lines up with normal read-chunk/pipe-buffering
   boundaries — plausible for round numbers like 4096 or 65536 against this function's own
   4096-byte read buffer. Under the original draft, hitting the cap that way would silently fall
   through to the exact unconditional blocking wait this fix exists to remove — the fix could no-op
   on realistic inputs.
2. **Leaving classification "unchanged" is not just cosmetic, it can misreport success.** The
   courtesy non-blocking `waitpid()` check the draft added (to prefer a real exit code over
   discarding it, when the host-side process happened to already exit) can reap `ctr` with
   `exit_code == 0` even though output was truncated and the guest process was potentially just
   killed. The existing `exit_code == 0 ? ok : crash` classification (unchanged in the draft) does
   not consult the truncation signal at all — a run whose output was capped and whose guest process
   may have been force-killed could report `ok`, implying the caller received the complete output
   when it did not.

A third, non-blocking finding on the same pass: `crash` (the fallback when the courtesy check
misses) is itself a weaker classification than warranted for a deliberate host policy action —
this project already has an idiom for exactly that (§4 below), and using it instead is a real
correctness improvement, not merely cosmetic. A fourth finding attacked the accompanying test-plan
draft directly (§5 below).

## 3. The fix

**A dedicated `output_capped` flag**, distinct from the pre-existing `output_truncated`, set
unconditionally at the force-close site in `run_ctr()`'s read loop — not inferred from whether the
triggering read happened to be a partial one:

```cpp
if (stream_it->sink->size() >= output_cap_bytes) {
    if (take < static_cast<std::size_t>(n)) outcome.output_truncated = true;
    outcome.output_capped = true;   // reliable "this stream was force-closed at its cap" signal
    close(fd);
    stream_it->open = false;
}
```

`run_ctr()`'s tail now treats `output_capped` the same as `timed_out`:

```cpp
bool const force_terminate = outcome.timed_out || outcome.output_capped;
if (force_terminate) {
    if (waitpid(pid, &status, WNOHANG) == pid) {          // courtesy, non-blocking, never waits
        outcome.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return outcome;
    }
    kill(pid, SIGKILL);
    for (int i = 0; i < 50; ++i) { ... }                   // unchanged bounded reap
    outcome.exit_code = -1;
    return outcome;
}
```

`KataBackend::exec()` extends the SLICE-4 guest-side kill trigger from `outcome->timed_out` alone to
`outcome->timed_out || outcome->output_capped`, issuing the identical
`ctr tasks kill --exec-id <exec_id> --signal SIGKILL <container>` call for either reason (the log
line names which one fired). Classification is now a real three-way branch instead of the
exit-code-only check:

```cpp
if (outcome->timed_out) {
    result_out.klass = exec_outcome_class::timeout;
} else if (outcome->output_capped) {
    result_out.klass = exec_outcome_class::policy_violation;
} else {
    result_out.klass = outcome->exit_code == 0 ? exec_outcome_class::ok : exec_outcome_class::crash;
}
```

`policy_violation` was chosen over `crash` to match this codebase's own established idiom for "the
host stopped this on policy grounds, not the workload's own fault" — the same class
`LinuxNativeJailBackend`'s idle-phase CPU-budget kill (`native_jail_backend.cpp:1440`) and
`MediatedPythonRunner`'s capability denials (`python_runner.hpp:74`) already use, rather than
`crash`, which risks a caller reading "the workload itself failed" and retrying unmodified — burning
another full cap's worth of guest resources for the same predictable result.

Like ADR-088's own fix, the exact `ctr` CLI surface this depends on (`ctr tasks kill --exec-id ...`)
is **not independently re-verified against a live Kata deployment this session** (none reachable) —
a wrong assumption fails into the existing stderr log line, not silently.

## 4. The abuse-corpus test (`tests/test_kata_backend_abuse_corpus_linux.cpp`, Case 2 extended)

The red-team pass's fourth finding attacked the initial test-plan draft directly: extending Case 2
with a heartbeat-file proof (mirroring Case 1's own proof of the timeout fix) is sound in shape, but
the original plan reused `contained_spec` unmodified — which had no bind mount for the heartbeat
file at all, and a `wall_ms` short enough that, because `run_ctr()`'s poll loop only exits when
**both** streams close (stdout capping alone does not end the loop while stderr's read fd stays
open, held by the still-alive `ctr` process), the case would very likely just run until the
`wall_ms` deadline regardless — landing in the already-proven `timeout` branch instead of exercising
the new `output_capped`-alone path at all, contingent on unverifiable `ctr` behavior this repo does
not control.

Fixed by:

- Adding the same `/work` bind mount Case 1 already uses.
- Setting a materially larger `wall_ms` (2500ms) so a fast `policy_violation` return (if `ctr` does
  die quickly from writing into the closed pipe) is not ambiguous with simply having hit the same
  deadline the timeout path would produce anyway.
- **Not asserting a single expected `klass`.** Whichever of `policy_violation` (fast host-side
  death) or `timeout` (wall_ms deadline reached instead, both streams force-closed together) a real
  deployment actually produces is not something this session can determine without one — but SLICE 5
  makes both paths trigger the identical guest-side kill, so the heartbeat-stopped-changing proof is
  valid under either outcome. The test asserts `klass` is one of the two (never `ok`/`crash`) and
  performs the heartbeat check regardless of which one fired.
- Heartbeat write ordering: the workload writes its heartbeat file **before** its flooding stdout
  write each loop iteration, so a wedged/blocked flood write (if the guest-visible effect of the
  host closing its read pipe is backpressure rather than an outright error) cannot make "heartbeat
  stopped changing" ambiguous between "process killed" and "process merely stuck on one write."

## 5. Residuals, carried forward explicitly

- The `ctr tasks kill --exec-id` flag syntax remains unverified against a live deployment (unchanged
  from ADR-088, not a new risk this ADR introduces).
- `output_truncated`'s original, narrower meaning ("bytes were actually dropped on this read") is
  preserved unchanged for any future reader — `output_capped` is a new, separate field, not a
  repurposing of the old one.
- No real Kata/containerd deployment was reachable this session — the extended test is
  compile-verified only, not executed, same disclosed limitation every Kata test in this tree
  already carries.
- `exec_outcome_class::oom`, `ResourceLimits::pids`/`fds`/`disk_bytes`/`net_bytes`, and
  `ExecRequest::source` Runner-mediation remain the same unchanged gaps ADR-088/`kata_backend.hpp`
  already name — untouched by this ADR.

## 6. Verification performed this session

- **Design → independent red-team → fix, before implementation** — an adversarial subagent review
  read the actual current `kata_backend.cpp`/`.hpp`, `sandbox.hpp`'s `exec_outcome_class` enum, every
  other backend's own `policy_violation`/`crash` usage, and the real (not summarized) test file
  before critiquing. Findings: 2 BLOCKING against the code design (the `output_truncated` signal
  gap; the unchanged-classification gap), 1 BLOCKING against the test plan (the wall_ms/mount
  omission likely exercising the wrong code path), 1 correctness improvement adopted
  (`policy_violation` over `crash`, backed by a real project precedent), 2 minor findings folded into
  the fix (heartbeat write ordering; the missing bind mount), 2 explicitly verified NOT problems (no
  regression to the already-shipped timeout path from sharing the `force_terminate` branch; the
  guest-side kill call is safe/idempotent to fire on the new trigger condition).
- **Compile verification**: the same WSL Ubuntu `build-linux` CMake cache used for ADR-088 (real
  g++/cmake/ninja against this exact checkout) rebuilt `agentengine_kata_backend` and all three Kata
  test binaries (`test_kata_backend_linux`, `test_kata_backend_slice2_linux`,
  `test_kata_backend_abuse_corpus_linux`) clean, with no new warnings or errors.
- **Full local Windows `ctest` regression run**: confirms zero regressions in every test unaffected
  by this change (this backend is Linux-only and gated out of the Windows build entirely, so this
  run's purpose is confirming the rest of the tree is untouched, not exercising the new code).
