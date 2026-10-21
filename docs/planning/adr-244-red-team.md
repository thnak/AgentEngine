# ADR-244 red-team log

Work log for [ADR-244](../../decisions/ADR-244-required-choices-step-1b.md). The ADR's §7 summarises; this
file keeps each finding with its evidence and what was done. Every cited location was re-checked against
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
