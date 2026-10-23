# ADR-244 — How does the engine make each of ADR-242 §2's implicit choices a required, named, recorded choice, at today's types, without the engine granting itself authority?

**Status:** Proposed (2026-10-21). Design, revision 5; slices 1, 2, 3 (3a, 3b, 3c) and 4 (4a, 4b, 4c) built. Four red-team rounds, each
rejecting the revision it read (§7); this revision folds round 4 in. Findings, evidence, the full inventory
and the revision history are in [`docs/planning/adr-244-red-team.md`](../docs/planning/adr-244-red-team.md).
Implements [ADR-242](ADR-242-direction-reset-developer-engine-linux-server-explicit-choice.md) §6 step 1b and
gate G-R13 (and the step-1b parts of G-R3 and G-R10). Precedes
[ADR-243](ADR-243-layer-and-developer-api.md)'s slice 1 (ADR-243 §5: "Step 1b first"), so it works on
today's types; ADR-243's facade later wraps what this ADR makes required, it does not undo it.

## 1. The question, and the baseline

ADR-242 §2 decided *what* becomes required. It did not decide *how*: where each requirement is checked,
what an unset choice looks like in a type, how "explicitly unbounded" is spelled, how a choice survives
clear, fork, restore and delegation, and how the engine's own sandbox users get past an unconditional
mount check without granting themselves authority. A wrong answer to the last one breaks I2; a wrong
answer to the others leaves a silent default in place or makes `0` mean "unbounded" again.

Baseline (main @ 7f224166): every row of ADR-242 §2's table still holds at its cited line, and the inventory
and red-team rounds found about forty more of the same kind (the planning log lists them). The ones this ADR
acts on are named where their decision is.

## 2. Decision

### 2.1 One vocabulary for "a value, or explicitly unbounded"

`core/bound.hpp` (built, slice 1): `Bound<T>` with `at_most(n)`, `unbounded()`, no default constructor and
no conversion from an integer; `limit()` (nullopt = unbounded), `covers(child)`, `tighter(a, b)`,
`describe()`. `check_bound(what, b)` refuses `at_most(0)` with `choice.zero_bound` and names
`unbounded()`; `choice_missing(resource, options)` builds `choice.missing.<resource>` listing the options.

- **Omission does not compile.** MSVC 19.51, clang and gcc-14 all reject an aggregate that leaves a
  `Bound` member out, a default-initialised outer struct, `Bound b = {}` and `optional::emplace()`
  (`tests/compile_fail/bound_omitted_member.cpp` keeps it true).
- **Zero is refused at every check site**, enumerated in G-C2. A real "none allowed" uses its own
  mechanism (`NetPolicy::deny_all`). This flips `test_agent_yaml_limits` Y8 (`max_turns: 0` accepted).
- **Ordering is `covers`/`tighter`**, never `std::min` on raw integers, wherever a parent's value narrows a
  child's.
- **CRTP tags** take `BoundValue`, a structural literal type with `consteval` factories, converted to `Bound`
  at registration (`MaxTurns<at_most(16)>`). A raw integer argument does not convert.
- **Option tables.** Each enumerated choice has one `constexpr` table that both its YAML parser and its
  `choice_missing` call read.
- **Engine floors are named.** Where the engine keeps a host-safety bound under an `unbounded()` choice
  (an output drain's 16 MiB, a wall-clock ceiling), the floor is a separately named engine bound, disclosed,
  and recorded beside the choice: `describe()` reads "unbounded (engine floor 16 MiB)". Slice 5 lists every
  such site (planning log).

### 2.2 Where each choice is checked

**Compile time where the developer writes the value in one expression; otherwise at the first point the
choice is needed, as a named error, before any effect.** A type whose construction can fail on a value gets a
factory returning `result<T>` (constructors do not throw, CONVENTIONS). Internal holders that need a
default-constructible slot hold `std::optional<ResourceLimits>`, set at `create()`.

| Choice | Representation | Unset is |
|---|---|---|
| `ResourceLimits` (8 fields) | each `Bound<…>`; `ResourceLimits::unbounded()` is the named preset | a compile error |
| Session authority, token budget, turn cap | `initialize(id, principal, SessionChoices)`; `SessionChoices{authority, token_budget, max_turns}`; `set_max_turns` takes `Bound` | a compile error; a never-initialised or cleared session fails `start_run` with `choice.missing.session` |
| Bridge and builder sessions | `AgentSessionOptions{session_id, principal, choices}` and the builders' `principal`, `secret_source`: required, no defaults (today `"s-agent"`, `"p-agent"`, `"p-quickstart"`, an `InMemorySecretStore`) | a compile error; `choice.missing.principal` / `.session_id` / `.secret_source` |
| `AgentMetadata` turn cap, token budget, approval, concurrency, strictness | `std::optional<Bound<…>>`, `std::optional<approval_mode>`, …, `std::optional<SandboxProfileDescriptor>` | `validate_agent_metadata` (`agent_registry.hpp:559`), extended, fails `choice.missing.<field>`; the CRTP path `static_assert`s each tag |
| Tool approval | every `Tool<>` declares exactly one of `Approval<…>` or `ApprovalChosenByHost` (`static_assert`); `ToolDescriptor::approval` is `std::optional`, no default | a compile error; a descriptor left unset (a host-chosen tool whose provider was given no mode, a hand-built one) fails `ToolRegistry::register_tool`, the session's per-turn table (the turn fails before the model is called) and `admit_call`/`background_task`, `choice.missing.tool.approval` |
| Sandbox registry at bind | `register_agent`, `bind_agent_session`, `make_agent_session` take the registry by reference (no `nullptr` default) | a compile error; an empty registry fails `choice.missing.sandbox_backend` |
| Spawn targets and requests, spawn quota, bounded reflection | `Bound` parameters and fields, no defaults | a compile error |
| Workflow termination | `TerminationBound::max_rounds`, `deadline_ms`: `Bound`, required | a compile error; YAML `choice.missing.workflow.<field>`, and a present but invalid value (`-1`, non-numeric) is refused |
| `McpServer`, `A2aServer` authority | `per_request(ceiling)` or `single_trusted_caller(held)` | a compile error; §2.3 |
| Sandbox backends | `build_default_sandbox_registry` deleted; the host registers each backend it chose | `choice.missing.sandbox_backend` |
| `KataBackend`, Docker/containerd surfaces | `create(config)` factories: image required and digest-pinned, `isolation` required with `Bound` limits | a compile error; tag-only fails `sandbox.image_not_pinned` |
| `LinuxNativeJailBackend` cgroup root; provider-client `rt::Runtime&` | constructor parameters | a compile error |
| `EnvSecretSource` prefix | `EnvSecretSource::create(prefix)`; `""` refused | `secret.empty_prefix` |
| Capability caps (`NativeExec`, `Exec`, `FsRead`, `FsWrite`, `NetOut`), C++, CRTP and declarative | `Bound<…>` fields, attenuated with `covers`; the declarative parser requires each cap or `unbounded` | a compile error; `choice.missing.grant.<field>` |
| Time limits | `http` `total_timeout` and `EffectContext::deadline`: `Bound<milliseconds>` / `Deadline::none()`, not `0` and a default `time_point` | a compile error |
| Agent YAML strictness | `profile:` parsed against the option table; `sandbox.fallback` refused until supported | `choice.unknown_option` naming the table |

**Stay optional, documented as unenforced:** `TerminationBound::token_budget` (the supervisor only books
it, `graph.hpp:222-235`), `AgentMetadata::telemetry` (enforced only by the harness, `harness.hpp:447,580`) and
`stateless_pool_size` (no pool exists). Requiring them would record a choice nothing enforces.

**Both layers are checked.** A raw `AgentSession` does not go through registration and the protocol surfaces
drive it directly, while approval, concurrency and strictness live only in `AgentMetadata`.

### 2.3 Authority: what each source means, everywhere it is carried

`set_require_authority` is removed. `EffectContext` carries three engine-set fields, assigned in every branch
of `apply_dispatch_authority`: `authority_source`, `authority_expires_at` (nullopt under
`single_trusted_caller`), and `session_nonce` (§2.4, built).

- **`per_request(ceiling)`**: the ceiling is a `CapabilitySet` or the named `CapabilitySet::no_ceiling()`. A
  `StartRun` or `ResolveInteraction` without a live authority fails `run.authority_required` /
  `run.authority_expired`. A request authority holding anything outside the ceiling is refused
  (`run.authority_exceeds_ceiling`, via `attenuate`), never silently narrowed. The bridge passes
  `AgentMetadata::capability_ceiling`; a `per_request` YAML agent with no `capabilities` fails
  `choice.missing.capability_ceiling` at bind, so YAML is never pushed to `no_ceiling()`.
- **Expiry is checked at admission**, as ADR-242 R3 rule 1 says ("a run admitted under live authority runs to
  its end under it"): at `StartRun`, at `ResolveInteraction`, and at each spawn, where a child is admitted
  against the parent's `authority_expires_at`. `RequestAuthority`'s expiry is `Expiry::at(t)` or the named
  `Expiry::never()`; the "already expired" default goes.
- **The chat client's own credentials are not the caller's**: provider `cap::Secret`s are held by the session
  for the model call only, never unioned into the request's authority or visible to tools.
- **`single_trusted_caller()`**: a request carrying an authority is refused (`run.authority_unexpected`)
  instead of being silently replaced. The admission skip for a request that names no caller
  (`agent_session_core.cpp:144`) is this option's disclosed residual; the server module refuses the option.
- **A null `capabilities` pointer authorizes nothing** and is never dereferenced.
- **Helpers that start runs on a session's behalf** (`run_with_bounded_reflection`, the builders' `ask`) take
  the authority to carry as a parameter on a `per_request` session.
- **`McpServer`/`A2aServer`**: `per_request(ceiling)` refuses a call without a grant (`mcp.authority_required`)
  and one outside the ceiling; `single_trusted_caller(held)` refuses a call that carries one
  (`mcp.authority_unexpected`).
- **Workflow executors** send no authority; a workflow tag built over a `per_request` session fails
  `workflow.node_requires_authority` (the tag carries the session's source, re-checked per call), and
  `WorkflowChatClient` is refused on a `per_request` session.
- **Spawned children**, on both paths (`perform_agent_spawn`, `multi_agent::spawn`): the child inherits the
  parent's source from `ctx.authority_source`. Under `per_request` its authority is the derived on-behalf-of
  principal, the minted child capabilities and the parent's expiry, never the parent's whole grant. The
  target's metadata is validated and applied (`validate_agent_metadata`, `declared_descriptors`): its turn
  cap is `tighter(target, request)`, its approval floor holds. Its budget is `tighter(target, remaining)`,
  charged to the parent, on both paths.

**Choices survive every way a session is re-made**: clear un-initialises it; fork copies `SessionChoices`;
the snapshot record stores them and restore requires the same source and covered budgets
(`session.choices_widened`); a record written before this ADR fails `session.record_without_choices`, and
`migrate_record(record, choices)` converts it, inferring nothing.

### 2.4 Mounts and network: unconditional, against an authority the engine was handed (built)

`authorize_spec` checks every host-path mount against `cap::SandboxMount` grants and every non-`deny_all`
network policy against `cap::SandboxNetOut` grants, whether or not any grant of that kind exists. A grant with
an empty `host_path_prefix` is refused (`sandbox.mount_grant_invalid`). Paths compare in one normal form (on
Windows `\` becomes `/`, ASCII case-insensitive; elsewhere byte-exact).

**Every path a backend uses is a checked mount.** Deployment paths (the worker's directory, `python_home`,
`extra_sys_path`, the pdf worker's directory and scratch) are read-only `MountSpec`s with
`guest_path == host path`, checked like any other; nothing is granted on the side. Every AppContainer grant
the engine makes goes through one function, `grant_spec_mounts`, after `authorize_spec` (G-C5(e)).

**The Windows grant model.** All native-jail sandboxes share one AppContainer SID, and an ACE on it outlives
every process (ADR-041). So:

- **Read-only grants** go to the shared SID and are idempotent by the DACL itself: an explicit ACE already
  carrying the mask means nothing is written.
- **Read-write grants go to the sandbox's own capability SID**, derived from a random name; its process token
  holds that capability, and the ACEs are revoked when the sandbox is destroyed (or when a create fails after
  granting). An ACE left by a crash names a SID no later token holds, so it grants nothing. One sandbox
  cannot write another's mount.
- **Every DACL edit, with the check before it, runs under one cross-process lock.** An edit is a
  read-modify-write; unserialized, a grant that read the DACL before a sibling's revoke writes the revoked
  ACE back (measured: the DACL filled to 64 KB under 8 concurrent threads).

**Deployment paths are never writable by a sandbox**, in this process or another:

1. *This process*: a ledger refuses a read-write mount and a deployment path that overlap, in either order,
   keyed by directory identity (`GetFinalPathNameByHandleW`: short names, `\\?\`, junctions and case resolve
   to one key). It is permanent for the process: a directory a sandbox could once write may hold what it wrote.
2. *Other processes, after the fact*: a deployment path whose DACL gives write to the shared SID or to any
   capability SID (a live or crashed sandbox's grant, an older engine's shared-SID grant) is refused.
3. *Other processes, later*: every deployment path carries an allow ACE for a fixed marker capability SID that
   no token holds; a read-write mount whose DACL names it (it is, or lies inside, a deployment path) is
   refused. A deny ACE cannot serve: measured, a deny for the package SID or for ALL APPLICATION PACKAGES does
   not stop a capability holder.

All three fail `sandbox.deployment_path_writable`. What none sees is in §6.

**Linux**: guest paths must be normalised absolute paths other than `/` (`sandbox.guest_path_invalid`), and
two mounts whose guest paths overlap are refused (`sandbox.mount_overlap`), so a later mount cannot mask an
earlier one.

**Whose authority is checked:**

- **A site with a calling `EffectContext`** sets `spec.capabilities` to a subset of `*ctx.capabilities`.
- **`MediatedPythonRunner`** is one runner per session, enforced by the runner. `initialize(host_grant, ctx)`
  takes an explicit host grant, binds the `session_nonce` of `ctx` (a minted one is required), and emits a
  `SandboxExec` "create" row. A run with no nonce, another session's, or a later epoch of the bound session
  (clear, restore, fork) is refused (`codeact.no_session`, `codeact.runner_bound_to_other_session`,
  `codeact.runner_session_moved`). The nonce is minted by the session: its never-reused identity
  (process-wide, out of line, so one counter per process) and an epoch that moves on `initialize`, clear,
  restore and fork. It is identity, not authority; a host reads it (`session_nonce()`) to create the session's
  runner. Each `mount_roots` entry must be one of the spec's read-write mounts (the relay opens files under
  it). Per-call file use stays checked per open against the caller's `FsRead`/`FsWrite`.
- **The runner's tool bridge** runs each bridged call under `config.capabilities` narrowed to the caller's
  `ctx.capabilities`, and the tool body sees that narrowed set. Approval is a required
  `BridgeApproval::per_call(decider)` or `standing_for(principal)`; a standing approval for the empty
  principal approves nobody.
- **`extract_pdf_text`** checks its mounts against the calling session's `SandboxMount` grants on both
  platforms and lists them in `required_grants()`.

### 2.5 Unmediated surfaces

`cap::UnmediatedShell{surface, network}`, scoped by surface id, is checked in `SandboxRuntime`, the point every
Docker/containerd consumer passes, for surfaces that declare `mediated = false`. A surface whose isolation is
not `deny_all` requires `network = true`. Each check emits a `SandboxExec` audit row. The containerd surface's
read-write host bind (`containerd_execution_surface.hpp:500-508`) goes through `authorize_spec` in the same
slice. The hidden substitutions go: `isolation` is required with `Bound` limits, and `unbounded()` passes no
flag; a stated limit the surface cannot enforce fails `sandbox.limit_unsupported`. Hostile container tests
keep real limits. `capability_kind::unmediated_shell` is added and its value stays reserved after step 3.

### 2.6 Recording

- `RunStarted` gains `authority`, `token_budget` and `max_turns`; the session exposes `choices()`.
- `validate_agent_metadata`, `register_agent` and the YAML compiler return the choice summary.
- Every `SandboxExec` "create" row carries `limits`, including any engine floor (§2.1), so the recorded value
  is the enforced one.
- A run without an event sink records nothing to an audit trail; nothing here requires one (§6).

## 3. Alternatives

- **Runtime validation everywhere.** Turns every omission into a test-time failure. Kept only for
  `AgentMetadata`, which YAML builds field by field.
- **A sentinel integer for unbounded.** An ordinary number in arithmetic and `min()`. Rejected.
- **Derive grants from configuration, or exempt deployment paths.** The engine granting itself authority.
  Rejected.
- **An in-memory deployment registry alone** (revision 4). The ACEs it guards outlive the process; a second
  process accepted a directory the first had made writable (executed). Rejected for the grant model.
- **A deny-write marker on deployment paths.** Does not stop a capability holder (measured). Rejected for an
  allow ACE naming a SID no token holds.
- **Scanning a deployment path's subtree for write ACEs.** The worker's directory can be a whole build tree.
  Rejected; the gap is a residual.
- **Mid-run expiry checks** (revision 4). Contradicts ADR-242 R3 rule 1, which is Judged. Rejected.
- **Silently narrowing a request authority to the ceiling.** Drops entries with no error. Rejected.
- **Binding the Python runner to a principal or a `session_id`.** Principals are shared; a restored copy
  reuses its id. Rejected for the nonce.

## 4. Gates

Each with a positive control (022 §5); each test is written first and fails where the property is absent.

| Gate | Claim | Disproof / positive control |
|---|---|---|
| **G-C1** | No unbounded by omission | `try_compile` negatives for every required field and parameter in §2.2, each beside a fully specified positive |
| **G-C2** | No unbounded by zero | `at_most(0)` refused at each check site, and YAML `-1` and non-numeric limits refused; `unbounded()` in the same place starts and is recorded |
| **G-C3** | A missing choice is a named error listing every option | each `AgentMetadata` field and a tool without approval fail `choice.missing.<field>`; round trip: every listed option parses, an unlisted one is refused with the list; positive: declared, it registers and runs |
| **G-C4** (built) | Mounts and egress checked unconditionally; grants name their paths | `test_sandbox_capability_authorization` G1a-d, G7, G15, G16; Kata slice 2 and 9/10; a planted `if (!grants.empty())` guard fails it |
| **G-C5** (built) | The engine does not grant itself paths, tools or approvals; deployment paths are never writable | `test_mediated_python_runner_choices` A (host grant missing a path), B (nonce binding), F (read-write over `python_home`); `test_rt_agent_session_nonce` N1-N7 (restore mutant fails); `test_native_jail_rw_capability_windows` W1-W2 (per-sandbox write, revoked), L1-L3 (each deployment layer, L3 from a second process; a mutant per layer fails it); `test_native_jail_ro_mount_dedup_windows` (DACL writes, not counts; mutant fails); `test_native_jail_grant_sites` (source scan; planted call fails); `test_native_jail_backend_concurrency_windows` (no write grant survives; unlocked mutant fails); `test_tool_bridge` G-C5(c)/(g); Linux `test_native_jail_backend_linux` (overlap, seven malformed guest paths); `test_extract_pdf_text` (no `required_grants()` refused) |
| **G-C6** | Authority sources mean what they say | `per_request(ceiling)` refuses missing, expired-at-admission and out-of-ceiling authority; `single_trusted_caller()` refuses a carried authority; provider secrets invisible to tools; both servers both ways; workflow tag and `WorkflowChatClient` refused under `per_request`; a YAML `per_request` agent without `capabilities` refused; null capabilities authorize nothing (ASan) |
| **G-C7** | Choices survive re-making; children never widen | clear, fork, restore and `migrate_record` as §2.3; on both spawn paths a child is refused a capability the minted set lacks, runs under a derived principal, is admitted against the parent's expiry, and runs under the target's turn cap and approval floor |
| **G-C8** | No backend by linkage, platform or null | no `build_default_sandbox_registry` symbol; empty or omitted registry fails `choice.missing.sandbox_backend`; positive: registering it resolves it |
| **G-C9** | Images pinned | tag-only refused by `KataBackend::create` and the surfaces; digests construct (thnak-server) |
| **G-C10** | Unmediated surfaces are a granted choice | a fake `mediated = false` surface (runs on every platform) and the real ones on thnak-server: no grant refused and audited, wrong surface id or `network = false` refused |
| **G-C11** | The test preset stays in tests | CI check over `include/`, `src/`, `tools/`, `examples/`; no production preset records an `unbounded()` it did not state; planted uses fail it |
| **G-C12** | Choices are recorded, as enforced | `RunStarted` and `choices()`; a source scan finds every `SandboxExec` "create" emitter and each carries `limits`; a recorded engine floor equals the enforced one |

G-R13 ("refuses to start ... with any one of them unset") is G-C1 + G-C3 + G-C8 plus the builder's
`choice.missing.secret_source`. Transport is not here: the core has none to default (ADR-242 R4).

## 5. Migration, in slices (each a commit with its gate; one PR per two or three slices)

1. **Vocabulary** (built).
2. **Mounts, network, the Python runner (G-C4, G-C5)** (built): unconditional checks, the grant model,
   deployment layers, the nonce, the bridge, `extract_pdf_text`, the Linux guest paths. ADR-242 §3a's Kata row
   and 008 §2 amended.
3. **Session choices and authority (G-C6, G-C7, part of G-C1/G-C2):** `SessionChoices`, the sources and their
   refusals, ceilings (sessions and both servers), admission-time expiry, clear/fork/restore/spawn,
   `RunStarted`, workflow validation, `AgentSessionOptions{session_id, principal, choices}`. The production
   `initialize` callers (`agent_session_bridge.hpp:355`, `session_builder.hpp` ×3, `eval_trial.hpp:459`,
   `agent_spawn_child_run.hpp:167`, `tools/test_driver/test_driver.hpp:2754,3320`, `tools/*_chat.cpp`,
   `examples/`) are rewritten by hand; tests by a script mapping each test's arguments to the equivalent
   choice. The bridge maps `max_turns` into a `Bound` here, refusing `0` (Y8 flips). G-C11 lands here.
   Built in three commits:
   - **3a, the session layer (built):** `SessionChoices`, the two sources and their refusals, admission-time
     expiry, clear/fork/restore/`migrate_record`, `RunStarted`, `AgentSessionOptions{session_id, principal,
     choices}`, the builders' `.choices()`, the test preset and its lint (G-C11). Tests:
     `test_rt_agent_session_choices` C1–C15 and five `try_compile` cases, with planted mutants. The two
     spawn paths and `eval_trial.hpp` state an interim `single_trusted_caller()` with their request's own
     bounds (an unset budget is `unbounded()`), marked in the code and in the planning log, until 3c.
   - **3b, servers, workflows, the bridge ceiling (built):** `McpAuthority` and `A2aServer`'s stated
     `AuthoritySource`, each refusing both ways (`mcp.`/`a2a.authority_required`, `_expired`,
     `_exceeds_ceiling`, `_unexpected`); `CapabilityGrant::expiry` an `Expiry`; the workflow tag carries its
     session's source (`workflow.node_requires_authority` at `initialize()`, named by
     `initialize_refusal()`, and per call); `WorkflowChatClient` refused inside a `per_request` session; the
     bridge bounds a `per_request` ceiling by `AgentMetadata::capability_ceiling` (`no_ceiling()` takes it, a
     wider one is `agent_session.override_widens`, none declared is `choice.missing.capability_ceiling`); a
     declared turn cap or budget of 0 refused in `validate_agent_metadata` and the YAML compiler (Y8 flipped).
     Tests: `test_rt_authority_surfaces` S1–S7, `test_mcp_capability_grant` claims 1, 2 and 8, Y8, with
     planted mutants.
   - **3c, spawn, reflection, ask, provider secrets (built):** `ChildSpawnRequest` states its source, expiry,
     budget and turn cap (nullopt refused, `choice.missing.spawn.*`); `perform_agent_spawn` copies the
     caller's source and expiry, refuses a context with none and an expired caller before anything is spent
     (`agent_spawn.authority_expired`), and states `tighter(target budget, declared budget, remaining)` and the
     declared turn cap; `multi_agent::spawn` re-initialises the factory's child under the caller's source,
     tightens its budget, refuses a carried authority (`multi_agent.request_authority_unexpected`) and charges
     the caller; `run_with_bounded_reflection`, `Bundle::ask`/`run` and the harness take a `RequestAuthority`
     overload; `AgentSessionCore::set_chat_client_secrets` holds the chat client's `cap::Secret`s for the model
     call only (the bridge and both quickstart builders use it). Tests: `test_rt_spawn_authority` A1–A4,
     P1–P3, M1–M3, R1, K1, K2, B1, with planted mutants.
4. **Agent metadata, tools, builders, spawn, workflows, reflection (G-C3, G-C8's null registry):**
   - **4a, tool approval (built):** `ApprovalChosenByHost` marks a tool whose mode the host states where it
     wires the tool: `AgentSpawnToolProvider` and `SandboxToolProvider` take it as a constructor argument,
     `SessionShellSandbox::tool_descriptor`, `mcp_tools_as_descriptors` and `wasm_tools_as_descriptors_from`
     as a parameter, `MandatorySandboxProvider`/`LiveShellSandboxProvider` through `set_tool_approval` (their
     descriptors stay unset, and refused, until it is called). `run_shell`, `run_command`, the four task-branch
     tools, `shell_exec` and `agent_spawn` are host-chosen. The engine's own tools with no effect outside the
     session (`search_tools`, `mount_tool`, `unmount_tool`, `quarantine_secret`, and `schedule_wakeup`, which is
     offered only to a session holding its capability) state `Approval<never_require>`. Every other `Tool<>` in
     the tree states its mode; the tests and examples that had none state `never_require`, the old default.
     Tests: `test_tool_approval_choice` T1–T6, `compile_fail` `tool_rejects_missing_approval` and
     `tool_rejects_two_approvals` (control `tool_approval_positive_control`), with planted mutants.
   - **4b, agent metadata choices (built):** `BoundValue<uint64_t>` (`core/bound.hpp`) with `consteval` `at_most(n)`
     (a `0` does not compile) and `unbounded()`; `MaxTurns<B>` and `TokenBudget<B>` take it, a bare integer does
     not convert. `AgentMetadata::max_turns`, `token_budget`, `approval`, `concurrency` and `sandbox_profile` are
     `std::optional` (`telemetry` stays defaulted, unenforced), and `validate_agent_metadata` fails an unset one
     `choice.missing.<field>` listing `agent_choice_options::*`, before checking a value (`choice.zero_bound` for
     `at_most(0)`). `register_agent<A>()` `static_assert`s all five tags. The bridge reads `*meta.max_turns` /
     `*meta.token_budget` as the declared bounds; `perform_agent_spawn` refuses a target with no turn cap or
     budget before the depth, cost token or worktree is touched. `register_agent`, `validate_agent_metadata`,
     `bind_agent_session`, `make_agent_session` and the harness's two entry points take the
     `SandboxBackendRegistry` by reference, with no default (the chat-client registry stays a nullable pointer, an
     honest "not evaluated"); an empty one fails `choice.missing.sandbox_backend`, including for a concrete-backend
     profile, and `SandboxBackendRegistry::empty()` is new. 002 §2, §3 and §6 amended. Tests:
     `test_agent_metadata_choices` M1-M6, `test_agent_registry_sandbox_backend_registry` (empty registry,
     flipped from "null registry passes"), `test_rt_spawn_authority` P4, `compile_fail`
     `agent_rejects_missing_max_turns`, `agent_rejects_raw_integer_max_turns`, `agent_rejects_zero_turn_cap`
     (control `agent_choices_positive_control`), with planted mutants; the tests and examples that had none state
     the old defaults (`ae_test::stated_metadata()`).
   - **Header split (2026-10-23, no behaviour change):** PR #217's edits to `core/bound.hpp` recompiled ~356 translation
     units (sccache hit rate 39%, a 30-minute Windows leg). `choice_missing` moved to `core/choice_error.hpp`,
     `BoundValue` to `core/bound_value.hpp`, and `bound.hpp` is frozen (all three are layer V); the validators,
     `validate_agent_metadata` and `invoke_agent_tool` moved to `src/core/agent_registry.cpp`, the option tables to
     `core/agent_choice_options.hpp`, and the test stubs to `tests/support/stub_sandboxes.hpp`. Slice 5 should put
     every `cap::*` limit field in one PR: `capability.hpp` reaches ~490 translation units whatever the split.
   - **4c, builders, spawn targets, declarative parsers (built):** the three quickstart builders have no default
     identity or store: `.session_id()`, `.principal()` (non-empty id) and, for the raw builder, `.store()` are
     required, `choice.missing.session_id` / `.principal` / `.secret_source` (the provider and composed builders
     already required a store, `quickstart_builder.no_store`, unchanged); `SpawnTargetRegistry::register_target`
     refuses a target whose metadata states no turn cap or budget, or a zero one (the spawn path keeps its own
     refusal); the YAML compiler reads approval and concurrency from `agent_choice_options` (an unlisted value is
     refused with every option named), accepts the named value `unbounded` for either limit, refuses any sandbox
     profile but `strict` (it used to become `is_strict = false` with all-zero traits), and records
     `sandbox.fallback` in `AgentMetadata::sandbox_fallback`, each name checked against the host's registry by
     `validate_agent_metadata`. A recorded chain is a statement, not a behaviour: nothing consults its order yet.
     002 §9 Q3 amended. `capabilities` for a `per_request` YAML agent waits for slice 5's declarative grant
     parser (the bridge already refuses such an agent without one, `choice.missing.capability_ceiling`). Tests:
     `test_session_builder` B26-B33, `test_rt_spawn_authority` P4, `test_agent_yaml_limits` Y9,
     `test_agent_yaml_compiler` F-3b/F-3c, `test_agent_metadata_choices` M5b.
5. **Resource limits, capability caps, time limits (rest of G-C1/G-C2, G-C12):** `Bound` fields, backends read
   `limit()`, every substitution site in the planning log removed or named as an engine floor, the
   declarative grant parser, `SandboxExec` `limits`.
6. **Backends, Runtime, secrets, unmediated surfaces (G-C8 to G-C10):** registry by registration, `create()`
   factories with pinned images, cgroup root, provider Runtime (coordinated with ADR-237's #180/#182),
   `EnvSecretSource::create`, `cap::UnmediatedShell{surface, network}`, the containerd host bind.

Each slice updates ADR-242 §3a in the same commit (a closed cell moves to "engine enforces" and cites its test,
G-R12) and amends the specs that state the old defaults (002 §3, 008 §2, 014's termination bound, 020 §4).
Every slice reruns the full suite on Windows and on Linux (thnak-server, gcc and clang, and the privileged
jail run); a test whose outcome changes is read by hand, never re-baselined.

## 6. Residuals

- **Deployment paths, what no layer sees** (§2.4): a read-write mount strictly inside a deployment path made by
  another process *before* that path was first recorded (no layer looks below the path); a read-write mount
  over an *ancestor* made by another process while a worker using the path is alive (re-checked at every
  worker creation, not continuously); and content a sandbox wrote while a directory was writable, which
  revoking the ACE does not undo. A host that mounts its interpreter directory read-write has made that choice.
- **Dead ACEs accumulate.** A crashed sandbox's capability-SID ACE stays (harmless, it names no live token)
  until the directory is deleted; enough crashes against one directory fill its DACL (64 KB) and later grants
  there fail with a Win32 error, not silently.
- **Read-only grants stay on the shared SID** (ADR-041): any native-jail sandbox can read what any other was
  granted read-only. Read-write is no longer shared.
- **The worker's own directory is a deployment path, and its grant covers everything beneath it.** In this
  repository's builds the worker sits at the build root, so its read-only grant covers the build tree and its
  marker makes the whole tree non-writable to sandboxes. A deployment installs the worker in a directory of its
  own.
- **The containerd surface's host bind is unchecked** until slice 6.
- **The sandbox providers' approval is checked per turn, not at construction**: they are default-constructed
  (`AgentSession::clear_in_process_state` needs that), so `set_tool_approval` is a setter and an unset provider
  fails the first turn that offers its tools, with the tool named. The engine's five session-internal tools
  carry the engine's stated `never_require`; a host that wants a human in front of `schedule_wakeup` does not
  grant its capability (or overrides the descriptor in its provider).
- **`extract_pdf_text` keeps its own backend instance** (and on Linux its cgroup root) until ADR-243 gives tools
  a backend seam; its scratch directory is shared across sessions.
- **Server/session authority mismatch is caught per request**, not at construction (`RunStarter` hides the
  session's mode); it fails closed with a named code.
- **Workflows cannot carry a caller's per-request authority** to node sessions yet; such nodes are refused.
- **`multi_agent::spawn`'s factory states the child's turn cap and budget**; `spawn()` keeps the turn cap and
  only tightens the budget, since the factory is the host's statement of the target (slice 4 adds target
  validation). `TrialSpec::extra_capabilities` (`eval_trial.hpp`) still merges a host's chat-client secret
  into the trial's own grants; slice 4 makes `TrialSpec` state it apart.
- **A chat client whose session never calls `set_chat_client_secrets`** sees the run's authority in its model
  call, as before; the bridge and the quickstart builders always call it, a hand-wired session chooses.
- **Unenforced, so optional:** `TerminationBound::token_budget`, `AgentMetadata::telemetry`,
  `stateless_pool_size`; and a raw session with no event sink records nothing to an audit trail.
- **`RuntimeConfig{}`'s thread counts** stay defaulted (sizing moves to ADR-243's `Engine`); `A2aServer`'s task
  store never evicts (the server module's).
- **Narrowing defaults stay** for now (the planning log lists them); ADR-243's facade decides which become
  required.
- **`single_trusted_caller` and `CapabilitySet::no_ceiling()` remain honest options**, not safe ones; both are
  named so a reviewer can find them.

## 7. Red-team record

Every cited location was re-checked before folding in; the planning log has each finding and its evidence.

- **Round 1** (revision 1): 3 FATAL / 9 MAJOR / 6 MINOR. No caller authority at runner creation; null
  capabilities; an unacknowledged exemption list.
- **Round 2** (revision 2): 1 FATAL / 10 MAJOR / 8 MINOR. Carrying the parent's grant to a child; the per-run
  runner check; the bridge's configuration-built authority.
- **Round 3** (revision 3): 0 FATAL / 9 MAJOR / 7 MINOR. The ceiling primitive, restore ordering, runner
  identity, the bridge `ctx`, never-revoked ACEs over deployment paths.
- **Round 4**, two reviewers. Code (slice 2), 1 FATAL / 3 MAJOR / 8 MINOR, most of it executed: the in-memory
  ledger did not survive the process (fixed by the grant model, proven across processes), spelling aliases,
  a dedup gate that could not fail, no session-level nonce test. Fixing them found a DACL edit race (fixed,
  proven by mutant). Design (revision 4), 1 FATAL / 15 MAJOR / 13 MINOR: undeclared tool approval ran
  unapproved; mid-run expiry contradicted ADR-242; defaulted principals; null registries; G-R13 rows with no
  gate; YAML divergences (`-1` unbounded, unknown `profile`, empty ceilings); spawn ignoring the target; hidden
  substitutions beyond Docker. All folded in above.
