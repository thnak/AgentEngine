# ADR-217 — A capped grant admits a tool that enforces the grant's caps (`EnforcesGrantedCaps`)

**Status:** **Proposed (2026-10-01).** Design (§3), one adversarial self red-team pass (§4, 13 findings,
the first two changed the design), implemented and proven (§5). Awaiting the project owner's Judge.

**Relates to:** GitHub issue #148. `006-Tool-and-Function-Plane.md` §3 (steps 4/7, authorize + bind).
`007-Capability-and-Trust-Model.md` §3 (attenuation, never widen; I2). ADR-009 (`CapabilitySet::bind`,
`BoundCapability`). Gap-audit #12 (`find_fs_read`/`find_fs_write`, the mediated shell's own fix of the
same bug class). ADR-208 §7 (the `with_granted_ceiling` driver workaround this removes). ADR-212 §3.5
(the wrapper rule this copies).

## 1. The problem

A host that grants a session a **capped** file grant, `FsRead{"work", "", 1 MiB}` +
`FsWrite{"work", "", 16 MiB, 1024 files}`, cannot use `run_shell`: every call is refused
`tool.capability_not_held` before the tool runs.

- Every `cap::decl::FsRead<M>`/`FsWrite<M>` tag produces an **uncapped** runtime capability
  (`size_cap_bytes = nullopt`, …): a compile-time declaration carries no numbers.
- Admission (`tool_pipeline.cpp`, step 4/7) binds each ceiling entry with `CapabilitySet::bind()`, which
  is `contains()`, which is `subsumes()` → `cap_covers()`. `cap_covers` reads "parent capped, request
  uncapped" as **widening** and refuses.

So the safer configuration (a capped grant) is the one that does not work, and a narrower grant refuses
a tool that a wider grant admits. The mediated shell already fixed this for its own per-operation
checks (gap-12: `find_fs_read`/`find_fs_write` return the grant's own cap and the shell enforces it);
admission was never fixed. ADR-208 worked around it in the test driver by rewriting run_shell's
declared ceiling to the capped grant before registering it.

## 2. What exists (verified on this branch, base `6c584b7`)

- `CapabilitySet::bind(r)` = `contains(r)` then `BoundCapability(r, ticket)` — the handle carries the
  **requirement**, not the grant. Callers: `admit_call` and `background_task` (`src/core/tool_pipeline.cpp`),
  `BackgroundJobRunner::submit` (`rt/background_job_runner.hpp`), `WasmBackend::invoke_tool`.
- `CapabilitySet::attenuate(narrower)` = `contains()` on every entry, then **`grant_root(narrower)`**: the
  requested set becomes a new, reusable grant. Callers: `invoke_agent_tool` (`agent_registry.hpp`),
  `agent_spawn.hpp`, `agent_spawn_capability.hpp`.
- Who enforces quantity caps at use:
  - the mediated shell: `require_fs_read`/`require_fs_write` → `ctx.capabilities->find_fs_*` (the held
    grant) → `cat` size cap, mkdir/cp/redirect quota and file-count cap;
  - `read_sandbox_file`: `find_fs_read` (declares an **empty** ceiling, so admission never refused it);
  - WASM host callbacks: `recover_capability<T>` → the **bound handle's** `cap::NetOut::byte_cap`;
  - the memory `recall` tool (`memory_provider.hpp`): its **own baked-in `read_cap_`**, which is also its
    declared ceiling — it does not consult the held grant.
- Static mirrors of admission: `agent_detail::check_capability_ceiling` (registration),
  `trust::enumerate_policy_reachability` (007 §9 G6 CI tool), `ToolRegistry::register_tool` (non-native
  provenance vs. its outer grant). All three use `contains()`.

## 3. Design

### 3.1 Quantity axes vs scope axes

Every kind's parameters split into:

- **quantity axes** — the `std::optional` caps where `nullopt` means "no limit": `FsRead.size_cap_bytes`,
  `FsWrite.quota_bytes`/`file_count_cap`, `NetOut.byte_cap`, `Exec`/`NativeExec` `cpu_ms_cap`/
  `wall_ms_cap`/`memory_bytes_cap` (exactly the `cap_covers` fields);
- **scope axes** — everything else: mount, path prefix, host allowlist, method list, program pattern,
  profile, key, name… These say *what* the capability reaches, not *how much*.

`capability_detail::clamp_quantities(parent, requested)` lowers each quantity cap of `requested` to
`cap_meet(parent, requested)` (the tighter one; `nullopt` = ∞) and leaves every scope axis as requested.
Kinds with no optional quantity axis use an identity overload.

### 3.2 `clamped_binding` / `bind_clamped`

`CapabilitySet::clamped_binding(r)`:

1. if `contains(r)` → `r` itself (every call that bound before binds to the identical capability);
2. else the first granted `c` of the same kind with `subsumes_payload(c, clamp_quantities(c, r))` →
   that clamped capability;
3. else `nullopt`.

The result is subsumed by a held grant **and** by `r`: never wider than either on any axis. Scope axes are
decided by the unchanged `subsumes_payload` rules; the clamp can only make the quantity axes pass.
`bind_clamped(r)` mints the per-invocation handle around that result, so **the handle carries the
grant's cap**, not the tool's uncapped declaration.

`contains()`, `bind()` and `attenuate()` are unchanged (§4 F2).

### 3.3 Opt-in per tool: `EnforcesGrantedCaps`

A new `Tool<>` policy tag `EnforcesGrantedCaps`, copied to `ToolDescriptor::enforces_granted_caps` by
`make_tool_descriptor<T>()` and `make_tool_descriptor_with_invoke<T>()`. It is the tool author's claim:
*"my invoke enforces the quantity caps of the capability it is bound to, or of the held grant
(`find_fs_read`/`find_fs_write`/`find_net_out`), never a number from my own declaration."* Hand-built
descriptors default to `false`. Code that replaces `invoke` must clear it unless the new invoke performs
every effect through the inner one (a recorder) or performs none (a replay double) — ADR-212 §3.5's rule.

One admission rule, `bind_ceiling_entry(held, tool, r)` (`tool_descriptor.hpp`): `bind_clamped` when the
tool declared the tag, else `bind`. Its boolean twin `ceiling_entry_admitted` is used by every static
mirror that should agree with the runtime. Used by:

| Site | Before | After |
|---|---|---|
| `admit_call` (sync pipeline) | `held.bind(r)` | `bind_ceiling_entry` |
| `background_task` (background pipeline) | `held.bind(r)` | `bind_ceiling_entry` |
| `BackgroundJobRunner::submit` | `bind(r)` | `bind_ceiling_entry` |
| `agent_detail::check_capability_ceiling` | `contains(r)` | `ceiling_entry_admitted` |
| `trust::enumerate_policy_reachability` | `contains(r)` | `ceiling_entry_admitted` |
| `ToolRegistry::register_tool` (non-native) | `contains(r)` | **unchanged** (§4 F6) |
| `WasmBackend::invoke_tool` | `bind(r)` | **unchanged** (§4 F12) |

`RunShellTool` declares `EnforcesGrantedCaps` (its live custom invoke goes through the mediated shell's
gap-12 lookups). Nothing else in the tree declares it.

### 3.4 The property, stated precisely

For a tool that declares `EnforcesGrantedCaps`: a grant whose **scope** covers the tool's ceiling admits
it regardless of its quantity caps, so a narrower (capped) grant never refuses what a wider one admits.
The bound handle and the held grant both carry the grant's caps, and the tool enforces them. For every
other tool, admission is byte-for-byte unchanged — deliberately (§4 F1).

## 4. Adversarial self red-team

**F1 (fatal to the first draft — design changed).** The first draft clamped for *every* tool (change
`bind()` itself, no tag). Counter-example in-tree: the memory `recall` tool declares
`capability_ceiling = {read_cap_}` and enforces **its own** `read_cap_` (`rank_memory_items(..., read_cap,
...)`), never the held grant. Under a held `FsRead{mem, "", 1 KiB}` with `read_cap_ = {mem, "", 1 MiB}`
the old rule refused; an unconditional clamp would admit it and the tool would read up to 1 MiB — past
the grant (an I2 widening on the quantity axis). The same holds for any tool declaring an uncapped
ceiling and enforcing nothing. A ceiling declaration cannot tell admission whether the tool's code
enforces the grant, so it must be a separate, explicit claim. → opt-in tag; default unchanged (P2, R4).

**F2 (fatal to the obvious fix).** Fixing it in `contains()`/`subsumes()` instead would make
`attenuate({uncapped})` succeed under a capped parent, and `attenuate` returns `grant_root(narrower)` —
the uncapped request itself becomes a new grant (`invoke_agent_tool`, `agent_spawn`). That converts a
capped grant into an uncapped one. → `contains`/`bind`/`attenuate` untouched; the clamp lives only in
the bind of one call (U3, U4).

**F3. Does the bound handle carry the grant's cap?** If `bind_clamped` minted the handle around the
requirement (as `bind()` does), a consumer that enforces from the handle — WASM's `cb_http_request`
reads `cap::NetOut::byte_cap` from it — would see `nullopt` and enforce nothing. → the handle carries the
clamped capability (P1). A planted mutant returning the unclamped requirement fails 6 checks (U1, U2, U6,
U8, U9, P1).

**F4. Scope sentinels.** `path_prefix = ""`, empty `host_allowlist`, empty `method_restrictions` are also
"unrestricted" sentinels. Clamping them to the grant's would silently re-target the tool (a different set
of files or hosts than it addresses), not merely cap an amount, and the tool cannot know. → scope axes
stay strict: different mount, narrower prefix, unlisted host, unrestricted methods under a restricted
grant are all still refused (U5, U8, P3, P4, A3).

**F5. Static analysis under-reporting.** `enumerate_policy_reachability` (a security gate) used
`contains()`; after the change it would report a tagged tool *unreachable* under a capped ceiling that
the runtime *admits* — the gate would miss a reachable effect. → it now uses `ceiling_entry_admitted`
(Q1, Q2). The registration check does too (A1–A3), so an agent with a capped ceiling may list `run_shell`;
at run time `invoke_agent_tool` binds it under `attenuate(ceiling)`, i.e. under the ceiling's caps.

**F6. `ToolRegistry` for non-native provenance stays strict.** Its check is the *only* place a WASM/MCP/A2A
tool's self-reported ceiling is compared with its provenance's outer grant; the outer grant is not
re-applied at call time (the session's held set is). Clamping there would let the quantity at call time
come from the session grant, possibly wider than the provenance's. No non-native producer sets the tag
anyway (MCP descriptors declare only `ToolCall`). → unchanged.

**F7. Can a model set the tag?** No. It comes from a compile-time policy tag or host code building a
descriptor; nothing derived from model output reaches `ToolDescriptor` (I3).

**F8. Wrappers.** The test driver's `recording_tool` keeps the flag (every effect goes through the inner
run_shell, which enforces); `double_tool` keeps it (serves a recording, performs no effect). The rule for
any other wrapper is stated on the field.

**F9. Several matching grants.** The binding comes from the first grant (in grant order) whose scope
covers the requirement; the shell's `find_fs_*` returns the first grant covering the *path* it touches.
They can name different grants, but each is a grant the caller holds, so neither is a widening.

**F10. Enforcement precision of run_shell (pre-existing, unchanged).** The quota check compares current
usage with the quota *before* a write, so one write can overshoot by its own size; `cp` checks the quota
but not the source's size cap (it does not reveal content). ADR-208 D10 and gap-12 already record these.
This ADR relies on the shell's enforcement; it does not change it.

**F11. Background paths.** `background_task` and `BackgroundJobRunner` use the same helper, so they
cannot drift from the sync path. They are not exercised by a dedicated test here: `run_shell` is
`captures_session_state` and can never be backgrounded, and no backgroundable tool declares the tag yet.

**F12. WASM left on `bind()`.** WASM host callbacks enforce from the bound handle, so `bind_clamped`
would be sound there, but it changes plugin admission and cannot be built or tested in this environment
(`AGENTENGINE_WITH_WASM=OFF`). → follow-up, not done.

**F13. Other kinds and other `contains()` call sites.** `Secret.ttl`, `Schedule`, `Background`,
`AgentCall` depth, `Clock` resolution have no "uncapped" sentinel; they use the identity clamp, so for
them `bind_clamped` is `bind`. Separately, `native_jail_handle_relay.cpp` checks a synthetic
`NetOut{{host}, nullopt, {}}` with `contains()` — the gap-12 false-denial class for a `byte_cap`-capped
NetOut grant, in a per-operation check rather than admission. Named, not fixed here.

## 5. Proof

`tests/backends/native_jail/test_capped_grant_admission.cpp` — **31/31 checks**:

- **U1–U9** (the primitive): uncapped FsRead/FsWrite bind under a capped grant *with the grant's caps*;
  `contains()`/`bind()`/`attenuate()` still refuse; different mount / narrower prefix refused; a
  tighter requirement keeps its cap, a looser one is lowered; an uncapped grant binds the requirement
  itself; NetOut byte_cap clamped but host and method scope strict; with two grants the covering one is
  used.
- **P1–P5** (real `invoke_tool`): a tagged probe declaring uncapped FsRead/FsWrite is admitted under the
  capped grant and its bound handles carry the grant's size cap, quota and file cap; **P2 (positive
  control)**: the same ceiling without the tag is refused `tool.capability_not_held` before invoke;
  different mount and narrower prefix refused; an uncapped grant binds the plain tool uncapped, as before.
- **R1–R4** (the real `run_shell`, `SessionShellSandbox`, real directory): admitted under a 16-byte read
  cap / 64-byte quota and its write lands; `cat` of a 200-byte file refused by the grant's size cap with
  no content leaked; a write over the quota refused and not created; **R4 (positive control, the #148
  bug)**: the same descriptor with the tag cleared is refused.
- **Q1–Q2**: policy reachability reports the tagged tool reachable and the plain one not.
- **A1–A3**: the agent registry's ceiling check agrees.

Planted mutant (clamped binding returns the unclamped requirement): 6 checks fail, as listed in F3.

Regression: `test_agentengine_test_driver` passes with `with_granted_ceiling` **removed** (its P5 records
a real `run_shell` write under the capped grant, D10 hits the quota), plus `test_tool_pipeline`,
`test_tool_pipeline_capability_reuse`, `test_capability_enforcement`, `test_capability_declaration_tags`,
`test_policy_reachability`, `test_agent_registry*`, `test_agent_spawn_capability`, `test_tool_registry`,
`test_rt_background_job_runner`, `test_session_shell_wiring`, `test_native_exec_capability`,
`test_sandbox_capability_authorization`, `test_mcp_capability_grant`, `test_capability_grant_bridge` —
19/19 (Windows, clang-cl Debug, WASM/Python off).

## 6. ADR-208's `with_granted_ceiling`

Removed. It rewrote run_shell's ceiling to the capped grant; with the tag, admission binds run_shell under
that grant by itself, and the replay double keeps the tag (F8). `read_sandbox_file` declares an empty
ceiling, so the workaround was already a no-op for it.

## 7. Residuals and follow-ups

- WASM `invoke_tool` could move to `bind_ceiling_entry` semantics (F12).
- `native_jail_handle_relay.cpp`'s uncapped-NetOut `contains()` check (F13).
- `ExecuteCodeTool` (`tools/cli_chat.cpp`) is not tagged: the Python bridge enforces the FsWrite quota
  from the grant but not, as far as this pass checked, the FsRead size cap.
- `ToolRegistry` non-native stays strict by design (F6).
