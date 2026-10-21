# ADR-244 — How does the engine make each of ADR-242 §2's implicit choices a required, named, recorded choice, at today's types, without the engine granting itself authority?

**Status:** Proposed (2026-10-21). Design, revision 3. Red-team round 1 rejected revision 1 (3 FATAL,
9 MAJOR, 6 MINOR); round 2 rejected revision 2 (1 FATAL, 10 MAJOR, 8 MINOR). This revision folds both
in (§7; the round-by-round findings are in
[`docs/planning/adr-244-red-team.md`](../docs/planning/adr-244-red-team.md)). Implements
[ADR-242](ADR-242-direction-reset-developer-engine-linux-server-explicit-choice.md) §6 step 1b and gate
G-R13 (and the step-1b parts of G-R3 and G-R10). Precedes
[ADR-243](ADR-243-layer-and-developer-api.md)'s slice 1 (ADR-243 §5: "Step 1b first"), so it works on
today's types; ADR-243's facade later wraps what this ADR makes required, it does not undo it.

## 1. The question, and the baseline

ADR-242 §2 decided *what* becomes required. It did not decide *how*: where each requirement is checked,
what an unset choice looks like in a type, how "explicitly unbounded" is spelled, how a choice survives
clear, fork, restore and delegation, and how the engine's own sandbox users get past an unconditional
mount check without granting themselves authority. A wrong answer to the last one breaks I2; a wrong
answer to the others leaves a silent default in place or makes `0` mean "unbounded" again.

Baseline, re-verified on main @ 7f224166 (2026-10-21): every row of ADR-242 §2's table still holds at its
cited line. The inventory and two red-team rounds found more of the same kind:

| Found | Where | Same defect as |
|---|---|---|
| Sandbox network grants are checked only if a `SandboxNetOut` grant exists | `sandbox/sandbox.hpp:255-256` | the mount row |
| Read-only deployment paths (worker dir, `python_home`, `extra_sys_path`; on Windows the pdf worker's dir and scratch) are granted outside `SandboxSpec.mounts`, unchecked | `native_jail_backend.cpp:629-642`, `tools/extract_pdf_text.hpp:348,357` | the mount row |
| `MediatedPythonRunner` creates its worker with an empty `EffectContext`; its bridged `agent.tools` calls run under `grant_root(config.capabilities)`, an authority built from configuration | `mediated_python_runner.cpp:60-73`; `tool_bridge.hpp:72-78` | the mount row; I2 |
| The one-runner-per-session binding (`CodeActRunnerBinding`, ADR-030) is opt-in host wiring, not enforced by the runner | `codeact_runner_binding.hpp:47-58` | I1 |
| `extract_pdf_text` picks its own backend (a function-local `static`, both platforms) | `tools/extract_pdf_text.hpp:346,395` | "registered by platform" |
| `single_trusted_caller` (today's `require_authority == false`) silently replaces a supplied authority with the construction-time set; `McpServer` does the reverse (a per-call grant silently replaces `held_`) | `agent_session_core.cpp:964-968`; `mcp/server.hpp:209-229` | the authority rows |
| A `per_request` authority is installed wholesale: the agent's capability ceiling is not applied | `agent_session_core.cpp:959-961` vs `agent_session_bridge.hpp:347-349` | I2 |
| A spawned child gets no authority mode; its capabilities are a copy that outlives a parent grant's expiry | `rt/agent_spawn_child_run.hpp:166-170` | the authority rows |
| `clear_core_state` resets budget and turn cap to unbounded; the snapshot record does not store them; `fork_core_from` does not copy them; `set_max_turns(std::optional)` can reset the cap to unbounded | `agent_session_core.cpp:748,752`, `:681-695`; `.hpp:311,634` | the budget row |
| Docker/containerd: tag-pulled default images; `isolation = {}` defaults that hide 512 MiB / 128 pids; a non-`deny_all` policy becomes bridge or host networking with no network check | `docker_execution_surface.hpp:666`, `docker_persistent_shell_surface.hpp:142`, `containerd_execution_surface.hpp:447,753`; `docker_execution_surface.cpp:589-590` | the Kata image, limits and mount rows |
| Capability caps are `std::optional` and "missing means uncapped": `NativeExec`, `Exec` (C++ and their CRTP tags, where `0` means unbounded), the declarative `NativeExec` grant parser, `FsRead::size_cap_bytes`, `FsWrite::quota_bytes`/`file_count_cap`, `NetOut::byte_cap` | `trust/capability.hpp:150-152,194-206,503,540-547,567-603`; `native_exec_grant.hpp:60-70`; `native_process_spawn.cpp:226-228` | the limits row |
| Default budgets: `ChildSpawnRequest::max_turns = 25`, `SpawnTargetDescriptor::child_token_budget = 50'000`, `SpawnQuota::max_spawns_per_principal = 100`, the builders' `max_turns = 25` and unset budget, `run_with_bounded_reflection(max_total_tokens = 0)` meaning no bound | `rt/agent_spawn_child_run.hpp:84`, `rt/agent_spawn.hpp:170,209`, `core/session_builder.hpp:845-846,1148-1149,1343-1344`, `bounded_reflection.hpp:111-138`, `harness.hpp:584-586` | the turn-cap and budget rows |
| `TerminationBound` requires one bound, but each omitted field is unbounded | `workflow/graph.hpp:220-243` | the budget row |
| `AgentMetadata::stateless_pool_size` (nullopt = off, from the `Stateless<N>` tag) | `core/agent_registry.hpp:74`, `agent.hpp:107-108` | the restrictive-defaults paragraph |
| `LinuxNativeJailBackend` defaults its cgroup root | `linux_native_jail_backend.hpp:88` | the Kata runtime row |

## 2. Decision

### 2.1 One vocabulary for "a value, or explicitly unbounded"

`core/bound.hpp` (built, slice 1): `Bound<T>` with `at_most(n)`, `unbounded()`, no default constructor and
no conversion from an integer; `limit()` (nullopt = unbounded), `covers(child)`, `tighter(a, b)`,
`describe()`. `check_bound(what, b)` refuses `at_most(0)` with `choice.zero_bound` and names
`unbounded()`; `choice_missing(resource, options)` builds `choice.missing.<resource>` listing the options.

- **Omission does not compile.** MSVC 19.51, clang and gcc-14 all reject an aggregate that leaves a
  `Bound` member out, a default-initialised outer struct, `Bound b = {}` and `optional::emplace()`
  (2026-10-21 probe; `tests/compile_fail/bound_omitted_member.cpp` keeps it true).
- **Zero is refused at every check site**, enumerated in G-C2. A real "none allowed" uses its own
  mechanism (`NetPolicy::deny_all`). This flips `test_agent_yaml_limits` Y8 (`max_turns: 0` accepted):
  G-R13 refuses a `0` budget, and ADR-242 is Judged.
- **Ordering is `covers`/`tighter`**, never `std::min` on raw integers, everywhere a parent's value
  narrows a child's (metadata against session options, spawn against `remaining_token_budget`, capability
  attenuation).
- **CRTP tags.** `Bound` has private members, so it cannot be a template argument. Tags take
  `BoundValue`, a structural literal type with `consteval` factories `at_most(n)` / `unbounded()`, and
  convert to `Bound` at registration (`MaxTurns<at_most(16)>`, `cap::decl::NativeExec<…,
  session_wall_ms = at_most(60'000), max_processes = unbounded()>`). A raw integer argument does not
  convert.
- **Option tables.** Each enumerated choice has one `constexpr` table that both its YAML parser and its
  `choice_missing` call read; a test asserts they are the same object.

### 2.2 Where each choice is checked

**Compile time where the developer writes the value in one expression; otherwise at the first point the
choice is needed, as a named error, before any effect.** A type whose construction can fail on a value
(an unpinned image, an empty prefix) gets a factory returning `result<T>`, because constructors do not
throw here (CONVENTIONS). Internal holders that need a default-constructible slot (`Instance` structs:
`native_jail_backend.hpp:324`, `linux_native_jail_backend.hpp:120`, `wasm_backend.cpp:253`,
`native_process_spawn.cpp:225`, `native_shell_session.cpp:377`) hold `std::optional<ResourceLimits>`, set
at `create()`, never a placeholder `unbounded()`.

| Choice | Representation | Unset is |
|---|---|---|
| `ResourceLimits` (8 fields) | each `Bound<…>`; `ResourceLimits::unbounded()` is the named all-unbounded preset | a compile error |
| Session authority, token budget, turn cap | `initialize(id, principal, SessionChoices)`; `SessionChoices{authority, token_budget, max_turns}` with `AuthoritySource::per_request()` / `::single_trusted_caller()` and `Bound`s; `set_max_turns` takes `Bound` | a compile error; a never-initialised or cleared session fails `start_run` with `choice.missing.session` |
| `AgentMetadata` turn cap, token budget, approval, concurrency, telemetry, strictness, stateless pool | `std::optional<Bound<…>>`, `std::optional<approval_mode>` …, `std::optional<SandboxProfileDescriptor>`, `std::optional<StatelessPool>` (`off()` / `of(n)`) | the existing `validate_agent_metadata` (`agent_session_bridge.hpp:310`), extended, fails `choice.missing.<field>`; the CRTP path `static_assert`s each tag (including `Stateless<N>` or a new `NotStateless`), with the tag table in the message |
| Session builders, spawn targets and requests, spawn quota, bounded reflection | `Bound` parameters and fields, no defaults | a compile error; a builder `build()` without one fails `choice.missing.<field>` |
| Workflow termination | `TerminationBound::max_rounds`, `deadline_ms`: `Bound`, required | a compile error; YAML `choice.missing.workflow.<field>` |
| `McpServer`, `A2aServer` authority | `McpAuthority` / `A2aAuthority`: `per_request()` or `single_trusted_caller(held)` | a compile error; see §2.3 for the per-request rules |
| Sandbox backends | `build_default_sandbox_registry` **deleted**; the host constructs each chosen backend and registers it | resolving against an empty registry fails `choice.missing.sandbox_backend` |
| `KataBackend`, Docker/containerd surfaces | `KataBackend::create(KataBackendConfig)` / `…Surface::create(image, isolation)`: image required and digest-pinned, `isolation` required with `Bound` limits | a compile error; tag-only fails `sandbox.image_not_pinned` |
| `LinuxNativeJailBackend` cgroup root | constructor parameter | a compile error |
| Provider-client Runtime | constructor parameter `rt::Runtime&`; the five private-Runtime sites removed | a compile error |
| `EnvSecretSource` prefix | `EnvSecretSource::create(prefix)`; `""` refused | a compile error; `secret.empty_prefix` |
| Capability caps (`NativeExec`, `Exec`, `FsRead`, `FsWrite`, `NetOut`), C++, CRTP and declarative | `Bound<…>` fields, attenuated with `covers`; the declarative parser requires each cap or the string `unbounded` | a compile error; `choice.missing.grant.<field>` |
| Unmediated shell surfaces | `SandboxRuntime::run()` / `run_live()` take the caller's `EffectContext const&` and require `cap::UnmediatedShell{network}` | `sandbox.unmediated_not_granted`, audited (§2.5) |

`TerminationBound::token_budget` stays optional. The supervisor does not enforce it
(`graph.hpp:222-235`: bookkeeping, host-monitored), so requiring it would record a limit nothing enforces
and invite a false "engine enforces" cell. It is named in §6 and documented as unenforced.

**Both layers are checked.** A raw `AgentSession` does not go through registration, and the protocol
surfaces drive it directly; approval, concurrency, telemetry and strictness live only in
`AgentMetadata`. Checking either alone leaves the other's defaults silent.

### 2.3 Authority: what each source means, everywhere it is carried

`set_require_authority` is removed.

- **`per_request()`**: a `StartRun` or `ResolveInteraction` without a live authority fails
  `run.authority_required` / `run.authority_expired`. The installed set is the request's authority
  **attenuated to the session's ceiling** when the session has one (the bridge sets it from
  `AgentMetadata::capability_ceiling`), so `per_request` never widens an agent past its declared ceiling.
- **`single_trusted_caller()`**: today's `false` branch, except that a request **carrying** an authority is
  refused with `run.authority_unexpected` instead of being silently replaced. Admission runs when the
  request names a caller and is skipped when it names none (`agent_session_core.cpp:144`); that skip is
  this option's disclosed residual, and the server module will refuse the option.
- **A null `capabilities` pointer is the empty set** wherever this ADR checks authority
  (`effect_context.hpp:47` defaults it to null).
- **`McpServer`**: `single_trusted_caller(held)` refuses a call that carries a grant
  (`mcp.authority_unexpected`); `per_request()` refuses one without (`mcp.authority_required`).
  `A2aServer` likewise. A server/session pair with different modes fails closed on every request with one
  of those named codes; the server cannot see the session's mode through `RunStarter`, so there is no
  construction-time check (§6).
- **Workflow executors** send `StartRun{input}` with no authority (`agent_workflow_executor.hpp:167`). A
  node session built `per_request()` is refused when the workflow is validated
  (`workflow.node_requires_authority`), not on its first run. Carrying a caller's authority through a
  workflow is R4/R7 work (§6).
- **Spawned children.** The child inherits the parent's source. Under `per_request`, the child's
  authority is **the derived on-behalf-of principal, the minted child capabilities**
  (`mint_child_spawn_capabilities`, attenuated from the caller's `ctx.capabilities`), **and the parent
  grant's expiry**, never the parent's whole grant. `EffectContext` gains `authority_expires_at`
  (`std::optional<steady_clock::time_point>`), set by `apply_dispatch_authority`, so the spawn tool can
  carry it. The child's budget is `Bound::tighter(target, remaining)`.

**Choices survive every way a session is re-made:**

- `clear_core_state` un-initialises the session (next `start_run` fails `choice.missing.session`).
- `fork_core_from` copies `SessionChoices` whole.
- The snapshot record stores `SessionChoices`. Restore succeeds when the session's choices are **covered
  by** the record's: each `Bound` equal or tighter, and `per_request` at least as strict as
  `single_trusted_caller`. Anything wider fails `session.choices_widened`. A record written before this
  ADR (no choice fields) fails `session.record_without_choices`; a host re-saves it after choosing.

### 2.4 Mounts and network: unconditional, against an authority the engine was handed

`authorize_spec` checks **every** host-path mount against `cap::SandboxMount` grants and **every**
non-`deny_all` network policy against `cap::SandboxNetOut` grants, whether or not any grant of that kind
exists. Kata's zero-grant network backstop (`kata.net_capability_required`) becomes redundant and is
removed with a test that the general check covers it.

**Every path a backend grants is a checked mount.** Deployment paths become read-only `MountSpec`s with
`guest_path == host path` (the Windows jail ignores guest paths; the Linux jail binds them where they are,
as `extract_pdf_text`'s Linux branch already does). `grant_ro_path_once` and `create_python_worker`'s
separate grants are removed. To keep ADR-004 finding 9 closed (unbounded DACL growth on the shared
AppContainer profile), **the mount step deduplicates read-only grants** (`grant_ro_deduped`, moved into
the mount loop); G-C5(d) checks the DACL size is stable across repeated creates.

**Whose authority is checked:**

- **A site with a calling `EffectContext`** sets `spec.capabilities` to a subset of `*ctx.capabilities`
  ("never more than the caller holds"; `WasmBackend` hands `spec.capabilities` to the component,
  `wasm_backend.cpp:268`, so the whole set would over-grant).
- **`MediatedPythonRunner`** (Windows-only) is **one runner per session, enforced by the runner**: it
  binds to the first session that runs it and refuses any other
  (`codeact.runner_bound_to_other_session`), so `CodeActRunnerBinding`'s rule no longer depends on host
  wiring. Creation takes an explicit host grant and the caller's context for audit:
  `initialize(CapabilitySet const& host_grant, EffectContext& ctx)`, with `authorize_spec` checking every
  mount and deployment path against `host_grant` and a `SandboxExec` "create" row emitted through `ctx`.
  Per-call file use stays checked per open against the caller's `FsRead`/`FsWrite`
  (`native_jail_handle_relay.cpp:159-172,307`). There is no per-run `SandboxMount` check: it would make
  every caller hold read-write `SandboxMount` over the runner's roots, which also authorises building
  other sandboxes over those paths (a widening).
- **The runner's tool bridge** runs each bridged `agent.tools` call under `config.capabilities`
  **attenuated to the calling `ctx.capabilities`**, not under `grant_root(config.capabilities)`.
- **`extract_pdf_text`** builds its spec with the calling session's grants on both platforms and exposes
  `required_grants()`, the read-only `SandboxMount`s a host must give sessions that may use it. Its
  scratch directory is fixed and shared across sessions (`extract_pdf_text.hpp:253-258`), disclosed in §6.

### 2.5 Unmediated surfaces

`cap::UnmediatedShell{bool network}` is checked in `SandboxRuntime`, the point every Docker/containerd
consumer passes (`LiveShellSandboxProvider`, `MandatorySandboxProvider`'s `run_command`, the task-branch
tools). A surface whose isolation is not `deny_all` (bridge or host networking) requires `network = true`;
`network = false` covers only a `deny_all` surface. The grant is per request. Each check emits a
`SandboxExec` audit row naming the surface and whether networking is on.

The hidden substitutions go: `isolation` is a required argument whose limits are `Bound`s, and
`unbounded()` means no flag is passed. A stated limit the surface cannot enforce (containerd's `pids`,
`containerd_execution_surface.hpp:437-440`) fails `sandbox.limit_unsupported`, as today. Hostile
container tests (`test_docker_isolation.cpp`, `test_docker_persistent_shell_surface.cpp`) state real
limits; the slice-5 rewrite may not map their omitted limits to `unbounded()` (machine safety).

`capability_kind::unmediated_shell` is added to the kind enum, `capability_from_kind` and the
policy-reachability tool. When step 3 deletes the surfaces, the kind's value stays reserved so persisted
kinds do not shift.

### 2.6 Recording

- `run_event_payload::RunStarted` gains `authority`, `token_budget` and `max_turns` (`describe()`), and
  the session exposes `choices()` for a host with no event sink.
- `validate_agent_metadata` returns the choice summary; `register_agent` and the YAML compiler return it
  to the host for startup diagnostics.
- Every `SandboxExec` "create" row carries `limits` (eight `describe()` strings); `MediatedPythonRunner`
  gains such a row (§2.4). The emitters are enumerated in the slice-5 test.
- A run without an event sink records nothing to an audit trail; that is the host's telemetry choice,
  which is itself required.

## 3. Alternatives

- **Runtime validation everywhere.** Every omission becomes a test-time failure instead of a compile
  error. Kept only for `AgentMetadata`, which YAML builds field by field.
- **A sentinel integer for unbounded.** An ordinary number in arithmetic and `min()`. Rejected.
- **A list of backend kinds** (revision 1). Cannot carry Kata's config. Rejected.
- **Derive grants from configuration, or exempt deployment paths.** The engine granting itself authority.
  Rejected (§2.4).
- **A per-run `SandboxMount` check on the Python runner** (revision 2). Widens every caller and does not
  touch interpreter state. Rejected for one-runner-per-session (§2.4).
- **Carry the parent's whole grant to a spawned child** (revision 2). Hands the child the parent's set.
  Rejected (§2.3).
- **Strict equality on restore** (revision 2). Refuses a host tightening a budget. Rejected for "covered
  by".

## 4. Gates

Each with a positive control (022 §5); each test is written first and fails on main where the property is
absent today.

| Gate | Claim | Disproof / positive control |
|---|---|---|
| **G-C1** | No unbounded by omission | `try_compile` negatives: `ResourceLimits{}`, `SandboxSpec` without `.limits`, `SessionChoices{}`, `KataBackendConfig{}`, `TerminationBound{}`, each capability with an omitted cap, `SpawnTargetDescriptor` without a budget, a provider client without a Runtime, `EnvSecretSource{}`, a CRTP agent missing each tag, and a CRTP tag given a raw integer; each beside a fully specified positive |
| **G-C2** | No unbounded by zero | `at_most(0)` refused with `choice.zero_bound` at each enumerated site: session `initialize`, `set_max_turns`, `validate_agent_metadata` (C++ and YAML), builders, spawn, `TerminationBound` validation, `authorize_spec` (limits), each capability cap; `unbounded()` in the same place starts and is recorded |
| **G-C3** | A missing choice is a named error listing every option | For each `AgentMetadata` field, YAML fails `choice.missing.<field>` and the message lists the same table object the parser reads; never-initialised and cleared sessions fail `start_run`; positive: declared, it registers and runs |
| **G-C4** | Mounts and egress are checked unconditionally | Host-path mount with no grant of any kind refused; non-`deny_all` network refused (native-jail, Kata, Linux jail); positive: covering grant succeeds; a planted `if (!grants.empty())` guard fails the test |
| **G-C5** | The engine does not grant itself paths or tools | Windows: (a) `initialize` with a host grant missing `python_home` fails; (b) a second session on a bound runner is refused, the first runs; (c) a bridged `agent.tools` call needing a capability the caller lacks is refused even though `config.capabilities` holds it; (d) the AppContainer DACL entry count is unchanged across 20 creates (positive control: without dedup it grows); (e) no grant call remains outside the mount loop (source check). Both platforms: `extract_pdf_text` from a session without `required_grants()` fails |
| **G-C6** | Authority sources mean what they say | `per_request()` refuses `StartRun`/`ResolveInteraction` without authority, and caps a request authority at the session ceiling (a capability in the grant but outside the ceiling is refused); `single_trusted_caller()` refuses a request with authority; `McpServer`/`A2aServer` both ways; a `per_request` workflow node is refused at validation; null `capabilities` refused, not dereferenced (ASan); positives for each |
| **G-C7** | Choices survive re-making; children never widen | Cleared session fails `start_run`; fork reports equal `choices()`; restore with tighter choices succeeds, wider fails `session.choices_widened`, a legacy record fails `session.record_without_choices`; a spawned child under `per_request` is refused a capability the parent's grant holds but the minted set lacks, and fails once the parent's grant expires; positive: a minted capability works while live |
| **G-C8** | No backend by linkage or platform | No `build_default_sandbox_registry` symbol; empty registry fails `choice.missing.sandbox_backend`; only-`wasm` never resolves `native-jail`; positive: registering it resolves it |
| **G-C9** | Images pinned | Tag-only refused by `KataBackend::create` and the three surfaces; digests construct (Kata and containerd on thnak-server) |
| **G-C10** | Unmediated surfaces are a granted choice on every path | Through `shell_exec`, `run_command` and a task-branch tool: no grant refused and audited; `network = false` refused for a networked surface; positives with each grant |
| **G-C11** | The test preset stays in tests | A CI check fails if `testing::` session presets appear under `include/`, `src/`, `tools/` or `examples/`; positive control: a planted use fails it |

G-R13 ("refuses to start ... with any one of them unset") is G-C1 + G-C3 + G-C8. Transport is not here:
the core has none to default (ADR-242 R4).

## 5. Migration, in slices (each a commit with its gate; one PR per two or three slices)

1. **Vocabulary** (built): `Bound<T>`, `check_bound`, `choice_missing`; `BoundValue` and the option tables
   land with their first users.
2. **Mounts, network, the Python runner (G-C4, G-C5)**, first: the only rows exploitable today. One commit
   for both platforms (no platform-split window, I6). Dev tools (`tools/cli_chat.cpp` and the others)
   grant their sessions the read-only `SandboxMount`s they need and the runner's host grant; no session
   gains read-write `SandboxMount` it did not have.
3. **Session choices and authority (G-C6, G-C7, part of G-C1/G-C2):** `SessionChoices`, the sources and
   their refusals, ceiling attenuation, `authority_expires_at`, clear/fork/restore/spawn, `RunStarted`,
   `McpServer`/`A2aServer`, workflow validation. The production `initialize` callers
   (`agent_session_bridge.hpp:355`, `session_builder.hpp` ×3, `eval_trial.hpp:459`,
   `agent_spawn_child_run.hpp:167`, `tools/test_driver/test_driver.hpp:2754,3320`, `tools/*_chat.cpp`)
   are rewritten by hand. Tests are rewritten by a script mapping each test's existing arguments to the
   equivalent choice (`set_require_authority(true)` → `per_request()`, a stated budget → `at_most`,
   nothing → `testing::trusted_unbounded()`); G-C11 lands in the same commit.
4. **Agent metadata, builders, spawn, workflows, reflection (G-C3):** `validate_agent_metadata`, CRTP
   `static_assert`s and `BoundValue` tags, YAML `unbounded`, option tables; 002 §3's table defaults and
   §9 Q3 ("absence = off") amended.
5. **Resource limits and capability caps (rest of G-C1/G-C2):** `Bound` fields, backends read `limit()`,
   the surfaces' hidden substitutions removed, the declarative grant parser, `SandboxExec` `limits`.
6. **Backends, Runtime, secrets, unmediated surfaces (G-C8 to G-C10):** registry by registration,
   `create()` factories with pinned images, cgroup root, provider Runtime (coordinated with ADR-237's
   #180/#182, which touch the same constructors), `EnvSecretSource::create`, `cap::UnmediatedShell`.

Each slice updates ADR-242 §3a in the same commit (a closed cell moves to "engine enforces" and cites its
tagged test, G-R12), and amends the specs that state the old defaults (002 §3, 008 §2, 014's termination
bound, 020 §4).

## 6. Residuals

- **Churn and mis-mapping.** Slice 3 rewrites most test files. Each slice reruns the full suite on Windows
  and on Linux (thnak-server, gcc and clang); any test whose outcome changes is read by hand, never
  re-baselined. Baselines (main @ 7f224166): Windows, everything passes except 13 Docker-daemon tests
  (daemon not running); Linux gcc, everything passes except `test_json_dump_escape` and the three
  containerd tests.
- **`extract_pdf_text` keeps its own backend instance**, and on Linux that instance's cgroup root, until
  ADR-243 slice 4 gives tools a backend seam. Its mounts are checked against the caller, so the residual is
  backend *choice*, not authority. Its scratch directory is shared across sessions.
- **Server/session authority mismatch is caught per request, not at construction** (`RunStarter` hides the
  session's mode). It fails closed with a named code.
- **Workflows cannot carry a caller's per-request authority** to node sessions yet; such nodes are refused
  at validation. R4/R7.
- **`TerminationBound::token_budget` is not enforced** and stays optional.
- **`RuntimeConfig{}`'s thread counts** stay defaulted; sizing moves to ADR-243's `Engine`.
- **`A2aServer`'s task store never evicts** (`a2a/server.hpp:171,182`); a required capacity belongs to the
  server module (ADR-242 §3a, R4).
- **The shared AppContainer SID** (ADR-041): grants are never revoked and all native-jail sandboxes share
  one SID, so at the OS level a granted path stays reachable across sessions. Unchanged by this ADR.
- **Other narrowing defaults stay** (`stream_retries = 0`, `suspend_for_approval = false`, `max_stalls`,
  `max_resets`); ADR-243's facade decides which become required.
- **`single_trusted_caller` remains an option**, with the admission skip above; honest for an embedded
  single-user host, refused by the server module.

## 7. Red-team record

Full findings: [`docs/planning/adr-244-red-team.md`](../docs/planning/adr-244-red-team.md). Every cited
location was re-checked before folding in.

- **Round 1** (revision 1): not ready, 3 FATAL / 9 MAJOR / 6 MINOR. Fatal: no caller authority at the
  Python runner's creation; null `*ctx.capabilities`; an unacknowledged exemption list
  (`grant_ro_path_once`, `create_python_worker`). Majors: the unmediated gate on one path only, optional
  capability caps, missed budgets, clear/fork/restore, `single_trusted_caller`'s silent replacement,
  spawned children, the registry-of-kinds, `AgentMetadata`'s unbounded spelling, the scripted rewrite.
- **Round 2** (revision 2): not ready, 1 FATAL / 10 MAJOR / 8 MINOR. Fatal: carrying the parent's live
  grant handed a spawned child the parent's whole set (fixed: minted set + parent expiry, §2.3). Majors:
  the per-run runner check rested on a wrong "shared runner" premise and widened callers (fixed:
  one-runner-per-session enforced); the tool bridge's configuration-built authority (fixed: attenuated);
  DACL growth from deployment mounts (fixed: dedup in the mount step, G-C5d); Linux claims; container
  networking unchecked (fixed: `UnmediatedShell{network}`); more implicit caps and budgets; the unenforced
  workflow token budget (kept optional); restore equality and legacy records; `per_request` skipping the
  ceiling; server/session authority mismatches and workflow nodes. Minors: tag names and 002 §9 Q3,
  factory functions for fallible values, G-C11's scope, the runner's missing audit row, the shared SID,
  hostile tests' limits, the persisted kind, pdf's required grants and shared scratch.
