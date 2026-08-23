# ADR-085 — Does `HandleRelay` give the jailed Python worker real, capability-mediated file and socket
# I/O, closing ADR-081 Slice 1's own named residual, without reopening I2/I3 or regressing the
# existing native-jail/mediated-Python-runner suite?

**Status:** Judged (design → red-team → prove complete, this session, 2026-08-23 — same day as
ADR-081, ADR-082, ADR-083, ADR-084). Real, new code, not a formalization of prior work — the full
design phase, two real implementation-time falsifications (each found by running the actual tests this
process requires, fixed, and disclosed rather than hidden), and an independent red-team pass (fresh
reviewer, no authorship context) that found and this same pass fixed one BLOCKING finding, one real
bug, and four MINOR issues, all happened in this session. Design phase, self-red-team, the falsified-
and-revised design, and the independent red-team's full findings text all live in
`docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md` (not deleted or condensed
away now that this ADR exists — CLAUDE.md's own `decisions/README.md` rule keeps a design draft's
reasoning, including what turned out wrong, as part of the permanent record); this ADR is the
synthesized decision record, citing that document rather than duplicating its ~500 lines verbatim.

**Relates to:** `decisions/ADR-081-jailed-python-worker-process-slice-1.md` (Judged — this ADR closes
that one's own §3-named residual, "Slice 2 — real file/listdir/socket relay... every such call is
denied today," verbatim), `decisions/ADR-004-appcontainer-native-jail-windows-backend.md` (Judged —
AC-S1's own measured evidence, "`socket.connect()` fails with WSAEACCES/WinError 10013 under zero
capabilities," is the reason this design cannot let the worker complete a real connect itself, and
turned out to be narrower evidence than this design's own red-team pass initially assumed it covered —
see Finding 1 below), `decisions/ADR-014-worktree-mount-path-canonicalization.md` (`open_within_mount_root`/
`list_within_mount_root`, reused unchanged, host-side, exactly as ADR-081 §3 anticipated),
`decisions/ADR-011-first-party-egress-proxy.md` (`sandbox::resolve_and_validate`/`is_blocked_address`,
reused for the new raw-connect SSRF check this design adds beyond the pre-worker-process design's own
baseline), `docs/planning/2026-08-22-component-role-audit-tracker.md` Findings Q/R (the original audit
this whole jailed-worker redesign traces back to).

## 1. The question

**Stated so it has a wrong answer:** ADR-081 §3 named Slice 2 explicitly out of scope: "open/listdir/
connect_* — Every such call is denied today (`not_implemented_this_slice`... a guest-visible
`PermissionError`/`OSError`), not silently permitted and not crashing." Six of the pre-existing
native-jail/mediated-Python-runner test files depended on real file or socket I/O and were left
disclosed-failing as a result (ADR-081 §4). The question this ADR answers: can `dispatch_worker_query`
give the worker real, capability-mediated `open`/`listdir`/socket-connect/send/recv/close behavior —
closing those six tests' own real regressions — using only primitives this codebase already has
(`open_within_mount_root`, `resolve_and_validate`, `agentengine::pal`'s socket wrappers), without (a)
ever letting the worker reach a file or host it wasn't capability-granted, (b) trusting anything the
worker claims about a `file_id`/`socket_id`/its own connection state, or (c) silently narrowing scope
beyond what is explicitly disclosed?

## 2. The design, and what changed between the plan and the shipped code

Full design and self-red-team: `docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md`
§0-§6. Summary:

- **`open`/`listdir`** (dispatch_open/dispatch_listdir, `native_jail_backend.cpp`): capability-checked
  (`cap::FsRead`/`cap::FsWrite`, the same lookup the pre-worker-process design used), quota-checked
  (live on-disk usage via `mount_root_usage`), size-cap-checked, then `open_within_mount_root`/
  `list_within_mount_root` (ADR-014, unchanged) run HOST-side.
- **File I/O** (`file_read`/`file_write`/`file_close`, three NEW wire kinds): **the design originally
  planned to `DuplicateHandle` the verified HANDLE into the worker process** so it could do ordinary,
  zero-round-trip I/O afterward. **This was FALSIFIED during implementation** (design draft §1a): the
  duplicate succeeds and is confirmed present/open in the worker (`GetHandleInformation`), but real I/O
  against it fails with `ERROR_INVALID_HANDLE` — a real, reproduced Windows AppContainer limitation on
  duplicated named-file-object handles without an explicit AppContainer-SID ACE, not a bug in the
  relay wiring. **Shipped design:** the host keeps the real `SafeFileHandle` open in its OWN process
  (`PythonWorkerState::open_files`, id-keyed, the same shape sockets already use) and relays
  `ReadFile`/`WriteFile` by `file_id`; the worker's `Internal_open` constructs a pure-Python
  `_AeRelayFile` object (`read`/`readline`/`__iter__`/`write`/`__enter__`/`__exit__`/`close`/`__del__`)
  instead of an `io.FileIO`. Two implementation bugs in `_AeRelayFile` were found and fixed by running
  the real tests this design's own prove phase required (design draft §1a): no `__del__` (a real
  `io.FileIO` closes deterministically on refcount-zero; `_AeRelayFile` needed the same), and
  `read(size)` originally ignored `size` entirely (broke `pandas.read_csv`'s own chunked C parser).
- **Socket relay** (`connect_authorize`/`connect_send`/`connect_recv`/`connect_close`, matching ADR-081
  Slice 1's own wire-protocol reservation exactly): the host performs the REAL `connect`/`send`/`recv`
  (the worker categorically cannot, per AC-S1), keyed by a host-minted `socket_id`
  (`PythonWorkerState::live_sockets`). `connect_authorize` adds `sandbox::resolve_and_validate`'s
  SSRF-protection check — NEW hardening beyond the pre-worker-process design's own baseline (that
  design only ever checked the `cap::NetOut` string match, never the resolved address class). Worker-side,
  the bootstrap installs pure-relay `connect`/`send`/`sendall`/`recv`/`close` on `socket.socket`; the
  socket_id<->socket correlation was originally planned as an ordinary instance attribute, which
  **also turned out to be false**: this vendored CPython's `socket.socket` instances carry no
  `__dict__` at all (`self._ae_socket_id = ...` raises `AttributeError`), found the same way as the
  DuplicateHandle finding — by actually running the new positive control this ADR's own prove phase
  required, not by review. Fixed with a `weakref.WeakKeyDictionary`.

Both falsifications are disclosed in full, not smoothed over, in the design draft's §1a and §2 addendum.

## 3. The independent red-team pass — findings and fixes

A fresh reviewer (separate agent invocation, no authorship context) read the design draft, ADR-081, and
the complete diff, and reported 6 findings. Full text of each finding and its fix:
`docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md` §7. Summary, per-claim
verdict:

| # | Severity | Claim under test | Verdict | Fix |
|---|---|---|---|---|
| 1 | **BLOCKING** | Every `socket.socket` method besides connect/send/sendall/recv/close is mediated | **WRONG, fixed** — `bind`/`listen`/`accept`/`connect_ex`/`sendto`/`recvfrom`/`makefile`/`dup`/`detach`/`share`/`shutdown` were the real, unmediated CPython implementation, zero capability check | Explicit denial (`_ae_socket_op_denied`), matching the existing `os.*` defense-in-depth posture. New positive control (SOCK-DENY, §4) proves it. |
| 2 | Real bug | `dispatch_connect_authorize`'s port cast is well-defined for any guest-supplied value | **WRONG, fixed** — `static_cast<uint16_t>` on an out-of-range `double` is UB, reachable via an ordinary guest typo | Port range-checked (0-65535) before the cast; out-of-range denied like any other policy violation. |
| 3 | MINOR (reasoning) | The outer `exec_session()` watchdog backstops a hung `dispatch_connect_authorize` call | **WRONG, corrected in place** — the watchdog can only kill the WORKER process; it cannot interrupt a HOST thread blocked in `select()`. The 5s inner timeout is the ONLY bound, not a "first layer" | Design draft §4 item 3 corrected with the wrong text struck through and kept visible, not deleted. |
| 4 | MINOR | `kMaxRelayChunkBytes` bounds the WORK done per relay call, not just the result | **WRONG, fixed** — `relay_base64::decode` ran before the clamp, so a single call could force decode/allocation of up to the wire frame's own 64 MiB ceiling | New `kMaxRelayBase64Chars` bound checked on the base64 TEXT before `decode` ever runs. |
| 5 | MINOR | `open_files`/`live_sockets` share one 16-entry ceiling | **WRONG comment, fixed** — each map is checked against its own `.size()`; a session can hold 16 of each (32 total), not 16 shared | Comment corrected to state only the constant, not a counter, is shared. |
| 6 | MINOR | `set_test_connect_resolver_override`'s "never used from production" is adequately enforced | **Real gap, fixed** — a process-wide static with only a bare set/reset pair in the test, not exception-safe | RAII guard (`TestResolverOverrideGuard`) in the test; header comment states the enforcement gap explicitly. |

Also checked by the reviewer and found sound (not a finding): `open_files`/`live_sockets` thread-safety
(traced `dispatch_worker_query` to confirm single-threaded-per-worker execution, serialized by
`call_mutex`), both falsified-and-fixed findings from §2 match the shipped code with no half-migrated
fallback, and capability checks run strictly before any I/O on every traced path including the quota
branch.

## 4. Executed evidence (this ADR's own prove phase)

**Build.** Full solution rebuild (`cmake --build . --config Debug`, MSVC/Visual Studio 18), zero
errors — both before and after the red-team fixes.

**The 6 tests ADR-081 §4 disclosed as failing**, re-run directly: 5 of 6 now fully pass
(`test_mediated_python_runner_smoke`, `_agent_files_data`, `_skill_mounts`, `_error_mapping`,
`_hostile_corpus`). `test_reference_agent_task_corpus` passes its two Slice-2-scoped checks (csv_sum,
list_dir) but its third (pandas_groupby) still fails — **confirmed via direct `git stash` bisection
against the unmodified pre-Slice-2 baseline to be a pre-existing, unrelated gap**: `import pandas`
itself fails with `PermissionError: [Errno 13]` reading `vendored_python_packages\pandas\__init__.py`,
reproduced byte-for-byte identically before this ADR's own diff ever existed — an ACL-grant gap in
`create_python_worker()`'s `extra_sys_path` mechanism, a different subsystem than this ADR's own
open/listdir/socket relay. Named as an explicit residual (§5), not silently left unexplained.

**New positive/negative controls, built and run this pass**
(`tests/test_native_jail_python_worker_handle_relay.cpp`, new):
- SOCK-1: a real `connect_authorize`+`connect_send`+`connect_recv`+`connect_close` round trip against
  a real local TCP loopback echo server, under a real granted `cap::NetOut` — **PASSES**. Required a
  new test-only resolver-override seam (`set_test_connect_resolver_override`, RAII-guarded per Finding
  6) because `resolve_and_validate`'s own SSRF protection categorically blocks every hermetic
  same-machine target, by design — the real constraint a production deployment faces too.
- FILE-RO-1: `_ae_internal.file_write` called directly on a read-opened `file_id`, bypassing
  `_AeRelayFile`'s own Python-level check — **PASSES**, denied by the real Win32 kernel
  (`ERROR_ACCESS_DENIED` via `WriteFile` against a `GENERIC_READ`-only handle), proving the access-mask
  claim is enforced by the OS, not merely asserted in a comment.
- SOCK-DENY (added during the red-team fix pass, Finding 1): `sendto`/`bind`/`listen`/`accept`/
  `makefile` are all denied outright — **PASSES**.

**Zero regression, both before and after the red-team fixes.** The other 10 native-jail tests ADR-081
§4 names: all PASS, both passes. Full project suite (`ctest -C Debug`, 265 tests including the new
file): **264/265 pass**, both before and after the fixes — the sole failure is the pandas-import gap
above, unchanged by this ADR's own diff.

## 5. Named residuals (not eliminated, disclosed)

- **The pandas-import ACL gap** (§4) — a real, pre-existing, out-of-scope gap in
  `create_python_worker()`'s `extra_sys_path`/`grant_ro` mechanism, orthogonal to this ADR's own
  open/listdir/socket relay. Not fixed here.
- **`ResourceLimits::net_bytes` reconciliation** with `cap::NetOut::byte_cap` for the raw-socket relay
  path — the tool-bridged path already has `narrow_by_resource_limit`; this design's own
  `kMaxRelayChunkBytes` is a flat constant, not grant-derived (design draft §5).
- **No UDP relay, no IPv6** — `sendto`/`recvfrom` are denied outright (Finding 1's fix), not relayed;
  `VerifiedEndpoint`/`pal::tcp_connect` are IPv4-only, matching `net_egress_proxy.hpp`'s own documented
  scope.
- **Non-ASCII UTF-8 text read via a size-bounded `read(size)` call** can see a `UnicodeDecodeError` at
  a chunk boundary a real `io.TextIOWrapper` would not — not exercised by any current fixture (all
  ASCII), named rather than silently accepted (design draft §1a/§5).
- **`execute_shell`'s own process-spawn model** (tracker Finding O) remains unreconciled with this
  design, unchanged from ADR-081's own identical disclosure.
- **Linux parity** — still the separate, still-unbuilt `docs/planning/linux-jailed-python-worker-design-draft.md`.
- **`set_test_connect_resolver_override`'s enforcement is convention-only** (Finding 6) — RAII-guarded
  in its one real caller, but nothing at the type-system or build-flag level prevents a future
  production call site from setting it. Named, not hidden.

## 6. Verdict

Per-claim: the core design (host-mediated, capability-checked, quota-checked file and socket relay,
never trusting worker-claimed state, never letting the worker exceed its granted access) is **CORRECT**,
demonstrated by the executed evidence in §4, not merely argued. Two claims from the original design plan
were **WRONG** and are recorded as such, not silently corrected away: `DuplicateHandle`-based file
relay does not work under this AppContainer configuration (§2); `socket.socket` instance attributes do
not work in this vendored CPython (§2). One claim from the design's own self-red-team was **WRONG**
(Finding 3, §3) and is corrected in place. The independent red-team pass's one BLOCKING finding
(Finding 1, unmediated socket methods) is fixed and proven with a new positive control. Judged.
