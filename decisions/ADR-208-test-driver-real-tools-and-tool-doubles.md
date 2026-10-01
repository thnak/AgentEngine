# ADR-208 — Test driver P5: one sandboxed real tool, and recorded tool doubles that replay it offline

- **Status**: **Judged with conditions (2026-09-28, fresh `general-purpose` judge, §10); both conditions met the same day: the D3 test now edits a recorded result and has its own controls, and the ADR text is corrected.**
- **Date**: 2026-09-26
- **Origin**: ADR-182 §12 R5 ("recorded tool doubles for real tools become P5"), §12 C-2 (real tools only when
  sandboxed), §11 Q2 (no effectful real tools in live mode); GitHub #109.
- **Touches**: `tools/test_driver/test_driver.hpp` (fixture fields, `make_session`, a driver-owned sandbox holder,
  the recording/double wrapper, export and replay), `tools/agentengine_test_driver.cpp` and
  `tools/agentengine_scenario_runner.cpp` (`--sandbox-root`), `CMakeLists.txt` (link
  `agentengine::mediated_shell_runner`), `tests/testing/test_agentengine_test_driver.cpp`, one new scenario and one new file
  fixture.
- **Invariants**: I2 (the grant is host-derived, never from a tool argument or a fixture's own claim), I3 (the tester's
  approval is model-derived and is made harmless by scope, ADR-182 C-2), I5 (a real tool's output is nondeterminism
  and crosses a recorded seam: 001 §7, 022 §3).

## 1. The problem

Every driver session today has only in-process test tools (`echo`, `gated_echo`, `fail`). A scenario proves the
engine's loop, never what happens when a tool with real effects is in the round (ADR-182 R5). Two things are missing:

1. **A real tool a driver session can use without the machine being at risk** (ADR-182 C-2; CLAUDE.md machine safety).
2. **A way to replay such a session offline.** A real tool's output is nondeterministic input to the run. 001 §7 lists
   "Sandbox execution | inputs, outputs, exit status | replaying outputs" as a recorded seam, and "a recorded run
   replays with no external service contacted". Nothing in the tree records or replays a tool result today (only
   model calls: `ChatCallRecording`).

## 2. Decision

### 2.1 One real tool: the mediated `run_shell`, plus `read_sandbox_file`

- The first real tools are `run_shell` (`src/backends/native_jail/session_shell_wiring.hpp`, ADR-096/ADR-103) and
  `read_sandbox_file` (`include/agentengine/tools/read_sandbox_file.hpp`).
- `run_shell` is the **mediated** shell: a host-side interpreter with a fixed command registry
  (`DefaultCommandRegistry`) over `MediatedFileSystemAdapter`, which opens every path through
  `open_within_mount_root` on the session's own scratch directory. It never starts an OS process, so there is no
  fork bomb or escape through a child to contain. Its FS view is the one both platforms already build
  (`agentengine::mediated_shell_runner`).
- **Not in this ADR**: `execute_code` (embedded CPython, one per process, ADR-030), `run_command` and task branches
  (Docker, Ledger), `read_content` (network), PDF tools, and every ADR-071 native provider. The driver keeps refusing
  `--allow-unsandboxed`-class tools outright; there is no such flag in P5 (stricter than ADR-182 §3.2).

### 2.2 Fixture and grant

- A file fixture opts in by naming the tools in `spec.tools` (`run_shell`, `read_sandbox_file`). Compiled fixture
  `shell` does the same for tests.
- **The grant is derived by the driver, not declared.** `spec.capabilities` stays refused unless empty. A session whose
  fixture names a real tool gets exactly `FsRead<"work">` and `FsWrite<"work">` on its own scratch mount, and nothing
  else. No fixture key names a path, a mount, a profile or a capability.
- **Scratch root is host-only.** The driver takes `--sandbox-root <dir>` on its command line (the checked-in
  `.mcp.json` sets it under the build directory). Without it, a fixture naming a real tool is refused
  (`test.real_tools_disabled`). Each session gets `<root>/<session-digest>/`, created empty at `session_start` (a
  leftover directory is removed first) and removed at `session_close` and driver exit.
- **Live mode + real tools is refused** (`test.bad_fixture`), per Q2. A live model's tool calls are exactly what a
  red-teamer would want to aim at a real tool.
- **`session_fork` of a session with real tools is refused** (`test.fork_unsupported`): the fork would alias or have to
  copy the scratch directory, and ADR-096 C2 already forbids aliasing. A follow-on can copy the directory.

### 2.3 Driver-owned sandbox holder

`SandboxToolProvider` is move-only, which makes `fork_from` a compile error for the session type (ADR-096 C2). The
driver's one `Session` type must stay forkable for the test-tool fixtures, so the driver does not compose that provider.
Instead `DriverHistoryProvider` gains an optional `std::shared_ptr<SessionShellSandbox>` (null for test-tool
fixtures). Its `on_context` sets `ctx.sandbox_fs` and contributes the descriptor exactly as `SandboxToolProvider` does.
The run-time refusal of §2.2 replaces the compile-time one.

### 2.4 Recording: every real-tool call is captured at the invoke seam

- In a session with real tools, each real tool's `ToolDescriptor::invoke` is wrapped (the pattern `cli_chat.cpp`
  already uses for its output cap). The wrapper calls the real invoke and appends a **tool exchange**:
  `{index, tool_name, arguments (canonical JSON), outcome: {result} | {error: {code, message}}, exec_events}`.
- `exec_events` is the list of `sandbox_exec_*` events the real tool emitted during that call (stage, outcome, error
  code; the `exec_id` is dropped as ADR-182 R6 already does). They are captured from the driver's event tap between
  the wrapper's entry and exit, which is safe because such a tool is never parallel-dispatched
  (`captures_session_state`, ADR-160). *(Superseded by §6 M1: the wrapper swaps `ctx.sandbox_exec_sink` for the
  call; `captures_session_state` is false for `read_sandbox_file`.)*
- Test tools are not recorded: they are deterministic and replay live, as today.
- `scenario_export` writes the exchanges as `tool_exchanges` next to `model_turns`. A session whose recording is
  incomplete (a call ended by cancel mid-invoke, an exchange over the size cap) is marked non-exportable with the
  existing `nondeterministic_reason`. *(Superseded by §6 G1: an over-size or non-UTF-8 result is replaced by an
  error before the model sees it and is recorded as that error, so the session stays exportable.)*

### 2.5 Replay: a double serves the recording and touches nothing

- Replay of a scenario with `tool_exchanges` builds the fixture's tool table with **doubles**: the same descriptor
  (name, description, schema, capability ceiling, approval mode, effect class), so the model request (C8 digest) and the
  approval gate are unchanged, with `invoke` replaced.
- No sandbox is created in replay: no scratch directory, no `SessionShellSandbox`, no `--sandbox-root` needed.
- The double serves exchanges in order. For call *N* it checks the tool name and the canonical arguments against
  exchange *N*. A difference, or a call past the last exchange, fails the call and the replay with
  `test.replay_mismatch at tool call N` (expected vs. actual), by the same terminal rule C8 uses for model requests.
- It re-emits the recorded `exec_events` through the call's `EffectContext` (same stage and outcome, a fresh
  counter-derived `exec_id`) and returns the recorded result or error. The event stream therefore compares exactly.
- Exchanges left unconsumed at the end fail the replay (`tool exchanges not consumed`).

## 3. Why this shape

- **Record at the invoke seam, not in the provider.** It is the one place every tool result passes through
  (`ToolDescriptor::invoke`), and it keeps the approval, capability and budget checks of the real pipeline in both
  record and replay: only the effect is swapped.
- **The mediated shell first.** It is the only real tool whose effects are confined by construction to one directory
  with no process creation, on both platforms. It is enough to test the loop's handling of real, stateful,
  nondeterministic output.
- **Argument matching is strict.** A double that answered any call would let an engine change that alters what the
  model asks the tool pass silently, the same failure C8 closed for model requests.
- Rejected: **re-running the real tool in replay and diffing.** Replay would then need a sandbox in CI and would still
  not be deterministic for any tool whose output depends on time or the host. A `--live-tools` drift check can come
  later as an opt-in, never gating.
- Rejected: **reusing `EvalStubToolProvider`.** It serves one canned reply per tool without looking at arguments, and
  it cannot record.

## 4. Claims (to prove)

| # | Claim | Positive control |
|---|---|---|
| D1 | A replay of a real-tool scenario constructs no sandbox and touches no scratch directory | a double that falls through to the real invoke fails D1's check |
| D2 | A replay whose tool call arguments differ from the recording fails with `test.replay_mismatch at tool call N` | argument check removed → the replay passes and D2 fails |
| D3 | A recorded result is what the model sees in replay: editing a recorded `result` fails the replay (C8 or event diff) | — (mutating the file is the control) |
| D4 | Unconsumed or missing exchanges fail the replay | — |
| D5 | A fixture cannot widen the grant, name a path, or turn on real tools without `--sandbox-root`; live + real tools and fork of a real-tool session are refused | each refusal removed → its check fails |
| D6 | The scratch directory starts empty, is per session, and is gone after `session_close` and after driver exit | — |
| D7 | A model-supplied path outside the mount (`cat ../x`, absolute paths, a symlink planted in scratch) is refused by the mediated FS in a driver session | — (existing ADR-096/103 claims, re-run through the driver) |
| D8 | The `sandbox_exec_*` pair is replayed so the whole event stream compares, not a filtered one | double stops re-emitting → the scenario fails at that event |

## 5. Residuals (known, accepted for P5)

- **Disk use in scratch is not capped** by the mediated shell itself; a scripted or live model can write large files.
  The driver caps the number of `run_shell` calls per session (the ordinary tool-call limits) but not bytes. To be
  checked by the red team. *(Superseded by §6 G1: no such ordinary limit existed; the grant is now capped and the
  driver has its own call cap.)*
- The recording holds whatever the tool returned. A tool reading a host file into its result would put that file into
  a scenario; with only the mediated shell over an empty per-session scratch, nothing outside the session can reach
  the result.
- Doubles make the replay prove the engine's handling of the recorded outputs, not that the real tool still produces
  them (R5 as before).


## 6. Red-team pass 1 (2026-09-26, `general-purpose` agent, no prior context) and the revisions

Result: 1 Critical, 7 Real gaps, 7 Minor. Verified true: the mediated shell starts no OS process (the session's
registry has no runners; builtins are cd/pwd/ls/cat/echo/export/mkdir/rm/mv/cp); every path goes through
`open_within_mount_root` and the guest cannot create links; `sandbox_exec_*` events are synchronous on the worker; every
result path (sequential, parallel batch, approval resume) ends at `tool.invoke`, and refusals before invoke are the
engine's own and replay identically; export already drops `exec_id`. False or loose in the draft: §5's "ordinary
tool-call limits" (no such cap exists), §2.4's reliance on `captures_session_state` (false for `read_sandbox_file`),
§2.5 omitted the payload's `backend`. Every finding is accepted; the revisions below supersede §2 where they differ.

- **C1 (Critical): two drivers sharing a root reused and deleted each other's directories** (session ids `s1, s2, …`
  repeat per process). *Revision:* each driver process creates its **own** directory under the root with an exclusive
  create, `d-<16 random hex>`, retried on collision; sessions live under it as `<session id>`. A driver only ever
  removes paths under its own directory. No startup sweep (a killed driver leaves its directory: disk only, residual).
- **G1: disk, memory and CPU were unbounded.** *Revision:* the derived grant is capped:
  `FsWrite{"work", "", quota 16 MiB, 1,024 files}` and `FsRead{"work", "", size cap 1 MiB}` (the shell enforces both);
  a per-session cap of 64 real-tool calls, enforced in the wrapper; a result over 64 KiB, or one that is not valid
  UTF-8 (M3), is replaced by an error in the wrapper (`test.real_tool_output_too_large` / `…_not_utf8`), before the
  model or any event sees it; the shell's wall-clock budget per call is set to 2 s (`SessionShellSandbox::create`
  gains an optional budget; the default is unchanged for other callers).
- **G2: calls could not be cancelled.** *Revision:* the wrapper refuses a real-tool call once `ctx.cancellation` is
  set (`test.real_tool_canceled`). With the call cap and the 2 s budget, a cancel or close waits at most one call.
- **G3: `fork_from` copies the history provider, so a fork would alias the sandbox.** *Revision:* the driver's
  provider has a copy constructor that copies only the test tools and never the sandbox, the real-tool descriptors or
  the filesystem view. `session_fork` of a real-tool session is refused before a target is made. A test calls
  `fork_from` directly and checks the copy has no real tools.
- **G4: the replay channel was unspecified.** *Revision:* doubles reach the driver only through `DriverConfig`
  (host-only), which `replay_scenario` sets from the file; no MCP argument can open a double session, and the 4 MB line
  cap does not apply. A scenario with both `segments` and `tool_exchanges` is refused. The driver binary never sets
  doubles (M5).
- **G5: replay must hold the same grant.** *Revision:* record and replay derive the grant with the same function;
  claim D9 (below).
- **G6: D1 could not fail.** *Revision:* the sandbox is built only by a factory in `DriverConfig`. The driver binary
  and the in-process tests set it; the scenario runner does not, and **does not link the mediated shell**, so a replay
  that tried to build a sandbox would not link. The in-process test also counts factory calls, but that count cannot
  fail: `replay_scenario` builds a fresh `DriverConfig` with no factory, so the test's factory is unreachable from a
  replay. The link-time guarantee is D1's real evidence (judge, §10).
- **G7: cleanup could follow links.** *Revision:* removal refuses a root that is a link; it removes only
  `<own dir>/<session id>` and walks the tree itself with `symlink_status`, removing a link (or junction) as an entry
  and never descending into it. Removal failures are reported in `session_close`'s result. Tests plant a link to a
  directory outside the root and a deep tree.
- **M1:** the wrapper captures `sandbox_exec_*` by wrapping `ctx.sandbox_exec_sink` for the call, not from the tap.
- **M2:** an error outcome records `failure_class` and `code` and `message`.
- **M4:** the registry gets stand-in descriptors for the real tools (same ceiling, approval and schema); `run_shell`
  is `never_require`, so the tester's approval is never consulted for it. Harmless by scope (ADR-182 C-2).
- **M6:** Windows device names (`NUL`, `CON`, `COM1`, `CONIN$`) are in the containment test set.
- **M7:** no digest in the directory name; the owned directory already separates processes.

Revised claims (replacing §4's table where they differ):

| # | Claim | Positive control |
|---|---|---|
| D1 | replay builds no sandbox: the runner does not link one, and a replay's config has no factory | structural (link); no runtime mutant can reach it |
| D2 | argument drift fails with `test.replay_mismatch at tool call N` | argument check removed → D2 fails |
| D3 | an edited recorded result fails the replay at the next model call (C8) | C8 request check off → D3 fails; D2's mutant → D3 still passes |
| D4 | unconsumed or missing exchanges fail the replay | (the edit is the control) |
| D5 | refusals: no `--sandbox-root`, live + real tools, fork of a real-tool session, doubles + segments | each refusal has a check, except live + real tools: no live fixture can name a real tool (file fixtures are never live, no compiled live fixture has one), so that refusal is unreachable today and holds structurally |
| D6 | per-process and per-session directories; two drivers on one root never touch each other's; removal never leaves the root through a link | shared-name directories → two-driver check fails |
| D7 | the mediated FS refuses `../`, absolute paths and device names through the driver | (engine claims re-run) |
| D8 | the `sandbox_exec_*` pair replays, so the whole event stream compares | re-emission removed → scenario fails |
| D9 | record and replay hold the same grant | replay without the grant → the scenario fails |
| D10 | caps: quota, output size, call count and cancel each refuse | each cap removed → its check fails |

## 7. Implementation findings (2026-09-26)

- **A capped grant cannot admit a tool whose declared ceiling is uncapped.** The pipeline's admission binds each
  declared requirement with `contains()` (`tool_pipeline.hpp`, step 4/7), and `cap_covers` reads an uncapped request
  under a capped grant as widening. `run_shell` declares `FsRead<"work">`/`FsWrite<"work">` uncapped, so under the
  capped grant of G1 every call was refused `tool.capability_not_held` before its invoke. The shell's own checks were
  fixed for exactly this (gap-12: `find_fs_read`/`find_fs_write`), admission was not. *Driver fix:*
  `with_granted_ceiling` narrows a real tool's declared ceiling to the capped grant, in record and replay alike. That is
  narrower than the tool's own declaration, never wider (I2), and the shell still enforces the quota from the held
  grant (D10). *Engine follow-up (not this ADR):* a quota-capped grant is unusable for any tool declaring an uncapped
  `FsRead`/`FsWrite` unless its host narrows the ceiling the same way.
- **The mediated shell has defects, found while recording the scenario (engine, not driver), filed as GitHub #140 and
  #141** (re-checked on main at 02680b4 through the driver's `shell` fixture, with disk state checked on the host):
  - #140: a `>` redirect resolves against the mount root, not the working directory; `cd /` is ignored; a `cd` to a
    missing directory reports success, and can leave the working directory on a path that does not exist. Hence
    `mkdir notes; cd notes; echo draft > plan.txt` "succeeds" without leaving `notes/plan.txt`. All effects stay
    inside the scratch: correctness, not containment.
  - #141: only the last statement's stdout is returned, and a newline is not a statement separator (`echo one`,
    newline, `echo two` prints `one echo two`).
  The checked-in scenario avoids both constructs so it does not lock either in.
- **The replay reports the tool mismatch before the model-request mismatch.** When a double refuses a call, the next
  model request differs too (C8), but the tool call is the cause, so it is named first.
- **The wall-clock budget is checked between statements** (ADR-100 §4 F3), so one long statement can overrun 2 s.
  Bounded in practice by the 16 MiB quota and the 1 MiB read cap; recorded as a residual.

## 8. Evidence

`tests/testing/test_agentengine_test_driver.cpp` P5 (all pass): REC (write, read back, exec pair, counts,
`<root>/d-*/<session>`), D1 (replay builds no sandbox; the runner does not link the shell), D2, D3 (an edited
`read_sandbox_file` result fails as `test.replay_mismatch at model call 2`: the C8 digest is what catches it), D4 (fewer, more and no exchanges), D5 (no
root, fork, segments + exchanges, `fixtures_list`), G3 (a copied provider has no real tools), D6 (two drivers with the
same session name keep their own files; close and exit remove the directories), D7 (ten `../`, absolute and device-name
probes read nothing outside), G7 (a planted directory link and a tree deeper than MAX_PATH are removed without
following the link), D8, D10 (64 KiB result, non-UTF-8 result, 16 MiB quota, call cap), G2 (cancel). Scenario
`tests/scenarios/scripted_shell_roundtrip.json` replays in ctest with no sandbox.

## 9. Positive controls (2026-09-28, on main after the build-optimization series)

Each mutant was applied to `tools/test_driver/test_driver.hpp`, the named check (or the checked-in scenario, replayed by
`agentengine_scenario_runner`) was seen to fail, and the header was restored from a copy. The tree was verified clean
afterwards and the driver test and all 13 scenarios pass.

| Claim | Mutant | Seen to fail |
|---|---|---|
| D2 | the double checks only the tool name, not the arguments | P5 D2 |
| D3 | the C8 request check always accepts (`if (true) return std::nullopt;`) | P5 D3 (and the C8 checks) |
| D3 (independence) | the D2 mutant above | P5 D3 still passes, so D3 no longer repeats D2 |
| D8 | the double no longer re-emits the recorded `sandbox_exec_*` events | `scripted_shell_roundtrip`: event 5 differs |
| D9 | replay drops the derived grant | `scripted_shell_roundtrip`: the call is refused before the double, so model call 1's request differs |
| C1/D6 | every driver uses one fixed directory under the root, and accepts it existing | P5 D6 (both drivers read the other's file) |
| G7 | removal follows a directory link | P5 G7 (the file outside the root is deleted) |
| G3 | the provider's copy keeps the real tools and the sandbox | P5 G3 |
| D10 | no 64 KiB result cap | P5 D10 (size) |
| D10 | no UTF-8 check | P5 D10 (UTF-8) |
| D10 | grant and ceiling without the quota | P5 D10 (quota: the copy succeeds) |
| D10 | no per-session call cap | P5 D10 (call cap) |
| G2 | no cancel check | P5 G2 |

D1 is structural: the scenario runner does not link the mediated shell, so a replay that built a sandbox would not link.
D4 is controlled by the tampered scenarios in the test itself. The two D3 rows were added after the judge (§10): the
first D3 test edited the first `hello` after `tool_exchanges`, which is exchange 0's arguments, so it only repeated D2.

## 10. Judge (2026-09-28, fresh `general-purpose` agent, no prior context)

Verdict: **Judged with conditions.** Nothing Critical; the grant is host-only as claimed. The judge re-ran the driver
test and all 12 scenarios (13/13), ran the D2 mutant itself (restoring the header by sha256), and replayed scratch
copies of `scripted_shell_roundtrip.json` with one recorded `result` edited: both failed at the next model call, so D3
held in the code. I2/I3 hold: the grant is a host constant, the root and factory come only from the command line, a
fixture can only name tools, doubles are reachable only through `replay_scenario` (whose config has no factory and no
root), and `with_granted_ceiling` only narrows. Every red-team revision (C1, G1–G7, M1, M2) was found in code. The
red team's parallel-dispatch concern does not apply: a batch runs in parallel only when every tool is `Parallelizable`,
and neither real tool is.

Conditions, both met on 2026-09-28:

1. **B1: the D3 test edited the wrong field** (exchange 0's arguments), so it repeated D2. *Fixed:* it edits exchange
   1's recorded `content` and expects `test.replay_mismatch at model call 2`; controls in §9.
2. **B2: ADR text did not match the code** (the `d<pid>-` directory name, D1's unreachable control, a claimed control
   for the unreachable live + real refusal, superseded §2.4/§5 text left unmarked, §8 not naming C8 as D3's evidence).
   *Fixed* in §2.4, §5, §6 and §8.

Residuals the judge accepted, added to §5/§7's:

- **B3: stray `tool_exchanges` are ignored** when the fixture names no real tool (D4 runs only in double mode). The
  C8 digest catches it in practice, because the tool list in the request differs.
- **B4: `read_sandbox_file` has no read-size cap of its own** (the 1 MiB `FsRead` cap is enforced by the shell only).
  Memory is still bounded by the 16 MiB quota, and the 64 KiB result cap replaces anything larger before the model
  sees it.
- Not tested: that `make_shell_sandbox` passes the 1 MiB read cap and the 2 s budget through; G2 relies on the engine
  passing the run's stop token as `ctx.cancellation` (documented at `agent_session_core.hpp`), tested only at the
  wrapper.
- The engine gap behind `with_granted_ceiling` (a quota-capped grant cannot admit a tool declaring an uncapped
  `FsRead`/`FsWrite`) needs an engine follow-up. *(2026-10-01: addressed by ADR-217, Proposed — `run_shell`
  declares `EnforcesGrantedCaps` and `with_granted_ceiling` is removed from the driver.)*

## 11. Follow-ups after the judge (2026-09-29)

- **B3 fixed in the driver.** A replay whose scenario has `tool_exchanges` but whose fixture names no real tool is
  refused (`test.bad_fixture`, "names no real tool") instead of leaving the exchanges unchecked. Check "P5 D4 (B3)";
  control: the refusal removed → it fails.
- **D1's in-process count relabelled** "P5 D1 (smoke)", since it cannot fail (§6 G6). D1 stays structural.
- **The 1 MiB read cap and the 2 s wall clock are now tested through the driver.** A 1.5 MiB file is refused by
  `cat`; a nested-loop script (40,000 reads of ~1 MB) stops at about 2 s. Controls: the read cap removed from the
  grant and ceiling → the first fails; `make_shell_sandbox` not passing `kRealToolWallClock` (10 s default) → the
  second fails.
- **Cancel end to end.** A `session_cancel` during a real-tool call stops the next one. The wrapper's cancel check
  removed does *not* make it fail: the engine's loop ends the run on cancel before the next dispatch, so the wrapper's
  check is a second layer, covered by its own unit check. Control for the end-to-end check: no cancel → the second
  call runs and it fails.
- **Engine issues filed:** #147 (the mediated shell parser calls `std::terminate` in Debug builds on a valid script of
  ~13,000 statements; MSVC iterator-debugging proxies allocate from the fixed arena inside `noexcept` moves; also, the
  grammar rejects POSIX `for x in a; do …; done`), #148 (the admission gap behind `with_granted_ceiling`), #149
  (`read_sandbox_file` ignores the granted `FsRead` size cap, B4; verified through the driver: it reads a 1.5 MiB file
  that `cat` refuses).
