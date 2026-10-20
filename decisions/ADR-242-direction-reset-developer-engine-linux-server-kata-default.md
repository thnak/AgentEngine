# ADR-242 — Direction reset: does AgentEngine stay a Windows-first SDK with one in-process Python and no networking, or become a Linux-server-first engine that developers build multi-user agent applications on, with a microVM as the default execution environment and every managed piece replaceable?

**Status:** Proposed (2026-10-20) — project-owner direction, drafted from a whole-system design review
(§1.2). Red-team round 1 (2026-10-20): **rejected** (2 FATAL, 6 MAJOR, 9 MINOR); round 2: **not ready**
(1 FATAL, 5 MAJOR, 4 MINOR); this revision folds both in (§10). **Binds nothing until the project owner marks it Judged**; on Judged, the
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
- `decisions/ADR-039-inbound-transport-host-pluggable.md` ("no first-party listener, ever");
  `decisions/ADR-084-kata-backend-slice-1.md` (Design A: hardware backends `named_only` permanently).
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
a code interpreter that serves many users.

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
  `named_only` (ADR-084 Design A), so it can never be a default.
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
or re-aim at developers building multi-user Linux agent servers, with managed defaults for every
resource and a seam to replace each?** Keeping the 2026-08 posture is the wrong answer if the target
product cannot be built on it without the developer re-implementing the interpreter host, the server,
and the tenancy layer, which §1.2 shows is the case today.

## 2. The governing principle: managed by default, replaceable by seam

AgentEngine is an **engine**: the developer writes their application, not the plumbing. For every
resource the engine manages, it ships:

1. **A working default that needs no developer code beyond choosing a preset** (R2), safe for a
   multi-user server, and
2. **exactly one documented seam** to replace or narrow it, following ADR-070's discipline (explicit
   opt-in, fails closed when unset, never mints authority, host code never model output, audited).

The managed resources are: runtime and threads; execution environment (interpreter and shell);
sandbox lifetime and recycling; inbound transport and server; identity-to-authority policy; secrets;
budgets and quotas; persistence. A resource with no default is a gap; a resource with a default but
no seam is a cage. Both are defects under this ADR.

**Measure:** developer code. A reference application (a multi-user agent server with a code
interpreter, `examples/` or `samples/`) is the yardstick. The follow-on API ADR (§6, step 2) fixes its
line budget; this ADR requires that the budget exists and CI measures it (G-R6).

## 3. Decisions

### R1 — Linux is the primary server target; Windows stays supported for development and desktop

021 §2's tiers become: Linux/x86-64 **Supported (server, primary)**; Windows 11/x86-64 **Supported
(development and desktop)**; Linux/arm64 CI-verified; Windows/arm64 best-effort; macOS unsupported
(unchanged). 021's honesty requirement holds: a tier is what CI proves, so Linux gains a clang leg
(today only gcc-14 runs on Linux) before the table says "Supported".

### R2 — A microVM is the default execution environment where the host can run one; `native-jail` becomes optional

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
- **Default resolution is a declared preset, never linkage plus a probe (closes round-2 N4).** The
  managed default lives in an engine **preset** the developer selects with the one line they already
  write to set up the engine (for example a "server" preset). The server preset *declares*
  `profile: microvm` with **no fallback**. Choosing the preset is the explicit deployment decision
  ADR-080 requires (`ADR-080:82`); nothing becomes eligible because it was linked or because a probe
  passed. Under that preset only the declared profile's backend is `Strict`-eligible: `native-jail` is
  not silently registered beside it (today's builder always registers it, `default_sandbox_registry.cpp:39-50`,
  which is exactly how a failed Kata probe would otherwise fall through to interpreter-less native-jail
  with no downgrade recorded). This answers the registry design's Revision-2 finding #4 ("a stronger
  backend silently winning Strict process-wide"): the winner is declared, audited at resolution, and
  never silent. A "desktop" preset declares `native-jail` on Windows. This is a narrow amendment to
  ADR-080's default-registry builder, recorded in §8.
- `native-jail`, `remote` and `wasm` stay, and the developer may choose any of them, or declare a
  fallback chain.
- **No silent downgrade** (008 §3, unchanged): if the declared profile is unavailable (for example Kata
  linked but its runtime missing), startup fails with a named error that says what to install or what
  fallback to declare. A declared fallback that is taken is recorded as a downgrade in startup
  diagnostics, traces and metrics.
- **Privilege separation (closes round-2 N2).** `KataBackend` needs host root today (`losetup`, `mount`,
  `nft`, `ip netns`, `ctr`; e.g. the loop-device disk quota at `kata_backend.cpp:440-569`), and R4 would put an internet-facing parser in the
  same process. Under the server preset the engine process is **unprivileged**: privileged sandbox
  operations run in a narrow helper reached over IPC, which accepts only typed sandbox operations. One
  parser bug in the server must not be host root across every tenant.
- ADR-084 Design A's `named_only` stays the mechanism for **unvetted** hardware backends; a backend
  becomes `Strict`-eligible by passing G-R3, G-R4 and G-R8, not by being hardware-backed. `KataBackend`
  is the first.
- **Linux without KVM.** The Linux native-jail backend has no interpreter today, so on such a host the
  only `execute_code` is `remote` until a Linux native-jail Python worker exists (§6 step 3b, G-R2).
  This is disclosed, not hidden behind "native-jail is the fallback".
- `cold_start_class` gains a `seconds` value; Kata stops claiming `milliseconds`.
- **Resource limits under `microvm` are listed, not assumed** (I8): enforced today are `wall_ms`, the
  output cap, `pids`, `disk_bytes` (rootfs upper layer) and `net_bytes` (slice 11 cases of
  `test_kata_backend_slice9_10_linux`, passing live 2026-10-20; the header comment at
  `kata_backend.hpp:44-47` still lists them as gaps and is stale); `cpu_ms` is a named gap (a CFS quota
  is a rate, not a budget) and `MountSpec::quota_bytes` is not enforced (`kata_backend.hpp:30`). A
  grant carrying a limit the backend cannot enforce fails closed at `create()` (R3). Under `microvm`,
  010 §3 item 3's CPU bound is met by a rate (vCPU count and CFS quota) plus `wall_ms`; an explicit
  `cpu_ms` request fails closed. §8 amends item 3 to say so (closes round-2 N8).

### R3 — The shell by default, the CodeAct interpreter by provider; CPython wherever it runs

**Two tiers, and only the first is on by default (owner direction, 2026-10-20: "`execute_code` must be
coming from a context provider, not by default. Kata can already have code interpreter (python in
shell). Code interpreter will go with CodeAct, will have stronger feature set.")**

- **Default: the shell.** A `microvm` session gets the shell tool. `python3` is an ordinary program
  inside the guest (`python3 script.py`, `python3 -c ...`), which is already a basic code interpreter,
  with no extra engine surface: no `agent.*` module, no shim, no persistent interpreter.
- **Opt-in: the CodeAct code interpreter, `execute_code`.** It is contributed **only** by a
  `ContextProvider` the developer attaches (the engine's existing context-provider seam), never
  registered by default on any profile. It is the stronger feature set: a persistent interpreter whose
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
     rebuilt before the run proceeds. Files survive in the worktree (D4). The shell's cwd and env are
     carried over host-side from the last run's recorded state (this is data that only sets up the next
     guest, never authority, I3). Interpreter memory does not survive, and this is disclosed (§7).
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
   - **Harvest** copies from staging into the worktree **at the end of every exec**, so the next model
     turn and every host tool see one state. It accepts regular files only: no symlinks, hard links
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
     attributed to the exec, and may reach only the tools the session exposes to `agent.tools`, the
     same `bridged_tools` list the native-jail worker is generated from today (`src/backends/native_jail/agent_tools_codegen.hpp:292-305`).
   - The host parses bytes from a root guest, so the parser carries a fuzz gate (G-R8).
   - A tool's result returned to the guest is visible to the guest; tools must not return secrets
     (the same rule as for model-visible output today).
6. **Network and secrets (M4).** The guest has a real network stack, so 008 §4's "no profile hands a
   guest a raw socket" does not hold for `microvm`. Egress leaves only through the ADR-240 bridge
   policy (G-R4). Secrets are **never** placed in the guest: a root guest in a session-lifetime VM can
   read anything injected into any exec. Secrets resolve host-side and are used host-side, through the
   shim (e.g. a tool call).
7. **Packages (010 §5).** A root guest can `pip install` from any destination egress allows. A package
   index is therefore an explicit egress grant, never a default; the pinned image (R3 above) is the
   default package set.

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

### R6 — One host-owned Runtime per process is the managed default

Engine library code creates no threads and no private Runtimes; `block_on` lives only at the host edge.
This is ADR-237's direction, finished: spec §7's `ThreadPool` and "writes no scheduler" rows are
replaced by the Runtime that exists. Provider clients that build an owned Runtime by default stop
doing so (an owned Runtime becomes an explicit test convenience).

### R7 — Multi-tenant is a first-class target

Tenant-scoped identity, authority, secrets and budgets are managed resources under §2. The mechanisms
(collapsing `Principal`/`SessionCaller`/`RequestAuthority`/`mcp::CapabilityGrant`, a host policy hook
mapping tenant and role to a `CapabilitySet`, tenant-scoped secret refs, a tenant-keyed budget layer)
are follow-on ADRs. This ADR only removes the assumption of one trusted tenant per process that
`native-jail`'s 008 §3 wording ("trusted-tenant deployments — the default") encodes.

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
| **G-R3** | The default is a declared preset, with no silent downgrade, never linkage or a probe | Under the server preset with Kata working, a session gets a Kata sandbox and resolution is audited; with Kata linked but its runtime missing and no fallback declared, startup fails with a named error and does **not** fall through to `native-jail`; with a fallback declared, the downgrade is recorded; with Kata linked and no preset chosen, Kata is not `Strict`-eligible; positive control: a test that lets resolution silently fall through must fail |
| **G-R4** | The VM boundary holds | First assert the guest really is uid 0 with `CAP_SYS_ADMIN` (else the cases below prove nothing). Then the guest cannot read an unmounted file, reach an unlisted destination, reach a host service on the bridge gateway (2026-10-19 finding: host sshd at 10.88.77.1:22), exceed the workspace quota, or get a link, device or FIFO through harvest; each with a granted counterpart that succeeds |
| **G-R5** | The server module is conformant and optional | 011 §10 G1 and 012 §8 G1 pass against the reference listener; the core links and passes without the server module; the HTTP parser has a fuzz gate (ADR-015 is the precedent); a server-module dispatch without per-request authority fails to **compile**, or, if that proves impossible, fails at runtime with a test that would pass if the check were removed |
| **G-R6** | Least developer code | The reference multi-user code-interpreter server stays within the line budget set by the API ADR, measured in CI |
| **G-R7** | No per-session threads | Process thread count after 1,000 idle sessions equals the Runtime's configured size, counting host-side helper processes (`ctr`) separately and bounded (bench, 023) |
| **G-R8** | The `agent.*` shim is not a confused deputy | Frames with a stale or foreign per-exec nonce, frames sent over another VM's channel, frames claiming wider authority, and calls with no exec in flight are refused; a call outside the bridged-tool allowlist is refused; a shim call shows in approvals, budgets and audit like a model tool call; the host-side parser is fuzzed to ADR-015's criteria (fixed duration, zero crashes); positive control: a well-formed in-exec call under a live grant succeeds |
| **G-R9** | Authority-bound VM; nothing runs between runs | First assert the guest is uid 0. Run 1 under grant A (mount M, destination D) starts a `setsid` background process and a ptrace injection into the persistent shell, both trying to connect to D after run 1 ends: no packet reaches D between runs (observed host-side), and the VM's vCPUs are paused (observed at the VMM). Run 2 under a narrowed grant without M and D: the VM was rebuilt, M and D are unreachable, and cwd/env carried over. Positive controls: run 2 under the same grant with only a new expiry reuses the VM (no rebuild) and still reaches M and D; an opted-in keep-running VM does deliver the between-runs connect |
| **G-R10** | Guest image, guest CPython and host Kata stack are pinned | The image is referenced by digest, and a tag-only reference fails CI; the guest CPython and the embedded CPython (where built) each match one pinned version constant, and a mismatch fails CI; the host-side Kata shim, VMM and virtiofsd versions match a pinned set (they parse guest bytes and are part of the TCB) |
| **G-R11** | The server process is unprivileged | Under the server preset, the engine process holds no `CAP_SYS_ADMIN`/`CAP_NET_ADMIN` and cannot open the containerd socket; sandbox creation still works through the privileged helper; the helper rejects any request that is not a typed sandbox operation; positive control: the same check run against today's root-running process fails |

G-R4 depends on ADR-240 reaching Judged. A Kata CI runner needs KVM; the self-hosted path is
`docs/planning/kata-backend-ci-runner-setup.md` (the server in `docs/guides/thnak-server-runbook.md`
qualifies).

## 6. Consequences and order of work

1. **This ADR** → Judged → apply §8.
2. **Layer and API redesign ADR**: the public C++ surface, the layer map (session and workflow out of
   `rt/`, worktree and ledger out of `core/`), type-erased chat client and session, the line budget.
3. **Unified execution surface**: collapse `SandboxBackend` / `ExecutionSurface` /
   `PersistentShellSurface` / `Runner` into one capability-taking surface; library `execute_code` and
   shell tools on it, with `execute_code` as an opt-in CodeAct `ContextProvider`; Kata guest image (pinned, bash + python3) and `agent.*` guest shim; persistent
   guest shell; authority-bound VM and per-run process cleanup (R3 rules 1–2); quota'd workspace with
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
| `rfcs/008` §1, §3 | Remove the "No `microvm` profile" bullet and "Priority: `native-jail` first"; add the `microvm` profile row and R2's default resolution; drop "trusted-tenant ... the default" |
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
| `decisions/ADR-039`, `ADR-084` | Status line: "Superseded in part by ADR-242" with a pointer |
| `decisions/ADR-080` | Narrow amendment: the default-registry builder takes a declared preset and registers only the declared profile's backend as `Strict`-eligible (R2); answers the registry design's Revision-2 finding #4 |
| `decisions/ADR-209`, `ADR-211` | Cross-reference only: ADR-209's lifetime vocabulary is reused for `microvm`; ADR-211's `host-contained` real-shell profile is reconciled with R2/R3 in the step-3 ADR |
| `AgentEngineSpecification.md` D5, `rfcs/026` §5 | CodeAct moves from "primary" to opt-in via a `ContextProvider` (R3) |
| `decisions/README.md` | ADR-242 row status |

## 9. What the Judge should challenge

- Whether R3's reading of I2 for guests ("effects inside the guest are not effects") is a sound
  reading of I2 or a quiet weakening of it, and whether rules 1–7 really close every path out.
- Whether R3 rule 1's "rebuild on any authority change" is affordable for a multi-user server, or
  forces a hot-grant design sooner than §7 admits.
- Whether R2's default makes a Linux developer's first run fail (no Kata installed) in a way that
  pushes them to disable isolation entirely.
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
| M6: ADR-080 misdescribed; default by auto-detection conflicts with it | CORRECT (`ADR-080:64`, `:82`) | Header; R2 default resolution via the registry builder; G-R3 |
| MINOR: missing §8 files, ungated pinning, G-R4 root assertion, G-R7 helpers, I8 limit list, I4/I5 background processes, §1.2 cites | Accepted (red-team evidence) | §8; G-R10; G-R4; G-R7; R2 limit list; R3 rule 2; §1.2 |

Round 2 (2026-10-20, a fresh independent agent, read-only): **not ready for the Judge** (1 FATAL,
5 MAJOR, 4 MINOR). It rated F2 and M1 closed; F1, M2, M3, M5 and M6 partially closed. N1, N2 and N4 were
re-checked by the author against the code (`kata_backend.hpp:537-541`, `kata_backend.cpp:440-569`,
`default_sandbox_registry.cpp:39-50`); the rest are accepted with the red-team's evidence.

| Finding | Disposition |
|---|---|
| N1 (FATAL): "no process outlives its run" was enforced inside a root guest | R3 rule 2 rewritten: host pauses the VM at the VMM and blackholes its port between runs; G-R9 hostile controls; §7 (pause unproven) |
| N2: default deployment runs the engine as host root beside an internet-facing parser | R2 privilege separation; G-R11; §7 |
| N3: rules 1–2 broke 010 §3 state persistence; token expiry forced rebuilds | Rule 2 no longer kills; rule 1 rebinds on expiry-only change and carries cwd/env; §8 010 §3; G-R2/G-R9 scoped to unchanged authority |
| N4: probe-based default contradicted ADR-080 and fell through silently to native-jail | R2 default is a declared preset; only the declared backend is eligible; narrow ADR-080 amendment; G-R3 rewritten |
| N5: copy-in/copy-out unspecified | R3 rule 4: host-formatted staging volume, harvest per exec, name/link checks, conflict rule, ledger effects |
| N6: shim dispatch path and replay protection unspecified | R3 rule 5: per-exec nonce, normal tool pipeline, bridged-tool allowlist; G-R8 |
| N7–N9 (MINOR): host TCB, `cpu_ms` vs 010 §3, pin comparison, fuzz criteria | G-R10; R2 CPU bound and §8; G-R8 |
| N10 (MINOR): expiry mid-run | R3 rule 1; §7 |

Owner direction folded in during round 2 (2026-10-20): `execute_code` comes only from an attached
CodeAct `ContextProvider`; the default is the shell with `python3` as an ordinary program (R3).
A third round on this revision is recommended before the Judge.
