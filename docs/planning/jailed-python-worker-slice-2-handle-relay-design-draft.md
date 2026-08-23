# Design draft: ADR-081 Slice 2 — real file/listdir/socket relay for the jailed Python worker
# (`HandleRelay`)

**Status:** Design draft, self-red-teamed once, then IMPLEMENTED and PROVEN in this same session —
unlike the sibling Linux-parity draft (`docs/planning/linux-jailed-python-worker-design-draft.md`),
this machine IS the Windows build/test environment ADR-081 itself was proven on, so this document is
not a paper-only artifact: it is the design phase of what becomes `decisions/ADR-085-...md`, pending an
independent red-team pass. **§1's own original design (`DuplicateHandle`-based file relay) was
FALSIFIED during implementation** — real I/O against a duplicated file handle inside the AppContainer'd
worker fails with `ERROR_INVALID_HANDLE`, reproducibly, even though the duplicate is confirmed present
and open (`GetHandleInformation` succeeds). §1 below is left in its ORIGINAL, falsified form
(struck through in spirit, not in fact — CLAUDE.md's own discipline: the reasoning that was wrong is
part of the record) with a correction notice; §1a is the revised, shipped design.

**Relates to:** `decisions/ADR-081-jailed-python-worker-process-slice-1.md` §3 ("Explicitly, narrowly
out of scope for this pass... Slice 2 — real file/listdir/socket relay (`HandleRelay` for the
`open`/`listdir`/`connect_authorize`/`connect_send`/`connect_recv`/`connect_close` `worker_query`
kinds). Every such call is denied today"), `decisions/ADR-004-appcontainer-native-jail-windows-backend.md`
§5.3 (AC-S1: "with zero granted capabilities, guest code cannot open an outbound socket" — measured as
`WinError 10013` on `connect()`, socket() itself succeeds), `decisions/ADR-014-worktree-mount-path-canonicalization.md`
(`open_within_mount_root`/`list_within_mount_root`, reused unchanged), `decisions/ADR-011-first-party-egress-proxy.md`
(`sandbox::resolve_and_validate`/`is_blocked_address`, reused for the new raw-connect SSRF check this
design adds), `src/backends/native_jail/mediated_python_worker_protocol.hpp` (the wire catalog this
design fills in, unchanged shape), `src/backends/native_jail/python_worker_mediation.cpp`'s
`Internal_open`/`Internal_listdir`/`Internal_do_connect` (the worker-side stubs this design completes).

## 0. What changed since the code this design ports

The pre-Slice-1 in-process design (`git show a60fc3d^:.../mediated_python_runner.cpp`) ran the whole
CPython interpreter inside the HOST process. `Internal_open` called `open_within_mount_root` directly
and wrapped the resulting real `HANDLE` into a Python file object in the same process; `Internal_do_connect`
capability-checked, then called the real, pre-captured `socket.socket.connect` directly — the actual
TCP connect happened using the interpreter's OWN process identity, which was the host's, i.e.
unrestricted.

That shape does not survive the worker-process split. `agentengine_python_worker.exe` runs inside a
zero-capability AppContainer (ADR-004 AC-S1); `ADR-004 §5.3`'s own measured evidence
(`WinError 10013`, `WSAEACCES`, on `connect()`) proves the OS itself denies outbound connect from
inside that identity regardless of what this codebase's own capability system decides — `socket()`
creation succeeds (an unbound socket is not a network access by itself), but the worker process
categorically cannot complete a real connect on its own. **This is the reason Slice 1's wire protocol
defines `connect_send`/`connect_recv`/`connect_close` as distinct query kinds** (§45 of the wire
protocol's own header comment) rather than a single `connect_authorize` gate followed by letting the
guest's own socket proceed unmediated the way the old in-process design did: there is no "let it
proceed" available anymore. Every byte of guest socket traffic must physically cross the RPC channel.

File access does not have the same categorical OS-level wall (AppContainer's ACL denial is a
*backstop*, ADR-004 §6 finding 1 — the primary boundary was always interpreter-level mediation, per
`native_jail_backend.hpp`'s own file header), but the worker process has no legitimate way to derive a
verified-safe `HANDLE` itself (`open_within_mount_root` needs `mount_root`, a host-only string per
`MediatedPythonConfig`'s own field comment — "may never cross into this process, I2"). So file access
also needs the host to do the real work; the question is only whether the *result* (a live file
handle) can cross the process boundary once, or whether every `read()`/`write()`/`seek()` needs its
own round trip like sockets do.

## 1. Design A (ORIGINAL, FALSIFIED — kept for the record, not deleted): file relay via
`DuplicateHandle`

**Disposition: FALSIFIED during implementation, not shipped.** §1a immediately below is what actually
shipped. This section is left exactly as originally drafted — CLAUDE.md's own `decisions/README.md`
rule ("a superseded ADR is marked Superseded and points forward; it is never deleted, because the
reasoning that was wrong is part of the record") applied here even though this is a draft, not yet a
numbered ADR, because the SAME reasoning applies: a design that reads well and turns out wrong on
execution is exactly the case this project's evidence discipline exists to catch and keep, not erase.

**The question, stated so it has a wrong answer:** can a single, real, access-restricted Windows
`HANDLE` be handed to an AppContainer'd process such that the process can use it for ordinary,
zero-round-trip I/O afterward, without that grant ever exceeding what the capability check already
decided?

**Design:** `dispatch_worker_query`'s `"open"` handler, host-side:

1. Parse `{mount_id, mount_relative, mode}` from the payload (unchanged shape from the old
   `Internal_open`'s own arguments — `parse_open_mode(mode)` is ported verbatim, same six-mode table:
   `r`/`rb`/`w`/`wb`/`a`/`ab`).
2. Resolve `mount_id` against `PythonWorkerSessionConfig::mount_roots` — **a NEW field this design
   adds**; today `PythonWorkerSessionConfig` (unlike the old `MediatedPythonConfig`) has no
   `mount_roots` at all, because Slice 1 never needed to resolve one host-side.
   `MediatedPythonRunner::initialize()` already has `config_.mount_roots` (it forwards each entry into
   `SandboxSpec::mounts` for the real ACL grant, per that struct's own comment) — this design adds
   forwarding the SAME map into `PythonWorkerSessionConfig::mount_roots` too, so `create_python_worker()`
   can retain it on `PythonWorkerState` for `dispatch_worker_query` to read later. No new grant
   surface, no widened authority — this is wiring a value that already exists host-side into a second
   place it is read from, not creating a new one.
3. Capability check: `ctx.capabilities->find_fs_write(mount_id, mount_relative)` /
   `find_fs_read(mount_id, mount_relative)`, exactly the branch old `Internal_open` used (§ "Gap-12
   fix" / "Milestone 3 Phase G4" comments in the ported code) — including the write-mode
   `quota_bytes`/`file_count_cap` live-usage check via `mount_root_usage()`.
4. `open_within_mount_root(mount_root, mount_relative, desired_access, creation_disposition)` — the
   IDENTICAL ADR-014 TOCTOU-safe primitive, unchanged, now called host-side instead of from inside the
   worker.
5. `granted_read->size_cap_bytes` check via `GetFileSizeEx` on the still-host-owned handle — unchanged
   from the old code, ported verbatim.
6. **New step, replacing the old "wrap in this process" tail:** `DuplicateHandle(GetCurrentProcess(),
   handle.get(), inst.worker->process.hProcess, &target_handle, /*dwDesiredAccess=*/0,
   /*bInheritHandle=*/FALSE, DUPLICATE_SAME_ACCESS)`. `DUPLICATE_SAME_ACCESS` is load-bearing: the
   duplicate carries EXACTLY the access rights `open_within_mount_root` was called with
   (`GENERIC_READ` xor `GENERIC_WRITE`, never both, never more) — never a caller-chosen broader mask.
   The source `SafeFileHandle` is then let go (its destructor closes the host's own copy) — the
   worker's duplicate is now the only reference to that kernel file object, so the host process is not
   left holding an open handle it no longer needs (matches `close_worker_handles`' own "don't leak"
   discipline elsewhere in this file).
7. Response: `{"ok": true, "handle_value": <uint32>, "for_write": bool, "binary": bool, "append": bool}`.
   `handle_value` is safe as a JSON number: 64-bit Windows guarantees the upper 32 bits of any HANDLE
   are always zero (documented WOW64 interop guarantee), so the value round-trips exactly through
   `json::Value`'s double-only number representation, the same reasoning `exec_seq` already relies on.

Worker-side `Internal_open` (`python_worker_mediation.cpp`), replacing today's
"host reported success but this worker build cannot yet receive a relayed file handle" dead branch:
`_open_osfhandle(handle_value, crt_flags)` → `io.FileIO`/`io.BufferedReader`/`io.BufferedWriter`/
`io.TextIOWrapper`, the exact same four-call construction sequence the old in-process code used,
ported verbatim (only the handle's PROVENANCE changed — it now arrived over the wire instead of from a
local `open_within_mount_root` call).

**Why not per-call RPC (`open`+`read`+`write`+`seek`+`close` as five separate query kinds) instead?**
Rejected: (a) the wire protocol's own header comment (`mediated_python_worker_protocol.hpp` §
"SCOPE, stated plainly") already commits to exactly the five-kind set that does NOT include
read/write/seek — adding them would be a wire-shape change beyond this design's stated scope; (b) a
real kernel `HANDLE`, once verified and duplicated with the exact granted access mask, is a strictly
narrower and cheaper mechanism than reimplementing POSIX read/write/seek semantics (append-mode
positioning, partial reads, `O_BINARY` vs `O_TEXT` newline translation) over JSON RPC — every one of
those semantics already exists correctly in CPython's own `io` module and the Win32 CRT, and
reimplementing them host-side would be new, untested surface with no security benefit (the access
mask is already the enforcement point; POSIX-level read/write past that point is ordinary,
already-correctly-scoped I/O;) (c) `listdir` has no analogous "handle" concept (a directory listing is
a one-shot enumeration, not an ongoing I/O stream), so it is answered directly (below), keeping the
handle-relay mechanism scoped to the one case that actually needs it.

## 1a. Design A, REVISED (SHIPPED): file relay via per-call RPC — the reason §1 above was wrong

**The falsifying evidence, reproduced directly, this pass:** `dispatch_open`'s `DuplicateHandle(
GetCurrentProcess(), handle.get(), ws.process.hProcess, &target_handle, 0, FALSE,
DUPLICATE_SAME_ACCESS)` call SUCCEEDS — the exact numeric handle value the host computes is confirmed,
byte-for-byte, to be the value the worker receives over the wire (a wire-transport correctness check,
not the actual claim under test). Inside the worker, `GetHandleInformation` on that same value
succeeds too — the OS agrees the handle is open and present in the worker's own handle table. Yet
constructing a real `io.FileIO` around it (`_open_osfhandle` + `io.FileIO(fd, mode)`) fails with
`OSError: [WinError 6] The handle is invalid` — for an ordinary user-created file with no explicit
AppContainer-SID ACE. This is a real, reproduced Windows AppContainer behavior: a file handle
DuplicateHandle'd into an AppContainer'd process is *present* but not *usable for I/O* unless the
underlying object's own security descriptor already grants the AppContainer identity access — which an
ordinary guest-mount file, created by host-side test setup code with default NTFS permissions, never
does. This is the SAME headline ADR-004 §6 finding 1 already named ("AppContainer's ACL model is not a
sufficient filesystem boundary") — proven here to cut in the OTHER direction too: not only can some
files be readable that shouldn't be (ADR-004's own `win.ini`/`hosts` finding), a legitimately-duplicated
handle to an ordinary file can be flatly unusable.

**The revised design:** the host never lets go of the real `HANDLE`. `dispatch_open` keeps the
capability-checked, quota-checked, size-cap-checked `SafeFileHandle` open in `PythonWorkerState::
open_files` (an id-keyed map, the SAME shape §2's `live_sockets` already uses for sockets — this
revision makes files and sockets structurally identical, not two different mechanisms), seeks it to
EOF once, host-side, for append mode (replacing the worker-side `_lseeki64` the original design relied
on), and answers `open` with `{"ok": true, "file_id": N, "for_write": bool, "binary": bool}` — no
`handle_value` on the wire at all. Three new query kinds, mirroring `connect_send`/`connect_recv`/
`connect_close` exactly: `file_read(file_id, max_bytes)` (host `ReadFile`s up to `min(max_bytes,
kMaxRelayChunkBytes)`, `ERROR_HANDLE_EOF` maps to an empty-bytes success, matching Python's own
`.read()` EOF contract exactly since a file read is never "not ready yet" the way a socket recv can
be — no `would_block` ambiguity to resolve here), `file_write(file_id, data_base64)` (host `WriteFile`s
up to `kMaxRelayChunkBytes`), `file_close(file_id)` (idempotent success, same reasoning as
`connect_close`).

**Worker-side, `Internal_open` no longer touches `_open_osfhandle`/`io.FileIO` at all.** It constructs
and returns an instance of a new pure-Python class, `_AeRelayFile` (installed once, in the bootstrap
source, and captured host-side into a TU-static `g_relay_file_cls` the same way `g_ask_pending_exc_type`
is captured) — `read(size=-1)`/`readline()`/`__iter__`/`__next__`/`write(data)`/`__enter__`/`__exit__`/
`close()`, each relaying through `_ae_internal.file_read`/`file_write`/`file_close`. Two implementation
bugs were found and fixed by the SAME "run the real tests, don't just read the code" discipline this
whole project's evidence bar demands, not caught by review alone:

- **No `__del__`.** A real `io.FileIO` closes deterministically on refcount-zero (CPython's ordinary,
  non-cyclic reclaim, not a GC-cycle-dependent finalizer) — the common `open(path, 'w').write(data)`
  idiom with no explicit `close()`/`with` block relied on this. `_AeRelayFile`, a plain Python class,
  has no such finalizer unless it defines `__del__` itself; without one, `test_mediated_python_runner_
  error_mapping.cpp`'s G4-R2-Q3 (a live-quota check reading current on-disk usage right after an
  unclosed prior write) failed — the earlier write's file was still open, host-side, so its bytes
  weren't being counted as committed usage the way a closed handle's would be. Fixed: `__del__` calls
  `close()`.
- **`read(size)` ignored `size` entirely.** The first version always read to EOF regardless of what
  was asked, because every existing call site (`_files_input`, `_data_read_json`, the smoke test's own
  `f.read()`) happens to call `read()` with no argument. `test_reference_agent_task_corpus.cpp`'s
  H1-T3 (`pandas.read_csv`) was the first real caller to pass an explicit size — pandas' C parser reads
  in bounded chunks and depends on getting AT MOST what it asked for per call. Fixed: a unified raw-byte
  buffer (`_buf`, shared by `read()` and `readline()` so a size-bounded read never discards bytes a
  later `readline()` needs) that fetches from the host only as needed and returns exactly `min(size,
  available)` when `size >= 0`.

**Why this is not a regression relative to §1's own "why not per-call RPC" rejection (item b):**
that rejection assumed the real kernel `HANDLE` mechanism would actually work; once it doesn't, the
POSIX-semantics-reimplementation cost §1 weighed against it is not optional overhead being traded away
for convenience, it is the only mechanism left standing. The semantics that matter (append positioning,
binary vs. text, EOF) are reimplemented in ~90 lines of pure Python (`_AeRelayFile`), not host-side C++,
keeping the security-relevant surface (capability check, quota check, size-cap check, the real
`ReadFile`/`WriteFile` calls) exactly where §1's own design already put it — only the wire shape and
the worker-side wrapper changed.

## 2. Design B: socket relay — host-owned live socket, `connect_send`/`connect_recv` byte relay

**The question:** since the worker process cannot complete a real connect at all (§0), how does guest
`socket.socket(...).connect(...)`/`.send(...)`/`.recv(...)`/`.close()` still work for a genuinely
authorized `cap::NetOut` grant, without ever handing the worker a capability it structurally cannot
have (a real outbound socket)?

**Design:** the host holds the ONE real socket per logical guest connection; the worker holds only an
opaque, host-minted integer id and relays every operation.

1. **`connect_authorize`** (payload `{host, port}`):
   - Capability check: `ctx.capabilities->contains(cap::NetOut{{host+":"+std::to_string(port)+":tcp"}, ...})`
     — the SAME requested-capability shape the old `Internal_do_connect` built. Not held →
     `{"ok": false, "error_code": "net.address_blocked", "message": "..."}"` — reusing the EXISTING
     error code `raise_mapped_denial` (`python_worker_mediation.cpp`) already maps to
     `ConnectionRefusedError`, so the worker-side exception-mapping table needs zero changes for this
     path. This is deliberately the SAME exception shape a "the address resolved to a blocked range"
     denial gets (next bullet) — matching `raise_connection_error`'s own long-standing comment ("an
     ordinary connection failure, not PermissionError", 026 §3's ADR-04-carried table) — a guest script
     cannot distinguish "you don't hold this capability" from "this address is not reachable," by
     design, same as before this redesign.
   - **New hardening, named explicitly, not silent:** `sandbox::resolve_and_validate(host, port)`
     (ADR-011) — the old in-process `Internal_do_connect` never called this; it went straight from
     "capability held" to the real connect. This design adds it because the worker-process split moves
     the actual connect into a NEW host-side code path that did not exist before, and every other
     guest-controlled connect this codebase performs (`HostEgressProxy::fetch`) already goes through
     `resolve_and_validate`'s resolve-once/no-DNS-rebind-window discipline — leaving the ONE other
     guest-facing raw-connect path unprotected would be inconsistent with 008/ADR-011's own posture,
     not a faithful "port the old behavior" choice. No existing test grants `cap::NetOut` and then
     depends on connecting to a private/loopback/metadata address, so this is not a regression against
     anything proven; it is closing a gap the old design carried unexamined.
   - `resolve_and_validate` fails (blocked range, unresolvable) → same `"net.address_blocked"`/
     `"net.host_unresolvable"` codes `HostEgressProxy` already uses for the identical failure — again,
     zero new worker-side mapping.
   - Both checks pass → `pal::tcp_connect(endpoint.ipv4_host_order, endpoint.port)` (non-blocking,
     `agentengine/pal/net.hpp`), then a bounded wait for writable-readiness (`select()`, timeout =
     `min(remaining wall_ms budget, kConnectTimeoutMs)` — a NEW small constant, generous but finite;
     see §4 item 3 for why this must never be unbounded even though the outer watchdog also bounds the
     whole call), then `pal::connect_result()`. Success → mint `socket_id` (a host-side, per-worker,
     strictly-increasing `std::uint64_t` counter — never worker-chosen, mirroring `exec_seq`'s own
     "host mints, worker never originates an id" discipline), store `{socket_id: fd}` in a NEW
     `PythonWorkerState::live_sockets` map, respond `{"ok": true, "socket_id": N}`. Failure (timeout,
     `ECONNREFUSED`, etc.) → `{"ok": false, "error_code": "net.host_unresolvable", "message": "..."}"`
     (the closest existing mapped code — a real connect failure IS "this host is not reachable," the
     same user-facing fact `resolve_and_validate` failing represents, so reusing it rather than minting
     a third near-duplicate code is the more honest mapping).
2. **`connect_send`** (payload `{socket_id, data_base64}`): look up `socket_id` in `live_sockets` —
   absent/foreign id → `{"ok": false, "error_code": "net.socket_closed", ...}` (a NEW, narrowly-scoped
   error code; maps to `OSError` worker-side, the ordinary Python exception for "you used an already-
   closed socket"). Present → base64-decode (the SAME `rt::base64_decode`/`base64_encode` pair
   `message_codec.hpp` already implements and this project already trusts for exactly this "arbitrary
   bytes across a JSON wire" job — not a second implementation), loop `pal::send_some` until the whole
   payload is accepted or a real send error occurs (a single guest `.send(data)` call is bounded by
   `data`'s own size, which output_discipline.hpp-style caps already bound elsewhere in this pipeline —
   see §4 item 2 for the cap this design adds explicitly here too), respond
   `{"ok": true, "sent": <bytes actually accepted>}"` — Python's own `socket.send()` contract already
   permits a short write, so this is not a new semantic, just relayed faithfully.
3. **`connect_recv`** (payload `{socket_id, bufsize}`): same id lookup; `bufsize` clamped to a fixed
   ceiling (`kMaxRelayChunkBytes`, §4 item 2) regardless of what the guest asked for (mirrors
   `net_egress_proxy.hpp`'s own `kHardResponseCeilingBytes` "the host's own ceiling applies regardless
   of what was requested" posture). `pal::recv_some` once (non-blocking; `would_block()` on a socket
   with nothing ready is NOT the same as "no data yet" from the guest's synchronous-`recv()`
   perspective, so this call blocks with its own short bounded retry loop — not the outer watchdog's
   `wall_ms`, which is for genuinely pathological hangs, not ordinary "the peer hasn't sent anything in
   the last 50ms" polling backoff). `Ok(n>0)` → `{"ok": true, "data_base64": ..., "eof": false}`.
   `Ok(0)` (orderly peer close) → `{"ok": true, "data_base64": "", "eof": true}` — the worker maps this
   to Python `recv()`'s own `b''`-means-EOF contract, no exception. A real error → close the socket
   host-side, remove it from `live_sockets`, respond with an error the worker maps to `OSError`.
4. **`connect_close`** (payload `{socket_id}`): `pal::close_fd`, erase from `live_sockets`, always
   responds `{"ok": true}` (closing an already-closed/foreign id is a no-op success, matching Python
   `socket.close()`'s own idempotent contract — never an exception a well-behaved `with socket.socket()
   as s:` block should have to catch).

**Worker-side change, the real redesign relative to Slice 1's stub:** `_ae_internal.do_connect`
(single capability-check-then-real-connect call) is replaced by a small pure-Python `_AeRelaySocket`
class (installed the same way the existing bootstrap installs `_ae_open`/`_ae_connect` wrappers —
`python_worker_mediation.cpp`'s Stage D bootstrap source) that guest code receives from
`socket.socket(AF_INET, SOCK_STREAM)` instead of a real `socket.socket` instance. Its `connect`/`send`/
`recv`/`close` methods each call one new `_ae_internal.*` C function (`connect_authorize`/
`connect_send`/`connect_recv`/`connect_close`, mirroring `open`/`listdir`/`call_tool`'s own existing
one-C-function-per-verb shape) that round-trips through `query_or_raise` exactly like every other
Stage D wrapper already does. **This is the one place this design's shape diverges from a literal
"port the old code" reading**, named explicitly per §0: the old design patched only `.connect` on a
REAL socket object; this design cannot, because the worker never gets a real socket object at all
(AF_INET/SOCK_STREAM creation itself is not blocked by AppContainer, per ADR-004's own AC-S1 evidence,
but nothing useful can be done with the result). `g_real_socket_connect`'s whole "capture the real
callable before patching, never expose it" mechanism (§0's own quoted rationale, "E4-PY9") becomes
UNUSED by this design (there is no real connect left to capture) — removed, not left as dead code with
a comment explaining why it is unused forever.

**A second real, measured finding during implementation:** the socket_id<->socket correlation was
originally planned as an ordinary instance attribute (`self._ae_socket_id = ...`) on the real
`socket.socket` object. This vendored CPython's `socket.socket` instances carry no `__dict__` at all —
the assignment raises `AttributeError: 'socket' object has no attribute '_ae_socket_id' and no __dict__
for setting new attributes`, discovered when `test_native_jail_python_worker_handle_relay.cpp`'s SOCK-1
positive control (§6) first ran a real `connect()`. Fixed with a `weakref.WeakKeyDictionary` keyed on
the socket object itself instead — chosen over a plain `id(self)`-keyed dict specifically to avoid a
stale entry aliasing a different, later socket object CPython could allocate at the same address after
the first is garbage-collected.

## 3. What this design deliberately does NOT change

- The `bridge_tool_call()`/`call_tool` relay path (Slice 1, already real) — untouched.
- `agent.ask` — untouched (still no-IPC, per its own file-header exception).
- `execute_shell`'s own still-separate process-spawn question (tracker Finding O) — still not unified
  with this fix, exactly as ADR-081 §3 already named as out of scope.
- Linux parity — still a separate, still-unbuilt residual
  (`docs/planning/linux-jailed-python-worker-design-draft.md`); this design is Windows-only, matching
  `NativeJailBackend`'s own existing platform scope.
- `execute_code`'s own `ResourceLimits::net_bytes`/`ResourceLimits`-level socket-count ceiling —
  `cap::NetOut::byte_cap` (per-grant) and this design's own `kMaxRelayChunkBytes`/`kMaxLiveSockets`
  (per-call/per-session, §4) are the only caps this design adds; reconciling a SandboxSpec-level
  `ResourceLimits::net_bytes` the way `narrow_by_resource_limit` already does for the tool-bridged path
  is a real, named, NOT-built residual for a follow-on pass (this design's own §5).

## 4. Self-red-team (authority-leak / DoS-resource / replay-misattribution lenses, matching ADR-081's
own RT1/RT2 shape)

1. **Authority-leak — does `file_read`/`file_write` ever exceed what the capability check granted at
   `open()` time?** REVISED per §1a: the worker never receives a raw HANDLE at all now, so there is no
   "does the WORKER's copy carry too much access" question to ask — the access mask lives entirely
   host-side, fixed at `open_within_mount_root()`'s call site to exactly `mode->desired_access`
   (`GENERIC_READ` xor `GENERIC_WRITE`, decided by the six-entry `parse_open_mode` table, never by
   anything the worker supplied beyond the mode STRING that table already closes over) and never
   re-derived from anything a later `worker_query` claims. `dispatch_file_write`/`dispatch_file_read`
   do not even need their own explicit mode check: calling `WriteFile` against a handle
   `open_within_mount_root` opened with `GENERIC_READ` only fails at the KERNEL level
   (`ERROR_ACCESS_DENIED`) regardless of anything this C++ code does or omits — a stronger invariant
   than §1's own original design had, where the check was "the duplicate's rights match," not
   "there is only ever one handle, and its rights were fixed once, at open, by the capability check."
2. **Authority-leak — can a worker request a `mount_id` this design didn't intend to expose?**
   No: `PythonWorkerSessionConfig::mount_roots` is populated ONLY from `MediatedPythonConfig::mount_roots`,
   which is host-authored config, never guest/model-influenced (same I2 posture the existing mount-ACL
   forwarding already relies on) — an unknown `mount_id` in the query payload hits the existing
   `mount_it == g_current_config->mount_roots.end()` branch (ported unchanged) and denies before any
   Win32 call, exactly as before.
3. **DoS/resource — can a slow/hanging TCP peer block the ENTIRE worker session indefinitely?**
   **Corrected during independent red-team review (this pass) — the original text below overstated the
   backstop; left visible, not silently rewritten, per CLAUDE.md's "the reasoning that was wrong is
   part of the record."** `connect_authorize`'s own bounded `select()` timeout (§2 item 1) is NOT a
   "first" layer with a real second layer behind it — it is the ONLY bound. `exec_session()`'s outer
   `wall_ms` watchdog kills the WORKER process via `TerminateJobObject`; it has no mechanism to
   interrupt a HOST thread blocked inside `dispatch_worker_query`'s own `select()` call, since that
   thread is not part of the worker's Job Object at all. If `connect_authorize`'s 5-second timeout were
   ever removed or a future call site added without its own bound, nothing would rescue a genuinely
   hung host thread — the session would hang forever, uninterruptible. The current code is safe only
   because `connect_authorize` (and `connect_recv`, non-blocking by construction, §2 item 3) each carry
   their OWN finite bound; there is no general "outer watchdog backstops host-thread work" property to
   lean on for a future call site. ~~`connect_authorize`'s own bounded `select()` timeout (§2 item 1) is
   the first backstop; the outer `exec_session()` watchdog's `wall_ms` phase deadline (unchanged, RT2's
   own Slice-1 mechanism) is the second, and it already applies to the WHOLE `exec_request`/
   `worker_query`* exchange loop, including time spent inside `dispatch_worker_query` — a connect or
   recv that ignores its own inner timeout (a bug) still gets killed by the outer watchdog, never hangs
   forever. This is the SAME "two-layer backstop" shape ADR-081 §4's own G-Q1 positive control already
   proved for CPU-bound busy-loops.~~
4. **DoS/resource — unbounded memory from a single `connect_recv`/`connect_send`.**
   `kMaxRelayChunkBytes` (proposed: 1 MiB) caps every single relay call regardless of what the guest
   asked for or sent — the SAME "the host's own ceiling applies regardless of the guest-declared or
   grant-declared cap" posture `net_egress_proxy.hpp`'s `kHardResponseCeilingBytes` already
   establishes for the tool-bridged HTTP path; a guest wanting to move more data than that just issues
   more `recv()`/`send()` calls, exactly like a real socket already requires for any transfer larger
   than the OS's own socket buffer.
5. **DoS/resource — unbounded `live_sockets` growth (a guest that calls `socket.socket().connect()`
   in a loop and never closes any of them).** `kMaxLiveSockets` (proposed: 16, generous for any
   realistic CodeAct script, matching `PythonWorkerSessionConfig::pids`'s own "correctness floor, not
   a tuned production budget" framing for a similar per-session cardinality cap) — `connect_authorize`
   fails closed with a NEW `"net.too_many_sockets"` error code (maps to `OSError`, "Too many open
   files" — matches the real OS errno CPython itself would raise for the analogous real-socket
   exhaustion case, so the guest-visible behavior is ordinary, not policy-flavored) once the cap is
   hit. All live sockets for a worker are force-closed by `terminate_worker`/`close_worker_handles`
   (this design adds a `live_sockets` sweep to whichever of those already runs on every teardown path
   — see §5) — no socket ever outlives the worker process it belongs to.
6. **Replay/misattribution — can a `connect_send`/`connect_recv` targeting one `socket_id` be
   confused with another session's socket?** `live_sockets` is a field of `PythonWorkerState`, i.e.
   scoped to exactly one worker `Instance` — there is structurally no cross-session map to misattribute
   into; a `socket_id` is only ever meaningful relative to the one worker that minted it, and
   `dispatch_worker_query` is already only ever called with the `Instance&` for the worker whose pipe
   the frame arrived on (unchanged from Slice 1). The `exec_seq` check the OUTER `exec_session()` loop
   already performs (RT1 Finding 1, unmodified by this design) covers "is this `worker_query` even from
   the call currently in flight" — `socket_id`'s own scoping is a narrower, independent guarantee this
   design adds on top, not a substitute for it.
7. **I3 — does the host ever trust anything the worker claims about a live socket's state?** No: `eof`/
   error classification in `connect_recv` comes from `pal::recv_some`'s own return value (a real,
   external signal from the OS), never from anything in the incoming `worker_query` payload beyond the
   `socket_id` used purely as a lookup key into host-owned state.

## 5. Named residuals this design leaves open (not silently dropped)

- `ResourceLimits::net_bytes` reconciliation with `cap::NetOut::byte_cap` for the raw-socket relay path
  specifically (§3) — the tool-bridged path already has `narrow_by_resource_limit`; this design does
  not wire an equivalent for `connect_send`/`connect_recv`'s own per-call `kMaxRelayChunkBytes`, which
  is a flat constant, not grant-derived.
- `live_sockets` teardown lands in `close_worker_handles` only, not `terminate_worker` — reasoned out
  during implementation: relayed sockets are HOST-owned (never assigned to the worker's Job Object), so
  `TerminateJobObject` cannot and does not affect them; `close_worker_handles` is the one point both
  `destroy()` and every `create_python_worker()` post-creation failure path already funnel through
  (per its own existing "don't leak" discipline for the HANDLE fields), and no new `worker_query` can
  be dispatched (hence no new socket minted) once `ws.alive` is false, so a watchdog kill leaving
  sockets open until the eventual `close_worker_handles()` call is a bounded hold, not a leak.
- No UDP, no IPv6 (`VerifiedEndpoint`/`pal::tcp_connect` are both IPv4-only already, matching
  `net_egress_proxy.hpp`'s own documented IPv4-only scope) — an IPv6-only host fails the SAME way a
  guest calling `HostEgressProxy` against one already does, not a new gap this design introduces.
- `select()`'s traditional `FD_SETSIZE` ceiling is irrelevant here (this design waits on exactly one fd
  per `connect_authorize` call, never a set spanning many sockets at once) but is worth naming as a
  reason NOT to later "optimize" this into a single multiplexed wait across `live_sockets` without
  re-checking that assumption.
- **Pre-existing, unrelated to this design:** `import pandas` inside the jailed worker fails with
  `PermissionError: [Errno 13]` reading `vendored_python_packages\pandas\__init__.py` — an ACL-grant
  gap in `create_python_worker()`'s `extra_sys_path`/`grant_ro` mechanism (a completely different
  subsystem from `dispatch_worker_query`'s open/listdir/socket relay), confirmed present on the
  unmodified Slice-1 baseline via direct `git stash` bisection (§6 item 2). Not fixed here — out of
  this design's own scope — but named so it is not mistaken for a HandleRelay regression by a future
  reader of `test_reference_agent_task_corpus`'s own remaining H1-T3 failure.
- Non-ASCII UTF-8 text read via a size-bounded `read(size)` call on a text-mode relayed file can see a
  `UnicodeDecodeError` at a chunk boundary a real `io.TextIOWrapper` would not (§1a's own note) — not
  exercised by any current fixture (all are ASCII), named rather than silently accepted.

## 6. What the prove phase must show — EXECUTED, this pass

1. **Build.** Full solution (`cmake --build . --config Debug`, all targets), zero errors. The six
   modified/new production files (`native_jail_backend.{hpp,cpp}`, `python_worker_mediation.cpp`,
   `mediated_python_worker_protocol.hpp`, `mediated_python_runner.cpp`, `relay_base64.hpp`) plus
   `CMakeLists.txt`'s new `agentengine::worktree_store`/`agentengine::net_egress_proxy` link edge on
   `agentengine_native_jail_backend`.
2. **The 6 tests ADR-081 §4 disclosed as failing** (`test_mediated_python_runner_smoke`,
   `_agent_files_data`, `_skill_mounts`, `_error_mapping`, `test_reference_agent_task_corpus`,
   `_hostile_corpus`) — re-run directly, this pass: **5 of 6 now PASS in full.**
   `test_reference_agent_task_corpus` passes its own Slice-2-scoped checks (H1-T1 csv_sum, H1-T2
   list_dir) but H1-T3 (pandas_groupby) still fails — **confirmed, by `git stash`-testing the
   pre-Slice-2 baseline directly, to be a pre-existing, unrelated environment gap**: `import pandas`
   itself fails with `PermissionError: [Errno 13] Permission denied` reading
   `vendored_python_packages\pandas\__init__.py`, reproduced byte-for-byte identically on the
   unmodified Slice-1 baseline — an ACL-grant gap in `extra_sys_path`'s `grant_ro` mechanism, wholly
   unrelated to `dispatch_worker_query`'s open/listdir/socket relay this design covers. Named as an
   explicit, out-of-scope residual (§5), not silently left unexplained.
3. **NEW positive control, built and run this pass**
   (`tests/test_native_jail_python_worker_handle_relay.cpp`, new): a real successful
   `connect_authorize`+`connect_send`+`connect_recv`+`connect_close` round trip against a real local
   TCP loopback echo server, under a real granted `cap::NetOut` — **PASSES**. Required adding a
   TEST-ONLY resolver-override seam (`NativeJailBackend::set_test_connect_resolver_override`, mirroring
   `sandbox::HostEgressProxy::resolver`'s own established precedent) because `sandbox::
   resolve_and_validate`'s own SSRF-protection blocked-range table (loopback/private/link-local/CGNAT)
   makes every hermetic same-machine target categorically unreachable by design — the same real
   constraint a production deployment would face, not a test artifact to route around insecurely.
4. **NEW negative control, built and run this pass** (same file): `_ae_internal.file_write` called
   DIRECTLY on a `file_id` opened for `"r"` (bypassing `_AeRelayFile`'s own Python-level `for_write`
   check entirely, via `f.close.__func__.__globals__['_ae_internal']` — the same white-box
   object-graph-introspection idiom E4-PY9 already uses) — **PASSES**: denied by the real Win32 kernel
   (`WriteFile` against a `GENERIC_READ`-only handle), proving §4 item 1's revised claim is not merely
   argued.
5. **Zero regression, verified two ways.** The other 10 native-jail tests ADR-081 §4 names, run
   directly: all PASS. The FULL project test suite (264 tests, `ctest -C Debug`): **263/264 pass** — the
   one failure is H1-T3's own pandas-import gap (item 2 above), confirmed pre-existing on the
   unmodified baseline via the same `git stash` bisection.
6. **Two real implementation-time findings, each fixed and disclosed, not merely a clean first pass:**
   the DuplicateHandle falsification (§1a) and the `socket.socket` `__dict__` finding (§2's own
   addendum) — both discovered by running the real tests this section required, not by review alone.
7. **Independent red-team pass — DONE, this pass.** A fresh reviewer (no authorship context, a
   separate agent invocation) read the design draft, ADR-081, and the full diff, traced the thread
   model directly in code rather than trusting this draft's own claims, and reported 6 findings — one
   BLOCKING, one real bug, four MINOR/comment-accuracy issues — reproduced below in full, followed by
   the fix applied for each. This satisfies the same explicit user instruction that produced ADR-004's
   own "Judged" promotion ("đảm bảo bạn gọi code review ở bước cuối cùng để kiểm tra").

## 7. Independent red-team findings and fixes

**Finding 1 — BLOCKING.** Only `connect`/`send`/`sendall`/`recv`/`close` were mediated on
`socket.socket`; every other method (`bind`/`listen`/`accept`/`connect_ex`/`sendto`/`recvfrom`/
`makefile`/`dup`/`detach`/`share`/`shutdown`) was still the real, unmediated CPython implementation,
reachable with zero capability check. ADR-004 AC-S1's own measured evidence is specifically
`socket.connect()` — nothing established that `bind()`/`sendto()` (UDP needs no `connect()` at all)
behave the same way under the zero-capability AppContainer; relying on that narrower evidence to cover
this wider surface would itself have been an unverified claim, inconsistent with this exact bootstrap's
own established practice of proactively denying `os.*`/`subprocess.*` rather than trusting the OS
backstop alone.
**Fix:** every other `socket.socket` method is now explicitly denied (`_ae_socket_op_denied`,
`python_worker_mediation.cpp`'s bootstrap source), the same defense-in-depth posture as the existing
`os.*` denial list. **New positive control**
(`test_native_jail_python_worker_handle_relay.cpp`'s SOCK-DENY block): proves `sendto`/`bind`/
`listen`/`accept`/`makefile` are all denied outright — **PASSES**.

**Finding 2 — real bug.** `dispatch_connect_authorize`'s `static_cast<std::uint16_t>(wp::get_number(
payload, "port"))` is undefined behavior for a port value outside 0–65535 — trivially reachable via an
ordinary guest typo (`s.connect((host, 99999))`), not an attack; the old, unmediated `socket.connect()`
validated this range before ever reaching C, and the new relay skipped that check.
**Fix:** the port is range-checked before the cast; out-of-range values are denied the same ordinary
way an unheld capability already is (`native_jail_backend.cpp:1088-1098`).

**Finding 3 — design-reasoning defect (MINOR).** §4 item 3's original text claimed the outer
`exec_session()` watchdog is a "second backstop" behind `connect_authorize`'s own 5-second `select()`
timeout. False: the watchdog can only `TerminateJobObject` the WORKER process; it has no mechanism to
interrupt a HOST thread blocked inside `dispatch_worker_query`'s own `select()` call. The current code
is safe only because `connect_authorize` carries its own finite bound, not because of any general
"outer watchdog rescues a stuck host thread" property.
**Fix:** §4 item 3 above is corrected in place, with the original (wrong) text struck through and kept
visible per CLAUDE.md's own "the reasoning that was wrong is part of the record" rule, not silently
rewritten.

**Finding 4 — MINOR.** `kMaxRelayChunkBytes` was applied only AFTER `relay_base64::decode`, so a
single `connect_send`/`file_write` call could force decoding (and the allocation that implies) of up
to the wire frame's own 64 MiB ceiling (`jailed_worker_rpc.hpp`'s `kMaxFrameBytes`) before 63/64 of it
was discarded — undercutting §4 item 4's own "the host's own ceiling applies regardless of what the
guest sent" claim by roughly 64x for the WORK done, even though the RESULT was already correctly capped.
**Fix:** a new `kMaxRelayBase64Chars` bound (the largest base64-encoded length that could ever decode
to a payload at or under `kMaxRelayChunkBytes`) is checked on the base64 TEXT before `decode` ever runs,
in both `dispatch_file_write` and `dispatch_connect_send`.

**Finding 5 — MINOR (comment/code mismatch).** `dispatch_open`'s own comment described
`kMaxLiveSockets` as a "session-wide open-resource ceiling... shared" with `live_sockets`, but each map
is checked against its own `.size()` — a session can actually hold `kMaxLiveSockets` files AND
`kMaxLiveSockets` sockets concurrently (32 total, not 16 shared). Low impact, but the comment overstated
what the code does.
**Fix:** comment corrected to state plainly that only the CONSTANT is reused, not a shared counter.

**Finding 6 — MINOR.** `set_test_connect_resolver_override` is a real, unconditionally-linked,
process-WIDE static (unlike `HostEgressProxy::resolver`, a per-instance field) with "never used from
production" enforced by convention only. The original test set it and reset it with a bare pair of
calls, not exception-safe — an early return/exception between them would leave the override live,
silently disabling SSRF protection for every OTHER session sharing the process afterward.
**Fix:** the test now uses an RAII guard (`TestResolverOverrideGuard`) that resets unconditionally in
its destructor; the header comment on the seam itself now states the enforcement gap explicitly and
points future callers at the guard pattern rather than a bare set/reset.

**Checked by the reviewer and found sound, not a finding (reported for completeness):** thread-safety
of `open_files`/`live_sockets` (traced `dispatch_worker_query` to confirm it only ever runs on the
single `exec_session()` call thread per worker, serialized by `call_mutex`'s `try_lock`); both
"falsified and fixed" findings from §1a/§2's own addendum match the shipped code with no half-migrated
fallback path; capability checks in `dispatch_open`/`dispatch_listdir`/`dispatch_connect_authorize` run
strictly before any I/O on every path traced, including the quota branch.

**Re-verified after fixes, this pass:** full solution rebuild, zero errors; the targeted regression set
(`test_native_jail_python_worker_slice1`, `test_native_jail_python_worker_handle_relay`,
`test_mediated_python_runner_hostile_corpus`) — all PASS, including the new SOCK-DENY positive control;
full project suite re-run to confirm zero collateral regression from the fixes.
