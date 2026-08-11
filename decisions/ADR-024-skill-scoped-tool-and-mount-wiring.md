# ADR-024 — Skill-scoped tool visibility, and materializing skill mounts into a real sandbox

**Status:** Judged — 2026-08-10, with a same-day addendum (§8) narrowing tool activation further, from
"every resolved skill, unconditionally" to "agent-triggered on demand." All three halves (§3's tool
scoping, §4's real mount wiring, §8's on-demand narrowing) are real, tested, landed code, each run
against a live model end to end (transcripts in §6 and §8).
**Relates to:** 009 §8b/§8c (skills, mount mechanism, `allowed-tools`), 026 §5/§6 (`agent.*` module
surface, "ordinary file operations"), 006 §3/§6 (the ten-step tool pipeline, `ToolTable`'s own
resolve-once-at-run-start invariant), 007 §3 (capabilities are the enforcement authority), 025 §5/§7
(worktree mounts, `materialize_mount`).

## 1. The question

009 §8's skill mechanism was previously implemented only as far as "resolve and mount a skill into
`SkillsProvider`'s own private, in-memory object store" — real, tested, but a dead end: nothing
outside that provider instance ever read from it, so a mounted skill's content was never reachable by
a real running agent, only by test-harness C++ calling `mount_read` directly. This was mistakenly
carried forward as "the skills feature is done."

**Stated so it has a wrong answer:** (a) How does a mounted skill's content become reachable by a real
sandboxed tool call, not just by test-harness code holding the object store directly? (b) Should a
skill's `allowed-tools` field additionally gate which *tools* are declared to the model and invocable
at all, or only scope *content* reachability the way every shipped agent SDK surveyed does it?

Part (b) is the contested half. Part (a) is close to mechanical once (b) is settled, but surfaced a
real, independent bug worth recording alongside it.

## 2. Prior art surveyed (informs, does not settle, part (b))

Local clones of Microsoft Agent Framework, the Anthropic C# SDK, and the OpenAI .NET SDK were read
directly (not from memory) before designing:

- **MAF**: `AgentSkillsProvider`/`SkillsProvider` exposes exactly three fixed, generic meta-tools
  (`load_skill`, `read_skill_resource`, `run_skill_script`) for every skill, every turn. Individual
  skills never become individual tool-schema entries. Tool *availability* never varies with which
  skill is active.
- **Anthropic**: file/skill content is scoped to a `container` object (`BetaContainerParams{id,
  skills[]}`, `ContainerUploadBlockParam`), reused across a conversation by id. The `code_execution`/
  `bash` tool declarations in `tools[]` are static regardless of container contents.
- **OpenAI**: `CodeInterpreterToolContainer{ContainerId | AutomaticCodeInterpreterToolContainerConfiguration{FileIds}}`
  is the same shape — a container-scoped resource set, with `CodeInterpreterToolDefinition` always
  statically declared. A real `Skills` API exists but its own spec (`containers/models.tsp`) has an
  explicit `// TODO: Add support for skills` on the one field that would attach it to a container —
  not yet wired even upstream.

**Finding, stated plainly:** no surveyed SDK varies tool *declaration* by loaded-resource state. All
three vary only what content a statically-declared tool can *reach*.

## 3. The decision on part (b) — user-directed departure from precedent

Asked directly, the user chose to scope tool visibility per mounted skill anyway (`allowed-tools`
determines which tools are both declared to the model and invocable, not merely which files are
readable), over the recommended precedent-matching alternative (content-only scoping). This ADR
records that decision and the design built to make it a *real* restriction rather than a cosmetic one.

### 3a. The real enforcement boundary (found while designing, not assumed)

`core/tool_pipeline.hpp`'s own file-top comment: a run's `ToolTable` is "resolved once into an
immutable table at run start — a mid-run change to what's registered cannot alter what a run is
allowed to call." Reading `invoke_tool()` itself confirms the actual authorization boundary is step 1,
`ToolDescriptor const* tool = table.find(request.tool_name)` — checked against whatever `ToolTable`
the **caller** passes at the invocation call site, never against whatever was declared to the model
via `ContextContribution.tools`.

Those are two independently-constructed values unless a caller deliberately makes them the same
object. A design that only filters `ContextContribution.tools` and leaves a wider table at the
`invoke_tool(...)` call site is **cosmetic** — a tool "hidden" from the model's declared list is still
callable if the model emits its name anyway and the invocation-time table still contains it. (I3 —
model output, including a tool name the model happens to emit, is data, never authority — only holds
if the invocation-time check is real; a wider invocation-time table would make the "hiding" purely a
discovery inconvenience, not a boundary.)

**Decision:** `core/skill_tool_scoping.hpp::scope_tools_to_mounted_skills(universe, allowed,
always_on)` returns one `ToolTable`, computed once per run (matching `ToolTable`'s own resolve-once
discipline, and matching skills' own "snapshotted per run", 009 §8c). The caller is required — by this
file's own prominent top comment, not by a type-system guarantee — to use that SAME table object for
both declaration and every `invoke_tool(...)`/`invoke_agent_tool(...)` call for the rest of the run.
`ToolTable::from_descriptors(vector<ToolDescriptor>)` was added (`tool_pipeline.hpp`) as the missing
runtime-construction path alongside the existing compile-time `from_tools<ToolTs...>()`, needed to
turn a filtered subset back into a real table.

### 3b. What was NOT built, and why

A type-level guarantee that declaration and invocation always share one table (e.g. a wrapper type
that owns both responsibilities) was considered and deliberately not built this pass: `AgentSession`
has no owned sandbox/tool-execution loop in production at all today (only `tools/cli_chat.cpp`'s
hand-driven loop does), so there is no single call site to attach that guarantee to yet — building one
now would be inventing structure around a session-lifecycle question (§7's own "explicitly out of
scope") this ADR does not resolve. The discipline is enforced today by convention plus a documented
comment, proven by `cli_chat.cpp` actually following it (§6).

## 4. Part (a) — materializing a mount into a real sandbox, and a real bug found along the way

`src/backends/native_jail/worktree_mount_sync.hpp`'s `materialize_mount` — real, tested (proven since
Milestone 3 Phase F1) — was the correct existing primitive for turning a worktree `Mount` into files
on a real host directory, exactly the shape `MediatedPythonConfig::mount_roots` needs. It had zero
non-test callers before this ADR.

**Bug found while wiring it up:** `SkillsProvider` set each mounted skill's `Mount.mount_id` to the
literal string `"/skills/" + name` (a path-shaped string). `native_jail`'s sandbox resolves a guest
path to a host mount root by treating the guest path's **first path segment** as the mount id
(`Internal_open`/`Internal_listdir`'s `split_guest_path`), matching the existing bare-token convention
(`"work"`, `"input"`, `"out"`). A mount id containing an embedded `/` never arises from splitting a
real guest path — this mount could never have worked against the native_jail backend as originally
shaped, independent of anything this ADR's own scope introduced.

**Fix:** `Mount.mount_id` is now the bare skill name. §8b's literal `/skills/<name>` text describes the
skill's *logical* location — still carried to the model verbatim via the advertisement message/
`PromptSkillSummary`, unaffected by this rename — not a requirement that the worktree-internal
capability-matching key be that exact string. This also gives genuine per-skill capability granularity
an operator can actually use: `cap::FsRead{"using-codeact", ...}` grants one skill's content without
exposing every other mounted skill, never expressible under one shared mount id.

`src/backends/native_jail/skill_mount_materializer.hpp::materialize_skill_mounts` is the new bridge:
resolves a `SkillsProvider` (via the new public, synchronous `ensure_loaded()` — `resolve_and_mount()`
was already fully synchronous internally; only `on_context`'s `task<>` wrapper needed a coroutine),
then for each mount calls `materialize_mount` against a real `MediatedFileSystemAdapter` rooted at
`host_root/<mount_id>`, returning `(mount_id, host_dir)` pairs ready to fold into `mount_roots`. Fails
closed — nothing written to disk for the call — if any skill's mount id collides with a caller-supplied
reserved set (`{"work", "input", "out"}`): a skill literally named `work` must never silently shadow
the sandbox's own working-directory mount.

## 5. Falsifiable claims and verdicts

| # | Claim | Evidence | Verdict |
|---|---|---|---|
| 1 | A tool absent from the scoped table is genuinely unreachable, not just undeclared | `tests/test_tool_table_scoping.cpp` R3: a real, registered tool (`alpha`) succeeds when called against the unscoped universe, and is rejected with `tool.unknown_name` (via `ToolInvocationAudit::error_code`) when called against the scoped table it was filtered out of — same tool, same capabilities, only table membership differs | **CORRECT** |
| 2 | A materialized skill's real content is reachable by real Python code through the actual mediated `open()`, not a shortcut | `tests/test_mediated_python_runner_skill_mounts.cpp` R1: `open('/real-mounted-skill/SKILL.md').read()` inside a real `MediatedPythonRunner::run()` returns the real frontmatter+body bytes | **CORRECT** |
| 3 | A skill named identically to a reserved sandbox mount is refused, not silently shadowing it | `tests/test_skill_mount_materializer.cpp` R2: a skill named `work` against `reserved_mount_ids={"work"}` fails with `skill.mount_id_reserved`, and the host directory is never created | **CORRECT** |
| 4 | A path-traversal attempt against a materialized skill mount is denied, not silently served | `test_mediated_python_runner_skill_mounts.cpp` R3, and a real bug in the FIRST version of this check: `ExecOutcome::klass == ok` does **not** mean guest code didn't raise — an uncaught Python exception is still a normal, well-formed run outcome (like a REPL cell), with the traceback in `stderr_text`. First version of R3 asserted on `klass`, always passed even before any fix, and was caught only by manually inspecting stderr with a debug print and seeing a real `OSError: '.'/'..' are not meaningful in a content-addressed tree path` come back — fixed to assert on `stderr_text` content | **CORRECT** (after a real false-positive test bug was found and fixed, not assumed) |
| 5 | This end-to-end chain works against a live model, not only against a fixed script | §6 below: a live OpenRouter session where the model, unprompted with any exact tool syntax, chose to call `execute_code` with `open('/using-the-code-interpreter/SKILL.md').read()` and the real file content came back | **CORRECT** |

## 6. Live evidence

Run against `agentengine_cli_chat.exe` (`~deepseek/deepseek-v4-flash-latest` via OpenRouter), two
turns: a plain greeting (no tool call — correct), then "read the file
`/using-the-code-interpreter/SKILL.md`... and print its contents". Startup banner confirmed, before any
turn ran: 5 skills mounted, materialized to 5 real host directories, `allowed-tools union across
mounted skills: execute_code`, `Tools actually declared+invocable this run: execute_code` — the SAME
list for both halves, by construction (§3a). The model called `execute_code` with
`open('/using-the-code-interpreter/SKILL.md').read()`; `stdout_text` carried the real file, including
the newly-added `allowed-tools: execute_code` frontmatter line; the model then summarized it correctly
in prose. This is the first time any skill's content reached an agent through a real tool call rather
than test-harness C++ holding the object store directly.

Offline: 175/176 tests pass. The one failure, `test_mediated_python_runner_hostile_corpus`, is
pre-existing, unrelated (real sandbox-mediation code untouched by this ADR), and was already reported
to the operator separately, out of this ADR's scope.

**Correction (2026-08-11):** this and every other ADR that repeated "pre-existing, unrelated flake"
citing this section were wrong about the NATURE of the failure. Re-run in isolation, repeatedly, it
was 100% reproducible, not intermittent — two real test-authoring bugs (`test_mediated_python_runner_
hostile_corpus.cpp`'s E4-PY8f used a Windows-path literal instead of the mediated-`open()` guest-path
convention, and E4-PY9's wrapper allowlist was missing `_ae_fs_denied`), not flakiness in the runner
itself. Both fixed the same day this note was added; the file compiles and passes cleanly now, and
the full suite is green with no known failures. "Pre-existing and unrelated" (real sandbox code, out
of scope) was directionally fine; "flake" was not — never re-verified before being repeated forward.

## 7. Residuals, named rather than silently assumed closed

- No type-level guarantee ties the declared and invocable tables together (§3b) — a future caller
  could reintroduce the cosmetic-scoping bug by constructing two independent tables. Currently
  prevented only by `cli_chat.cpp` following the documented convention, proven once, not enforced
  structurally.
- `allowed_tool_names()`/scoping only covers `native_jail`'s `MediatedPythonRunner`. `MediatedShellRunner`
  (bound to one mount id, not a map) and the `wasm` backend (Mount consumption explicitly out of scope
  in its own source) have no equivalent wiring. **Assessed in depth (2026-08-11), deliberately not
  forced this session, for concrete reasons rather than left as a bare "not done":**
  `worktree_mount_sync.hpp`'s `materialize_mount`/`materialize_subtree` (the primitive
  `skill_mount_materializer.hpp` already reuses for Python) is confirmed genuinely backend-agnostic —
  it only depends on the abstract `FileSystemAdapter&` interface, not `MediatedPythonRunner`. But:
  (a) the **wasm** backend's `fs-read`/`fs-write` host callbacks are literal
  `trap_error("not implemented in M2's minimal host")` stubs (`wasm_backend.cpp`), and
  `SandboxSpec::mounts` is never even read by `WasmBackend::create()` — there is no real I/O to
  materialize a skill mount INTO yet; that's a separate, larger, already-deliberately-scoped-out
  prerequisite (D3/ADR-010's own M2 task text), not a materialization-wiring gap. (b)
  **`MediatedShellRunner` has zero production callers anywhere in the tree today** (not even
  `tools/cli_chat.cpp` constructs one) — extending its single-`mount_id` binding to a real
  multi-mount design (mirroring Python's `split_guest_path`-derived per-call mount resolution) would
  be building a mechanism with no real consumer to prove it against, the same shape of risk this
  project's own design reviews have repeatedly flagged elsewhere this cycle. Revisit either half once
  its real prerequisite exists: wasm fs I/O for (a), a real `MediatedShellRunner` production call
  site for (b) — not before.
- `AgentSession` still owns no sandbox and no tool-call loop in production — every proof here runs
  through `cli_chat.cpp`'s hand-driven loop, not through `AgentSession::handle()` itself. Building that
  ownership is a separate, larger architectural question this ADR does not attempt.
  **Closed by `decisions/ADR-027-agent-session-tool-call-loop.md` (Judged, 2026-08-11):** a real
  multi-round tool-call loop now lives inside `AgentSession::handle()` itself, replacing the hand-
  driven `cli_chat.cpp` loop this residual describes.
- Per-skill `cap::FsRead` grants are all-or-nothing per mounted skill for the CLI's own demo (it grants
  every materialized skill); nothing in this design *requires* that — an operator embedding this
  differently can grant a real subset — but no test proves the subset case specifically.

## 8. Addendum (2026-08-10, same day) — on-demand, agent-triggered mounting

§3's design activated every RESOLVED skill's tools unconditionally, from turn 1. The user directed a
further narrowing: a skill's tools should only activate after the agent itself explicitly triggers it.

**MAF researched in depth as prior art before designing** (`AgentSkillsProvider`/`SkillsProvider`, both
C# and Python source read in full, not from memory). The definitive finding: **MAF is stateless and its
"load before use" rule is advisory only, never enforced in code.** No "loaded" flag is tracked anywhere;
`ProvideAIContextAsync`/`before_run` re-derives everything from the skill catalog every turn, and the
tool list is always exactly the same 3 fixed schemas regardless of load state. `load_skill`'s full-body
result is an ordinary, ephemeral tool-result chat message — whether a later turn still has it depends
entirely on the chat-history layer's retention, unrelated to the skills provider. Read directly:
`read_skill_resource`/`run_skill_script` never check that `load_skill` ran first for that skill; a model
can call either for any catalog skill without ever loading it. This is a real, structural gap relative
to this project's own I2 ("no ambient authority") — MAF's own design does not claim otherwise; its real
enforcement is a per-call approval gate, unrelated to load ordering.

**Decision:** build real, persistent, per-run state instead of replicating MAF's chat-history-dependent
approach. `include/agentengine/core/mounted_skills_state.hpp::MountedSkillsState` — a plain mutable set,
consulted fresh every turn, not embedded in any message. A new tool, `mount_skill(name)` (`tools/
cli_chat.cpp::MountSkillTool`), is always declared/invocable (`always_on` in `scope_tools_to_mounted_
skills`, added in §3 but unused until now) and records intent into that state; `SkillsProvider::
allowed_tool_names_for(mounted_names)` (new, restricted union) replaces `allowed_tool_names()`'s
unrestricted union as the input to scoping. `ToolDeclaringHistoryProvider::on_context` additionally
injects each mounted skill's full body as its own system message every turn it's mounted — real,
reliable re-injection this project's state makes possible, unlike MAF's fragile equivalent.

**The declared/invocable consistency requirement (§3a) got sharper, not weaker.** Because mount state
can now change *mid-conversation*, `main()`'s invocation-time table can no longer be computed once
before the interactive loop (as it was under §3's static model) — it must be recomputed every round,
from the same live `MountedSkillsState` `on_context` just read. `core/skill_tool_scoping.hpp`'s own top
comment was revised accordingly: the invariant is "declared and invocable stay derived from the same
live state, on the same cadence" — not "computed exactly once, ever."

**A real architectural gap, worked around rather than solved:** `Tool<>::invoke()` is a static call with
only `EffectContext&` — no way to reach `AgentSession`-owned `StateT`. `MountedSkillsState` is therefore
shared via the same function-local-static, single-process-scoped idiom `cli_chat.cpp` already used for
`shared_python_runner()`/`shared_exec_state()` — correct for this CLI's single-process scope, not a
general solution for a multi-session production `AgentSession`. Named, not solved, matching §7's own
"`AgentSession` owns no sandbox/tool-loop in production" residual — this is the same gap, one layer up.
**Closed by `decisions/ADR-028-session-scoped-stateful-tools.md`** (`make_tool_descriptor_with_invoke
<ToolT>`, Judged): a `Tool<>` conformer's `invoke()` can now be backed by a provider-owned member
function with real, per-`AgentSession`-instance state. `decisions/ADR-030-session-scoped-codeact-
wiring.md` (Proposed) then ports `MountedSkillsState`/`MediatedPythonRunner`/`ExecState` off the
process-wide statics named here onto that mechanism for real.

**I2/I3 argument, stated explicitly:** `mount_skill` cannot and does not grant new authority. Every
resolved skill's files are already readable via a `cap::FsRead` the operator granted unconditionally at
session start (009 §8b, unaffected by mount state); mounting only activates tool *declarations* the
operator already pre-authorized by configuring that skill's source in the first place. A model calling
`mount_skill` selects a point within an already-fixed ceiling — it never widens the ceiling itself.

**Evidence:** `tests/test_on_demand_skill_mount.cpp` — core-tier, no live model — proves the negative
(`invoke_tool` rejects a skill-named tool, `tool.unknown_name`, before that skill is mounted) then the
positive (the identical call succeeds after `MountedSkillsState::mount(...)`, through the same real
pipeline). Run live against a real OpenRouter model: on the very first turn, the model called
`mount_skill("using-the-code-interpreter")` before attempting `execute_code` — not because it was told
to, but because `execute_code` genuinely was not in its declared tool list until the next internal
round, once the mount was recorded. It never had to be rejected; the declaration-side scoping alone
correctly shaped what the model attempted. 176/177 tests pass — the one failure is the same
`test_mediated_python_runner_hostile_corpus` failure §6 above records and corrects
(2026-08-11 note): a real, deterministic test bug, not a flake — fixed the same day.

**Residuals**, additional to §7's own list: `MountedSkillsState` has no expiry/unmount within a run
(matches 009 §8c's "snapshotted per run" framing, applied one layer up); no test exercises an operator
granting only a *subset* of resolved skills' file capabilities while allowing mounting of skills whose
files aren't actually granted (an edge case §8b's own "mounting grants no new file authority" claim
implies should fail at the file-read layer, not the mount layer — untested).
