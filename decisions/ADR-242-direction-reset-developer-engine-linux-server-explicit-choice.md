# ADR-242 — Direction reset: does AgentEngine stay a Windows-first SDK with one in-process Python and no networking, or become a Linux-server-first engine that developers build multi-user agent applications on, with a microVM as a first-class execution option, nothing chosen implicitly, and every managed piece replaceable?

**Status:** Judged (2026-10-20, project-owner sign-off on PR #209) — project-owner direction, drafted from a whole-system design review
(§1.2). Red-team round 1 (2026-10-20): **rejected** (2 FATAL, 6 MAJOR, 9 MINOR); round 2: **not ready**
(1 FATAL, 5 MAJOR, 4 MINOR); round 3: **not ready** (table errors, 6 MAJOR); round 4 (§2/§3a only): **not ready** (7 errors);
round 5 (R8 only): **not ready** (1 FATAL, 5 MAJOR); this revision folds all five in, plus the owner's three
decisions of 2026-10-20 (§10). No owner decision is open. **Judged: it binds.** The
amendments in §8 are applied in one change, and the follow-on ADRs in §6 carry the mechanisms.

**Supersedes in part:**
- `AgentEngineSpecification.md`: D2's second bullet ("one runtime, permanently", "no `microvm`
  profile", "the shell is not a wrapped binary"), D3 (CRTP as *the* authoring idiom), D5's "CodeAct is the
  primary action space" (now opt-in, R3) and its "the shell is engine-native" paragraph (for `microvm` only), and §7 (the `ThreadPool` and "writes no scheduler" rows).
- `rfcs/008-Sandbox-and-Isolation.md` §1 ("No `microvm` profile", "Priority: `native-jail` first"), §3
  ("trusted-tenant ... the default"), §4 (gains a `microvm` column), §9 G1 (scoped per profile).
- `rfcs/010-Python-Code-Interpreter.md` §2 (the architectural "one runtime" argument; the WASI ecosystem
  argument stands), §1–§2's "no real shell process" model (scoped to `native-jail`/`wasm`), §5 and §9
  G2/G3/G6/G7 (scoped per profile).
- `rfcs/021-Platform-Support-and-Portability.md` §2 (tier table); `rfcs/026-Agent-Facing-Runtime-Surface.md`
  (the shell row and §3's error mapping, per profile); `rfcs/002` (interpreter locked to `native-jail`).
- `rfcs/008` §2/§2a's `SandboxBackend` concept shape (gains the required worktree, R8);
  `decisions/archive/ADR-101-sandbox-backend-tree-refinement-reconciliation.md` (Proposed; "refine via an
  additive concept, never widen the locked one", superseded by R8's required worktree).
- `decisions/archive/ADR-039-inbound-transport-host-pluggable.md` ("no first-party listener, ever");
  `decisions/archive/ADR-084-kata-backend-slice-1.md` (Design A: hardware backends `named_only` permanently).
- The matching locked decisions in `CLAUDE.md`.

ADR-080 is **not** superseded: it already lets a microVM backend become `Strict`-eligible through an
explicit host `register_backend()` call (`ADR-080:64`) and rejects eligibility by mere linkage
(`ADR-080:82`). R2 is built on that, not against it. (The first draft misattributed ADR-084's rule to it.)

**Unchanged, explicitly:** the *statements* of invariants I1–I8 (spec §4), with §3.R3 recording how I2
applies to a guest; WASM Component Model as the plugin ABI (D2's first bullet); no WASI Python; files
belong to the worktree, and the interpreter and shell share one `ExecState` (D4), which `microvm` must
meet (G-R2); cross-session sandbox reuse is prohibited in every profile (008 §6); ADR-070's Delegated
Decision Seam discipline; ADR-071's `NativePythonProvider` scope; MIT licence.

## 1. The question, and why now

### 1.1 Owner direction (2026-10-20)

The project owner set the target in this session: *"Kata is stronger and it can have its own bash,
python inside so native jail can be an optional, we let developer use any they want"*, and *"developer
is target, we help them have a strong C++ API set to implement their application with less code so
we can be called an engine. Manage but allow custom."* The concrete product is an agent server with
a code interpreter that serves many users. Later the same day: *"there is nothing default, developer
choose their own with their own risk"*, and *"we better have a table to show them all and note what
developer have to manage until engine have new solution"* (§2, §3a). And: *"agent always work inside
`SandboxBackend`, Backend always have a worktree inside it, it is required (no matter what implement of
it, no sandbox still an implement of SandboxBackend)"* (R8).

### 1.2 What the review found (2026-10-20, main @ 6c62e611)

A three-area design review, with locked decisions treated as hypotheses, found that the code has
already outgrown several of them, and that the remaining locks now block the product:

- **The code interpreter is not an engine feature.** `execute_code` exists only in the dev CLI
  (`ExecuteCodeTool`, `tools/cli_chat.cpp:575`) over a `static` Windows `NativeJailBackend`
  (`tools/cli_chat.cpp:426`); `MediatedPythonRunner` takes a concrete `NativeJailBackend&`; CMake
  refuses the Python runner on POSIX (`CMakeLists.txt:1124`), and the Linux native-jail backend has no
  interpreter at all. `KataBackend` is Linux-only. The strongest backend and the interpreter never exist
  on one platform.
- **The microVM backend exists but is fenced out.** ADR-084 built `KataBackend` under a "narrowed
  exception" to the no-microvm lock, registered through `register_hardware_isolation_backend` as
  `named_only` (ADR-084 Design A), so it can never win `Strict` resolution.
  It was live-verified on 2026-10-19/20 (`docs/guides/thnak-server-runbook.md`).
- **"No HTTP networking" is no longer what the code does.** `rt::http`, an mbedTLS client and
  `pal::tcp_listen`/accept ship today; the one thing missing is the useful part (A2A wire dispatch,
  an MCP HTTP transport, a listener), so 011 §10 G1 and 012 §8 G1 can never pass and every host
  re-writes the auth glue where ADR-061's red-team found real defects.
- **The spec describes a runtime that no longer exists.** Spec §7 says "AgentEngine writes no
  scheduler"; `rt/lanes.hpp` is one. ADR-237's runtime and the older `ThreadPool`/`block_on`/
  detached-thread model run side by side; provider clients build a private Runtime by default.
- **The authoring idiom costs more than it saves.** `Agent<Derived, Policies...>` is an empty struct,
  tools are `std::function`, model calls are already virtual, yet the templated `AgentSession<3 params>`
  header is referenced directly by 148 files. Per-turn cost is a network call; the template layer buys
  no measured saving.
- **The platform RFC is out of date.** 021 §2 lists Linux as "Next (not yet gated)" while a Linux
  native-jail (pivot_root, seccomp, cgroups v2), Kata, and a Linux CI leg exist.

So the question, stated with a wrong answer: **should the engine keep its 2026-08 posture (portable
SDK, one in-process mediated Python, `native-jail` first, host owns all networking, CRTP authoring),
or re-aim at developers building multi-user Linux agent servers, with engine-managed options for every
resource, chosen explicitly by the developer, and a seam to replace each?** Keeping the 2026-08 posture is the wrong answer if the target
product cannot be built on it without the developer re-implementing the interpreter host, the server,
and the tenancy layer, which §1.2 shows is the case today.

## 2. The governing principle: engine-managed options, chosen by the developer, nothing implicit

AgentEngine is an **engine**: the developer writes their application, not the plumbing. For every
resource the engine manages:

1. **The engine ships managed options.** Each is a complete implementation the developer can pick
   with little code.
2. **Nothing is implicit.** No option is selected by linkage, by platform, or because a probe passed.
   If the developer chooses nothing for a resource the application needs, startup fails closed with a
   named error that lists the options.
3. **Choosing is accepting the risk of that option.** The engine's duty is to say honestly what each
   option enforces and what it leaves to the developer, in §3a's table. The developer's duty is to
   cover the "you manage" column or accept it. A choice follows ADR-070's properties 1, 3, 4 and 5
   (explicit, never mints or widens authority, host code never model output, audited). Property 2 ("an
   unset seam reproduces today's behaviour") is **strengthened**: unset is a named startup error, not
   today's behaviour.
4. **Each resource has one documented seam** for a custom option the engine does not ship.
5. **Invariants are not options.** A developer chooses *which* backend and *where* authority comes
   from; never the *absence* of a boundary check or an authority. "Own risk" covers the residuals each
   option discloses in §3a; it never relaxes I1–I8 themselves, which ADR-070 §3 rejected and this ADR
   does not reopen. **Budgets and resource limits** are a separate, policy rule (not I8, which is about
   benchmark-gated performance budgets): a value is required, chosen by the developer, and
   **"explicitly unbounded" is an allowed value** (owner decision, 2026-10-20). It must be stated in
   code (a named `unbounded` value, never a `0` or a missing field), and it is recorded in startup
   diagnostics and the run's audit trail, like any other choice. The weakest options
   (ADR-071 native providers, the Docker/containerd surfaces, and `none` once it exists) are honest
   options because their row says so, not exceptions to I2: each must be an explicit, audited choice;
   today only the ADR-071 providers are.

**Today's implicit, permissive choices are defects under this section** (each verified on main
@ 6c62e611; each is fixed in §6 step 1b):

| Implicit today | Where | Becomes |
|---|---|---|
| Sandbox mounts are checked only if a `SandboxMount` grant exists; every current call site passes none | `sandbox/sandbox.hpp:208-213` (`authorize_spec`) | Mount authorization is unconditional: a mount with no covering grant is refused |
| Session authority is optional (`require_authority` off unless set), so construction-time capabilities apply to every caller | `rt/agent_session_core.hpp:1369` (member default), `:521` (setter); `agent_session_core.cpp:144` | The developer chooses the authority source: per-request (`require_authority`) or an explicit, named single-trusted-caller mode. **Fixed for sessions** (ADR-244 slice 3a: `AuthoritySource`, required at `initialize`) |
| `McpServer` falls back to its construction-time capability set | `protocol/mcp/server.hpp:198-207` | Per-request grant required, or the same explicit single-trusted-caller mode. **Fixed** (ADR-244 slice 3b: `McpAuthority`, required at construction; `A2aServer` the same) |
| No token budget unless one is set (`nullopt` = unbounded) | `core/agent_registry.hpp:70` (origin), `core/effect_context.hpp:200-202` (copy) | A budget value is required |
| `native-jail` (and `wasm`) registered by platform | `default_sandbox_registry.cpp:39-50` | Only chosen backends are registered (R2) |
| `KataBackend` defaults to `docker.io/library/busybox:latest`, a tag pulled from the network | `kata_backend.hpp:460` | No image default; a digest is required (G-R10) |
| A provider client builds a private Runtime when none is passed | `protocol/openai/chat_client.hpp:473-478` | The Runtime is required (R6) |
| Every `ResourceLimits` field defaults to 0 = unlimited; on Kata an unset `disk_bytes` builds an unbounded host `tmpfs` (host memory exhaustion) | `sandbox/sandbox.hpp:113-122`; `kata_backend.cpp:398-407` | Each limit value is required |
| `KataBackend` also defaults its runtime type and CNI network name | `kata_backend.hpp:459-462` | Required |
| `EnvSecretSource` defaults its prefix to `QUARK_SECRET_` | `trust/secret.hpp:140` | Source and prefix required |
| The Docker/containerd surfaces take no `EffectContext` or `CapabilitySet`: the contained command runs with whatever authority the caller holds | `sandbox/execution_surface.hpp:29-33` | Choosing one is an explicit, audited choice, like ADR-071's providers; §6 step 3 replaces them with the one execution surface |

**Restrictive implicit defaults become required choices too** (owner decision, 2026-10-20). They only
narrow, never widen, so they are not I2 defects, but rule 2 allows no implicit choice of either kind:
`max_turns = 16`, `approval_mode::policy_driven`, `concurrency_mode::sequential`,
`telemetry_capture::metadata_only`, and `SandboxProfileDescriptor`'s `is_strict = true`
(`core/agent_registry.hpp:69-76`) all become values the developer states. A preset (§2) may bundle
them, as an explicit choice of every value it carries.

**Presets** are optional named bundles of choices (for example "server with Kata"). Picking a preset is
an explicit choice of every row it bundles; a preset never activates itself.

The managed resources are: runtime and threads; execution environment (shell and code interpreter);
sandbox lifetime and recycling; inbound transport and server; identity-to-authority policy; secrets;
budgets and quotas; persistence. A resource with no option is a gap; a resource with no seam is a cage.
Both are defects under this ADR. §3a's table is also the honest roadmap: a cell moves from "you manage"
to "engine enforces" only when a test proves it (G-R12).

**Measure:** developer code. A reference application (a multi-user agent server with a code
interpreter, `examples/` or `samples/`) is the yardstick. The follow-on API ADR (§6, step 2) fixes its
line budget; this ADR requires that the budget exists and CI measures it (G-R6).

## 3. Decisions

### R1 — Linux is the primary server target; Windows stays supported for development and desktop

021 §2's tiers become: Linux/x86-64 **Supported (server, primary)**; Windows 11/x86-64 **Supported
(development and desktop)**; Linux/arm64 CI-verified; Windows/arm64 best-effort; macOS unsupported
(unchanged). 021's honesty requirement holds: a tier is what CI proves, so Linux gains a clang leg
(today only gcc-14 runs on Linux) before the table says "Supported".

### R2 — A microVM is a first-class execution option; `native-jail` is one option among several; none is the default

- A **`microvm` profile** is added to 008 §3, backed first by `KataBackend` (Kata 4.x, Cloud
  Hypervisor, KVM). It carries the code interpreter **and** the shell: the guest has its own `bash`
  and `python3`, which is the "ordinary environment" 026 asks for, literally.
- **The shell model changes under `microvm`.** 026's shell row and 010 §1–§2 specify that "there is no
  real shell process": commands are parsed (ShellRunner, ADR-015) and dispatched to capability-gated
  primitives. Under `microvm` the shell **is** a real `bash` in the guest, and authority is enforced at
  the VM boundary (R3), not per command. ShellRunner stays the shell for `native-jail` and `wasm`.
- **D4's shared `ExecState` still binds.** The guest keeps one persistent shell per session, and, when
  the CodeAct provider is attached (R3), one persistent Python interpreter that runs in the shell's cwd
  and env. A `cd` or exported variable is visible to the next call and to Python, and with the provider a
  value computed in one turn is still there in a later one (010 §3 item 1, §9 G3/G6, scoped to `microvm`
  as G-R2, under unchanged authority; R3 rule 1 says what an authority change costs). Today's Kata
  `exec()` runs a fresh `/bin/sh -c` per call and does not meet this.
- **No default backend (closes round-2 N4).** The engine registers no execution backend on its own.
  The developer registers the backend or backends they choose, directly or by choosing a preset, through
  ADR-080's explicit `register_backend()`; nothing becomes eligible by linkage, platform or probe.
  Today's `build_default_sandbox_registry` registers `native-jail` (and `wasm`) automatically by platform
  (`default_sandbox_registry.cpp:39-50`), which is how a failed Kata probe could otherwise fall through to
  interpreter-less Linux native-jail with no downgrade recorded; that builder becomes an explicit
  choice. This answers the registry design's Revision-2 finding #4 ("a stronger backend silently
  winning Strict process-wide"): the winner is the one the developer named, audited at resolution. A
  narrow amendment to ADR-080, recorded in §8.
- `microvm`, `native-jail`, `remote`, `wasm`, the Docker/containerd shell surfaces and ADR-071's native
  providers are all options (§3a lists what each enforces). The developer may also declare a fallback
  chain.
- **No silent downgrade** (008 §3, unchanged): if the chosen backend is unavailable (for example Kata
  linked but its runtime missing), startup fails with a named error that says what to install or what
  fallback to declare. A declared fallback that is taken is recorded as a downgrade in startup
  diagnostics, traces and metrics.
- **Privilege separation (closes round-2 N2).** `KataBackend` needs host root today (`losetup`, `mount`,
  `nft`, `ip netns`, `ctr`; e.g. the loop-device disk quota at `kata_backend.cpp:440-569`), and R4 would put an internet-facing parser in the
  same process. The engine's **supported** deployment for an internet-facing server with Kata is an
  **unprivileged** engine process plus a narrow privileged helper reached over IPC, which accepts only
  typed sandbox operations (G-R11). One parser bug in the server must not be host root across every
  tenant. Running the engine itself as root stays possible, as an explicit choice (the "Deployment
  privilege" row in §3a).
- **Vetted vs unvetted.** A backend is marked *vetted* in §3a once it passes G-R3, G-R4 and G-R8, not
  because it is hardware-backed. The developer may still choose an unvetted one, at their own risk:
  ADR-084 Design A's `named_only` registration stays the way to do that. `KataBackend` is the first
  candidate for vetted.
- **Linux without KVM.** The Linux native-jail backend has no interpreter today, so on such a host the
  only `execute_code` is `remote` until a Linux native-jail Python worker exists (§6 step 3b, G-R2).
  This is disclosed, not hidden behind "native-jail is the fallback".
- `cold_start_class` gains a `seconds` value; Kata stops claiming `milliseconds`.
- **Resource limits under `microvm` are listed, not assumed** (I8): enforced today are `wall_ms`, the
  output cap, `disk_bytes` (rootfs upper layer) and `net_bytes` (slice 11 enforcement cases of
  `test_kata_backend_slice9_10_linux`, passing live 2026-10-20; the header comment at
  `kata_backend.hpp:44-47` still lists them as gaps and is stale). `pids` is accepted but not proven: its
  case is a smoke test (`test_kata_backend_slice9_10_linux.cpp:195-201`) and the fork bomb is a
  documented absence (`test_kata_backend_abuse_corpus_linux.cpp:271`); `memory_bytes` is passed through
  but OOM is never classified (`kata_backend.hpp:121`, `:370`); `fds` is unverified (`:379-382`); `cpu_ms` is a named gap (a CFS quota
  is a rate, not a budget) and `MountSpec::quota_bytes` is not enforced (`kata_backend.hpp:30`). A
  grant carrying a limit the backend cannot enforce fails closed at `create()` (R3). Under `microvm`,
  010 §3 item 3's CPU bound is met by a rate (vCPU count and CFS quota) plus `wall_ms`; an explicit
  `cpu_ms` request fails closed. §8 amends item 3 to say so (closes round-2 N8).

### R3 — The shell and the CodeAct interpreter are two tiers, each chosen; CPython wherever it runs

**Two tiers, each attached explicitly; neither exists unless chosen (owner direction, 2026-10-20: "`execute_code` must be
coming from a context provider, not by default. Kata can already have code interpreter (python in
shell). Code interpreter will go with CodeAct, will have stronger feature set.")**

- **Tier 1, the shell, chosen by attaching a shell `ContextProvider`.** Under `microvm`, `python3` is an
  ordinary program inside the guest (`python3 script.py`, `python3 -c ...`), which is already a basic code interpreter,
  with no extra engine surface: no `agent.*` module, no shim, no persistent interpreter. No session
  gets a shell it was not given.
- **Tier 2, the CodeAct code interpreter, `execute_code`.** It is contributed **only** by a
  `ContextProvider` the developer attaches (the engine's existing context-provider seam), never
  registered implicitly on any profile. It is the stronger feature set: a persistent interpreter whose
  state survives turns, and the host-backed `agent.*` library (tools, files, ask, output). Everything
  below about the persistent interpreter and the shim (rules 2 and 5) applies only when this provider is
  attached.
- This supersedes spec D5's "CodeAct is the **primary** action space": CodeAct is the **strongest**
  action space and is opt-in. D5's "the environment is unremarkable" (no architecture in the prompt,
  026 §1) still binds both tiers. The engine still hard-codes the tool name `"execute_code"`
  (`agent_session_core.cpp:1262`, `skill_premount.hpp:46`); those become the provider's, in §6 step 3.

The code interpreter, where attached, runs CPython either **embedded under `native-jail`** (unchanged,
ADR-003 import gating, 008 §1b mediation) or **as the guest's own CPython inside the microVM**. WASI
Python stays rejected on 010 §2's ecosystem argument, which this ADR does not touch.

010 §2's architectural argument was that two Pythons confuse the agent and its verifier. This ADR
answers it rather than ignoring it:

- **Same language, pinned.** Both hosts run CPython of the same pinned minor version; the guest image
  is digest-pinned and host-declared. Version skew between them is a CI failure, not a residual.
- **One contract.** The `agent.*` CodeAct module (tools, files, ask, output) becomes a
  transport-neutral guest protocol, with one conformance suite that runs against every backend. Today
  it exists only inside the native-jail worker (`python_worker_mediation.*`).
- **Known differences are listed and tested,** not discovered: e.g. per-`open()` mediation exists
  only in native-jail.

**The security model differs, and this ADR states it** (I2 must hold in both):

- *native-jail*: authority is checked **inside** the interpreter, on every call against the **current**
  capabilities (audit hooks, import gating, HandleRelay per `open()`, `native_jail_handle_relay.cpp:157-205`),
  on top of the OS jail.
- *microvm*: everything inside the guest is untrusted, including root, and is treated as one opaque
  principal-bound process. **How I2 applies to a guest:** I2 forbids reaching an *effect* without an
  explicitly passed capability. A real shell's name-to-binary lookup inside the guest reaches only
  effects inside the guest, which affect nothing outside it. Spec D5 called that lookup ambient
  authority because, under an in-process shell, the "inside" *is* the host. Under `microvm` it is not,
  and this ADR records that reading. It holds only if **every** path out of the guest is checked, and
  **every rule below is enforced from the host**, because a root guest controls everything inside the
  VM, including kata-agent and the guest kernel. A rule the guest could switch off is not a rule.

1. **Authority-bound VM (closes F1).** A VM is bound, at `create()`, to the authority it was built
   under: principal and capability set. Authority is replaced per run (`apply_dispatch_authority`,
   `agent_session_core.cpp:953-961`) and is mandatory per request under R4. Before a run is admitted,
   the engine compares the run's authority with the VM's:
   - **same principal, identical capability set, new expiry** (the common case, since request tokens
     expire): the VM's binding is updated to the new expiry; no rebuild;
   - **anything else** (different principal, any change in the capability set): the VM is destroyed and
     rebuilt before the run proceeds. Files survive in the worktree (D4). **On a principal change, only
     the worktree carries over**: the guest-written env (which can hold `LD_PRELOAD`, `BASH_ENV`,
     `PROMPT_COMMAND`) would otherwise run inside the next principal's VM under its mounts and egress.
     On a capability change under the same principal, the cwd and an allowlist of env *names* carry
     over host-side (data that only sets up the next guest, never authority, I3). Interpreter memory
     does not survive, and this is disclosed (§7).
   - **Expiry during a run:** a run admitted under live authority runs to its end under it, bounded by
     its budgets. That is the engine's existing per-run authority model, not a new one, and is
     disclosed (§7).
   Hot-adding or revoking a grant without a rebuild is a later ADR.
2. **Nothing in the guest runs between runs (closes round-2 N1, N3).** Killing guest processes at run
   end would be enforced by the guest, so it is not a rule. Instead, when a run ends, the **host pauses
   the VM at the hypervisor** (the VMM stops its vCPUs; *not* containerd's task pause, which Kata
   implements inside the guest) **and blackholes its bridge port**, then resumes both at the next
   admitted run. Processes are not killed, so the persistent shell and interpreter keep their state
   (010 §3 item 1). Anything a background process does happens inside some run, under that run's
   authority, and is attributed to it (I4). Leaving a VM running between runs (a dev server, say) is an
   explicit host opt-in under ADR-070, audited. *That Cloud Hypervisor's pause is reachable through the
   runtime-rs shim on this deployment is unverified; proving it is part of §6 step 3.*
3. **Grant-to-path translation is specified and fails closed (M2).** `execute_code` declares
   `FsRead`/`FsWrite` (`tools/cli_chat.cpp:573`); Kata enforces `cap::SandboxMount`/`SandboxNetOut`
   (`kata_backend.hpp:68-70`). The engine translates one to the other by one documented rule. Path
   limits (`path_prefix`) are enforced at harvest by name (rule 4). Size and count limits
   (`size_cap_bytes`, `quota_bytes`, `file_count_cap`, `trust/capability.hpp:114-124`) are enforced by the
   staging volume's quota and at harvest. A limit that neither can enforce makes `create()` fail with a
   named error, never silently widen.
4. **The guest writes to a host-owned staging volume, never to the worktree or a shared disk (closes
   M2, round-2 N5).**
   - Each sandbox gets a **host-created, host-formatted** filesystem with its own quota, exported to the
     guest over virtiofs. The host kernel only ever parses filesystem metadata the host itself wrote:
     **the host never mounts a filesystem image the guest wrote.** A full volume stops that sandbox,
     not its neighbours (I8).
   - Host tools never read the staging volume. They see only the worktree.
   - **Copy-in** writes the worktree's granted paths into staging when the VM is created and, with the
     VM paused, before any exec that follows a host-side worktree change.
   - **Harvest** copies from staging into the worktree **at the end of every exec, with the VM paused**
     (a running guest could swap a file between check and copy), so the next model turn and every host
     tool see one state. The host mounts staging `nosuid,nodev,noexec`, opens every path with
     `openat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)` and checks type and size with `fstat` on the opened
     descriptor, never on the name. It accepts regular files only: no symlinks, hard links
     with `nlink > 1`, devices or FIFOs, and no path containing `..`, an absolute path, or a name that
     collides with another after case or Unicode normalisation. Paths are checked against the grant's
     `path_prefix`; sizes (apparent, so sparse files count in full) and counts against the grant.
   - Each harvested change is an attributed worktree effect in the ledger (025, I4).
   - **Conflict:** if a host-side tool changed a path during the exec that the guest also changed, the
     worktree wins and the guest's change to that path is refused with a named error the model sees.
5. **The `agent.*` shim is a specified protocol, dispatched like any tool call (M3, round-2 N6). It
   exists only when the CodeAct provider is attached.**
   - The channel is created by the host and bound to one VM and one session.
   - Every frame carries a **host-issued per-exec nonce**; frames with a stale or foreign nonce, or that
     arrive when no exec is in flight, are refused.
   - Authority comes from the host's current `EffectContext`, never from a field in the frame.
   - A shim call goes through the session's **normal** tool pipeline (approval, budgets, audit),
     attributed to the exec, and may reach only an explicit allowlist of bridged tools the developer sets
     when attaching the provider (the native-jail worker's equivalent today is its `bridged_tools`
     input; the step-3 ADR specifies the allowlist itself).
   - The host parses bytes from a root guest, so the parser carries a fuzz gate (G-R8).
   - A tool's result returned to the guest is visible to the guest; tools must not return secrets
     (the same rule as for model-visible output today).
6. **Network and secrets (M4).** The guest has a real network stack, so 008 §4's "no profile hands a
   guest a raw socket" does not hold for `microvm`. Egress leaves only through the ADR-240 bridge
   policy (G-R4). Secrets are **never** placed in the guest: a root guest in a session-lifetime VM can
   read anything injected into any exec. Secrets resolve host-side and are used host-side, through the
   shim (e.g. a tool call).
7. **Packages (010 §5).** A root guest can `pip install` from any destination egress allows. A package
   index is therefore an explicit egress grant, never implicit; the pinned image (R3 above) is the
   package set unless the developer grants more.

ADR-071's `NativePythonProvider` is unaffected and remains distinct from `execute_code`.

### R4 — AgentEngine ships an optional server module; the core stays transport-agnostic

ADR-039's "no first-party listener, ever" is superseded. A separately linked `agentengine_server`
target (off unless linked) provides the MCP Streamable HTTP, A2A and AG-UI transport adapters and a
reference listener on `rt`. The core library still accepts already-parsed requests, so a host that
owns its own socket and TLS (the ADR-039/061 path) keeps working, unchanged.

The server module may not reintroduce ambient authority: **per-request authority is mandatory** in it.
`McpServer`'s construction-time `held_` fallback and session admission skipped by default
(`agent_session_core.cpp:144`) are not reachable through the server module. Its HTTP parsing is
internet-facing and therefore carries a fuzzing gate (G-R5).

### R5 — The C++ API is the product; templates only where a measurement says so

- D3 changes from "C++23 CRTP-policy authoring" to **"a C++23 API designed for least developer code,
  plus the declarative format (015), bound by I6"**. CRTP remains available as authoring sugar that
  compiles to descriptors; it is no longer the mandated shape of the public surface.
- CONVENTIONS.md's "no virtual for policy on the hot path" narrows to **measured** hot paths (023
  budgets). Per-turn, I/O-bound surfaces (chat clients, sessions, tool dispatch) may be type-erased.
- Python/.NET bindings stay deferred (D3, unchanged).
- The concrete layer and API redesign is the follow-on ADR (§6, step 2), not this one.

### R6 — One host-owned Runtime per process, passed explicitly

Engine library code creates no threads and no private Runtimes; `block_on` lives only at the host edge.
This is ADR-237's direction, finished: spec §7's `ThreadPool` and "writes no scheduler" rows are
replaced by the Runtime that exists. Provider clients that build an owned Runtime when none is passed stop
doing so: the developer passes the Runtime (an owned Runtime becomes an explicit test convenience).

### R7 — Multi-tenant is a first-class target

Tenant-scoped identity, authority, secrets and budgets are managed resources under §2. The mechanisms
(collapsing `Principal`/`SessionCaller`/`RequestAuthority`/`mcp::CapabilityGrant`, a host policy hook
mapping tenant and role to a `CapabilitySet`, tenant-scoped secret refs, a tenant-keyed budget layer)
are follow-on ADRs. **The identity-to-authority policy is host root authority** (it reaches
`CapabilitySet::grant_root()`), so it is *not* an ADR-070 Delegated Decision Seam, which may never mint
authority (ADR-070 §4a); it is the developer's root of trust and is documented as such. A custom budget
option may only tighten a budget. This ADR only removes the assumption of one trusted tenant per process that
`native-jail`'s 008 §3 wording ("trusted-tenant deployments — the default") encodes.

### R8 — Every agent runs inside a sandbox from a `SandboxBackend`, and every backend hosts a worktree

Owner direction (2026-10-20): *"agent always work inside `SandboxBackend`, Backend always have a worktree
inside it, it is required (no matter what implement of it, no sandbox still an implement of
SandboxBackend)"*. Three rules:

1. **No agent runs outside a sandbox.** Each **agent** (not each session: a session may run several,
   025 §3) runs in exactly one sandbox, a `SandboxHandle` created by the backend kind the developer
   chose (§2: never implicit). A backend *instance* may serve many sandboxes and is shared through the
   registry (`sandbox_backend_registry.hpp:132`); the sandbox is what belongs to the agent.
   - A child agent (`agent.spawn`, a workflow node) gets its **own** sandbox, unless its sub-worktree
     mode is `shared` (025 §3), in which case it shares its parent's.
   - Cross-session reuse stays prohibited (008 §6); `fork_from` and session restore always create a
     new sandbox.
   - Creation is lazy: an agent that never runs a shell or code never boots one (no 6 s Kata boot for a
     chat-only agent), but its binding to a backend kind exists from the start.
   - Process and code effects go through the sandbox. **File tools operate on the worktree directly**
     (D4: file semantics do not depend on the backend). Host-side tools that are plain host functions
     stay host code under capabilities, as today.
2. **"No sandbox" is a backend, not an absence.** `none` becomes a real conformer, **`NoSandboxBackend`**:
   no isolation, host processes as the engine's user, the worktree materialised in a host directory.
   - **It can never be picked by `Strict` (closes round-5 F1).** 008 §3 says `Strict` is "never `none`",
     but the code does not enforce it: `resolve_strict` ranks only by strength and platform
     (`sandbox.hpp:94-111`) and `register_backend` defaults to `eligible`
     (`sandbox_backend_registry.hpp:122-124`). `NoSandboxBackend` therefore registers only through a
     dedicated `register_unisolated_backend()`, structurally `named_only`, and `resolve_strict` rejects
     it by type. It is reachable only by `resolve_named`, as an explicit, audited choice (§2 rule 5).
   - `NoSandboxBackend::exec` checks `cap::NativeExec`, so the "who may run a process" gap (`cap::Exec`
     unenforced, §3a) does not carry over to it.
   - **ADR-071's providers keep their tool names, their `cap::NativeExec` checks over structured
     `{program, args}` and their argv confinement, and spawn through `NoSandboxBackend`** (closes
     round-5 M5). `NativePythonProvider` is still never a substitute for `execute_code` (ADR-071 §6.5,
     CLAUDE.md). Whether a CodeAct `execute_code` exists over `NoSandboxBackend` at all is a separate
     decision for the step-3 ADR.
   - Under `NoSandboxBackend` the harvest rules confine only the harvest step, not the process: a host
     process can write anywhere the engine's user can. Disclosed in §3a.
3. **A worktree is part of the `SandboxBackend` contract, required for every conformer.** The concept
   (`sandbox/sandbox.hpp:373-380`, today `traits` + `create`/`exec`/`destroy`) gains the worktree verbs:
   materialise the agent's granted worktree mounts into the sandbox's working filesystem, and harvest
   changes back.
   - **Authorised against `FsRead`/`FsWrite` over the agent's granted mounts (worktree ref, subtree,
     guest path), per 025 §5** (closes round-5 M2), not by whole tree. Host staging paths stay inside
     the backend and are never visible to the agent (026 §2). ADR-101's `authorize_tree_path()` remains
     the pattern for the host-path side only.
   - The verbs are tied to a handle and run before or at `create()`, because mounts are fixed when the
     sandbox is created (`docs/planning/proofs/execution_surface/probe_bind_mount_ordering_hazard.sh`).
   - **`WasmBackend` conforms by materialising only manifest-granted subtrees; an empty grant set
     conforms** (closes round-5 M3). Plugins never see the session worktree unless granted.
   - A type without the verbs is not a `SandboxBackend`. `RegisteredSandboxBackend`'s type erasure (today
     three closures) and every test fake grow to carry them.

**How this reads with D4 (confirmed by the owner, 2026-10-20: "always present in BE").** D4 says files
belong to the worktree, not to the sandbox. R8 keeps that: the worktree stays engine-owned,
content-addressed and durable, and outlives any sandbox; what R8 adds is that every backend **must host**
it as its working filesystem. "A worktree inside every backend" means the worktree is always *present in*
the backend, not that it *lives in* it or dies with it. D4 is unchanged.

**Consequences:**
- Three of the four overlapping execution abstractions collapse into one: `ExecutionSurface` and
  `PersistentShellSurface` fold into `SandboxBackend` with its worktree. **`Runner` stays**: runners run
  *inside* a sandbox, and the shell and interpreter remain two `Runner`s sharing one `ExecState` (D5,
  010 §1a; closes round-5 M4). The Docker and containerd surfaces become `SandboxBackend` conformers
  (ADR-101 already proved `DockerSandboxBackend`), which closes their authority gap, because
  `create`/`exec` take `EffectContext&`.
- R3 rule 4's staging volume and harvest are Kata's implementation of the required worktree verbs; the
  harvest rules (regular files only, `openat2`, name checks) bind every conformer.
- ADR-096's `SandboxToolProvider` scratch directory ("not a 025 worktree") becomes a worktree.
- ADR-101's `TreeCapableSandboxBackend` stops being a separate, optional refinement: its verbs move into
  `SandboxBackend` itself, which is the "widening" ADR-101 declined.

### 3a. Options and responsibilities (as of 2026-10-20, main @ 6c62e611)

What each option enforces **today**, and what the developer must manage until the engine has a
solution. "Engine enforces" lists only what an enforcement test in this repository shows (a smoke test
that only proves a setting is accepted does not count). Rows move from "you manage" to "engine
enforces" only under G-R12. Every row is also subject to §2's defect list until §6 step 1b lands.
**Every resource limit applies only when you set it: an unset limit (0) means none** (`sandbox.hpp:113-122`).

**Execution environment**

| Option | Status today | Engine enforces | You manage until the engine solves it | Engine solution (planned) |
|---|---|---|---|---|
| `microvm`: Kata (Linux + KVM) | `SandboxBackend` works; live-verified 2026-10-20. Not yet wired to the shell or CodeAct tools | Hardware VM boundary; mounts are those the `SandboxSpec` lists, each checked against a covering `cap::SandboxMount` grant, unconditionally since ADR-244 slice 2a (`authorize_spec`; `test_sandbox_capability_authorization` G1a-d, G7, G15, G16); egress allowlist and `net_bytes` at the host bridge (ADR-240, Proposed); `disk_bytes` (rootfs upper layer) and `net_bytes` **when set**, `wall_ms` (exec and destroy only, when set), output cap | **Mounts:** grant exactly the mounts you list (an ungranted mount is refused); a read-write mount is a live `rbind` with no `nosuid`/`nodev` (`kata_backend_detail.hpp:444`), so whatever a root guest writes (symlinks, setuid files) lands in your host directory. **Limits:** set every limit; an unset `disk_bytes` gives the guest an unbounded host `tmpfs`, which can exhaust host memory (`kata_backend.cpp:398-407`); `pids` and `fds` are not proven; OOM is never reported; no CPU-time limit; `quota_bytes` not enforced; `wall_ms` does not bound `create()` (`kata_backend.hpp:36-41`). **Who may create a sandbox:** `cap::Exec` is enforced by no backend (`:362-366`). **Image:** the engine's default is `busybox:latest`, pulled by tag on first use (`:448-450`, `:460`), and the runtime type and CNI network name are defaulted too (`:459-462`); supply a pinned image with `bash`/`python3`. **Host:** install Kata, containerd, CNI and KVM; set the host firewall (`FORWARD` on the bridge, `docs/guides/thnak-server-runbook.md`); ADR-240's preconditions (a dedicated bridge, nft ≥ 1.0.9, kernel ≥ 6.3); a lost nft table is caught only at the next exec; the host's NAT-hairpin WAN address is not recognised as host-local; allowlist IPs are pinned at `create()`. **Process:** the engine runs as root; guest processes keep running between calls (`sleep infinity` task, `:401`); guest output reaches the model as tool output (data, I3). **Capacity:** no admission cap (~170 MB idle to 2 GiB busy per VM); no reaper for idle VMs or crash leftovers (stale netns were found 2026-10-20); ~6 s cold start; shell state is not kept between calls | §6 step 1b (unconditional mount check, no image default), step 3 (persistent shell, staging volume, pause between runs), privileged helper (G-R11), recycling ADR |
| `native-jail`, Windows | Works. The CodeAct interpreter exists but is wired only in the dev CLI | AppContainer, Job Object (memory, pids, when set), restricted token; mediated CPython with per-`open()` capability checks and import gating (ADR-003, ADR-004) | Set every limit; the jail can still read OS system paths outside your grants (ADR-004:282, AC-S3); who may create a sandbox (`cap::Exec` is enforced by no backend, `kata_backend.hpp:362-366`); wiring `execute_code` yourself until the library provider exists; CPU time (the Job Object CPU cap fired in 3 of 11 runs, ADR-004:286, 021; rely on `wall_ms`); the same opt-in mount check (`sandbox.hpp:208-213`) | §6 steps 1b, 3 |
| `native-jail`, Linux | Backend works | Namespaces, `pivot_root`, seccomp-BPF, cgroups v2, `no_new_privs` (ADR-083); any egress request fails closed (`linux_native_jail_backend.cpp:279-284`) | Set every limit (cgroup limits apply only when set); no Python interpreter (commands only); a same-kernel boundary, not hardware; the same opt-in mount check; who may create a sandbox (`cap::Exec` unenforced) | §6 steps 1b, 3b |
| `remote` | Stub, no logic (`src/backends/remote/README.md`) | Nothing | Everything: bring your own cluster integration through the `SandboxBackend` seam | Not scheduled |
| `wasm` | Works for WASI components | Capability-based WASI | No shell or Python | — |
| Docker/containerd shell surfaces (ADR-209) | Works | ADR-209 lifetime limits (idle, age, runs) | **Authority:** the contained command runs with whatever authority your calling code holds; no capability is checked (`execution_surface.hpp:29-33`). Container hardening, resource limits and network: these surfaces "inherit none of RFC 008's guarantees" | R8: become `SandboxBackend` conformers (`EffectContext` on every call), §6 step 3 |
| ADR-071 native providers (`NativeBashProvider`, `NativePythonProvider`, ...) | Works, explicit opt-in | The opt-in and its audit only | Everything: code runs on the host as the engine's user | R8: re-expressed as tiers over `NoSandboxBackend` (scope unchanged) |
| `none` → `NoSandboxBackend` (R8) | **Not implemented** (spec only, 007 §6; no backend and no T2/T3 enforcement in code) | Nothing | Everything: no isolation; do not rely on the spec's "refuses T2/T3 code" | R8 (`NoSandboxBackend` with a worktree, as an explicit, audited choice), §6 step 3 |

**Agent-facing tiers** (R3; each is attached explicitly, on top of one execution option above)

| Tier | Status today | Engine enforces | You manage until the engine solves it | Engine solution (planned) |
|---|---|---|---|---|
| Shell `ContextProvider` | `LiveShellSandboxProvider` (Docker/containerd) and `NativeShellSessionProvider` exist; nothing over Kata | What its execution option enforces, plus ADR-209 lifetimes | Over Kata: a shell provider of your own until step 3 | §6 step 3 |
| CodeAct `ContextProvider` (`execute_code`) | Exists only in the dev CLI over Windows native-jail | Over native-jail: per-call mediation of `agent.*` | Everything over Kata or Linux; until step 3, wire it yourself | §6 steps 3, 3b |

**Other resources**

| Resource | Engine provides today | You manage until the engine solves it | Engine solution (planned) |
|---|---|---|---|
| Runtime and threads | `rt::Runtime` | Pass one shared Runtime; a provider client builds a private one if you do not (~20 threads per client on a large host). Streaming calls and MCP background tasks each use a detached thread, so bound concurrency yourself | R6, finish ADR-237 |
| Inbound transport and server | Typed MCP/A2A/AG-UI dispatchers that take already-parsed requests | The socket, HTTP, TLS, authentication, and the wire binding (A2A has no wire dispatch at all) | R4 server module |
| Identity to authority | Capability checks against the authority given. **Sessions: engine enforces** (ADR-244 slice 3a): every session states `SessionChoices::authority`, and an unstated one fails `choice.missing.session`; `per_request(ceiling)` refuses a missing, expired-at-admission or out-of-ceiling authority, and `single_trusted_caller()` refuses a carried one (`test_rt_agent_session_choices` C1–C15, with planted mutants). **Servers, workflow nodes and the bridge: engine enforces** (ADR-244 slice 3b): `McpServer`/`A2aServer` state their source and refuse a missing, expired or out-of-ceiling grant under `per_request` and a carried one under `single_trusted_caller`; a workflow agent node over a `per_request` session is refused at `initialize()` and per call, and `WorkflowChatClient` inside one; the bridge bounds a `per_request` ceiling by the agent's declaration (`test_mcp_capability_grant` claims 1, 2, 8; `test_rt_authority_surfaces` S1–S7; planted mutants) | **Choosing the authority source** for each session and server; under `single_trusted_caller` the construction-time capabilities apply to every request, which is the choice's disclosed meaning. Mapping users and tenants to capabilities (no policy engine, 007 §5); this is your root of trust (R7) | §6 step 1b, R4 server mode, R7 |
| Secrets | Env and file sources | Per-tenant scoping (the namespace is flat) and vault integration (through the `SecretSource` seam); the env prefix defaults to `QUARK_SECRET_` (`secret.hpp:140`) | §6 step 1b, R7 |
| Budgets and quotas | Per-run token budget (ADR-193) when set; the host egress proxy's concurrent-connection caps per run, per principal and global (8 / 32 / 64, `net_egress_proxy.hpp:221-223`; Kata egress does not pass through this proxy) | **Choosing a token budget and turn cap**: a session cannot start without them (`SessionChoices`, ADR-244 slice 3a; `Bound::unbounded()` is a stated value, `at_most(0)` is refused, `test_rt_agent_session_choices`); the agent registry's own `nullopt` budget is still slice 4; tenant-level quotas; model spend | §6 step 1b, R7 |
| Task stores | MCP and A2A task stores | The A2A store never evicts; bound it or recycle the process | R4 server module |
| Sandbox lifetime | ADR-209 limits for Docker/containerd and native shells | Kata VMs are never reaped or recycled; destroy them yourself | Recycling ADR (the 2026-10-19 proposal) |
| Persistence | `rt::SessionStore`, `rt::AppendLogStore` with `FileAppendLogStore` (spec §7) | A corrupt record mid-file loses what follows it (spec §7, ADR-195); backups and replication | — |
| Audit and telemetry | Run events (`core/run_event.hpp`), sandbox resolution audit hook (`SandboxBackendResolutionAuditHook`) | Exporting, retaining and alerting on them | 016 |
| Deployment privilege | — | Kata requires the engine process to run as root today (`losetup`, `mount`, `nft`, `ip netns`, `ctr`); internet-facing use without the privileged helper is your choice and risk | R2 privileged helper, G-R11 |

## 4. Alternatives considered

- **A. Keep the 2026-08 posture.** Steelman: smaller surface, one Python, nothing internet-facing to
  secure. Rejected: §1.2 shows the target product cannot be built on it without the developer writing
  the interpreter host, the server and the tenancy layer; "smaller surface" moves the surface into
  every host, where it is unaudited.
- **B. Kata only; drop `native-jail`.** Steelman: one security model. Rejected: no Kata on Windows,
  and many Linux hosts (cloud VMs without nested virtualisation, containers) cannot run KVM. Those
  hosts need a local fallback; on Windows `native-jail` already is one, on Linux it becomes one only once
  §6 step 3b gives it an interpreter (R2, "Linux without KVM").
- **C. Strong isolation only through `remote`.** Steelman: the 008 §1 argument; let a cluster own it.
  Rejected: it makes a single-box server require a Kubernetes cluster, and the in-tree Kata backend
  already works on one box.
- **D. Chosen:** §3.

## 5. Falsifiable gates

Each gate needs a positive control (022 §5): a test that would fail if the property were absent.

| Gate | Claim | Disproof / positive control |
|---|---|---|
| **G-R1** | Linux is a CI-proven tier | Linux gcc **and** clang legs are required checks; a Linux-only `-Werror` break fails the PR |
| **G-R2** | One `execute_code` contract on every backend, with D4's shared `ExecState` | With the CodeAct provider attached, one conformance suite (incl. `agent.*`) passes on native-jail (Windows), native-jail (Linux, once step 3b lands) and microvm; a capability not granted is refused on each, and a deliberately granted one succeeds; a `cd`/`export` in the shell is visible to the next Python call and the next turn, under unchanged authority; without the provider, no `execute_code` tool and no shim channel exist (a test that finds either must fail) |
| **G-R3** | Nothing is chosen implicitly, and nothing downgrades silently | With no execution backend chosen, a session that needs one fails at startup with a named error listing the options; with Kata linked but not chosen, it is never selected; with Kata chosen but its runtime missing and no fallback declared, startup fails with a named error and does **not** fall through to `native-jail`; with a fallback declared, the downgrade is recorded; positive control: a test that lets resolution pick a backend nobody chose must fail |
| **G-R4** | The VM boundary holds | First assert the guest really is uid 0 with `CAP_SYS_ADMIN` (else the cases below prove nothing). Then the guest cannot read an unmounted file, reach an unlisted destination, reach a host service on the bridge gateway (2026-10-19 finding: host sshd at 10.88.77.1:22), exceed the workspace quota, or get a link, device or FIFO through harvest; each with a granted counterpart that succeeds |
| **G-R5** | The server module is conformant and optional | 011 §10 G1 and 012 §8 G1 pass against the reference listener; the core links and passes without the server module; the HTTP parser has a fuzz gate (ADR-015 is the precedent); a server-module dispatch without per-request authority fails to **compile**, or, if that proves impossible, fails at runtime with a test that would pass if the check were removed |
| **G-R6** | Least developer code | The reference multi-user code-interpreter server stays within the line budget set by the API ADR, measured in CI |
| **G-R7** | No per-session threads | Process thread count after 1,000 idle sessions equals the Runtime's configured size, counting host-side helper processes (`ctr`) separately and bounded (bench, 023) |
| **G-R8** | The `agent.*` shim is not a confused deputy | Frames with a stale or foreign per-exec nonce, frames sent over another VM's channel, frames claiming wider authority, and calls with no exec in flight are refused; a call outside the bridged-tool allowlist is refused; a shim call shows in approvals, budgets and audit like a model tool call; the host-side parser is fuzzed to ADR-015's criteria (fixed duration, zero crashes); positive control: a well-formed in-exec call under a live grant succeeds |
| **G-R9** | Authority-bound VM; nothing runs between runs | First assert the guest is uid 0. Run 1 under grant A (mount M, destination D) starts a `setsid` background process and a ptrace injection into the persistent shell, both trying to connect to D after run 1 ends: no packet reaches D between runs (observed host-side), and the VM's vCPUs are paused (observed at the VMM). Run 2 under a narrowed grant without M and D: the VM was rebuilt, M and D are unreachable, and cwd/env carried over. Positive controls: run 2 under the same grant with only a new expiry reuses the VM (no rebuild) and still reaches M and D; an opted-in keep-running VM does deliver the between-runs connect |
| **G-R10** | Guest image, guest CPython and host Kata stack are pinned | The image is referenced by digest, and a tag-only reference fails CI; the guest CPython and the embedded CPython (where built) each match one pinned version constant, and a mismatch fails CI; the host-side Kata shim, VMM and virtiofsd versions match a pinned set (they parse guest bytes and are part of the TCB) |
| **G-R11** | The server process is unprivileged | In the supported internet-facing Kata deployment, the engine process holds no `CAP_SYS_ADMIN`/`CAP_NET_ADMIN` and cannot open the containerd socket; sandbox creation still works through the privileged helper; the helper rejects any request that is not a typed sandbox operation; positive control: the same check run against today's root-running process fails |
| **G-R12** | §3a's table is true | Every "engine enforces" cell cites a tagged enforcement assertion that has both a containment check and a positive control; a CI check fails if a cited test is missing, is only a smoke test, or did not run on its leg within 14 days (self-hosted Kata runners included); such a cell is moved to "you manage". Positive control: a planted cell citing a smoke test is rejected. Today no cell cites a tagged test, so this gate fails until §6 adds them; that is intended |
| **G-R13** | Nothing implicit, for every resource | With no choice made for execution backend, authority source, token budget, resource-limit values, turn cap, approval mode, concurrency mode, telemetry capture, strictness, Runtime, secret source or transport, the engine refuses to start (or to build a session) with a named error listing the options; positive control: a test that starts with any one of them unset must fail; an explicit `unbounded` budget starts and is recorded in startup diagnostics, while a `0` or missing budget is refused |
| **G-R14** | Every agent has a sandbox, every backend hosts the worktree, and `NoSandboxBackend` is never implicit | (a) A `try_compile` negative test: a type without the worktree verbs fails the concept, beside a conformer that compiles (ADR-071 §5g's pattern). (b) Building an agent with no backend kind fails with a named error; positive control: the same build with one succeeds. (c) One shared conformance suite on every conformer, `NoSandboxBackend` included: a file written inside appears in the worktree after harvest, a worktree change appears inside after materialise, an ungranted ref or subtree is refused both ways; positive control: a conformer that skips harvest fails the suite. (d) A registry holding only `NoSandboxBackend` fails `resolve_strict`. (e) A forked session and a restored session each get a different handle; a `branch` child gets its own sandbox, a `shared` child its parent's |

G-R4 depends on ADR-240 reaching Judged. A Kata CI runner needs KVM; the self-hosted path is
`docs/planning/kata-backend-ci-runner-setup.md` (the server in `docs/guides/thnak-server-runbook.md`
qualifies).

## 6. Consequences and order of work

1. **This ADR** → Judged → apply §8.
   1b. **Close §2's implicit permissive choices**: unconditional mount authorization; an explicit
   authority source (per-request or a named single-trusted-caller mode) for sessions and `McpServer`;
   a required token budget and required resource-limit values (an explicit `unbounded` is allowed);
   required `max_turns`, approval mode, concurrency mode, telemetry capture and strictness; no backend registered by platform; no
   `KataBackend` image, runtime or network default; a required secret source and prefix; a required
   Runtime; the Docker/containerd surfaces and `none` as explicit, audited choices. Each is a behaviour change with its own regression test.
2. **Layer and API redesign ADR**: the public C++ surface, the layer map (session and workflow out of
   `rt/`, worktree and ledger out of `core/`), type-erased chat client and session, the line budget.
3. **One execution seam (R8)**: `SandboxBackend` gains the required worktree verbs; `ExecutionSurface` /
   `PersistentShellSurface` fold into it (`Runner` stays, running inside a sandbox); Docker/containerd become conformers; `NoSandboxBackend`
   is built; ADR-071's providers move onto it; library `execute_code` and
   shell tools on it, with `execute_code` as an opt-in CodeAct `ContextProvider`; Kata guest image (pinned, bash + python3) and `agent.*` guest shim; persistent
   guest shell; authority-bound VM and between-run VMM pause (R3 rules 1–2); quota'd workspace with
   copy-in/copy-out (rule 4); lifetime and recycling above the surface (the 2026-10-19 recycling
   proposal).
   3b. **A Linux native-jail Python worker**, so a Linux host without KVM has a local `execute_code`.
4. **Finish ADR-237** (R6).
5. **Server module and server mode** (R4).
6. **Tenancy layer** (R7).

Steps 2–4 touch the same headers; they should be sequenced, not run in parallel.

## 7. Residuals and risks (disclosed)

- **KVM availability.** Many cloud VMs and container hosts do not expose nested virtualisation, so
  microvm is unavailable there and `native-jail` or `remote` is used. *Unverified which providers do;
  needs a dated research note before any deployment guidance is written.*
- **Cold start.** Kata boots in about 6 s (measured, 2026-10-19). A warm pool of **blank** VMs, never a
  used one (008 §6), is the planned mitigation; not designed yet.
- **Density.** About 170 MB per idle VM, up to 2 GiB per busy one (about 28 busy VMs on a 61 GB host).
  An admission cap is required before "multi-user" is claimed. Not tested above 40 real VMs.
- **Guest image supply chain.** A digest-pinned image is a new artifact the project must build, sign
  or checksum, and update.
- **Two CPython hosts can still differ** (wheels available in the image, OS libraries). G-R2 bounds
  the `agent.*` contract, not every library behaviour.
- **Internet-facing parsing.** The server module puts an HTTP parser on the network edge, where
  ADR-061 already found defects in host-written glue. G-R5's fuzz gate is necessary, not sufficient.
- **Spec churn.** Superseding locked decisions invites relitigating the rest. The "Unchanged" list
  in the header is the boundary.
- **Rebuild cost of the authority-bound VM.** Any change in a session's authority costs a VM rebuild
  (about 6 s) and loses interpreter memory. A server that varies authority per request will rebuild
  often; hot grant changes are a later ADR. Measured cost per rebuild is not yet known.
- **Copy-in/copy-out cost.** A per-sandbox workspace volume replaces a live bind mount; large
  workspaces pay a copy. Not measured.
- **Linux without KVM has only `remote`** until §6 step 3b lands.
- **The shim is new attack surface** parsing bytes from a root guest, present only with the CodeAct
  provider. G-R8 is necessary, not sufficient.
- **Hypervisor pause is unproven on this stack.** R3 rule 2 needs the VMM's pause reachable from the
  engine through the runtime-rs shim; if it is not, the fallback is destroying the VM at run end, which
  loses interpreter and shell state every run. Unverified.
- **Mid-run expiry.** A run admitted under live authority finishes under it (R3 rule 1), bounded by its
  budgets. A long run can outlive its token.
- **The privileged helper** (R2, G-R11) is a new component and a new IPC surface; until it exists, the
  server preset cannot be claimed safe for internet-facing use.
- **Tool results reach the guest.** A tool that returns secret material through the shim hands it to a
  root guest; the rule that tools return no secrets is a convention, not enforced.

## 8. Amendments applied on Judged

| File | Change |
|---|---|
| `CLAUDE.md` | Replace the "no `microvm`", "one interpreter" and "no HTTP networking" locked decisions with R2/R3/R4; add §2's principle; adjust "v1 authoring surfaces" to R5 |
| `rfcs/008` §1, §3 | Remove the "No `microvm` profile" bullet and "Priority: `native-jail` first"; add the `microvm` profile row and R2's no-default resolution; drop "trusted-tenant ... the default" |
| `rfcs/008` §4 | Add a `microvm` column: real guest network stack, egress only via ADR-240; secrets never in the guest (R3 rule 6) |
| `rfcs/008` §9 G1 | "Same outcome classification" scoped per profile, with the `microvm` mapping listed |
| `rfcs/010` §1–§2, §5 | Keep the ecosystem argument; replace the architectural argument with R3; scope the "no real shell process" model to `native-jail`/`wasm`; package index as an explicit egress grant (R3 rule 7) |
| `rfcs/010` §3 | Item 1 (state survives turns) holds under the CodeAct provider and unchanged authority; item 3's CPU bound is met by rate plus `wall_ms` under `microvm` |
| `rfcs/010` §9 | G2 (dangerous shell paths unreachable) and G7 (interpreter mediation) scoped to `native-jail`/`wasm`; G3/G6 (shared `ExecState`) bind `microvm` too, via G-R2 |
| `rfcs/026` shell row, §3 | Per profile: parsed shell under `native-jail`/`wasm`, real guest `bash` under `microvm`; §3's error mapping listed per profile |
| `rfcs/002` | The interpreter is no longer locked to `native-jail` (`rfcs/002:187`); D3/R5 wording |
| `rfcs/015`, `rfcs/027` | Add `microvm` to the declarative profile vocabulary (`rfcs/015:46`) and to 027's vocabulary |
| `rfcs/021` §2 | R1's tier table |
| `AgentEngineSpecification.md` | D2's second bullet per R2/R3; D3 per R5; D5's engine-native-shell paragraph scoped to `native-jail`/`wasm`, with R3's reading of I2 for guests; §7 runtime table per R6 |
| `CONVENTIONS.md` | The hot-path virtual rule per R5 |
| `README.md` | The RFC-index lines describing "no microvm" / one runtime (`README.md:131-132`) |
| `OpenQuestions.md` | Close or re-point the entries resting on the superseded locks (`OpenQuestions.md:932-943`) |
| `decisions/archive/ADR-039`, `ADR-084` | Status line: "Superseded in part by ADR-242" with a pointer |
| `decisions/archive/ADR-080` | Narrow amendment: no backend is registered implicitly by platform or linkage; the registry holds only the backends the developer chose, directly or through a preset (R2); answers the registry design's Revision-2 finding #4 |
| `decisions/archive/ADR-209`, `ADR-211` | Cross-reference only: ADR-209's lifetime vocabulary is reused for `microvm`; ADR-211's `host-contained` real-shell profile is reconciled with R2/R3 in the step-3 ADR |
| `AgentEngineSpecification.md` D5, `rfcs/026` §5 | CodeAct moves from "primary" to opt-in via a `ContextProvider` (R3) |
| `rfcs/008` §2/§2a | The `SandboxBackend` concept gains the required worktree verbs; `none` is `NoSandboxBackend` (R8) |
| `rfcs/025` | Every backend hosts the session worktree (R8), D4 unchanged |
| `decisions/archive/ADR-101` | Status line: "Superseded in part by ADR-242 R8" (the additive-only refinement) |
| `decisions/archive/ADR-096`, `ADR-071` | Cross-reference: the scratch directory becomes a worktree; ADR-071's providers move onto `NoSandboxBackend` with unchanged scope |
| `decisions/README.md` | ADR-242 row status |

## 9. What the Judge should challenge

- Whether R3's reading of I2 for guests ("effects inside the guest are not effects") is a sound
  reading of I2 or a quiet weakening of it, and whether rules 1–7 really close every path out.
- Whether R3 rule 1's "rebuild on any authority change" is affordable for a multi-user server, or
  forces a hot-grant design sooner than §7 admits.
- Whether requiring every value (budgets, limits, turn cap, approval mode, concurrency, telemetry,
  strictness) makes the minimum program long enough to defeat §2's "least developer code" measure, and
  whether presets are enough to keep it short.
- Whether "nothing implicit" (§2) makes a developer's first run fail in a way that pushes them to
  pick the weakest option (`none`, ADR-071 providers) just to get started, and whether §3a's table is
  enough to stop that.
- Whether §2's "exactly one seam per resource" is achievable without re-creating the four overlapping
  execution abstractions under new names.

## 10. Red-team record

Round 1 (2026-10-20, one independent agent, read-only, given the ADR and the repository): **reject**.
F1, F2 and M6 were re-checked against the code and spec by the author before folding in; the rest are accepted as the red-team reported them, with the red-team's own `path:line` evidence, and were not independently re-checked.

| Finding | Verdict | Disposition |
|---|---|---|
| F1: mounts fixed at `create()` while authority changes per run | CORRECT (`agent_session_core.cpp:953-961`) | R3 rules 1–2; G-R9 |
| F2: spec D2/D5 contradict R2/R3 yet D2 was listed unchanged | CORRECT (`AgentEngineSpecification.md:131-136`, `:205-209`) | Header "Supersedes"; R3's reading of I2; §8 |
| M1: Linux native-jail has no interpreter | Accepted (red-team evidence) | R2 "Linux without KVM"; Alternative B; §6 step 3b; G-R2 |
| M2: grant-to-mount translation unspecified; live rw mount; quota unenforced | Accepted (red-team evidence) (`kata_backend.hpp:25-30`, `:68-70`) | R3 rules 3–4; G-R4 |
| M3: shim unspecified and ungated | Accepted (red-team evidence) | R3 rule 5; G-R8 |
| M4: 008 §4 sockets and secrets unamended | Accepted (red-team evidence) | R3 rule 6; §8 |
| M5: inherited gates (010 §9, 008 §9 G1, 026 §3, 010 §5) unscoped | Accepted (red-team evidence) | §8; R3 rule 7; G-R2 |
| M6: ADR-080 misdescribed; default by auto-detection conflicts with it | CORRECT (`ADR-080:64`, `:82`) | Header; R2 "no default backend" (after the owner's later direction); G-R3 |
| MINOR: missing §8 files, ungated pinning, G-R4 root assertion, G-R7 helpers, I8 limit list, I4/I5 background processes, §1.2 cites | Accepted (red-team evidence) | §8; G-R10; G-R4; G-R7; R2 limit list; R3 rule 2; §1.2 |

Round 2 (2026-10-20, a fresh independent agent, read-only): **not ready for the Judge** (1 FATAL,
5 MAJOR, 4 MINOR). It rated F2 and M1 closed; F1, M2, M3, M5 and M6 partially closed. N1, N2 and N4 were
re-checked by the author against the code (`kata_backend.hpp:537-541`, `kata_backend.cpp:440-569`,
`default_sandbox_registry.cpp:39-50`); the rest are accepted with the red-team's evidence.

| Finding | Disposition |
|---|---|
| N1 (FATAL): "no process outlives its run" was enforced inside a root guest | R3 rule 2 rewritten: host pauses the VM at the VMM and blackholes its port between runs; G-R9 hostile controls; §7 (pause unproven) |
| N2: the engine runs as host root beside an internet-facing parser | R2 privilege separation as the supported deployment; G-R11; §3a; §7 |
| N3: rules 1–2 broke 010 §3 state persistence; token expiry forced rebuilds | Rule 2 no longer kills; rule 1 rebinds on expiry-only change and carries cwd/env; §8 010 §3; G-R2/G-R9 scoped to unchanged authority |
| N4: probe-based default contradicted ADR-080 and fell through silently to native-jail | First fixed with a declared preset; then superseded by the owner's "nothing default" direction: R2 registers no backend implicitly; narrow ADR-080 amendment; G-R3 rewritten |
| N5: copy-in/copy-out unspecified | R3 rule 4: host-formatted staging volume, harvest per exec, name/link checks, conflict rule, ledger effects |
| N6: shim dispatch path and replay protection unspecified | R3 rule 5: per-exec nonce, normal tool pipeline, bridged-tool allowlist; G-R8 |
| N7–N9 (MINOR): host TCB, `cpu_ms` vs 010 §3, pin comparison, fuzz criteria | G-R10; R2 CPU bound and §8; G-R8 |
| N10 (MINOR): expiry mid-run | R3 rule 1; §7 |

Owner direction folded in during round 2 (2026-10-20): `execute_code` comes only from an attached
CodeAct `ContextProvider`; the default is the shell with `python3` as an ordinary program (R3).
Owner direction folded in after round 2 (2026-10-20): nothing is default, the developer chooses and
owns the risk (§2, R2), with a table of every option and what the developer must manage (§3a, G-R12).
Round 3 (2026-10-20, a fresh independent agent, read-only): **not ready** (table errors; 6 MAJOR;
MINORs). The author re-checked against the code: the opt-in mount check (`sandbox.hpp:208-213`), the
`pids` smoke test and fork-bomb absence, `require_authority` (`agent_session_core.hpp:311`, `:521`), the
unset token budget (`effect_context.hpp:200-202`), the egress proxy caps (`net_egress_proxy.hpp:221-223`),
the `busybox:latest` default, `cap::Exec` unenforced, `fds` unverified, `wall_ms` not bounding `create()`,
`sleep infinity`, OOM unclassified, and Linux native-jail's egress refusal. All held.

| Finding | Disposition |
|---|---|
| Table: "only granted mounts" false; `pids` unproven; missing Kata items; Linux native-jail egress; identity row missed `require_authority`; budgets row described connection caps as budgets; no persistence, audit or tier rows | §3a rewritten; R2 limit list |
| §2 rule 3 is not ADR-070's seam (property 2; minting) | §2 rule 3 reworded; R7: authority policy is host root authority, not a seam |
| "Own risk" would let invariants become options | §2 rule 5; §2 defect list; §6 step 1b |
| G-R3 covered only execution backends | G-R13 |
| Rule 1 carried guest-written env across a principal change | Rule 1: worktree only on principal change; env-name allowlist otherwise |
| Harvest TOCTOU; copy-in unspecified | Rule 4: harvest with VM paused, `openat2` + `fstat`, staging `nosuid,nodev,noexec`; copy-in specified |
| G-R12 could not fail on a false claim | G-R12: tagged enforcement assertions, recency, smoke tests rejected |
| MINOR: leftover "default" wording; §6 "process cleanup"; image default vs G-R10; rule 5 cite; root "listed as a choice" | R3 title and text; §6; §2 defect list; rule 5; §3a "Deployment privilege" |

A fourth round is recommended only on §2 and §3a, the parts changed most.

Round 4 (2026-10-20, §2/§3a only, a fresh independent agent): **not ready** (7 errors). The author
re-checked: `none` has no backend and no T2/T3 enforcement (only a comment at `trust/capability.hpp:146`);
`ResourceLimits` all default to 0 (`sandbox.hpp:113-122`) and Kata's unset `disk_bytes` is an unbounded
host `tmpfs` (`kata_backend.cpp:398-407`); the Docker/containerd surfaces take no authority
(`execution_surface.hpp:29-33`); ADR-004 AC-S3; the restrictive defaults (`agent_registry.hpp:69-76`).
All held.

| Finding | Disposition |
|---|---|
| `none` row claimed "Works" and T2/T3 refusal | Row: not implemented |
| `ResourceLimits` default 0 = unlimited (unbounded host `tmpfs` on Kata) | §2 defect row; §3a note and cells; step 1b; G-R13 |
| Docker/containerd authority gap | §2 defect row; rule 5; §3a cell |
| Rule 5 tied token budgets to I8 | Rule 5: budgets and limits are policy; "explicitly unbounded" left to the owner (§9) |
| Secret prefix and Kata runtime/CNI defaults | §2 defect rows; step 1b |
| `cap::Exec` missing from native-jail rows; Windows reads beyond grants | §3a cells |
| Restrictive implicit defaults | §2 listed as an open owner decision; §9 |
| Citations: `require_authority` member default, token-budget origin | Corrected |

The two open owner decisions from round 4 were settled by the owner on 2026-10-20 (below).

Round 5 (2026-10-20, R8 only, a fresh independent agent): **not ready** (1 FATAL, 5 MAJOR, 3 MINOR).
The author re-checked F1 (`sandbox.hpp:94-111`, `sandbox_backend_registry.hpp:122-124`) and M1
(`rfcs/025:66-78`); both held.

| Finding | Disposition |
|---|---|
| F1: `NoSandboxBackend` could win `Strict` resolution | R8 rule 2: `register_unisolated_backend()`, `named_only` by type; G-R14 (d) |
| M1: "session" where the owner said "agent"; sub-agents and `branch` | R8 rule 1 per agent; child sandboxes by sharing mode; G-R14 (e) |
| M2: worktree verbs gated by host-path `SandboxMount` | R8 rule 3: `FsRead`/`FsWrite` over granted mounts, 025 §5 |
| M3: `WasmBackend` has no working filesystem | R8 rule 3: manifest-granted subtrees; empty set conforms |
| M4: folding `Runner` contradicted D5 | `Runner` stays inside the sandbox; §6 step 3 |
| M5: ADR-071 would become the code tier | R8 rule 2: providers keep names, checks and scope; `execute_code` over it is a step-3 decision |
| MINOR: file tools via the backend; lazy creation and fork; concept cite; G-R14 controls | R8 rule 1; rule 3 cite; G-R14 (a)–(e) |

**Owner decisions, 2026-10-20.** (1) "Explicitly unbounded" is an allowed budget and limit value, stated
in code and recorded (§2 rule 5, G-R13). (2) The restrictive defaults become required choices (§2, §6
step 1b, G-R13). (3) R8's reading is confirmed: the worktree is always present in every backend; D4
unchanged. No open owner decision remains; the ADR awaits the owner's Judged.
