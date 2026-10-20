# ADR-144 — Decomposing `native_jail_backend.cpp` and `kata_backend.cpp`: extract the genuinely separable, leave the staged-construction core alone

- **Status:** Proposed — implemented, verified for real on both platforms (Windows/MSVC with real
  AppContainer/Job Object execution; Linux/GCC-14.2.0 in the established `/root/ae-verify` WSL2
  checkout), full rebuild (zero errors, zero new warnings) and full `ctest` clean on both platforms
  (same pre-existing-failure baseline as before this change), `naming_lint.py` clean.
  **Independent red-team round on each backend, same day: clean bill of health on both, one real
  adversarial mutation test performed and reverted, two cosmetic issues found and fixed on the
  native_jail side.**
- **Date:** 2026-08-31.
- **Scope:** `src/backends/native_jail/native_jail_backend.cpp`,
  `src/backends/native_jail/native_jail_handle_relay.cpp` (new),
  `src/backends/kata/kata_backend.cpp`, `src/backends/kata/kata_backend_detail.hpp` (new),
  `CMakeLists.txt`.
- **Related specs:** `docs/planning/agent-session-decomposition-design-draft.md` (the sibling
  decomposition analysis for `rt::AgentSession`, whose method — full-body read, explicit data-
  coupling trace, extract only what's genuinely disjoint — this ADR reuses directly, having learned
  from that draft's own finding that method-signature grouping alone gives false positives for
  "separable").

## 1. The question

After a broader code-quality survey found `agentengine::rt::AgentSession` already decomposed as far
as a prior, three-round red-teamed design draft judged safe, the next two largest files in the tree
were `src/backends/native_jail/native_jail_backend.cpp` (1550 lines, `NativeJailBackend`, the
Windows AppContainer + jailed-Python-worker sandbox backend) and `src/backends/kata/kata_backend.cpp`
(1459 lines, `KataBackend`, the Kata Containers VM-isolated sandbox backend). Neither had a prior
decomposition design draft. Could either be meaningfully split, and — the explicit lesson carried
over from the `AgentSession` pass — which parts, if any, would only *look* separable by method name
without actually being separable by data flow?

## 2. Findings

Both files turned out to have the same shape as `AgentSession`: a handful of genuinely
self-contained utility clusters (free functions or static-and-effectively-free-function methods, no
real coupling to instance state) sitting alongside one tightly sequenced, staged-construction core
bound together by a rollback closure and/or state handed off across multiple entry points — not
safely splittable without a much larger, separate design effort.

**`native_jail_backend.cpp`**: a sibling header, `native_jail_win32_helpers.hpp`, already existed
(used by `python_worker_mediation.cpp`/`python_worker_main.cpp`) and its own comment said it was
"lifted out of `native_jail_backend.cpp`'s anonymous namespace" in the first place — but
`native_jail_backend.cpp` never actually switched to it, and its own local, duplicate
`widen()`/`narrow()`/`HandleGuard` had diverged: the shared header's `HandleGuard` explicitly
deletes copy ctor/assignment and defines proper move semantics; the local copy declared neither,
making it *implicitly copyable* — a real latent double-close risk (two copies of a `HandleGuard`
could each `CloseHandle()` the same handle), not merely cosmetic duplication. Separately, the ten...
(corrected during red-team to *nine*) `dispatch_open`/`dispatch_listdir`/`dispatch_file_read`/
`dispatch_file_write`/`dispatch_file_close`/`dispatch_connect_authorize`/`dispatch_connect_send`/
`dispatch_connect_recv`/`dispatch_connect_close` methods turned out to already be PRIVATE STATIC
member functions of `NativeJailBackend` (declared in `native_jail_backend.hpp`, taking
`PythonWorkerState&` — a private nested type only an actual member has access to) — meaning their
out-of-line *definitions* could move to a second translation unit of the same CMake target with zero
interface change at all, the ordinary, safe C++ practice of splitting one class's method bodies
across multiple `.cpp` files. Everything else (`create()`/`exec()`/`destroy()`, worker
spawn/handshake, the watchdog thread, `exec_session()`'s protocol driver, `dispatch_worker_query()`'s
own routing) mutates `PythonWorkerState`'s `phase`/`phase_deadline`/`kill_reason`/`alive` fields from
three separate call sites each — the exact "looks separable by name, isn't by data-flow" shape
`AgentSession`'s own `run_rounds()` had.

**`kata_backend.cpp`**: `run_ctr()` (the generic host-subprocess spawn/poll-drain/timeout/kill
primitive), `fresh_id()`/`ipv4_to_dotted()`/`parse_allowlist_entry()` (small string/id helpers), and
`build_oci_spec_json()`/`OciSpecInputs` (the OCI runtime-spec JSON builder) are genuinely free
functions touching no `KataBackend` member state at all — ~470 of 1459 lines. `create()`'s own
staged-construction pipeline (mount-path validation, disk-quota/overlay staging, mount-point
pre-creation with the ADR-140 symlink-escape guard, netns/CNI/nftables wiring, final `ctr run
--config` + `Instance` registration) is bound together by `cleanup_partial()`'s single rollback
closure — a closure over ~10-13 stack locals from `create()`'s own frame, called from ~15 early-return
sites spread across every one of those stages — and by `Instance`'s own state handoff to `destroy()`.
Not a cluster with a name that happens to look separable: the rollback-plus-handoff *is* the design.
Not attempted.

## 3. What was built

**`native_jail_backend.cpp`**: removed the local, diverged `widen()`/`narrow()`/`HandleGuard` and
switched to `#include "backends/native_jail/native_jail_win32_helpers.hpp"` — since
`native_jail_backend.cpp` is directly inside `namespace agentengine::native_jail {`, the same
namespace the shared header's symbols live in, no call sites needed qualification changes. Moved the
nine `dispatch_*` methods' out-of-line definitions, their shared anonymous-namespace helpers
(`deny()`, `parse_open_mode()`/`ParsedOpenMode`, the relay-size constants, the
`g_test_connect_resolver_override` test seam), and `set_test_connect_resolver_override()` verbatim
into a new `src/backends/native_jail/native_jail_handle_relay.cpp`, registered as a second source
file of the existing `agentengine_native_jail_backend` CMake target. Declarations in
`native_jail_backend.hpp` are completely unchanged.

**`kata_backend.cpp`**: moved `run_ctr()`/`ProcessOutcome`/`kOutputSafetyCapBytes`/
`kProcessTimeoutSeconds`/`fresh_id()`/`ipv4_to_dotted()`/`parse_allowlist_entry()`/`OciSpecInputs`/
`build_oci_spec_json()` verbatim into a new header-only `src/backends/kata/kata_backend_detail.hpp`
(wrapped in the same anonymous-namespace shape the original had, since exactly one translation unit
includes it), included by `kata_backend.cpp`. `cni_env()`, `kKataWorkdirRoot`,
`targets_own_workdir()`, and `trim_trailing_newline()` stayed — the first three are coupled into the
netns/CNI cluster and the mount-precreation guard the survey found non-separable; the fourth sits
just outside the plan's own approved line-range boundary and was left in place rather than expanding
scope mid-implementation.

## 4. Verification

**Windows/MSVC** (native_jail): full rebuild, zero errors. All 8 native_jail test suites run directly
against real Windows AppContainer/Job Object execution — `test_native_jail_backend_windows`,
`test_native_jail_abuse_corpus_windows`, `test_native_jail_ambient_authority_windows`,
`test_native_jail_parity_windows`, `test_native_jail_session_boundary_windows`,
`test_native_jail_teardown_cycles_windows`, `test_native_jail_python_worker_slice1`,
`test_native_jail_python_worker_handle_relay` — all `ALL PASS`, exit 0. Full `ctest`: 293 total, 1
pre-existing unrelated failure, zero regression. `naming_lint.py` clean.

**Linux/GCC-14.2.0** (kata, `if(NOT WIN32)` gated — cannot build on Windows at all): synced to the
established `/root/ae-verify` WSL2 checkout, `AGENTENGINE_BUILD_KATA_BACKEND=ON` toggled on
(ADR-120's own toggle-and-revert pattern, applied to a different flag). `agentengine_kata_backend`
built 4/4 steps, zero errors, zero warnings, on the first attempt. `AGENTENGINE_KATA_SANDBOX_TESTS=ON`
toggled on too: all four `test_kata_backend_*_linux.cpp` binaries built and linked clean against the
refactored backend. Running them produces the SAME, already-disclosed, pre-existing environment
failure (`ctr images mount ... no such device`, ADR-110 §4's own named gap — this WSL2 distro has no
live Kata/containerd deployment) as before the refactor, not a new failure; subtests not needing a
live mount (net-policy capability gating, mount-exclusion checks, allowlist grammar validation) all
pass, positively exercising the relocated `parse_allowlist_entry()`. Both flags reverted to OFF,
rebuilt, full `ctest`: 189 total, the identical 6 pre-existing Docker-CLI-unreachable failures, zero
regression.

## 5. Independent red-team round (same day, this ADR's own final-review pass)

**`native_jail_backend.cpp`: clean bill of health, one genuine adversarial mutation test performed
and reverted, two cosmetic issues found and fixed.** Traced every `HandleGuard` use site by hand
(default-construct-then-assign, the explicit single-`HANDLE` constructor, `.release()`/`.close_now()`
— never a whole-object copy or move-assignment) and confirmed the shared header's copy-deleted/
move-enabled version is a strict behavioral tightening with zero call-site impact. Diffed the moved
`dispatch_*` bodies against the deleted original text and confirmed the two real, previously-shipped
invariants — the base64-decode-before-cap check in `dispatch_file_write`/`dispatch_connect_send`,
and the port-range-check-before-cast in `dispatch_connect_authorize` — survived verbatim, in the same
order. **Adversarially confirmed the disclosed test-coverage gap is real, not merely asserted**:
temporarily reordered the port-range check to run *after* the `static_cast` (reintroducing the
pre-fix UB), rebuilt, reran `test_native_jail_python_worker_handle_relay` — it still passed, since no
test anywhere in the repo exercises either invariant directly (grepped for
`kMaxRelayBase64Chars`/`port out of range`/`OverflowError` in `tests/`, zero hits). Reverted, rebuilt,
reconfirmed `ALL PASS`. **Fixed**: a stray UTF-8 BOM the implementer's own editing tool had introduced
at the top of `native_jail_backend.cpp` (confirmed absent from the pre-refactor `HEAD` version via
`git show`, so a real artifact of this change, not pre-existing) — removed. Corrected a cosmetic
miscount in this file's own new top comment ("the ten HandleRelay `dispatch_*`" — there are nine, not
ten).

**`kata_backend.cpp`: clean bill of health, no bug found.** Re-synced the WSL2 checkout to the exact
current Windows working-tree state (confirmed byte-identical via `cmp`, not just `diff`, after an
earlier process-substitution comparison gave a false "every line differs" result from an unrelated
BOM/CRLF handling quirk in that specific invocation). Confirmed the moved block in
`kata_backend_detail.hpp` is byte-for-byte identical to the deleted original, including the SLICE 5
`output_capped`-set-unconditionally logic and `parse_allowlist_entry()`'s exactly-two-colons
validation. Traced every symbol the moved code needs (`result<>`/`error`/`failure_class`/`MountSpec`
via `kata_backend.hpp`, `json::` via the header's own `json_value.hpp` include, `environ` via the
header's own `extern` declaration) and confirmed `kata_backend.cpp` itself has zero remaining direct
uses of anything that moved. Rebuilt for real in WSL2 independently of the implementer's own pass,
same clean result. One disclosed, non-bug residual named by this round rather than left implicit:
`kata_backend_detail.hpp`'s anonymous-namespace wrapping means a second `.cpp` including it would get
its own independent copy of everything (not an ODR violation, since anonymous-namespace symbols have
internal linkage — just silent duplication and possibly-divergent per-TU constants if one copy were
ever hand-edited independently) — not a live defect today, since exactly one translation unit
includes it, but worth reconsidering if a second consumer is ever added.

## 6. Not done

- No attempt to split `create()`/`exec()`/`destroy()`, worker spawn/handshake, the watchdog, or
  `exec_session()` in `native_jail_backend.cpp` — the survey's own data-flow trace found these
  mutate `PythonWorkerState`'s phase/deadline/alive fields from three separate call sites each, the
  same non-separable shape `run_rounds()` has in `agent_session.hpp`.
- No attempt to split `create()`'s staged-construction pipeline, `cleanup_partial()`, or `destroy()`
  in `kata_backend.cpp` — bound together by one rollback closure over ~10-13 locals called from ~15
  early-return sites, and by `Instance`'s own state handoff.
- No new unit-level test for the relay-handler dispatch functions — `PythonWorkerState` and the
  `dispatch_*` methods are all PRIVATE; adding coverage would need a `friend` declaration, judged a
  real encapsulation change beyond this refactor's own "zero behavior/interface change" scope, not
  bundled in as a side effect. Disclosed as a residual (§7), not silently dropped.
- No live Kata/containerd deployment available in this environment to exercise `create()`'s
  staged-construction pipeline (unchanged by this ADR either way) against a real attack — same,
  separate, already-disclosed environment gap named since ADR-109/110.

## 7. Residuals

- **Real, disclosed, adversarially-confirmed test-coverage gap, unchanged by this refactor**: neither
  the base64-decode-before-cap check nor the port-range-check-before-cast in the relocated
  `dispatch_*` functions has any dedicated regression test — confirmed by directly breaking one and
  observing the full existing suite still pass. A future edit could silently reorder or drop either
  check and nothing in CI would catch it. Pre-existing before this ADR, not introduced by it; closing
  it needs the `friend`-declaration design decision named in §6, not bundled here.
- `kata_backend_detail.hpp`'s anonymous-namespace-in-a-header shape, named in §5 — safe today
  (exactly one consumer), a real thing to reconsider only if a second `.cpp` ever includes it.
- Same pre-existing, disclosed gaps this ADR's own verification ran into but did not change: the
  Docker-CLI-unreachable-via-WSL2 gap (ADR-115 and every Linux-verification ADR since), and the
  `ctr images mount` environment defect (ADR-110 §4) blocking real functional exercise of
  `KataBackend::create()` in this specific WSL2 environment.
