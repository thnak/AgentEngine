# Issue #180 -- sandbox-critical conversions onto `rt::` (design pass, read-only)

Scope: the shell's `RegisteredRunner::invoke`, the process spawners, and the ADR-011 egress proxy. Nothing was
edited, built, or run; every claim below comes from reading the tree at branch `wave5-183b5-184-breaker`
(HEAD `c6af8f90`). Items I could not verify are listed in section 9. This pass is the **design** step of
`design -> red-team -> prove -> judge`; section 6 is written to be handed to a red-team.

## 0. Summary and three corrections to the task framing

1. **`RegisteredRunner::invoke` blocks nothing and has no production caller.** The shell is a synchronous
   tree-walking interpreter that spawns no process. The production session wiring builds an *empty* registry
   (`src/backends/native_jail/session_shell_wiring.hpp:163-164,204`, `registry_()` with no `register_*` call); a repo-wide
   grep for `register_runner`/`RegisteredRunner` outside `tests/` finds only the two definitions
   (`command_registry.hpp:46`, `mediated_command_registry.hpp:36`) and comments. The only registrations are in
   tests, and the one that composes Python does `rt::block_on(python.run(...))` inside the closure
   (`tests/backends/native_jail/test_mediated_shell_runner_python_composition.cpp:80-83`). Converting it is
   only worth doing *after* the Python worker session is async (slice E6), otherwise it is a `task` around a
   blocking callee. It should be the **last** slice, not the first.
2. **`HostEgressProxy` is not a listener and has no thread-per-connection.** `net_egress_proxy.cpp` is a
   blocking HTTP(S) **client**: gates, `resolver(host, port)` once, then `tcp_connect` + `select()` read loop
   (`net_egress_proxy.cpp:747-806`, `:419-486`, `:73-84`). There is nothing to "stay a dedicated thread". Its async twin already exists:
   `rt::http_request` (`include/agentengine/rt/http.hpp`) re-implements the same ADR-011 resolve-once /
   connect-to-verified-address rule through `rt::connect_resolved` + an `AddressPolicy`. The conversion is a
   policy-translation problem (IPv4-only blocked-range table -> `AddressPolicy` over `pal::IpAddress`, which is
   IPv4 **and** IPv6), not a threading one. One caller genuinely cannot be async: the WASM `http-request` host
   import (`wasm_backend.cpp:565-567`), where the guest is blocked inside the call.
3. **No spawner honours cancellation today, and `rt::run_process` has zero production users.** None of
   `native_jail_backend.cpp`, `native_process_spawn.cpp`, `docker_execution_surface.cpp`, `kata_backend*.{cpp,hpp}`
   reads `ctx.cancellation` (`effect_context.hpp:215`) or `ctx.deadline` (`effect_context.hpp:55`) -- `NativeJailBackend::exec_blocking`
   even leaves its `EffectContext&` unnamed (`native_jail_backend.cpp:228-229`), and `ExecutionSurface::run`/`drain_to` take no
   `EffectContext` by design (`execution_surface.hpp:31`). So "kill-on-cancel" is a **new** property, not an existing one to
   preserve, and it needs seam changes (a stop token reaching `ExecutionSurface`, `PersistentShellSurface`,
   `NetEgressBackend::fetch`). `rt::run_process` (`rt/process.hpp:110`) is exercised only by
   `tests/rt/test_rt_reactor_process.cpp` (P1-P19); and `pal::ProcessSpec` (`pal/reactor_process.hpp`) cannot express several things the
   jail needs (AppContainer attributes, Job resource limits, extra pipe pairs, `CREATE_SUSPENDED` window, `clone()` namespaces).

Recommended order (section 7): **E1** egress (async `fetch` on `rt::http`, `fetch_blocking` kept as a documented sync edge sharing one pure gate) ->
**E2** a shared argv-runner on `rt::run_process`, re-pointing the 7 hand-rolled copies (Docker x4, containerd, kata `run_ctr`, ...) -> **E3**
`pal::ProcessSpec` resource limits + `native_process` -> **E4** native-jail one-shot `exec` -> **E5** poll loops -> **E6** Python worker session
(needs its own ADR) -> **E7** `RegisteredRunner::invoke` + shell evaluator as coroutines. Two things should **not** be converted:
the WASM epoch ticker thread and the Linux `clone()` jail's poll loop (offload-with-polled-stop instead); see section 8.

## 1. Execution context: who calls these, from where

- Every converted-seam conformer today is `co_return xxx_blocking(...)` -- a coroutine that completes **without suspending**, so the
  blocking body runs on whatever thread resumed the engine coroutine (a lane, or the `block_on` thread at an edge). ADR-237 §14 step 50 states this
  explicitly: "Moving those bodies onto `rt::process` / `rt::http` is #180's ... a conformer that blocks a lane is the named residual"
  (`ADR-237` lines ~2623-2625). Examples: `ShellRunner::run` (`shell_runner.hpp:~34-36`), `MediatedShellRunner::run` (`mediated_shell_runner.hpp:53-57`),
  `MediatedPythonRunner::run` (`mediated_python_runner.hpp:138-141`), `HostEgressProxy::fetch` (`net_egress_proxy.hpp`, `co_return fetch_blocking(req, granted)`).
- The tool bodies that reach spawners are `make_sync_invoke` lifts (`tool_descriptor.hpp:181-185`, "transitional", e.g. `native_providers.hpp:~236-247`
  for `spawn_native_process`) or Phase-D `co_await surface.run(...)` (`sandbox_runtime.hpp:201,289`; `mandatory_sandbox_provider.hpp:1013`).
- Whether a lane holds `AgentSession::session_mutex_` for the duration is ADR-237 §4.5 rule 3's claim (`agent_session_core.cpp:102` per the ADR text); I did not
  re-verify the call site. If true, a blocking exec stalls both a lane **and** every other entry point of that session -- the cost the conversion buys back.
- **Latent hazard that bounds the order of work.** `block_on` is refused on threads marked `ScopedBlockOnRefusal`: today only `rt::offload` workers
  (`block_on.hpp:~66-72`, `offload.hpp:42-44`), but the header says "a lane worker later". Production sandbox code already calls `block_on` from sync edges:
  `tool_bridge.hpp:77` (every Python `agent.tools` call, inside `dispatch_worker_query` inside `exec_session`),
  `native_shell_session_provider.hpp:276` (`open()` quota charge), `skill_mount_materializer.hpp` (config time). The day lanes are marked, the Python tool
  bridge throws `rt.block_on_on_offload_worker`-class refusals. This is also why **wrapping `exec_session` in `rt::offload` is not an option** for the Python
  worker: the body would call `block_on` on an offload worker and be refused. E6 therefore needs a real async design, not a pool hop.

## 2. Component A -- the shell and `RegisteredRunner::invoke`

### 2.1 Inventory

| What | file:line | Note |
|---|---|---|
| `RegisteredRunner{ name/runner_name, std::function<result<ExecOutcome>(ExecRequest const&, ExecState&, EffectContext&)> invoke }` | `command_registry.hpp:~43-47`; `mediated_command_registry.hpp:~34-37` | sync closure; two registries (legacy `native_jail::` and `native_jail::mediated_shell::`) |
| `RegisteredTool::invoke` (argv, ctx) | `command_registry.hpp:59`; `mediated_command_registry.hpp:46` | same conversion class; triage rows 51, 53 (`ADR-237` ~3183-3185) |
| call sites | `shell_dispatch.cpp:~270` (runner), `:288` (tool); `mediated_shell_dispatch.cpp:292` (runner), `:295` (tool) | inside `dispatch_command`, called from `evaluate_pipeline` (`mediated_shell_dispatch.cpp:312,339`) |
| evaluator recursion | `evaluate_pipeline` :312 -> `evaluate_and_or` :390 -> `evaluate_statement(s)` :413/:456 (`std::visit` lambda) -> `evaluate` :472 | all synchronous; the `deadline` is a plain `steady_clock` value checked once per statement (`:421`) |
| the interpreter itself | `MediatedShellRunner::run_blocking` `mediated_shell_runner.hpp:60-96` | measures the deadline once per `run()` (`:72`), caps output (`:88-91`) |
| the only real blocking callee | `MediatedPythonRunner::run_blocking` -> `backend_.exec_session` (`mediated_python_runner.cpp:84-94`) | see E6 |

The legacy `shell_dispatch.cpp` has no production call site at all (its own comment, `shell_dispatch.cpp:~288-305`); the live path is `mediated_shell_dispatch.cpp`. The shell
builtins (`cd pwd ls cat echo export mkdir rm mv cp`) touch only the injected `FileSystemAdapter`; `test_*_no_process_creation.cpp` pins that no process is created.

### 2.2 What conversion means

`invoke` becomes `std::function<task<result<ExecOutcome>>(ExecRequest, ExecState&, EffectContext&)>` (request by value, per the step-50 convention); the dispatch chain becomes
coroutines: `dispatch_command`, `evaluate_pipeline`, `evaluate_and_or`, `evaluate_statement(s)`, `evaluate`, and `run_blocking`'s body. Concrete constraints from the ADR's own rules:
- **No capturing-lambda coroutines** (ADR-237 §14 rule). `evaluate_statement` dispatches through `std::visit` with a capturing generic lambda (`:427-455`); it must become a
  free coroutine selecting on `std::get_if` / an `if constexpr` chain over named free functions.
- `registry`, `fs`, `state`, `ctx`, `locals` are borrowed references today; each becomes a reference that must outlive the task. `locals` is passed by `const&` to a
  per-iteration `loop_locals` copy (`:441-446`) -- under a coroutine the copy must live in the frame, not a temporary.
- The wall-clock `deadline` keeps its current meaning (checked per statement). It does **not** become a timer race: an awaited runner can overrun it exactly as a blocking one
  can today, and the callee has its own bound (`wall_ms`). Adding a timer here is a behaviour change, not part of this slice.
- The `RegisteredTool::invoke` rows are a *separate, still-unsafe* seam (ADR-052: bypasses the tool pipeline, `shell_dispatch.cpp:~288-305`). Converting its signature must not be
  read as wiring it; the existing comment forbidding direct invocation stays.

### 2.3 Properties that must not change (shell side)

| Property | Enforced at | Pinned by |
|---|---|---|
| Runner invocation needs `RunnerCall{name}` capability, checked **before** `invoke` | `mediated_shell_dispatch.cpp:~279-281` (`require_capability(... cap::RunnerCall{runner_name})`); legacy `shell_dispatch.cpp:~245-249` (`has_capability`) | `test_mediated_shell_runner_python_composition.cpp` E3-PY3 (denied call, then a later NameError proves CPython never ran); `test_shell_runner_proof.cpp` (Sh-G4 area) |
| The runner receives the **identical** `ExecState&` and `EffectContext&` (no attenuated-then-widened copy) | `mediated_shell_dispatch.cpp:~288-292`, `shell_dispatch.cpp:~268-272` | E3-PY2 (shared `ExecState` round-trip) |
| Per-`run()` wall-clock budget bounds nested loops (ADR-100 F3) | `mediated_shell_dispatch.cpp:421-425`; deadline set once `mediated_shell_runner.hpp:72` | `test_mediated_shell_runner_wall_clock_timeout.cpp` WCT-1..3 |
| `shell.capability_denied` etc. are hard stops; command-not-found is an ordinary failure | `mediated_shell_dispatch.cpp:~345-358` (`kHardStopCodes`) | `test_mediated_shell_runner_smoke.cpp`, hostile corpus |
| Output caps applied at the boundary | `mediated_shell_runner.hpp:88-91` | smoke / hostile corpus |
| Reserved-name registration refusal | `*_command_registry.hpp register_runner` | `test_shell_runner_proof.cpp` (shadow_ls) |

### 2.4 Risks (adversarial)

- **A hurried conversion that `co_await`s the runner and forgets the capability check ordering**: the check and the invoke are two adjacent statements; an `await` inserted between them
  (e.g. to "prepare" the request) opens a window where `ctx.capabilities` could be mutated by a sibling task. `ctx` is forwarded unchanged by contract (Sh-G4), so any copy-then-await would break it.
- **Frame lifetime of borrowed `ExecState&`/`ctx`**: a stop that cancels the shell task while a runner is suspended destroys the frame; the runner (Python, E6) must not keep touching `state`/`ctx`.
  ADR-237 §4.5 rule 3's chain-wide in-flight count is documented as not fully built (step 50 / `ADR-237:2883` per the grep); do not rely on it.
- **`std::visit` -> coroutine refactor drops the `deadline` check** (it is only in `evaluate_statement`); WCT-2 is the tripwire, keep it.
- **Silent loss of fail-fast**: `evaluate_statements` returns on the first `!r` (`:463-465`); an `co_await` in a loop that swallows the error is easy to write.
- **Nested `block_on` left behind**: if `invoke` is made async but the registered Python closure still does `rt::block_on` (as the test does), the combination only works on unmarked threads.

### 2.5 Gate

The existing `RunnerCall` negative control (E3-PY3) and WCT-2 must pass unchanged on the `task`-shaped registry. New falsifiable gates: (a) a runner that suspends (a test runner awaiting `rt::sleep_for`) is
resumed on the right home and the shell's output/ExecState are intact; (b) mutant: move the capability check after the `co_await` of a parked sub-runner -> E3-PY3 must fail; (c) mutant: drop the per-statement deadline check in the coroutine
evaluator -> WCT-2 must fail. Windows-provable alone (no process involved; `MediatedPythonRunner` composition test needs the embedded Python, which the repo builds on Windows).

## 3. Component B -- process spawners

### 3.1 Inventory

Seven distinct hand-rolled "spawn, drain with a cap, enforce a wall deadline, kill" implementations plus three poll loops and two helper threads. `rt::run_process` replaces the first group's core loop;
the rest need primitives `pal::Process` does not have.

| # | Spawner | Platform | file:line | Blocking calls on the path | Called from |
|---|---|---|---|---|---|
| B1 | `spawn_native_process` | Win | `native_process_spawn.cpp:163-258` | `CreateProcessW` `:209`; `Job::wait_or_kill` (`WaitForSingleObject`) `:243`; `drain_pipe_bounded` (`ReadFile`) **after** the wait `:252-254`; `TerminateProcess` `:231,236` | `native_providers.hpp:213` inside a `make_sync_invoke` (`:~236-247`) |
| B2 | `NativeJailBackend::exec_blocking` | Win | `native_jail_backend.cpp:228-402` | `CreateProcessW` `:344` (CREATE_SUSPENDED, AppContainer, HANDLE_LIST); `assign_process` `:362`; `ResumeThread` `:368`; `wait_or_kill` `:370-371` (`job_object_limits.cpp:160`); `drain_pipe_bounded` `:376-377` | `SandboxBackend::exec` seam |
| B3 | `JobObjectLimits::wait_or_kill` | Win | `job_object_limits.cpp:150-213` | `WaitForSingleObject(wait_ms)`, then `TerminateJobObject` + `WaitForSingleObject(INFINITE)` `:198`; `wall_ms==0` -> `INFINITE` `:159` | B1, B2 |
| B4 | Python worker: `create_python_worker` | Win | `native_jail_backend.cpp:574-826` | `CreatePipe` x2 (non-overlapped) `:~645`; `CreateProcessW` `:700`; **`std::thread` watchdog** `:729`; `WaitForSingleObject` | `MediatedPythonRunner::initialize` |
| B5 | Python worker: `exec_session` | Win | `native_jail_backend.cpp:934-1060` | `FramedChannel::send/recv` = blocking `WriteFile`/`ReadFile` (`jailed_worker_rpc.cpp:21,39`) in a `while(!got_final)` loop `:983-1030`; `try_lock` single-flight `:954` | `MediatedPythonRunner::run_blocking` `mediated_python_runner.cpp:94` |
| B6 | Python worker host services run *inside* the B5 loop | Win | `dispatch_worker_query` `:828-932`: `bridge_tool_call` -> `rt::block_on(invoke_tool)` (`tool_bridge.hpp:77`); `dispatch_connect_authorize` `native_jail_handle_relay.cpp:~335-384` (`resolve_and_validate`, blocking `tcp_connect`, `select` 5 s `:~372-374`); `dispatch_connect_send/recv` (`pal::send_some/recv_some`); file relay `ReadFile` `:250` | called synchronously per worker frame |
| B7 | session watchdog loop | Win | `native_jail_backend.cpp:511-572` (thread declared `native_jail_backend.hpp:299`) | polls `poll_memory_limit_once` + `query_usage` every 5 ms (call) / 100 ms (idle); `WaitForSingleObject(stop_event, interval)` `:570`; joined in `stop_watchdog` `:475-481` | own thread (not detached) |
| B8 | `LinuxNativeJailBackend::exec` | Linux | `linux_native_jail_backend.cpp:~395-520` | `clone()`+`CLONE_NEW*`, cgroup attach `:~427`; poll loop `waitpid(WNOHANG)` + `sleep_for(5ms)` `:~436-463`; `kill(SIGKILL)+waitpid` `:~455-456`; drain **after** wait | `SandboxBackend::exec` |
| B9 | `DockerExecutionSurface` process helpers | Win+Linux | `docker_execution_surface.cpp`: Win `run_capture` `:35-148`, Win `run_argv` `:204-333` (`CreateProcess`, `PeekNamedPipe`+`WaitForSingleObject(20)` loop, `TerminateJobObject` `:129,306`); POSIX `run_capture` `:334-415` (`posix_spawn`, `poll`, `kill+waitpid` `:395-398`), POSIX `run_argv` `:416-500` | `reset/run/drain_to_blocking` |
| B10 | `ctr` runners | Linux | `kata_backend_detail.hpp:103-290` (`run_ctr`: `posix_spawnp`, `poll` `:190`, `kill`/`waitpid` `:257-271`); `containerd_execution_surface.hpp:92-324` (`ctr_cli_detail::run_argv`, `:112`) | `KataBackend::create/exec/destroy_blocking` (≥ 25 `run_ctr` sites `kata_backend.cpp:97-1071`), `ContainerdExecutionSurface` |
| B11 | `NativePwshSession` | Win | `native_shell_session.cpp`: `open` startup poll `sleep_for(10ms)` `:401`; `send` poll `exists(x)`/`WaitForSingleObject(proc,0)`/`sleep_for(5ms)` `:498-507`; `kill_job` `:275-281` | `native_shell_session_provider.hpp` (`block_on(try_consume)` `:276`) |
| B12 | `DockerPersistentShellSurface` | Win+Linux | `docker_persistent_shell_surface.cpp:242` `sleep_for(200ms)` in a `ps`-count wait | `close/open` bodies |
| B13 | Kata egress verify retry / flock | Linux | `kata_backend.cpp:896` `sleep_for(200ms)`; `egress_policy_host.cpp:226` `sleep_for(20ms)` in a 30 s `flock` loop | host-side policy install/verify (`nft`/`tc` via `runner_`) |
| B14 | WASM epoch ticker | any | `wasm_backend.cpp:78-85` `std::jthread` (10 ms tick, `kEpochTickMs` `:36`), joined in `~SharedEngine` `:~61-66` | process-global singleton |

### 3.2 What `rt::process` offers and what it lacks

Offers (verified in `rt/process.hpp`, `pal/reactor_process.hpp`): `run_process(reactor, ProcessSpec, stop, ProcessRunLimits{output_cap_bytes, wall, exit_drain_grace}, stdin_bytes)`; concurrent drain of
stdout/stderr (closes H-e); Job at creation (`PROC_THREAD_ATTRIBUTE_JOB_LIST`, KILL_ON_JOB_CLOSE) / own process group on Linux; exact `HANDLE_LIST`; explicit env (`inherit_environment=false` default); stop and wall
deadline kill the tree and still wait for the exit; destroying the frame kills. Tests: `test_rt_reactor_process.cpp` P1-P19 (e.g. P7 stop kills, P8 grandchild gone, P9 wall deadline, P10 drain grace, P11 frame destroyed, P13 reactor shutdown).

Lacks (each is a real gap, not a nicety):
1. **Job resource limits** (memory, CPU time, active-process count) and the **memory-limit notification** (`JOB_OBJECT_MSG_JOB_MEMORY_LIMIT` via a completion port, `job_object_limits.cpp:~115-127,~226`). `native_process`
   and the jail depend on these (`native_process_spawn.cpp:222-236`; `native_jail_backend.cpp:383-398` oom classification).
2. **AppContainer** `SECURITY_CAPABILITIES` and `CHILD_PROCESS_POLICY` attributes (`native_jail_backend.cpp:281-327`), and the `CREATE_SUSPENDED` -> assign -> resume window (`:344,:362,:368`). `rt::process` assigns the job at creation, which is
   strictly better for containment, but cannot carry the AppContainer token.
3. **Extra pipe pairs.** The Python worker talks over two private anonymous pipes (`native_jail_backend.cpp:~645-665`, handle list of exactly `{down_read, up_write}`), not stdio -- the guest's `print` owns stdout.
   `pal::Process` models stdin/stdout/stderr only.
4. **Linux namespaces/cgroups/pivot_root** (`clone` with `CLONE_NEWPID`, `setup_jail`, cgroup attach): `pal::Process` uses `pidfd_spawn`/`posix_spawn` and cannot express it.
5. A **Reactor handle** at the conformer. `HostEgressProxy`, `NativeJailBackend`, `DockerExecutionSurface` etc. are constructed without one (`NativeJailBackend backend;` in tests; `HostEgressProxy{}` in
   `read_content.hpp:~236` and `wasm_backend.cpp:566`). The established answer is step 57's: a trailing borrowed `pal::Reactor*`, lazily creating a private one on first use when null (`ADR-237` step 57), i.e. client-owned.
6. A **stop token at the seam** for `ExecutionSurface`/`PersistentShellSurface`/`NetEgressBackend` (no `ctx` there). A stop token is not authority (I2/I3 unaffected); it is an additive parameter.

### 3.3 Properties that must not change (spawners)

| Property | Enforced at | Pinned by |
|---|---|---|
| **No ambient handles** into the child: exact `HANDLE_LIST` (AC-S1: a zero-capability child once read an unrelated inherited secret) | `native_jail_backend.cpp:~285-300,:~314-322` (exec), `~660-672` (worker); `native_process_spawn.cpp` (handle list); `rt::process` does this itself (P6) | `test_native_jail_ambient_authority_windows.cpp` (labels not read); `test_rt_reactor_process.cpp` P6 |
| **No host stdin**: explicit NUL handle (2026-08-23 red-team BLOCKING) | `native_jail_backend.cpp:~270-282` | `test_native_jail_backend_windows.cpp` / ambient-authority tests (not verified which label) |
| **Minimal environment block**, never `nullptr` (008 §9 G3, I2) | `native_jail_backend.cpp:~337-340` (`build_minimal_environment_block`); `native_process_spawn.cpp:53` | `test_native_jail_ambient_authority_windows.cpp` |
| **Child process creation denied** (`PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`) | `native_jail_backend.cpp:~302` | abuse corpus (`test_native_jail_abuse_corpus_windows.cpp`) |
| Job assigned **before the entry point runs** so every limit applies from instruction 1 | `native_jail_backend.cpp:344-368` (CREATE_SUSPENDED) | `test_job_object_limits.cpp` (memory, job-time); C2 `exceeding memory_bytes reports oom` |
| **Wall-clock kill of the whole job**, then reap | `job_object_limits.cpp:~185-210` | C2 `a CPU-spinning exec is killed by wall_ms and reports timeout`; `test_native_process_spawn.cpp` R-S4 (`hung process is classified as timeout`, `promptly`); `test_job_object_limits.cpp::test_wall_clock_kill_standalone` |
| **Output cap** (bytes beyond cap discarded, never unbounded host memory); `cap=0` still has a 16 MiB floor | `native_jail_backend.cpp:154-173`; `native_process_spawn.cpp:85-110`; `rt::process` `output_cap_bytes` (P4) | C2 stdout capture; abuse corpus C3; P4 |
| **oom classification** from a real kernel signal, with a 90 % peak-usage fallback | `native_jail_backend.cpp:383-398`; `job_object_limits.cpp:~205-215` | C2 `exceeding memory_bytes reports oom`, `an ordinary nonzero exit reports crash, not oom (positive control)` |
| **Command-line quoting is injection-safe** | `native_process_spawn.cpp:121-160`; `docker_execution_surface.cpp:168-201` | `test_native_process_spawn.cpp` Q1, Q2, R-Q3 |
| **Empty `cwd` rejected**, never defaults to host cwd | `native_process_spawn.cpp:~163-180` | `test_native_process_spawn.cpp` S1 |
| **Single-flight per session** and **no silent respawn** of a killed worker | `native_jail_backend.cpp:~945-958` (`try_lock`, `session_terminated`) | `test_native_jail_python_worker_slice1.cpp` G-lifecycle-2 (`fails fast`, `session_terminated`) |
| **exec_seq binding**: a `worker_query`/`exec_response` with the wrong seq terminates the worker, never dispatched (RT1 F1) | `native_jail_backend.cpp:~1003-1018,1038-1048` | `test_native_jail_python_worker_slice1.cpp` (labels beyond G-Q1/G-lifecycle-2 not read) |
| **Kill reason comes from the watchdog, never from the worker's own claim** (I3) | `native_jail_backend.cpp:~994-1003` | G-Q1 (`infinite busy-loop is classified as timeout`) |
| **Idle-phase CPU budget** kills a guest daemon thread (RT2 F1) | `native_jail_backend.cpp:537-557` | `test_native_jail_python_worker_slice1.cpp` (not verified which label) |
| **Teardown**: orderly shutdown handshake then KILL_ON_JOB_CLOSE as backstop; destroy of a destroyed handle fails closed | `native_jail_backend.cpp:404-439` | C2 `exec() on a destroyed SandboxHandle fails closed`; `test_native_jail_teardown_cycles_*` |
| **Python host services keep their gates**: connect needs `NetOut`, `resolve_and_validate`, port range, max sockets | `native_jail_handle_relay.cpp:~335-366` | `test_native_jail_python_worker_handle_relay.cpp` SOCK-1.. (only SOCK-1 read) |
| ExecRequest mediation / `cap::Exec` gate decisions are **upstream** of the spawners (ADR-094) | `ExecRequest` mediation layer | not touched by these slices; keep it that way |

### 3.4 Risks (adversarial -- assume a hurried converter)

1. **Order of limit application.** B2 resumes the thread only after `assign_process` (`:362-368`). A conversion to `rt::run_process` that spawns *then* applies limits (or applies none, because `ProcessSpec` has none) lets the guest run
   unlimited. `rt::process`'s "Job at creation" is safe only if the limits are part of that Job's creation, not a later call. Gate: a memory-hog child must be oom-classified with no head start.
2. **Silent loss of the AppContainer token.** If E4 is shipped before `ProcessSpec` can carry it, the guest runs at the host's integrity level and every functional test still passes (the guest just *works*). Positive control needed:
   a child that reads a path outside the granted mounts must be denied (the abuse/ambient corpora). `ProcessSpec` must **fail closed** when a requested platform restriction is unsupported (never ignore it).
3. **Behavioural change in output-overflow classification.** B2/B1 drain *after* the wait; a child writing more than the 1 MiB pipe buffer blocks until `wall_ms` kills it and is reported `timeout` (the code comment at
   `native_jail_backend.cpp:~250-256` states it). `rt::run_process` reads concurrently (P3/P4): the same child now exits normally with `truncated`. That is the intended H-e fix but it changes a classification some corpus tests
   may encode (abuse corpus "unbounded output"). Treat as a deliberate, reviewed delta, not a silent one.
4. **Cancel leaves the child running** (new property). `NativeJail`'s caller currently *cannot* cancel. After conversion a `stop` must kill the **job**, not just stop reading. `OpAwaiter` cancel (`rt/process.hpp`) cancels *one operation*; kill-on-stop is `run_process`'s join policy, so
   any code that composes the per-operation awaitables itself (as E6 would) must re-implement it. Make `run_process`-style join the only path for one-shot spawns.
5. **Stop callback racing teardown.** `NativeJailBackend::destroy_blocking` erases the `Instance` (and its `JobObjectLimits`, whose destructor is the KILL_ON_JOB_CLOSE backstop) (`:404-439`). If a conversion keeps a task suspended inside `exec` while `destroy`
   runs on another lane, the `Instance` dies under the task. Today a lane-blocking `exec` makes that impossible by accident. ADR-237 §4.5 rule 3/3a (owners close asynchronously, in-flight count) is the stated answer but the ADR itself records the chain-wide count as not fully built. Gate: concurrent `exec` + `destroy`.
6. **Watchdog vs reactor interaction (E6).** The watchdog kills via `TerminateJobObject` and sets `ws.alive=false`/`kill_reason` (`:520-557`); `exec_session` classifies from `kill_reason` when the pipe breaks (`:994-1003`). If an async read observes EOF and the classification reads `kill_reason`
   before the watchdog stored it, a timeout becomes `crash`. Today the order is guaranteed because the same terminate that closes the pipe is the thing that stored the reason *first* (`:~522-524`: terminate, then store -- note this is already store-after-terminate; verify the race is benign: the reader's `recv` returns only after the worker is dead,
   which is after `TerminateJobObject` but possibly before `kill_reason.store`). **This looks like an existing race I could not rule out** (section 9, item 1).
7. **Process-tree containment differs by platform.** `rt::process` on Linux uses a process group; the docs say a `setsid` grandchild escapes (`pal/reactor_process.hpp` CONTAINMENT note; "the jail's job, cgroups"). The Docker/containerd/Kata runners are host-side CLI wrappers where the real containment is the daemon, so a group kill of the *CLI* is
   unchanged in meaning (and the existing code already knows it only kills the wrapper: `kata_backend.cpp:920-945`).
8. **`kill` of the CLI does not kill the guest** (`kata_backend.cpp:920-945` documents it; a `ctr tasks kill --exec-id` follow-up exists). A cancel path must run that follow-up too, on a **fresh** stop-immune call -- otherwise the first real cancel orphans guest work.
9. **Reactor lifetime.** `run_process` "must finish before its reactor is destroyed" (ADR step 2 note, documented not enforced). A lazily-created private reactor owned by a backend that is destroyed while a call is parked is the exact use-after-free the ADR warns about.
10. **Thread-hostile globals.** `run_ctr` takes care *not* to mutate `environ` ("not thread-safe against concurrent `posix_spawn`", `kata_backend_detail.hpp:~100`); `rt::process` passes an explicit env (good), but concurrent spawns now really happen on the reactor thread -- the 64-spawn test P5 exists; keep it in the gate set.

## 4. Component C -- the ADR-011 egress proxy

### 4.1 Inventory

| What | file:line | Blocking? |
|---|---|---|
| `HostEgressProxy::fetch` -> `co_return fetch_blocking` | `net_egress_proxy.hpp` (seam), `net_egress_proxy.cpp:747-806` | completes without suspending |
| gates C1 (single-target grant), C3 (scheme), C9 (CRLF), C7 (method) | `net_egress_proxy.cpp:752-795` | pure, synchronous -- stay synchronous |
| resolve (`resolve_and_validate`, IPv4 only, `getaddrinfo`) | `:318-386`, called `:797` via `resolver` | **blocking `getaddrinfo`**, uncancellable (ADR-237 §1 table row 001 §5) |
| `perform_http_exchange` / `perform_https_exchange` | `:419-486` / `:495-558` | `tcp_connect` + `wait_ready` (`select`, `:73-84`) + `recv_some`; idle bound `io_timeout_ms()` (90 s default); **no overall deadline** |
| streaming twins | `:667-707`, `:711-743` | provider plane has already moved off these (`rt/http.hpp` header) |
| callers | tools via `co_await proxy.fetch(req, *granted)` (`read_content.hpp:299`, `extract_pdf_text.hpp:217`, other `extract_pdf_*`); WASM `fetch_blocking` `wasm_backend.cpp:565-567`; Kata/native-jail reuse `resolve_and_validate` only (`kata_backend.cpp:~239`, `native_jail_handle_relay.cpp:361`) | |
| `net_bytes` accounting | `narrow_by_resource_limit()` narrows the grant's `byte_cap` by `ResourceLimits::net_bytes` **before** the call (`wasm_backend.cpp:~552-560`) | **per-response**, not cumulative; cumulative `net_bytes` enforcement exists only in the Kata nft quota (`egress_policy_render.cpp:171-192`). So there is no cumulative host-side accounting in this proxy to preserve -- I verified by grep that no other code accumulates it. |
| thread usage | none | -- |

### 4.2 What conversion means

`fetch` (the async seam) becomes `co_await rt::http_request(HttpContext{reactor, dns_pool, policy, tls, options}, request, stop)`; the gates run first, unchanged and shared; `resolver` (the test seam returning an IPv4 `VerifiedEndpoint`) is replaced by
`rt::HostResolver`/policy (`dns.hpp` "TEST SEAM" note). `resolve_and_validate`'s "validate" half is a pure blocked-range check (`is_blocked_address`, `:288`) -> an `AddressPolicy` (`std::function<bool(pal::IpAddress const&)>`, `rt/dns.hpp:99`).
`fetch_blocking` stays for the WASM import (section 8) but **must share the gate with the async path** so they cannot drift (the ADR-237 "uncallable twin" lesson, step 57).

### 4.3 Properties that must not change

| Property | Enforced at | Pinned by (`tests/sandbox/test_net_egress_proxy.cpp` unless noted) |
|---|---|---|
| C1 exactly one allowlist entry; grant, not request, names host/port/scheme | `:752-758` | `a NetOut grant with zero/two allowlist entries is rejected` + `resolver was never called` |
| C3 http always, https only with TLS vendored | `:763-777` | `an https allowlist entry is rejected when no TLS client is vendored` / `the resolver IS called ... once a TLS client is vendored`; `tests/sandbox/test_https_egress.cpp` |
| C9 CRLF in method/path/header name/value rejected **before any network activity** | `:780-785`, `reject_crlf :295` | `CRLF in path/method/header ... is rejected`, `resolver was never called for any CRLF-bearing request` |
| C7 method restrictions (case-insensitive) | `:787-795` | `a method outside method_restrictions is rejected`; `... resolver WAS called once the method passed` |
| C4/C5/C6 blocked ranges incl. numeric-encoding bypasses; **resolve once, connect to the verified address** (no second lookup) | `is_blocked_address :288`, `resolve_candidates :318-350`, connect `:430`, `:507` | the range matrix (`loopback ... 0.0.0.0/8 is blocked`; `8.8.8.8 is NOT blocked`), `2130706433`/`0177.0000.0000.0001`/`0x7f000001` literals, `fetch() with the real resolver refuses a loopback target` (+ `positive control: direct connect to the loopback target succeeds`); `test_provider_egress_address_policy.cpp` |
| C8 byte cap enforced **during** the read; hard 16 MiB ceiling even with no declared cap | `:423,:462,:478` | `a response exceeding a small declared byte_cap is aborted mid-stream`; `... hard ceiling is aborted even with no declared byte_cap` |
| C10 no redirect followed | whole file (no redirect logic) | `a 3xx response is returned successfully, not treated as an error` |
| chunked bodies dechunked, not left raw | `dechunk_body :158`, `dechunk_response_body_if_needed :246` | ADR-011 chunk test (label not read) |
| audit/error codes (`net.address_blocked`, `net.byte_cap_exceeded`, `net.connect_failed`, ...) consumed by the WASM mapping | `wasm_backend.cpp:~569-590` | wasm tests (not read) |

### 4.4 Risks (adversarial)

1. **IPv6 is the headline risk.** `is_blocked_address` takes an IPv4 host-order `uint32_t` (`:288`) and `resolve_candidates` is IPv4-only (`:318`; the ADR calls out "IPv4 only"). `rt::connect_resolved` hands the policy a `pal::IpAddress`
   that can be IPv6, and the only `AddressPolicy` in the tree is `accept_all()` (`src/protocol/sse_stream_pump.hpp:34`). A policy written as "if it isn't in my v4 table, allow" lets `::ffff:127.0.0.1`, `::1`, `fe80::`, NAT64 `64:ff9b::/96`, 6to4,
   and an AAAA record pointing at a private v4 straight through -- an SSRF regression introduced by a conversion that passes every existing test (all v4). **Required:** the guest policy rejects every non-IPv4 address (fail closed, preserving today's behaviour) until a v6-aware table with its own matrix exists.
2. **The cap changes meaning.** The proxy caps the whole response buffer (head + body, `buf.size() >= effective_cap`, `:461`); `rt::http` caps **body wire bytes** with head limits separate (`rt/http.hpp` header). A guest-granted `byte_cap` of N now admits N body bytes plus headers. Decide and document; flood tests must run against both.
3. **No stop token reaches the seam.** `NetEgressBackend::fetch(req, granted)` has no `ctx` (`net_egress_proxy.hpp` concept); the issue text itself says "the egress proxy passes no stop token today". The seam needs a `stop_token` parameter, and the 8 call sites (3 tools + helpers) pass `ctx.cancellation`. Forgetting to thread it
   compiles and silently reproduces the bug.
4. **No overall deadline today** (only per-wait idle, 90 s). A trickling peer pins a lane indefinitely. `rt::http` has `HttpOptions::total_timeout` default 0 = none -- a conversion that leaves the default keeps the hole; set a non-zero bound (an I8 gain, and a behaviour change to call out).
5. **TLS hostname verification** must remain against the *original* host name, not the address (`perform_https_exchange`'s `host_header` doubles as the SNI/verify target, `net_egress_proxy.hpp` comment). `rt::http` says it keeps this (`HttpContext::tls`); the guest path must pass the allowlist host, never the resolved address. Test with a cert-name mismatch.
6. **`Host:` header now carries the port** (rt::http RFC 9110 §7.2) and rejects caller-supplied Host/CL/TE/Connection (stricter). A guest that sets `Content-Length` was forwarded unchecked before; now an error -> the WASM mapping collapses unknown codes to `network-error` (`wasm_backend.cpp:~571-590`), which is a visible but benign change.
7. **A second DNS pool** (`rt::Runtime`'s) is used instead of the blocking `getaddrinfo`; its "canceled lookup still occupies a worker" residual (`rt/dns.hpp`) is acceptable but means a flood of guest lookups can occupy the DNS pool -- per-session budget on lookups is not part of this slice; note it.
8. **WASM sync edge diverging.** If `fetch_blocking` keeps its own copy of the exchange it will quietly keep the old IPv4/cap semantics while the async path changes. Shared gate **and** a parametrised test over both are the mitigation.
9. **`kata_backend.cpp` and `native_jail_handle_relay.cpp` call `resolve_and_validate` directly** (`:~239`, `:361`) and do their own blocking `tcp_connect`+`select`. They are *not* `HostEgressProxy` callers; deleting or changing `resolve_and_validate` for the proxy would break them. Out of E1's scope (E6 owns the relay).

## 5. Cross-cutting design decisions needed before any slice

- **D-A Reactor ownership.** Follow step 57 and the provider clients: a trailing borrowed `pal::Reactor*` (and a DNS `OffloadPool*` for egress); null -> lazily create a private one on first use. Backends built in tests as `NativeJailBackend backend;` keep compiling.
- **D-B Stop plumbing.** Additive `std::stop_token` on `NetEgressBackend::fetch`, `ExecutionSurface::run/drain_to`, `PersistentShellSurface::open/exec/drain_to/close`; `SandboxBackend::exec` already has `EffectContext&` (read `ctx.cancellation`). Not an authority, so I2/I3 are untouched, but the design must state it in the concept comments.
- **D-C `pal::ProcessSpec` extensions** are *host-supplied, narrowing-only* values (limits, AppContainer SID, extra pipes), passed explicitly, never ambient; an unsupported request is an error (`unsupported`), never silently dropped. This is the ADR-070 "Delegated Decision Seam" shape and needs its own ADR (new pal surface, security-critical).
- **D-D Deadline sources.** Wall deadlines stay as today (`ResourceLimits::wall_ms`, grants' `wall_ms_cap`); `ctx.deadline` is a hint nobody reads in this area -- do not start honouring it in the same slices (separate behaviour change).

## 6. Notes for the red-team and on gates

Per-slice gates are in section 7, each with a named mutant. "Linux/Docker/Kata" means a real Linux box: per project memory WSL2 here can build and run native Linux and Docker; Kata cannot run here. The three highest-value attack
targets: the IPv6 `AddressPolicy` (4.4 item 1), the lost-limits/lost-AppContainer-token failure that every functional test would miss (3.4 items 1-2), and the watchdog/EOF classification race (9 item 1).

## 7. Recommended order and slice boundaries

### E1 -- egress proxy: async `fetch` on `rt::http`, sync edge kept (Windows-provable; ~medium)
Scope: extract `egress_gate(req, granted) -> result<GatedTarget>` from `fetch_blocking` (pure; C1/C3/C9/C7, `net_egress_proxy.cpp:752-795`); `fetch(req, granted, stop)` awaits `rt::http_request` with a
**v4-only** `AddressPolicy` built from `is_blocked_address`, a non-zero `total_timeout`, and a body cap equal to the old total cap minus a documented head allowance (or the cap redefined explicitly); `fetch_blocking` keeps the
old exchange for the WASM import but calls the same gate. Add the `stop_token` to the concept and the callers (`read_content.hpp:299`, `extract_pdf_*`).
Gates (each must fail on its mutant):
- every existing `test_net_egress_proxy.cpp` check parametrised over **both** paths (mutant: skip `reject_crlf` on the async path -> `CRLF ...` checks fail);
- `resolver was never called` for each gate failure holds on the async path (mutant: resolve before the gate);
- NEW: IPv6 loopback, `::ffff:127.0.0.1`, an AAAA-only host and a v4-mapped private address are `net.address_blocked` (mutant: `accept_all` for v6 -> fails); positive control: a public v4 literal connects;
- NEW: cancel mid-body returns `canceled` within a bound and the socket is closed (mutant: drop the `stop` argument -> hangs/fails); NEW: trickling peer ends at `total_timeout` (mutant: 0);
- NEW: byte cap flood on both paths with the documented cap meaning; 3xx not followed; TLS cert-name mismatch rejected (needs `AGENTENGINE_WITH_HTTPS`; Windows-capable);
- ASan/TSan-style run is Linux; everything else Windows-provable.
Exit: `HostEgressProxy` async path has no blocking call; WASM import documented as the one sync edge.

### E2 -- one shared argv runner on `rt::run_process`; re-point 7 copies (E2a Windows+WSL; end-to-end Docker/Kata needs Linux; ~medium)
Scope: a single `sandbox::run_host_program(reactor*, argv, env, cwd, limits, stop)` -> `task<result<HostProgramOutcome>>` over `rt::run_process` (`rt/process.hpp:110`) and `rt_io_error::run_result_to_error`. Then, one commit each: Docker Win `run_capture`/`run_argv`
(`docker_execution_surface.cpp:35,204`), Docker POSIX `run_capture`/`run_argv` (`:334,:416`), `ctr_cli_detail::run_argv` (`containerd_execution_surface.hpp:112`), `run_ctr` (`kata_backend_detail.hpp:103`). Keep each conformer's *own* argv validation
(`docker_cli_reject_*` `:516-545`, `ctr_cli_detail::reject_unsafe_token`) untouched -- that is the injection defence, not the spawner's.
Gates: shared-runner tests on cmd.exe (Windows) and `/bin/sh` (WSL): wall deadline kills a tree (grandchild gone, as P8), output cap + `truncated`, cancel kills, `output_incomplete` when a grandchild holds the pipe; **mutant** "kill the CLI but not run the follow-up `ctr tasks kill --exec-id`" must fail a Kata test (needs a fake `ctr` script -- Linux; live Kata is **not runnable here**).
Daemon-requiring tests (Docker, Kata) are CI-only and must be hand-added to `ci.yml` exclusion lists per project memory.

### E3 -- `pal::ProcessSpec` resource limits + `native_process_spawn` (Windows only; ~medium-large, needs a small ADR)
Scope: `ProcessSpec` gains `{memory_bytes, cpu_ms, max_processes}` applied to the Job **at creation** (same attribute list), plus an optional memory-limit notification surfaced as a result flag. `spawn_native_process` (`native_process_spawn.cpp:163`) becomes `task` over it.
Gates: `test_native_process_spawn.cpp` S1-S4/R-S4 unchanged; NEW memory-hog child -> oom flag with no unlimited window (mutant: apply limits after spawn -> fails); `test_job_object_limits.cpp` still green for the jail; empty-cwd rejection (S1) still first; quoting (Q1/Q2/R-Q3) untouched because `build_command_line` is reused.

### E4 -- native-jail one-shot `exec` (Windows only; large; ADR + red-team mandatory)
Scope: `ProcessSpec` gains `windows_appcontainer{sid}` + `child_process_restricted`; `exec_blocking`'s body (`native_jail_backend.cpp:228-402`) becomes `task` over `rt::run_process`. The pipe-buffer comment at `:250-256` (drain-after-wait) is retired: concurrent drain changes
the overflow classification (risk 3) -- the abuse corpus must be re-read and any expected value changed in the same commit with reasoning.
Gates: `test_native_jail_backend_windows.cpp` C2 set; `test_native_jail_abuse_corpus_windows.cpp`; `test_native_jail_ambient_authority_windows.cpp` (AC-S1 handle leak, NUL stdin, minimal env); `test_native_jail_parity_windows.cpp`; `test_native_jail_teardown_cycles_windows.cpp`; concurrency test; NEW cancel kills the whole job; mutant: omit the AppContainer attribute -> ambient-authority/abuse corpus must fail (if it does not, that is a **missing test**, and E4 must add one first).
Linux jail (B8): not converted by E4 (section 8).

### E5 -- poll loops become suspensions (small each; B11 Windows, B12 Windows/Linux, B13 Linux)
Replace `sleep_for` polls with `co_await rt::sleep_for(reactor, d, stop)`: `native_shell_session.cpp:401,506`, `docker_persistent_shell_surface.cpp:242`, `kata_backend.cpp:896` (a retry), `egress_policy_host.cpp:226` (flock backoff; **keep its 30 s deadline and the fail-closed install semantics**; this is host-side
`nft`/`tc` policy under an `flock`, where the lock itself stays blocking -- do not hold a `std::mutex` across the new await). Gate: step 57's G17-style test (stop mid-poll returns `canceled` fast, no extra iteration; mutant restores the blocking sleep).
`NativePwshSession::send` also polls a *file* (`exists(x)`), which has no reactor primitive; it keeps polling, now with a suspending sleep. Windows-provable for B11.

### E6 -- Python worker session: async read loop, watchdog kept (Windows only; largest; own ADR; **ADR candidate: design -> red-team -> prove -> judge**)
Scope: (1) `pal::Process` gains an **extra pipe pair** (or the worker protocol moves to stdio framing, which collides with guest `print` -- not recommended); (2) `exec_session`'s `FramedChannel` becomes async reads/writes (`jailed_worker_rpc.cpp:21,39`);
(3) `dispatch_worker_query` awaits `invoke_tool` directly -- **removing `tool_bridge.hpp:77`'s `block_on`** -- and the HandleRelay `connect_*`/file handlers become coroutines over `rt::connect_resolved`/`rt::tcp`; (4) `call_mutex` single-flight becomes an async try-lock (`try_lock` semantics preserved: a second concurrent call must still fail with `native_jail.already_in_progress`, not queue).
The **watchdog thread stays** (section 8): it is joined, instance-owned and independent of lanes/reactor, and its exact 5 ms / 100 ms polling of the job's memory-limit port has no reactor primitive.
Gates: slice1 tests (G-Q1 busy-loop -> timeout, G-lifecycle-2 no respawn); handle-relay tests (SOCK-1.. incl. the denied/blocked cases); exec_seq mismatch tests; concurrent exec+destroy (risk 5); the watchdog/EOF classification race (risk 6, section 9 item 1); NEW cancel mid-exec kills the worker and the next call is `session_terminated`; mutant: stop token ignored.

### E7 -- `RegisteredRunner::invoke` + shell evaluator as coroutines (Windows-provable; small-medium; **last**)
As section 2. Only now is the awaited callee real. Gates as section 2.5.

## 8. Honest calls on what should NOT be converted

1. **WASM epoch ticker (B14, `wasm_backend.cpp:78-85`) -- keep, document as the §4.5 rule-1 exemption.** It is a `std::jthread`, joined (never detached) in `~SharedEngine`, ticking an engine-global epoch that is the *only* thing that interrupts a runaway guest. Making it a reactor timer couples the guest
   wall-clock guard to the health of the reactor thread; a guard must not depend on the thing it guards. It is not I/O, and a reactor repeat-timer would also need a new "periodic" primitive. Rule 1's text targets *detached* threads and `std::thread` outside `pal/`/backends; this is a backend-owned, joined thread.
2. **The native-jail session watchdog (B7) -- keep in E6, same reasoning.** It enforces the wall-clock and memory-limit kills (`:520-557`); it must still fire if a lane is wedged. Converting it to timers is a possible later step, but it buys nothing for the lane-blocking problem (the blocking part is the read loop, B5) and adds the race in risk 6.
3. **`LinuxNativeJailBackend::exec` (B8) -- do not move onto `rt::process`.** `clone(CLONE_NEW*)`, `pivot_root`, cgroup attach are outside `pal::Process`'s model (3.2 item 4); forcing them in means growing a platform-specific spawn abstraction for one backend. Convert it with `rt::offload` **without** passing the stop to the offload (so the caller is not resumed early): the body receives the `stop_token` **by value** (§4.6: inputs by value)
   and its existing 5 ms loop (`:~436-463`) treats `stop_requested()` like `wall_exceeded` -> `SIGKILL`+`waitpid`. The caller then resumes only after the child is reaped, preserving "child gone when `exec` returns". Caveat: that body must not call `block_on` (refused on offload workers) -- it does not today. Needs Linux (WSL2 is available); not provable on Windows.
4. **The Kata egress `flock` (B13) -- keep blocking.** It is a cross-process advisory lock; only its backoff `sleep_for` converts (E5). The install itself runs `nft`/`tc` through `runner_` and is correctness-critical (`kata_backend.cpp:~881-902` fails closed); do not restructure it in #180.
5. **The WASM `http-request` import (the one remaining `fetch_blocking` caller)** -- stays a documented sync edge; wasmtime host functions here are synchronous and the guest is parked inside them (`wasm_backend.cpp:565`). A `block_on` there would be refused if lanes are ever marked, so it keeps the independent blocking exchange behind the *shared* gate (E1).
6. **`RegisteredRunner::invoke` before E6 -- do not convert** (no production caller; would only wrap a blocking callee).
7. **`resolve_and_validate` stays** (used by Kata and the Python relay); E1 does not delete it.

## 9. Not verified / open

1. **Watchdog/EOF classification race (existing).** `terminate_worker`/the watchdog call `TerminateJobObject` and only then `ws.kill_reason.store(...)` (`native_jail_backend.cpp:520-533`). `exec_session` reads `ws.kill_reason.load()` when `recv` fails (`:994-995`). A blocked `ReadFile` returns when the worker dies, which can precede the store. I did not run it; if real, a timeout can be reported as `crash`. Worth a probe before E6 (it is independent of any conversion).
2. I did not read `tests/backends/native_jail/test_native_jail_ambient_authority_windows.cpp`, the abuse corpora or `test_native_jail_python_worker_slice1.cpp` beyond G-Q1/G-lifecycle-2, so several "pinned by" cells name the file, not a label.
3. Whether `session_mutex_` is held across a tool's blocking exec (`agent_session_core.cpp:102`) -- taken from the ADR text, not re-read.
4. `pal::IpAddress` layout / v6 handling in `rt::connect_resolved` and whether `accept_all` is the only policy: grep for `AddressPolicy` found only `provider_chat_wire.hpp:62` and `sse_stream_pump.hpp:34`.
5. Exact `rt::http` cap semantics (head vs body) taken from its header, not its implementation; E1's first act is to measure it.
6. `ExecRequest`/`cap::Exec` top-level gate (ADR-094) is described as unenforced-by-design in memory; I did not re-check it and do not depend on it.
7. `.codegraph` exists but I used grep/Read only; no CodeGraph query was run, so symbol-level call graphs (who calls `HostEgressProxy::fetch` beyond the 3 tools seen) may be incomplete.
8. Nothing was built or executed, including WSL2; all test-existence statements are from reading source, not from a green run.
