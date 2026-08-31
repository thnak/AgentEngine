# ADR-120 — Real Linux verification of ADR-119: `RunCommandTool`'s new double gate, and all three real production callers, hold on GCC 14.2.0

- **Status:** Proposed — real verification pass, no production code changed. The pre-existing
  `/root/ae-verify` WSL2 Ubuntu 24.04 checkout (ADR-115, reused by ADR-118) fast-forwarded from
  `fb280b7` to this branch's exact HEAD (`f2c66e6`, the ADR-119 red-team-closure commit) and
  rebuilt/retested with the same real GCC 14.2.0 toolchain.
- **Date:** 2026-08-30.
- **Scope:** No files changed. This ADR records a verification run, not a code change.
- **Related specs:** Closes ADR-119 §5's "no Linux verification" residual, including its own two
  named unknowns (`tools/containerd_shell_chat.cpp` and `tests/test_composed_containerd_providers_
  live.cpp` — edited by ADR-119 but never built or run in that pass). Reuses ADR-118's own
  environment and diffing methodology directly.

## 1. The question

ADR-119 widened the real `Capability` variant with `cap::RunCommand` and gave `RunCommandTool` a real
ceiling, then fixed all three real production callers (`tools/cli_chat.cpp`, `tools/sandboxed_shell_
chat.cpp`, `tools/containerd_shell_chat.cpp`) to grant it — but landed on Windows/MSVC only.
`containerd_shell_chat.cpp` and its own composed-providers test are Linux-only (`NOT WIN32`-gated) and
were never built in that pass. Does the new double gate actually hold on real GCC 14.2.0, and — the
part ADR-119 itself could not check at all on Windows — does the THIRD real production tool
(`containerd_shell_chat.cpp`) actually compile and actually work end-to-end against a real containerd
daemon with its new grant?

## 2. What was done

Reused ADR-118's own `/root/ae-verify` checkout (confirmed clean, sitting at `fb280b7`) and
fast-forwarded it (`git fetch` + `git merge --ff-only`) to `f2c66e6` — a genuine 15-file,
571-insertion fast-forward including both the ADR-119 commit and its same-day red-team closure.
Reused the same pre-configured `build-linux-verify` (GCC 14.2.0).

**Full incremental rebuild: 604/604 targets, zero errors** — including `tests/test_composed_
containerd_providers_live.cpp`, which had never been built on Linux before (ADR-118's own pass
predates ADR-119, so this file did not exist with its `cap::RunCommand` grant until now).

**The one thing ADR-119 could not check at all — `containerd_shell_chat.cpp` itself — was built for
real.** This build's default configuration has `AGENTENGINE_WITH_HTTPS=OFF` (the same as ADR-115/118's
own baseline), which gates both `agentengine_sandboxed_shell_chat` and `agentengine_containerd_shell_
chat` out of the default target set entirely. Reconfigured with `-DAGENTENGINE_WITH_HTTPS=ON` and built
both targets directly: **both compile and link clean**, with no MSVC-`C1128`-equivalent limit on GCC
(confirming ADR-119 §2's own claim that the COFF section-count fix was MSVC-specific and GCC needed no
analogous change). Reverted `AGENTENGINE_WITH_HTTPS` back to `OFF` afterward for an apples-to-apples
`ctest` comparison against ADR-118's own baseline.

**The real containerd-backed test passes completely, exercising the new grant end-to-end against a
real daemon.** `ctr version` confirmed a live, reachable containerd daemon in this distro (unlike
Docker — see below). Ran `tests/test_composed_containerd_providers_live` directly:

```
ALL CHECKS PASSED -- ComposedContextProvider<SandboxToolProvider,
MandatorySandboxProvider<ContainerdExecutionSurface>> composes as the REAL, production AgentSession's
actual HistoryProviderT, driven end to end through the real, unmodified session.start_run() ->
invoke_tool() 10-step pipeline, for BOTH a native-jail run_shell call and a containerd-backed
run_command call in one session -- the first real production use of
MandatorySandboxProvider<ContainerdExecutionSurface> through that pipeline anywhere in this codebase.
```

Every check passed, including `[2] run_command genuinely executed in a real containerd container
through the composed session's real invoke_tool() pipeline` — this is the exact call this ADR's own
`cap::RunCommand` grant (added to this test file by ADR-119) makes possible; without it, `invoke_tool()`
step 4/7 would reject the call before it ever reached the container. This is a COMPLETE, unconditional
proof, not narrowed by any environment gap the way the Docker-shaped tests below are.

**Full `ctest`: 182/182 total, 177 passed (97%) — identical total and identical 5 failures to ADR-118's
own baseline**, confirming zero regression from ADR-119's changes:

```
The following tests FAILED:
	168 - test_composed_sandbox_providers_live (Failed)
	176 - test_sandbox_runtime (Failed)
	177 - test_docker_orphan_reap (Failed)
	178 - test_mandatory_sandbox_provider (Failed)
	179 - test_task_branch_tools (Failed)
```

All five share the identical, already-diagnosed Docker-CLI-reachability root cause ADR-115/118 named
(`docker` on `PATH` resolves to the bridged Windows Docker Desktop binary, unreachable from the Linux
side) — this environment gap is completely unrelated to ADR-119's own subject and was not re-diagnosed
from scratch here, only reproduced.

**The part that actually matters for THIS ADR — `RunCommandTool`'s new capability ceiling — is verified
completely and unconditionally, independent of that Docker gap**, using the exact diffing methodology
ADR-118 established: ran `test_mandatory_sandbox_provider` directly and compared its real failure output
against every `check()` call site in the file (this test's own `check()` only prints on failure):

```
FAIL: a direct run_command invoke() succeeds
FAIL: session.history() contains a real role::tool message proving the command genuinely executed in
      a real container, driven end to end through session.start_run() -> the real, unmodified
      invoke_tool() 10-step pipeline -- never a direct accessor call
FAIL: the real Ledger checkpoint invoke_tool() committed contains the real file, read back
      independently of the tool's own reported reply
FAIL: sibling_a's own real command succeeds
FAIL: sibling_b's own real command succeeds
FAIL: sibling_a's own file is correct
FAIL: sibling_b's own file is correct
```

Every one of these seven failures requires a REAL container to actually run a command (direct-accessor
calls in section [2], sibling-isolation checks in section [4]) — none of them is ADR-119's own new code,
and none of them ever passes through `invoke_tool()`'s capability check at all (sections [2]/[4] call
`ToolDescriptor::invoke` directly, bypassing the ceiling by construction, exactly as ADR-119 §2 itself
documents). By contrast, section [3]'s OWN first check (`"start_run() driving a real run_command tool
call through invoke_tool() succeeds"` — now gated on holding `cap::RunCommand`) and ALL FOUR of section
[7]'s new fail-closed checks (`"run_command is rejected..."`, `"no real run_command call ever ran..."`,
`"a rejected-at-authorization call never reaches SandboxRuntime::run()..."`, `"the command never ran, so
no_capability.txt was never written"`) are absent from the failure list — meaning all five passed. This
is structural, the identical reasoning ADR-118 established for the task-branch case: the capability-
ceiling check itself happens at `invoke_tool()`'s step 4/7, strictly BEFORE `SandboxRuntime::run()` is
ever reached, so neither the "granted -> allowed" nor the "withheld -> rejected, zero side effects"
direction ever touches Docker.

`test_capability_enforcement` and `test_policy_reachability` (the two tests most directly exercising
the widened variant's general `CapabilitySet`/`subsumes()` machinery and the `capability_kind_name()`
arm) both **Passed** cleanly in the full suite run.

## 3. What this closes, and what it does not

**Closes for real, unconditionally**: ADR-119's own "no Linux verification" residual — the widened
`Capability` variant compiles correctly under GCC 14.2.0, `RunCommandTool`'s new double-gate enforcement
is proven end-to-end in BOTH directions through the real `invoke_tool()` pipeline using checks that do
not depend on Docker at all, AND — the one thing ADR-119 itself could not check on Windows —
`tools/containerd_shell_chat.cpp` (the third real production caller) both compiles clean and, via
`test_composed_containerd_providers_live`, is proven to genuinely execute a real `run_command` call
against a real containerd container carrying the new `cap::RunCommand` grant end-to-end.

**Does NOT close**: the same disclosed, pre-existing, environment-caused Docker-CLI-reachability gap
ADR-115/118 already named for this exact WSL2 distro — `test_mandatory_sandbox_provider`'s and
`test_task_branch_tools`'s own Docker-dependent checks (real command execution via `DockerExecutionSurface`)
remain unverified on Linux, for the identical reason and with the identical root cause, unrelated to
ADR-119's own subject.

## 4. What was NOT done

- No attempt to install or configure a real Linux-native Docker daemon in this WSL2 distro — same,
  separate environment decision ADR-115/118 already deferred, not revisited here.
- `tools/cli_chat.cpp` was not rebuilt in this pass — it is Windows-only in practice (gated on
  `AGENTENGINE_BUILD_PYTHON_RUNNER`, which this Linux checkout has never configured), matching every
  prior Linux-verification ADR's own scope; ADR-119's own Windows-side verification already covered all
  three of its provider branches directly.
- `AGENTENGINE_WITH_HTTPS` was toggled ON only long enough to build the two HTTPS-gated production
  binaries, then reverted to `OFF` for the `ctest` comparison against ADR-118's baseline — the ~24
  network-provider tests that become registered-but-unbuilt under `HTTPS=ON` in this partially-built
  configuration were not exercised (they are unrelated to ADR-119 and were never part of any prior
  Linux-verification ADR's own baseline either).

## 5. Residuals

- The Docker-dependent halves of `test_mandatory_sandbox_provider`/`test_task_branch_tools`/
  `test_sandbox_runtime`/`test_docker_orphan_reap`/`test_composed_sandbox_providers_live` remain
  unverified on Linux — unchanged, pre-existing, environment-caused, not this ADR's own gap to close.
- Every other residual named by ADR-108 through ADR-119 remains unchanged — this ADR closes only
  ADR-119's Linux-verification gap.
