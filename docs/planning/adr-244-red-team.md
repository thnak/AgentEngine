# ADR-244 red-team log

Work log for [ADR-244](../../decisions/ADR-244-required-choices-step-1b.md). The ADR's §7 summarises; this
file keeps each finding with its evidence and what was done, plus the inventory and history the ADR no longer
carries. Every cited location was re-checked against
main @ 7f224166 before folding in.

## Round 1 — 2026-10-21, against revision 1

Verdict: **not ready** (3 FATAL, 9 MAJOR, 6 MINOR).

| # | Sev | Finding | Evidence | Disposition (revision 2) |
|---|---|---|---|---|
| 1 | FATAL | `MediatedPythonRunner` has no caller authority to pass at creation | `mediated_python_runner.hpp:130` (`initialize()` takes nothing), `.cpp:73` (`EffectContext init_ctx{}`) | explicit host grant at `initialize`; revision 2's per-run check later replaced (round 2 M1) |
| 2 | FATAL | `*ctx.capabilities` dereferences null on real paths | `effect_context.hpp:47`; `agent_session_core.cpp:967`, `:681-695` | null is the empty set |
| 3 | FATAL | `grant_ro_path_once` and `create_python_worker`'s `grant_ro` bypass `authorize_spec` | `extract_pdf_text.hpp:346-357`; `native_jail_backend.cpp:626-640` | deployment paths become checked mounts |
| 4 | MAJOR | `cap::UnmediatedShell` covered `shell_exec` only; surfaces default to tag-pulled images | `mandatory_sandbox_provider.hpp:51,215-237`; `docker_execution_surface.hpp:666`, `docker_persistent_shell_surface.hpp:142`, `containerd_execution_surface.hpp:753` | gate in `SandboxRuntime`; pinned images |
| 5 | MAJOR | `NativeExec`/`Exec` caps optional, omitted = unlimited | `capability.hpp:150-152,194-206`; `native_process_spawn.cpp:226-228`; `native_shell_session.cpp:378-380` | `Bound` caps |
| 6 | MAJOR | Missed budgets: workflows, spawn, builders, `set_max_turns`, `remaining_token_budget`, `RuntimeConfig`, cgroup root | `graph.hpp:221-257`; `agent_spawn.hpp:170`; `session_builder.hpp:845-846,1148-1149,1343-1344`; `agent_session_core.hpp:634`; `linux_native_jail_backend.hpp:88` | added; `RuntimeConfig` sizing a residual |
| 7 | MAJOR | clear/fork/restore drop the choices | `agent_session_core.cpp:748,752`, `:681-695`; `.hpp:311,351-389` | rules + G-C7 |
| 8 | MAJOR | `single_trusted_caller` silently ignores a supplied authority | `agent_session_core.cpp:964-968`; `a2a/server.hpp:127-142` | refused |
| 9 | MAJOR | Spawned children get an engine-chosen authority mode and outlive parent expiry | `agent_spawn_child_run.hpp:166-170` | inherited; revision 2's mechanism later fixed (round 2 F1) |
| 10 | MAJOR | `span<backend_kind>` cannot carry `KataBackendConfig` | `default_sandbox_registry.cpp:70` | host registers instances |
| 11 | MAJOR | `AgentMetadata` could not spell unbounded; narrowing had no order | `agent_session_bridge.hpp:310,324-334` | `optional<Bound>`, `covers`/`tighter`, existing validator extended |
| 12 | MAJOR | scripted rewrite could put the test preset in production | 6 production `initialize` callers in `include/` | by hand + G-C11 |
| 13 | MINOR | Docker/containerd have no defined "unbounded" (0 → 512 MiB / 128 pids) | `docker_execution_surface.cpp:590`, `containerd_execution_surface.hpp:448` | substitution removed |
| 14 | MINOR | internal `Instance` holders would force fake defaults; "never a set it constructs" forbade attenuation | `native_jail_backend.hpp:324` etc.; `wasm_backend.cpp:268` | `optional<ResourceLimits>`; "a subset of the caller's" |
| 15 | MINOR | recording skipped without a sink | `emit_run_event_for` | `choices()` accessor; telemetry is a required choice |
| 16 | MINOR | gates that could pass with the property absent | G-C3, G-C5, G-C9 | tightened |
| 17 | MINOR | slice 2 platform split; pdf backend needs API work | `extract_pdf_text.hpp:340-345` | one commit; backend choice a residual |
| 18 | MINOR | factual errors: line numbers, admission skip, `stateless_pool_size`, CRTP compile-time check | `extract_pdf_text.hpp:346,395`; `agent_session_core.cpp:144`; `agent_registry.hpp:74` | corrected |

## Round 2 — 2026-10-21, against revision 2

Verdict: **not ready** (1 FATAL, 10 MAJOR, 8 MINOR).

| # | Sev | Finding | Evidence | Disposition (revision 3) |
|---|---|---|---|---|
| F1 | FATAL | Carrying the parent's live grant into a spawned child installs the parent's whole set, since `per_request` replaces capabilities wholesale; `EffectContext` has no expiry to carry | `agent_session_core.cpp:959-961`; `agent_spawn_child_run.hpp:74-76,170`; `effect_context.hpp:38-60` | child authority = derived principal + minted child set + parent expiry; `EffectContext::authority_expires_at`; G-C7 negative |
| M1 | MAJOR | Runner is one worker per object and ADR-030 already forbids sharing; a per-run `SandboxMount` check widens callers and leaves interpreter state untouched; per-open `FsRead`/`FsWrite` already checks file use | `mediated_python_runner.hpp:98-102`; `codeact_runner_binding.hpp:47-58`; `native_jail_handle_relay.cpp:159-172,307,353` | one runner per session, enforced by the runner; no per-run mount check |
| M2 | MAJOR | Bridged `agent.tools` run under `grant_root(config.capabilities)` | `tool_bridge.hpp:72-78` | attenuated to the caller per call; G-C5c |
| M3 | MAJOR | Deployment paths as plain mounts regrow the AppContainer DACL (ADR-004 finding 9); guest path unspecified | `native_jail_backend.cpp:204-216,613-623`; `sandbox.hpp:231-233` | dedup in the mount step; `guest_path == host path`; G-C5d |
| M4 | MAJOR | Runner is Windows-only; Linux pdf already uses mounts; a required cgroup root makes pdf's static backend hardcode one | `CMakeLists.txt` WIN32 block; `extract_pdf_text.hpp:395-412` | claims corrected; residual names the cgroup root |
| M5 | MAJOR | Docker/containerd networking never passes `authorize_spec` | `containerd_execution_surface.hpp:447`; `docker_execution_surface.cpp:589` | `UnmediatedShell{network}` |
| M6 | MAJOR | More implicit caps/budgets: CRTP `NativeExec` tags (0 = unbounded), declarative grant parser, `FsRead`/`FsWrite`/`NetOut` caps, bounded reflection, `SpawnQuota`, surfaces' `isolation = {}`, A2A task store | `capability.hpp:503,540-547,567-603`; `native_exec_grant.hpp:60-70`; `bounded_reflection.hpp:111-138`; `harness.hpp:584-586`; `agent_spawn.hpp:209`; `a2a/server.hpp:171,182` | all added except the A2A store (R4 residual); `BoundValue` NTTP |
| M7 | MAJOR | `TerminationBound::token_budget` is not enforced | `graph.hpp:222-235` | kept optional, named |
| M8 | MAJOR | Restore equality refuses tightening; legacy records unhandled | `agent_session_core.hpp:358-372` | "covered by"; `session.record_without_choices` |
| M9 | MAJOR | `per_request` skips the agent's ceiling | `agent_session_bridge.hpp:347-349` | attenuated to the session ceiling |
| M10 | MAJOR | `McpServer` per-call grant replaces `held_`; `A2aServer` cannot see the session's mode; workflow nodes send no authority | `mcp/server.hpp:209-229`; `a2a/server.hpp:137-145`; `agent_workflow_executor.hpp:167` | refusals both ways; mismatch residual; `per_request` nodes refused at validation |
| m1 | MINOR | Tag is `Stateless<N>`; requiring it overturns 002 §9 Q3 | `agent.hpp:107-108`; `agent_registry.hpp:69` | named; 002 amended in slice 4 |
| m2 | MINOR | Constructors cannot fail with a named code | — | `create()` factories |
| m3 | MINOR | G-C11 misses `tools/` and `examples/` | `tools/test_driver/test_driver.hpp:2754,3320` | scope widened; hand-rewritten |
| m4 | MINOR | Runner creation emits no audit row | — | `initialize(host_grant, ctx)` emits one |
| m5 | MINOR | Shared AppContainer SID, grants never revoked | ADR-041 | named in §6 |
| m6 | MINOR | Slice-5 rewrite must not unbound hostile Docker tests | `test_docker_isolation.cpp:232` | stated |
| m7 | MINOR | `UnmediatedShell` kind touches the kind enum and persistence | `capability.hpp:281,316` | value reserved on deletion |
| m8 | MINOR | pdf tool makes hosts grant internal paths; shared scratch | `extract_pdf_text.hpp:253-258` | `required_grants()`; disclosed |
| — | gates | G-C2 sites not enumerated; G-C5b/d and G-C7 passed with the property absent | — | enumerated and rewritten |

## Round 3 — 2026-10-21, against revision 3

Verdict: **not ready** (0 FATAL, 9 MAJOR, 7 MINOR). The reviewer also confirmed slice 2's in-progress
`authorize_spec` edit was read, not modified. One finding was found worse on re-check: the empty-prefix
case of `path_prefix_covers` (below, m1).

| # | Sev | Finding | Evidence | Disposition (revision 4) |
|---|---|---|---|---|
| M1 | MAJOR | No intersection primitive for the `per_request` ceiling (`attenuate` is all-or-nothing, `narrow_to_ceiling` silently drops); raw sessions get "no ceiling" implicitly; bridge chat-client secrets sit outside the ceiling | `capability.hpp:926-936`; `agent_session_bridge.hpp:231-243,349`; `secret.hpp:259` | required `per_request(ceiling)` / named `no_ceiling()`; `attenuate` refusal; client secrets held by the session, not unioned |
| M2 | MAJOR | "`per_request` at least as strict as `single_trusted_caller`" is false | `agent_session_core.cpp:970-977` | restore requires the same source |
| M3 | MAJOR | Expiry checked only at run start; `RequestAuthority::expiry{}` means already expired; "forever" is `time_point::max()` | `agent_session_core.cpp:966,1703-1708`; `.hpp:99` | checked every turn and tool dispatch; `Expiry::at`/`never` |
| M4 | MAJOR | `multi_agent::spawn` reuses the parent principal and is voided by `per_request`; `WorkflowChatClient` runs at host-static authority; no carrier for "the parent's source" | `multi_agent.hpp:180-226`; `workflow_as_chat_client.hpp:117-118,171-175` | `ctx.authority_source`; same rule on both spawn paths; `WorkflowChatClient` refused under `per_request` |
| M5 | MAJOR | Workflow validation cannot see node sessions | `graph.hpp:297`; `agent_workflow_executor.hpp:97-100`; `workflow_supervisor.cpp:27` | source carried in the tag; checked at tag build and per call |
| M6 | MAJOR | The runner has no session identity: principal shared, `run_id` parsable, restored copies reuse `session_id_`, clear+reinit hands globals to the next user | `sandbox.hpp:289`; `agent_session_core.cpp:807` | engine-minted `session_nonce` (identity + epoch); runner refuses after its session's epoch moves |
| M7 | MAJOR | Bridge attenuation gates only the pipeline; the tool body still sees the caller's full `ctx`; `config.approved` is a standing cross-caller approval | `tool_bridge.hpp:59,68-78`; `agent_spawn.hpp:432,519` | `ctx` swapped for the call; `BridgeApproval::per_call` / `standing_for(principal)` |
| M8 | MAJOR | Read-write grants over deployment paths persist as never-revoked ACEs: cross-session code injection | ADR-041; `native_jail_backend.cpp` | per-backend deployment-path registry; overlapping read-write refused |
| M9 | MAJOR | Legacy records bricked with no migration | `session_store.hpp:196` | `migrate_record(record, choices)` |
| m1 | MINOR | `path_prefix_covers` treats only `/` as a boundary and is case-sensitive; **and `""` covers everything** (found on re-check) | `capability.hpp:556-564` | normalised comparison; empty-prefix grant refused |
| m2 | MINOR | Linux binds mounts in vector order: masking | `linux_native_jail_backend.cpp:159-173` | deployment mounts first; overlaps refused |
| m3 | MINOR | Dedup keyed on raw string, grows with unique paths | `native_jail_backend.cpp:134-145` | normalised key; growth per unique path disclosed |
| m4 | MINOR | `NativeShellSessionProvider` is also a `PersistentShellSurface` | `persistent_shell.hpp:5` | gate only `mediated = false` surfaces |
| m5 | MINOR | Null capabilities: "empty set" vs "refused" | — | "authorizes nothing" |
| m6 | MINOR | Reflection and `ask` re-send `StartRun` without authority | `bounded_reflection.hpp:157`; `session_builder.hpp:482,493,618` | authority parameter |
| m7 | MINOR | Ungated recording; `examples/` missing from the caller list; Y8 flips in slice 3 | `agent_spawn.hpp:528` | G-C12; list and slice fixed |

## Implementation notes (slice 2)

- **Slice 2a** (cb7132d8): `authorize_spec` unconditional. G1a-c and G7 in
  `test_sandbox_capability_authorization` failed on the old code and pass on the new.
- **Slice 2b**: empty-prefix `SandboxMount` refused (G15 showed an empty prefix authorized mounting `/etc`);
  Windows host paths normalised (G16); `SessionNonce` minted by the session; `MediatedPythonRunner` takes an
  explicit host grant, audits its creation and binds one session nonce; deployment paths are checked mounts
  with deduplicated read-only grants and a process-wide grant ledger (`sandbox.deployment_path_writable`); the
  tool bridge narrows to the caller and swaps `ctx`, and `BridgeApproval` replaces `approved`. Gate test:
  `tests/python/test_mediated_python_runner_choices.cpp` (A, B, D, F) plus `test_tool_bridge` G-C5(c)/(g).
- **Found while proving G-C5(a):** the worker binary sits at the build root, so its directory grant covers
  `python_home` and the whole build tree. Pre-existing; disclosed in ADR-244 §6.
- **One unexplained failure:** `test_native_jail_backend_windows` failed once (10.18 s, about twice its normal
  time) in the first ctest run after the slice-2b mount-loop rebuild, with no output captured. It then passed
  21 consecutive runs (3 through ctest alone, 3 in the same combined ctest invocation, 15 direct). Not called a
  flake; watched in every full-suite run that follows.
- **Pre-existing on main, out of scope:** `tests/rt/agent_session/test_rt_agent_session_farm_ops_live_e2e.cpp:501`
  does not compile under MSVC (a `co_return` inside a plain `std::find_if` lambda, C7617). Only built where
  the Python runner is enabled (`build-py`).
- **Slice 2c**: `extract_pdf_text`'s worker directory and scratch directory are ordinary read-only mounts
  on both platforms, checked against the caller's `SandboxMount` grants; `tools::required_grants()` lists
  them for the host. `NativeJailBackend::grant_ro_path_once` is removed; its test became
  `test_native_jail_ro_mount_dedup_windows` (one grant across 11 creates, plus an ungranted mount refused).
  `test_extract_pdf_text` gained the negative: `FsRead` without the worker grants is refused
  `sandbox.mount_not_authorized`. Built and run with `AGENTENGINE_WITH_PDF=ON` (Windows, Linux).
- **Linux jail**: overlapping guest paths refused (`sandbox.mount_overlap`) before anything is bound;
  `/work` and `/workshop` do not overlap (positive control).

## Round 4 — 2026-10-21, two reviewers

One reviewer attacked the code slice 2 shipped (PR #213), with executed probes; the other attacked revision 4's
design, mainly slices 3 to 6. Both verdicts: **not ready**.

### Code (slice 2): 1 FATAL, 3 MAJOR, 8 MINOR

| # | Sev | Finding | Evidence | Disposition |
|---|---|---|---|---|
| C1 | FATAL | The grant ledger lives in memory, but its ACEs outlive the process: a read-write mount in process 1 leaves `(M)` on the shared SID, and process 2 accepts the directory as `python_home` | executed (`ledger_probe rw` then `py`; `icacls` shows `(M)`); re-run by the author | **new grant model** (ADR §2.4): read-write ACEs go to a per-sandbox capability SID, revoked on destroy; deployment paths checked in three layers. Feasibility probed first (`scratchpad/capsid/capsid2.cpp`): a capability holder writes, a sibling of the same package does not. A package-SID deny marker was tried and **does not work** (a capability allow is not stopped by a package-SID or ALL APPLICATION PACKAGES deny), so layer 3 uses an allow ACE for a marker capability SID no token holds. Gate: `test_native_jail_rw_capability_windows` W1, W2, L1-L3 (L3 spawns a second process). |
| C2 | MAJOR | Ledger keys compare spellings: `\\?\`, 8.3 short names and non-ASCII case reach the same directory under another key | executed (alias modes) | ledger keyed by `GetFinalPathNameByHandleW`; layers 2 and 3 read the DACL itself, so spelling cannot matter there. Gate: L1 (`\\?\` spelling refused). |
| C3 | MAJOR | G-C5(d) cannot fail: Windows merges an identical ACE, so the DACL count is flat with or without dedup; and the dedup cache skipped re-granting a recreated directory | executed (`nodedup_probe`: 20 undeduplicated grants, count 5 → 7 once) | in-memory cache removed; `grant_sid_path` is idempotent by reading the DACL; the test counts DACL **writes** (`acl_write_count`). Mutant M1 (always write) fails it. ADR-004 finding 9's growth premise does not hold on this Windows build; recorded. |
| C4 | MAJOR | No test that the session mints the nonce: deleting the epoch advance in restore leaves every test green | argued | `test_rt_agent_session_nonce` N1-N7, read from the context the chat client receives; the restore mutant fails N6 and N7. |
| C5 | MINOR | `SessionNonce` is a public struct; the identity counter is `inline` (one copy per DLL) | argued | counter moved out of line (`next_session_identity()`); the nonce stays plain data and is documented as identity, not authority; `session_nonce()` accessor for hosts |
| C6 | MINOR | The runner binds on its first run, not at creation | argued | binds at `initialize`, which requires a minted nonce |
| C7 | MINOR | `standing_for(Principal{})` approves every empty-principal caller; `cli_chat` rebuilt the approval for whoever called | argued | an empty standing principal approves nobody; `cli_chat` pins the principal that bound the runner |
| C8 | MINOR | Linux: `//work` vs `/work` not an overlap; a relative guest path lands beside the jail root; `""` and `/` accepted | executed (comparison) | guest paths must be normalised absolute paths other than `/` (`sandbox.guest_path_invalid`), checked before the grants; duplicates are overlaps. Linux test covers seven bad spellings. |
| C9 | MINOR | The relay opens files under `session.mount_roots`, never compared with the spec's mounts | argued | each root must be a read-write spec mount (`native_jail.mount_root_not_mounted`) |
| C10 | MINOR | Partial failure leaves ledger entries and read-write ACEs behind | argued | everything checked before anything is granted; the ledger admits a whole spec atomically; read-write grants are revoked by `RwGrants`' destructor when a create fails |
| C11 | MINOR | G-C5(e) (no grant call outside the mount loop) did not exist | grep | `test_native_jail_grant_sites` (a source scan with a positive control); a planted stray call fails it |
| C12 | MINOR | `path::string()` is the ANSI code page under MSVC; the backend uses UTF-8 | argued | UTF-8 everywhere (`utf8_path`, `narrow`) |

Held up (executed): `..` and `.` refused, including through `\\?\`; a junction alias put the ACE on the junction,
not its target; every backend that reads mounts calls `authorize_spec`; the bridge's narrowing uses `subsumes`.

**Found while fixing C1** (not in either review): concurrent grants and revokes on one directory raced. Each
edit is a read-modify-write of the DACL, so a grant that read the DACL before a sibling's revoke wrote the
revoked ACE back; `test_native_jail_backend_concurrency_windows` (8 threads × 40 cycles on one mount) filled the
DACL to its 64 KB limit (Win32 error 122). Every DACL edit, with its check, now runs under one cross-process
named mutex. The test gained a check that no AppContainer SID can write the mount once every sandbox is gone;
removing the lock fails it on every run (3/3).

### Design (revision 4): 1 FATAL, 15 MAJOR, 13 MINOR

| # | Sev | Finding | Disposition (revision 5) |
|---|---|---|---|
| D1 | FATAL | A tool that declares no `Approval<>` runs unapproved (`tool_descriptor.hpp:53`, `tool.hpp:314`); the agent-level choice changes nothing for it | slice 4: every `Tool<>` declares approval (`static_assert`); a hand-built `ToolDescriptor` or a YAML tool without one fails registration, `choice.missing.tool.approval` (G-C3) |
| D2 | MAJOR | Mid-run expiry contradicts ADR-242 R3 rule 1 and §7, both Judged | the mid-run check is dropped: expiry is checked at admission (`StartRun`, `ResolveInteraction`, each spawn), as ADR-242 says |
| D3 | MAJOR | Slice 2 changed enforcement but not ADR-242 §3a or 008 §2 | fixed in this commit: §3a's Kata row, 008 §2 amended |
| D4 | MAJOR | Bridge and builder sessions default `session_id`/`principal` (`"s-agent"`, `"p-agent"`, `"p-quickstart"`): shared identity, approvals and quotas | slice 3: both required, `choice.missing.principal` / `choice.missing.session_id` |
| D5 | MAJOR | A null sandbox registry passes `Strict` with no backend (`agent_registry.hpp:372`) | slice 4: the registry is a required parameter; null fails `choice.missing.sandbox_backend` |
| D6 | MAJOR | G-R13's backend and secret-source rows have no gate; builders fill `InMemorySecretStore` | G-R13 = G-C1 + G-C3 + G-C8 + the two new cases (D5's registry, a builder without a secret source fails `choice.missing.secret_source`, slice 4) |
| D7 | MAJOR | The bridge has no way to receive `SessionChoices`; it would fabricate one | slice 3: `AgentSessionOptions::choices`, no default; the bridge passes it through and slice 4 tightens it with the metadata |
| D8 | MAJOR | `McpServer`/`A2aServer` `per_request()` takes no ceiling | `per_request(ceiling)` or `no_ceiling()` on both, G-C6 |
| D9 | MAJOR | The containerd surface live-binds a host directory read-write, never checked | slice 6 checks it through `authorize_spec`; until then a residual (§6) |
| D10 | MAJOR | Hidden substitutions beyond Docker/containerd (`value_or(5 min)`, `value_or(1 GiB)`, output `0` → 16 MiB, Kata timeouts, Anthropic `max_tokens` 4096) | slice 5 lists every site (below). An engine host-safety floor becomes a separately named, disclosed engine bound, recorded beside the choice; G-C12 checks the recorded value is the enforced one |
| D11 | MAJOR | Telemetry and the stateless pool are required but enforced nowhere outside the harness | both stay optional and are documented as unenforced; §2.6's "required telemetry" claim removed |
| D12 | MAJOR | Workflow YAML `max_rounds: -1` is undefined behaviour (in practice unbounded); non-numeric limits ignored | slice 4 names it: `as_bounded_integer`, refuse invalid, accept `unbounded`; G-C2 YAML case |
| D13 | MAJOR | Agent YAML `profile:` any unknown string is non-strict; `sandbox.fallback` dropped | slice 4: parsed against the option table; `fallback` refused until supported |
| D14 | MAJOR | YAML agents have an empty `capability_ceiling`, so `per_request` refuses everything and pushes to `no_ceiling()` | binding a `per_request` YAML agent without `capabilities` fails `choice.missing.capability_ceiling` |
| D15 | MAJOR | Spawn ignores the target's metadata (turn cap 25, approval floor, `validate_agent_metadata`) | slice 4: `perform_agent_spawn` validates the target and applies `declared_descriptors`; child cap = `tighter(target, request)`; G-C7 case |
| D16 | MAJOR | `http.hpp:271` `total_timeout{0}` = none; `EffectContext::deadline{}` = none | slice 5: `Bound<milliseconds>`; `Deadline::none()` |
| D17 | MINOR | More zero/empty = unbounded: `Window<0>`, `max_concurrency = 0`, plugin limits, `MountSpec::quota_bytes`, empty `guest_path_prefix`, `cap::Clock{}` resolution 0 | listed below; converted in slice 5 or named in §6 |
| D18 | MINOR | §6's narrowing defaults list is incomplete | full list below |
| D19 | MINOR | G-C3 "same object" passes by construction | round trip: every listed option parses; an unlisted one is refused with the list |
| D20 | MINOR | G-C12's emitter list is hand-kept | a source scan for `SandboxExec` "create" emitters |
| D21 | MINOR | G-C10 needs Docker, so it never runs on the Windows baseline | a fake `mediated = false` surface |
| D22 | MINOR | G-C11 matches names only | G-C11 also checks that no production preset records an `unbounded()` it did not state |
| D23 | MINOR | Live clock reads in mid-run expiry (I5) | moot after D2; admission uses the injected clock |
| D24 | MINOR | `UnmediatedShell{network}` is nearly boolean | scoped by surface id |
| D25 | MINOR | `multi_agent::spawn` child budget not `tighter(remaining)` nor charged | same rule as `perform_agent_spawn` |
| D26 | MINOR | `validate_agent_metadata` cited at its call site | fixed |
| D27 | MINOR | 413 lines; inventory, history and implementation notes belong here | moved (below) |

### Slice 5 substitution sites (D10, D16, D17)

`native_process_spawn.cpp:87` (output 0 → 16 MiB), `:246` (`wall_ms_cap.value_or(5 min)`);
`native_shell_session_provider.hpp:287` (`value_or(1 GiB)`); `native_jail_backend.cpp` and
`linux_native_jail_backend.cpp` output drains (0 → 16 MiB); `kata_backend.hpp:497,500` (0 → `kProcessTimeoutSeconds`);
`anthropic/chat_client.cpp:227` (0 → 4096 tokens); `rt/http.hpp:271` (`total_timeout{0}`); `effect_context.hpp`
(`deadline{}`); `HistoryProvider<Window<0>>` (`agent_session_bridge.hpp:96,219`); `rt/scope.hpp:70`
(`max_concurrency = 0`); `plugin/plugin.hpp:21-22`; `MountSpec::quota_bytes = 0` (`sandbox.hpp:133`, enforced
nowhere); `SandboxMount::guest_path_prefix ""`; `cap::Clock{}` (`capability.hpp:155`, resolution 0 = finest).

### Narrowing defaults that stay for now (D18)

`stream_retries = 0`, `suspend_for_approval = false`, `max_stalls`, `max_resets`, an unset `ApprovalDecider`
(deny), `NetPolicy::deny_all = true`, `sandbox_lifetime::per_session`, `sharing_mode::branch`, `EgressLimits`
(8/32/64 connections, 16 MiB), `RetryPolicy` (3 attempts), `ContentReplayGateway` (2/8),
`reflection_max_iterations = 3`, `BackgroundJobRunner` (1024), `multi_agent::Budget` (`max_in_flight` 0 → 1),
`SandboxProfileDescriptor::is_strict = true` (so `optional{SandboxProfileDescriptor{}}` reintroduces an implicit
strict), and `AGENTENGINE_NET_IO_TIMEOUT_MS` (a choice made by an environment variable).

### Revision 4's inventory (moved from ADR-244 §1)

Baseline re-verified on main @ 7f224166: every row of ADR-242 §2's table held at its cited line. The inventory and
the first two rounds found more of the same kind:

| Found | Where | Same defect as |
|---|---|---|
| Sandbox network grants are checked only if a `SandboxNetOut` grant exists | `sandbox/sandbox.hpp:255-256` | the mount row |
| Read-only deployment paths (worker dir, `python_home`, `extra_sys_path`; on Windows the pdf worker's dir and scratch) are granted outside `SandboxSpec.mounts`, unchecked | `native_jail_backend.cpp:629-642`, `tools/extract_pdf_text.hpp:348,357` | the mount row |
| `MediatedPythonRunner` creates its worker with an empty `EffectContext`; its bridged `agent.tools` calls run under `grant_root(config.capabilities)` | `mediated_python_runner.cpp:60-73`; `tool_bridge.hpp:72-78` | the mount row; I2 |
| The one-runner-per-session binding (`CodeActRunnerBinding`, ADR-030) is opt-in host wiring | `codeact_runner_binding.hpp:47-58` | I1 |
| `extract_pdf_text` picks its own backend (a function-local `static`) | `tools/extract_pdf_text.hpp:346,395` | "registered by platform" |
| `single_trusted_caller` silently replaces a supplied authority; `McpServer` does the reverse | `agent_session_core.cpp:964-968`; `mcp/server.hpp:209-229` | the authority rows |
| A `per_request` authority is installed wholesale, ceiling unapplied | `agent_session_core.cpp:959-961` vs `agent_session_bridge.hpp:347-349` | I2 |
| A spawned child gets no authority mode; its capabilities outlive a parent grant's expiry | `rt/agent_spawn_child_run.hpp:166-170` | the authority rows |
| `clear_core_state` resets budget and turn cap; the record does not store them; fork does not copy them; `set_max_turns(std::optional)` can reset the cap | `agent_session_core.cpp:748,752`, `:681-695`; `.hpp:311,634` | the budget row |
| Docker/containerd: tag-pulled default images; `isolation = {}` hiding 512 MiB / 128 pids; non-`deny_all` becomes bridge or host networking unchecked | `docker_execution_surface.hpp:666`, `docker_persistent_shell_surface.hpp:142`, `containerd_execution_surface.hpp:447,753`; `docker_execution_surface.cpp:589-590` | the Kata image, limits and mount rows |
| Capability caps are `std::optional`, missing = uncapped (`NativeExec`, `Exec`, CRTP tags with `0` = unbounded, the declarative parser, `FsRead`, `FsWrite`, `NetOut`) | `trust/capability.hpp:150-152,194-206,503,540-547,567-603`; `native_exec_grant.hpp:60-70`; `native_process_spawn.cpp:226-228` | the limits row |
| Default budgets: `ChildSpawnRequest::max_turns = 25`, `child_token_budget = 50'000`, `max_spawns_per_principal = 100`, builders' `max_turns = 25`, `run_with_bounded_reflection(max_total_tokens = 0)` | `rt/agent_spawn_child_run.hpp:84`, `rt/agent_spawn.hpp:170,209`, `core/session_builder.hpp:845-846,1148-1149,1343-1344`, `bounded_reflection.hpp:111-138`, `harness.hpp:584-586` | the turn-cap and budget rows |
| `TerminationBound` requires one bound, but each omitted field is unbounded | `workflow/graph.hpp:220-243` | the budget row |
| `AgentMetadata::stateless_pool_size` (nullopt = off) | `core/agent_registry.hpp:74`, `agent.hpp:107-108` | restrictive defaults |
| `LinuxNativeJailBackend` defaults its cgroup root | `linux_native_jail_backend.hpp:88` | the Kata runtime row |
| An empty-prefix `SandboxMount` covers every path; Windows backslash and case mismatches | `trust/capability.hpp:556-564` | the mount row |
| `RequestAuthority::expiry{}` = already expired; "never" is `time_point::max()` | `rt/agent_session_core.hpp:99` | "unbounded only as a named value" |
| `multi_agent::spawn` reuses the parent principal, no expiry, `per_request` voids the narrowing | `rt/multi_agent.hpp:180-226` | the spawned-child row |
| `WorkflowChatClient` runs inner nodes under host-static contexts | `workflow_as_chat_client.hpp:117-118,171-175` | the authority rows |
| The Python tool bridge's `config.approved` is a standing, defaulted approval | `tool_bridge.hpp:59` | the approval row |

### Revision history (moved from ADR-244 §3 and §7)

Revision 2 replaced revision 1's backend-kind list, per-run runner check and parent-grant carrying. Revision 3
replaced revision 2's strict restore equality and its "`per_request` is at least as strict" ordering. Revision 4
added the ceiling primitive, the nonce, the bridge `ctx` swap and the deployment-path registry. Revision 5
replaces revision 4's in-memory deployment registry and DACL dedup with the capability-SID grant model, and drops
the mid-run expiry check.
