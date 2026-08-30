# ADR-119 — Widening the real `Capability` variant for `RunCommandTool` too

- **Status:** Proposed — implemented, verified against a REAL Docker daemon (Windows/MSVC), full
  project rebuild (zero errors) and full `ctest` clean (251/252, same pre-existing unrelated
  matplotlib/pandas gap), `naming_lint.py` clean. Not yet independently red-teamed; not yet
  Linux-verified.
- **Date:** 2026-08-30.
- **Scope:** `include/agentengine/trust/capability.hpp` (one new `cap::`/`cap::decl::` alternative,
  one new `capability_kind` enumerator, four exhaustive-switch/`if constexpr` sites extended),
  `include/agentengine/trust/policy_reachability.hpp` (`capability_kind_name()` extended),
  `include/agentengine/sandbox/mandatory_sandbox_provider.hpp` (`RunCommandTool` gains a real
  `Capabilities<...>` ceiling; both capability-gating comment blocks rewritten),
  **three real production CLI tools** (`tools/cli_chat.cpp`, `tools/sandboxed_shell_chat.cpp`,
  `tools/containerd_shell_chat.cpp` — each now grants `cap::RunCommand` on the session it builds),
  three test files driving `run_command` through the real pipeline (`tests/test_mandatory_sandbox_
  provider.cpp`, `tests/test_composed_sandbox_providers_live.cpp`, `tests/test_composed_containerd_
  providers_live.cpp`), and `CMakeLists.txt` (one `/bigobj` fix — see §4). `decisions/ADR-102-
  identity-native-sandbox-implementation-phase-1.md` (two disclosure updates pointing here).
- **Related specs:** `decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md` §26 (the
  locked design decision this ADR revisits, with a new ADR per CLAUDE.md's "do not relitigate without
  an ADR" rule), `decisions/ADR-117-task-branch-capability-variant-widening.md` (the identical
  double-gate shape, precedent for this ADR's own design and the source of most of its exhaustive-
  switch/`cap::decl::` mechanics), `decisions/ADR-118-linux-verification-adr117.md` (the Linux-
  verification methodology this ADR's own follow-on will reuse).

## 1. The question

ADR-102 Phase 4 deliberately gave `RunCommandTool` ZERO static `Capabilities<...>` ceiling, and ADR-114
§2 / ADR-117 §5 both explicitly kept it out of scope as "a broader reconsideration of `RunCommandTool`'s
own, separately-decided zero-ceiling design." Should `RunCommandTool` get the same double-gate
treatment ADR-117 just gave the task-branch tools — a real `Capabilities<cap::decl::RunCommand>`
ceiling layered on top of its own, unchanged `IdentityAuthority`/`Grant<T>`/`AsyncQuota<T>` model — and
can it be done without silently breaking the real, shipped production tools that already depend on the
old, single-gate contract?

## 2. Findings

**This is materially higher-stakes than ADR-117.** ADR-117's own widening was judged safe to land
immediately because ADR-114 disclosed ZERO real production callers of the task-branch tools outside one
test file. `RunCommandTool` is the opposite case: it is ADR-102's own flagship result, and THREE real,
shipped production CLI tools call `bind_sandbox()` and drive `run_command` through the real
`session.start_run() -> invoke_tool()` pipeline today — `tools/cli_chat.cpp` (three provider branches:
openai/openrouter/anthropic), `tools/sandboxed_shell_chat.cpp`, and `tools/containerd_shell_chat.cpp`.
Giving `RunCommandTool` a real ceiling without ALSO granting it in all three tools would not merely be a
disclosed residual — it would silently break `run_command` for every real user of all three tools,
which is exactly the "widening a closed variant is real, security-critical surgery" risk this design
line has always named. This ADR's own scope therefore explicitly includes fixing all three real
callers, not just the library-side change, in the SAME commit — unlike ADR-117, which had nothing real
to fix.

**Checked, not assumed, which of the three tools already grant anything.** `tools/cli_chat.cpp` already
builds and sets a real `CapabilitySet` (three near-identical blocks, one per provider branch, each
granting `cap::Secret`/`cap::FsRead`/`cap::FsWrite`) — needed a new grant added to each of the three
blocks. `tools/sandboxed_shell_chat.cpp` and `tools/containerd_shell_chat.cpp` both use `quickstart::
ComposedQuickstartSessionBuilder`'s `.grant(...)` chain, granting only `FsRead`/`FsWrite` for `SandboxToolProvider`'s
own `run_shell` — their own comments explicitly documented (now-stale) that `run_command` "deliberately
has NO static ceiling here." Both needed one new `.grant(Capability{cap::RunCommand{}})` line.

**Checked, not assumed, which real-pipeline test call sites needed updating.** Grepped every test that
references the string `"run_command"` and classified each by whether it drives the tool through the
real `invoke_tool()` pipeline (needs the new grant) or calls the provider/descriptor directly (bypasses
`Tool<>`'s declared ceiling entirely, unaffected — the identical "direct-call bypass is test-only, never
reachable by a real caller" property ADR-117's own red-team confirmed for the task-branch tools):
- `tests/test_mandatory_sandbox_provider.cpp` section [3] — real pipeline, updated.
- `tests/test_composed_sandbox_providers_live.cpp` and `tests/test_composed_containerd_providers_live.cpp`
  — both drive `run_command` through the real pipeline in one composed session alongside `run_shell` —
  both updated.
- `tests/test_mandatory_sandbox_provider.cpp` sections [2]/[4] — call `contribution->tools[0].invoke(...)`
  directly on the `ToolDescriptor`'s own closure, never through `agentengine::invoke_tool()` — confirmed
  unaffected (the capability ceiling is data `invoke_tool()`'s step 4/7 consults; the raw `invoke`
  closure itself never reads it).
- `tests/test_mandatory_sandbox_provider_composed.cpp` — only checks tool *declaration* (`on_context()`
  contribution), never invokes the real closure at all (that file's own top comment says so explicitly)
  — confirmed unaffected.
- `tests/test_task_branch_tools.cpp` — only checks that `run_command` is the ONE contributed tool name
  before `bind_task_branch_tools()` is called; never invokes it — confirmed unaffected.

**A real, new regression found and fixed while verifying, not merely anticipated.** Building the
real production tools after the change (not merely the library) surfaced a genuine, ADR-119-caused
MSVC `C1128` ("number of sections exceeded object file format limit") on `agentengine_sandboxed_
shell_chat` — confirmed, not assumed, by `git stash`-scoping the change: the SAME target built clean on
the unmodified tree. (`agentengine_cli_chat`'s own identical-looking `C1128` failure was separately
confirmed, the same way, to be pre-existing and unrelated to this change — ADR-114's own commit message
already named it, and it already carries the project's own `/bigobj` fix, which is no longer sufficient
for that file's own, larger reasons unrelated to this ADR.) Fixed by giving `agentengine_sandboxed_
shell_chat` the identical, already-established `/bigobj` remedy `agentengine_cli_chat` already uses,
scoped to that one target (`CMakeLists.txt`). `agentengine_containerd_shell_chat` is Linux-only
(`NOT WIN32`-gated) and GCC has no equivalent COFF section-count limit, so no analogous fix is needed
there.

**Capability naming: a dedicated tag, not a reuse of `cap::Exec`.** `cap::Exec` already exists
(`capability_kind::exec`) but names a materially different mechanism — sandbox PROFILE selection for
nested/managed execution (008 §4) — not this Ledger/Docker/containerd-backed execution surface. Minted
`cap::RunCommand`/`cap::decl::RunCommand` instead, a fieldless marker matching `cap::Entropy`/
`cap::Elicit`/`cap::TaskBranch`'s own shape, mirroring `MergeCost`'s and `TaskBranch`'s own "a dedicated
tag, not reused" discipline (ADR-114 §2, ADR-117 §2).

## 3. What was built

`include/agentengine/trust/capability.hpp`: `cap::RunCommand{}` and `cap::decl::RunCommand{}`
(fieldless), added as the 22nd `Capability`-variant alternative and a new `capability_kind::
run_command` enumerator; `capability_kind_of()`, `capability_from_kind()`, and
`is_inert_for_text_derived_declassification()` (returns `false` — running an arbitrary shell command is
never provably inert) each get a new arm; `to_capability()` and `subsumes_payload()` (unconditionally
`true`, matching every other fieldless marker) each get a new overload.

`include/agentengine/trust/policy_reachability.hpp`: `capability_kind_name()` gets a new arm
(`"run_command"`).

`include/agentengine/sandbox/mandatory_sandbox_provider.hpp`: `RunCommandTool` now declares
`Tool<RunCommandTool, Capabilities<cap::decl::RunCommand>>`. Both the file's own top-comment SCOPE
paragraph and the comment immediately above `RunCommandTool` are rewritten to state the new double-gate
contract explicitly (the identity/quota model is unchanged and still does the real bounding; the new
ceiling is a separate, static membership/audit gate on top) and to name every real caller this ADR
updated in the same change.

`tools/cli_chat.cpp`: each of the three provider branches' `grants` vector gains
`Capability{cap::RunCommand{}}`. `tools/sandboxed_shell_chat.cpp` and `tools/containerd_shell_chat.cpp`:
each gains `.grant(Capability{cap::RunCommand{}})` in its builder chain; both files' own stale
"`run_command` ... deliberately has NO static ceiling here" comments are corrected to describe the new
double-gate contract.

`tests/test_mandatory_sandbox_provider.cpp`: section [3]'s `held` now grants `cap::RunCommand`. A new
section [7] proves the fail-closed direction end-to-end, mirroring ADR-117 §7's identical proof shape
for the task-branch tools: a session with `bind_sandbox()` called (the identity/quota gate, satisfied)
but an explicitly empty `CapabilitySet` gets `run_command` rejected as a real `role::tool` error result
— `start_run()` itself still completes normally, no real reply ever appears in history, zero `RunCost`
is spent, and the target file is never written to the real Ledger branch — all four checked
independently, not merely inferred from one assertion. `tests/test_composed_sandbox_providers_live.cpp`
and `tests/test_composed_containerd_providers_live.cpp`: both `held` sets now also grant
`cap::RunCommand`.

`CMakeLists.txt`: `agentengine_sandboxed_shell_chat` gets the `/bigobj` MSVC compile option (guarded by
`if(MSVC)`, matching `agentengine_cli_chat`'s own established precedent for the identical limit).

## 4. Verification

Built and ran `test_mandatory_sandbox_provider` against a REAL Docker daemon: **ALL CHECKS PASSED**,
including the updated section [3] and the new section [7]. Sanity-checked the same way this design
line always does: temporarily reverted `RunCommandTool`'s ceiling back to `Tool<RunCommandTool>` (no
`Capabilities<...>`, the ADR-102 Phase 4 shape) and confirmed all four of section [7]'s new assertions
genuinely FAIL against that pre-fix code — then restored and re-confirmed a full pass.

Built `agentengine_cli_chat`: reproduces the SAME pre-existing `C1128` the unmodified tree also
produces (confirmed via `git stash`) — not a regression, not fixed here (out of this ADR's own scope;
named as a pre-existing, disclosed, unrelated defect). Built `agentengine_sandboxed_shell_chat` (via the
`AGENTENGINE_WITH_HTTPS=ON` build directory): failed with a NEW `C1128` before the `/bigobj` fix
(confirmed via `git stash` to be genuinely introduced by this ADR's own change, not pre-existing);
builds clean after the fix. `agentengine_containerd_shell_chat` is Linux-only and not buildable in this
environment — will be confirmed during this ADR's own Linux-verification follow-on.

Full project rebuild (main Debug build directory): zero errors. Full `ctest`: **251/252**, the one
failure being the same, unrelated, pre-existing matplotlib/pandas gap ADR-111/112/114/116/117 already
named — confirmed still the only failure, nothing newly broken, including `test_composed_sandbox_
providers_live` (Passed, exercising the updated grant end-to-end against real Docker).
`python tools/naming_lint.py`: clean — the new `cap::`/`cap::decl::` type follows the identical
fieldless-marker shape the lint already accepts for `Entropy`/`Elicit`/`TaskBranch` with no new
special-case entry needed.

## 5. What was NOT done

- **No independent red-team pass yet.** Given the materially larger blast radius than ADR-117 (three
  real production tools, not zero), a red-team round is the expected next step before this is
  considered closed — arguably more load-bearing here than for ADR-117, not optional polish.
- **No Linux verification.** `tools/containerd_shell_chat.cpp` and `tests/test_composed_containerd_
  providers_live.cpp` were both edited but never built or run in this pass (Linux-only, `NOT WIN32`) —
  named as a real, disclosed gap, not silently assumed correct by analogy to the Docker-shaped
  sibling files.
- **`RunShellTool` (ADR-096, `src/backends/native_jail/`) was not touched.** It already has a real
  `Capabilities<cap::decl::FsRead<"work">, cap::decl::FsWrite<"work">>` ceiling — this ADR only closes
  the `RunCommandTool` gap, which is `RunShellTool`'s own sibling on `ComposedContextProvider`, not a
  broader audit of every tool's capability posture.
- **`agent_library_manifest.hpp`'s discovery registry was not checked this time** (ADR-117 §5 checked
  its equivalent and found nothing needed) — a real, disclosed gap in this ADR's own diligence, named
  honestly rather than silently assumed clean by analogy.

## 6. Residuals

- Everything named in §5 not otherwise closed.
- `agentengine_cli_chat`'s own pre-existing `C1128` (`/bigobj` already applied, still insufficient) is
  unrelated to this ADR and remains unfixed — a real, disclosed, separate build-limit issue for whoever
  picks it up next.
- The two new fieldless-marker capability kinds (`task_branch`/`task_branch_commit` from ADR-117, now
  `run_command` from this ADR) bring the real `Capability` variant to 22 alternatives — any future
  exhaustive switch written without grepping for the now-five sites (four in `capability.hpp`, one in
  `policy_reachability.hpp`) risks silently missing an arm, the same risk ADR-117 §6 already named.
