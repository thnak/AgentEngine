# ADR-153 — Reaching `agent.memory` from a CodeAct sandbox: `recall` bridging + a read-only `/memory` mount

**Status:** Judged (2026-08-31, project-owner sign-off). Red-teamed, implemented (with two named
must-fix corrections to the original sketch, not the draft verbatim), and proven — real code, 12 new
checks (R1-R3) all pass against a real `MemoryProvider`/`bridge_tool_call()`/`materialize_memory_mount()`,
and a full rebuild of every test target that touches `MemoryProvider` or `ToolBridgeConfig` confirms no
regression — see §5/§6 below.

**Relates to:** `026-Agent-Facing-Runtime-Surface.md` §5 (`agent.memory`'s spec: "ordinary read access
to the memory worktree's files under `/memory`... the ranked/on-demand view is `agent.tools.recall`, an
ordinary tool"), GitHub issue #31's own comment (the audit that found this gap), `include/agentengine/
core/memory_provider.hpp` (`MemoryProvider::make_recall_tool_descriptor()`), `src/backends/native_jail/
tool_bridge.hpp` (`ToolBridgeConfig`, `bridge_tool_call()`), `src/backends/native_jail/
skill_mount_materializer.hpp` (the read-materialization precedent this ADR's own mount helper reuses
the shape of), `include/agentengine/core/tool_pipeline.hpp` (`invoke_tool()`'s step 4/7 authorize+bind,
the mechanism this ADR's must-fix finding closes a gap in), `decisions/ADR-156-agent-notes-write-back.md`
(not yet written — the write-back half over the SAME `/memory` mount this ADR's materializer produces).

## 1. The question

Per issue #31's audit comment: `MemoryProvider`'s `recall` tool (the ranked/on-demand memory view 026
§5 names as `agent.tools.recall`) is real and tested, but reachable only through `ContextContribution.
tools` — the model's native, direct tool-calling loop — never through `ToolBridgeConfig.bridged_tools`,
the only thing `generate_agent_tools_module_source()` ever renders into a running CodeAct sandbox's
`agent.tools`. The raw-file-read half of `agent.memory` ("ordinary read access... under `/memory`") had
no mount wiring at all.

**Stated so it has a wrong answer:** can a host make `recall` reachable as `agent.tools.recall(...)`
from inside `execute_code`, and materialize `/memory` as a real host directory a script can read, using
only ALREADY-real, ALREADY-tested primitives (`bridge_tool_call()`, `materialize_mount()`), without
opening any new ambient-authority path (I2) — the sandbox's own capability set must stay genuinely
separate from the native session's, never silently inherit it?

**Found during red-team (§4): the naive answer is NO as originally sketched.** Two real, must-fix
defects — not hypothetical — see §4.

## 2. The design

Two independent, additive-only, host-opt-in pieces. Neither one is reachable unless a host explicitly
wires it — nothing in this codebase automatically funnels `ContextContribution.tools` into a sandbox's
`ToolBridgeConfig`, and this ADR does not add such a funnel (a deliberate scope boundary: an automatic
funnel would inherit whatever the native session holds, exactly the I2 hazard `tool_bridge.hpp`'s own
file-top comment already warns against).

**Piece 1 — `MemoryProvider::make_recall_tool_descriptor()` made public.** It was private, reachable
only from `on_context()`'s own push into `ContextContribution.tools`. A host embedding both this
provider and a CodeAct sandbox can now call it directly and fold the returned `ToolDescriptor` into
their own `ToolBridgeConfig.bridged_tools` (`ToolTable::from_descriptors({...})`), matching `tool_
bridge.hpp`'s own "host explicitly assembles the sandbox's own capability set" discipline exactly.

**Piece 2 — `native_jail::materialize_memory_mount()`** (`src/backends/native_jail/
memory_mount_materializer.hpp`), reusing `materialize_mount()` (the SAME already-tested primitive
`materialize_skill_mounts()` already established), narrowed to exactly one caller-supplied `Mount`
(`memory_mount(principal)`) rather than an enumerable set. One-shot, matching skills' own "snapshotted
per run" precedent — not live-synced (named residual, §7). `agent.notes`' write-back half (ADR-156,
not yet written) reuses the SAME mount, via `harvest_mount()`, after the run completes.

## 3. Deliberately out of scope

- Any automatic `ContextContribution.tools` → `ToolBridgeConfig` wiring — see §2's own scope boundary.
- Live (mid-script) sync of the `/memory` mount — one-shot only, matching skills.
- `agent.notes`' write-back (a file written under `/memory` landing as a real `AgentAuthored`
  `MemoryItem`) — ADR-156, a separate follow-on reusing this ADR's mount.
- `agent.output`/`agent.progress` — ADR-154/ADR-155, unrelated modules.

## 4. The red-team attack

**Run this pass** against the real, current-tree source, by actually building and running the design
as originally sketched (not merely reading it) — both findings below were CAUGHT BY A FAILING TEST,
not found by inspection first.

### Finding 1 (MUST-FIX) — bridging `make_recall_tool_descriptor()`'s original closure bypasses the sandbox's own capability check entirely

The original closure captures `read_cap_` **by value from `MemoryProvider`'s own construction** and
never touches `ToolDescriptor::capability_ceiling` (left at its default-constructed empty vector).
`invoke_tool()`'s step 4/7 (`tool_pipeline.hpp:606-620`) only ever binds against `tool->
capability_ceiling` — an empty ceiling means that loop never runs, so `held.bind(...)` (where `held` is
`bridge_tool_call()`'s own `sandbox_capabilities`, built from `ToolBridgeConfig::capabilities`) is never
even consulted. The tool's real authority is entirely the closure's own captured `read_cap_`, fixed at
provider-construction time — a value that belongs to the NATIVE session, not the sandbox.

**Concrete consequence:** a host that bridges this descriptor into a `ToolBridgeConfig` with an EMPTY
`capabilities` list (e.g. because they meant to bridge some OTHER tool and forgot this one needs its
own grant, or because they assumed — reasonably, given every other tool in this codebase — that an
empty `capabilities` list denies everything) gets a sandbox that can STILL read memory, reaching straight
through to the native session's own authority. This is exactly the failure mode `tool_bridge.hpp`'s own
header comment names as the reason `bridge_tool_call` never accepts an agent-level `CapabilitySet` as a
parameter at all — reopened here through a different door (a runtime-constructed `ToolDescriptor` with
an unset `capability_ceiling`) that comment did not anticipate.

**Caught by:** the negative-control check in `test_memory_codeact_bridging.cpp` ("recall is DENIED
through a bridge config with no FsRead grant") FAILED against the original code — the call succeeded
when it should have been denied.

**Fix (implemented, §2):** `make_recall_tool_descriptor()` now sets
`d.capability_ceiling = {Capability{read_cap_}}`. This makes the SAME descriptor correctly gated for
BOTH callers: the native `on_context()` path (where the session's own held capabilities were already,
by construction, the same ones this provider was built with, so this is a no-op behavior change there —
confirmed by `test_memory_provider.cpp` still passing unchanged) and the bridge path (where `held` is
now genuinely `ToolBridgeConfig::capabilities`, and the call is denied unless that list actually
contains a matching `FsRead`).

### Finding 2 (must-fix, materialization) — `memory_mount_id()`'s own format is not a valid filesystem path segment or a sane guest-visible name

`memory_mount_id(principal)` returns `"memory:" + tenant_id + ":" + id` — a string deliberately
designed (Milestone 5 Phase I1, `memory.hpp`'s own comment) to be a safe, collision-resistant *opaque
capability-matching key*, compared only as a string inside `CapabilitySet`. It was never designed to be
a filesystem path segment. `materialize_skill_mounts()`'s own precedent uses `mount.mount_id` directly
as a host subdirectory name — safe for skills only because a skill's `mount_id` happens to be its own
bare name (no reserved characters). Reusing that same pattern verbatim for memory, using
`memory_mount_id(principal)` as the host subdirectory name, **fails on Windows with a real
`ERROR_INVALID_NAME`-class error** (a colon mid-path is illegal) — and even where it wouldn't fail
outright, it would expose a principal's own tenant/id string as a path a model has to name literally,
against 026 §5's own "guessable from its name" bar and needlessly leaking internal id structure.

**Caught by:** `materialize_memory_mount()`'s own positive-control check failed with
`memory.mount_directory_create_failed: ... The filename, directory name, or volume label syntax is
incorrect.` — a real, observed OS error, not a theoretical concern.

**Fix (implemented, §2):** `materialize_memory_mount()` takes a separate, HOST-CHOSEN `mount_name`
parameter (a plain presentation token, e.g. `"memory"`) for the host directory and the returned pair's
key — matching `memory_mount()`'s own header comment ("the guest-visible mount PATH... is a separate,
host-chosen presentation detail"). The REAL `mount_id` (`memory_mount_id(principal)`) is still what
`materialize_mount()` itself checks `granted` against internally — the fix only separates "what this
gets called on disk / in `mount_roots`" from "what capability-matching key gates it," it does not
change or weaken the authority derivation.

### Finding 3 (confirmed-clean) — no cross-principal leakage introduced

`materialize_memory_mount()` calls `memory_mount(principal)` directly — the SAME already-hardened
derivation (Milestone 5 Phase I1, `memory.hpp`) every other memory call site uses; this ADR adds no new
principal-to-mount derivation logic of its own. `test_memory_worktree.cpp`'s own G2-R7/R8/R9 (two
principals' mounts are distinct, a wrong-principal capability is rejected outright) exercise that
derivation unchanged, still passing (§5).

### Overall verdict

**The design does NOT survive as originally sketched — it needed two named, structural corrections
(Findings 1/2), not a different approach.** Both were caught by a real failing test before being fixed,
not found by inspection alone. With both corrections applied, the design is sound: Finding 3 confirms no
new principal-leakage surface; the full related-test rebuild (§5) confirms no regression elsewhere.

## 5. Executed evidence

**Windows, MSVC (Visual Studio 18 Community), MSBuild, Debug, existing `build/` tree** (CMake
reconfigured once to register the new `test_memory_codeact_bridging` target).

New target, direct run (`build/tests/Debug/test_memory_codeact_bridging.exe`):

```
  ok: setup: the memory worktree bootstraps
  ok: setup: writing the memory item to recall/materialize succeeds
  ok: R1: the descriptor is real and named 'recall'
  ok: R1: recall is DENIED through a bridge config with no FsRead grant
  ok: R1: recall SUCCEEDS through a bridge config granting the matching FsRead
  ok: R1: the recalled content re-enters tainted (003 §2)
  ok: R1: the reply actually carries the real memory item's content, not a stub -- a genuine round trip through the bridge
  ok: R2: materialize_memory_mount succeeds for a bootstrapped principal with a real FsRead grant
  ok: R2: the returned mount name is the host-chosen presentation token, not the colon-bearing internal mount_id
  ok: R2: the real memory item's content is a real file under the materialized host directory -- a genuine worktree-to-host round trip
  ok: R3: a reserved mount name colliding with the chosen 'memory' token is refused
  ok: R3: the failure carries the specific, named reserved-collision error code
  ok: R3: NOTHING was written to disk for this call -- fails closed before any directory creation, not partway through
test_memory_codeact_bridging: ALL PASS
```

**Both must-fix findings verified non-vacuous**: Finding 1's negative-control check and Finding 2's
positive-control check were both OBSERVED TO FAIL against the pre-fix code (quoted in §4 above), then
observed to pass after each fix — a demonstrated closure, not merely an argued one.

**No regression** — every test target that touches `MemoryProvider` or `ToolBridgeConfig`, rebuilt and
rerun from the same tree:

```
test_memory_provider: OK
test_tool_bridge: all checks passed
test_memory_ranking_formula: all checks passed
test_memory_worktree: OK
test_skill_mount_materializer: ALL PASS
```

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| `recall` is reachable through `bridge_tool_call()` using the same mechanism every other bridged tool uses | **CORRECT (after fix)** | R1's checks; Finding 1's fix |
| The bridge path is capability-gated identically to the native path — denied without the grant, allowed with it | **CORRECT (after fix)** | R1 negative + positive control both pass |
| `/memory` materializes to a real host directory a sandbox could mount | **CORRECT (after fix)** | R2's checks; Finding 2's fix |
| The reserved-mount-id collision guard fails closed, nothing written | **CORRECT** | R3's checks |
| No new cross-principal leakage surface | **CORRECT (confirmed-clean)** | Finding 3; `test_memory_worktree.cpp` unaffected |
| No regression to `MemoryProvider`'s native tool-calling path or the tool bridge generally | **CORRECT** | Full related-test rebuild, all green |

## 7. Residuals to name up front

- One-shot materialization only — a script's own write into the mounted directory is visible to
  itself immediately (ordinary filesystem semantics within one process) but does not become a durable
  `MemoryItem` without ADR-156's harvest step, and a SECOND `execute_code` call in the same run will not
  see a FIRST call's write unless the host re-materializes between calls.
- `materialize_memory_mount()` requires the principal's memory worktree to already be bootstrapped
  (`ensure_memory_worktree()`) — same precondition `materialize_mount()` itself already has ("this
  mount's ref has never been committed" is a hard error, not an implicit bootstrap); this ADR does not
  add auto-bootstrap.
- `agent.notes`' write-back half (a script's write landing as a real `AgentAuthored` `MemoryItem`) is
  untouched — ADR-156, a separate follow-on reusing this ADR's own `mount_name`/`Mount` pair via
  `harvest_mount()`.
- No end-to-end proof through a REAL running `NativeJailBackend` sandbox (an actual `execute_code` call
  whose Python script calls `agent.tools.recall(...)` and reads a real mounted file) — this ADR proves
  `bridge_tool_call()`/`materialize_memory_mount()` directly, the same layer `test_tool_bridge.cpp`/
  `test_skill_mount_materializer.cpp` already prove their own mechanisms at, not the full worker-process
  integration.
