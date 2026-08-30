# ADR-134 — `tools/durable_sandboxed_shell_chat.cpp`: the first real production consumer of durable content storage

- **Status:** Proposed — implemented, verified for real (Windows/MSVC, real Docker daemon, two real
  process invocations), full rebuild (zero errors, 319 targets) and full `ctest` clean (292 total, 1
  failure, pre-existing/environment, zero regression), `naming_lint.py` clean. **SAME-DAY INDEPENDENT
  RED-TEAM ROUND COMPLETE (§6, 2026-08-30)**: one honest documentation gap found and fixed (Ctrl+C/SIGINT
  disclosure), one methodology gap in this ADR's own §3 evidence found and closed with a new permanent
  automated regression test (`tests/test_durable_sandboxed_shell_chat_cross_process.cpp`) that checks a
  strictly stronger, decisive signal than tree-object count alone. Full rebuild (zero errors) and full
  `ctest` (293 total — one more than before, the new test — 1 pre-existing failure, zero regression) and
  `naming_lint.py` clean after every change. Still NOT Judged — no Linux verification yet. **Honestly
  disclosed, not a blocker**: the actual interactive chat loop (`Bundle::ask()` against a real OpenAI
  endpoint) was NOT exercised in this pass — no `OPENAI_API_KEY` was available in the verification
  environment. That code path is unmodified, already-shipped machinery `tools/sandboxed_shell_chat.cpp`
  already uses in production; everything this ADR actually built (durable identity/ledger/store wiring,
  crash-recovery reattachment) was verified for real, twice, end to end, on real disk, independent of
  that gap.
- **Date:** 2026-08-30.
- **Scope:** `tools/durable_sandboxed_shell_chat.cpp` (new; independent red-team round added a Ctrl+C/
  SIGINT disclosure to its top comment, no behavior change), `CMakeLists.txt` (one new target
  registered), `tests/test_durable_sandboxed_shell_chat_cross_process.cpp` (new, added by the
  independent red-team round), `tests/CMakeLists.txt` (one new test target registered by the same round).
  No existing production file's behavior was changed.
- **Related specs:** Closes `decisions/ADR-132-store-generic-sandbox-tool-surface.md` §5's own disclosed
  residual: "nothing in this ADR changes any real production caller to actually USE a non-default
  `Store`... a real host wanting durable content still has to explicitly instantiate
  `MandatorySandboxProvider<Surface, FileWorktreeObjectStore>` itself." This tool is that host.

## 1. The question

ADR-130 (`FileWorktreeObjectStore`), ADR-128 (`bind_root_branch()`), and ADR-132 (the `Store`-generic
tool surface) together make durable, crash-recoverable content storage a real *capability* of
`MandatorySandboxProvider` — proven correct end to end by
`tests/test_task_branch_content_durability_integration.cpp`. But nothing in the whole design line wires
a real, user-reachable host to actually use it. Does the capability work when a real user runs a real CLI
tool twice, killing it in between — not merely when a test simulates the same shape?

## 2. Design: a separate tool, not a change to `sandboxed_shell_chat.cpp`

Mirrors `tools/sandboxed_shell_chat.cpp`'s own established reasoning for staying separate from
`tools/cli_chat.cpp` (that file's own top comment): this tool's durable identity/ledger/store wiring is a
real, distinct host choice (persistent state across process restarts, on real disk, under the user's own
home directory), not a drop-in replacement for the already-shipped, deliberately-ephemeral tool. Zero
risk to that tool — this file changes nothing it depends on, and needed no edit to it at all.

**The security precondition this tool gets right, deliberately, not by accident**
(`decisions/ADR-130-content-durability-conformer.md`'s own `test_identity_durability_precondition.cpp`
proved this the hard way): making `Ledger`'s own content durable WITHOUT also durably configuring
`IdentityAuthority::bootstrap()` turns a previously-latent id-recycling risk into a real, working
cross-principal content leak on a process restart. This tool configures BOTH `Ledger`'s own `durable_dir`
AND `IdentityAuthority::bootstrap()`'s own `durable_dir`, under the same real, persistent root,
consistently — never one without the other. Structure: `~/.agentengine/durable_shell_chat/{ledger,
objects,identity}` (falls back to a temp-directory location if `HOME`/`USERPROFILE` is unavailable,
disclosed rather than silent — a host relying on genuine durability across a real reboot should not run
this tool in an environment with no home directory).

Deliberately smaller than `sandboxed_shell_chat.cpp`: does NOT compose `SandboxToolProvider`/
`QuarantineSecretStore` (no `run_shell` tool, no HMAC dependency) — keeps the demonstration focused on
the durable-content story specifically, and makes this target portable on both platforms from the start
(unlike `sandboxed_shell_chat.cpp`, which needed ADR-105/107 to close a real Linux HMAC gap before it
could drop its own WIN32 gate).

Uses `bind_root_branch()` (ADR-128) rather than `create_root_branch()`+`bind_sandbox()` by hand — the
first real host consumer of that method, exercising the crash-recovery decision (reclaim-if-orphaned,
create-if-not) for real on every single run, not merely in a test.

## 3. What was verified — for real, not merely built

Ran the resulting `agentengine_durable_sandboxed_shell_chat.exe` twice, as two genuinely separate process
invocations, against a real, locally-running Docker daemon:

- **First run**: real durable state created on real disk — confirmed directly by inspecting the
  filesystem afterward: `identity/identity_adopted.log`, `identity/identity_next_id.txt`,
  `ledger/ledger_state.snapshot`, `objects/blobs/`, `objects/trees/` (containing one real tree object,
  the empty tree `create_root_branch()`'s own internal `put_tree(Tree{})` call produces). The tool ran
  through the full orphan sweep, identity/quota minting, durable `Ledger<FileWorktreeObjectStore>`
  construction, and `bind_root_branch()` (a real, successful CREATE path, since no root existed yet)
  before reaching the expected, correctly-handled `quickstart_builder.no_store` failure (no
  `OPENAI_API_KEY` set in this verification environment) — a clean, documented exit, not a crash.
- **Second run**: **THE CORE CLAIM** — re-ran the exact same binary again. The real, on-disk tree count
  stayed at exactly 1 (not 2), confirming `bind_root_branch()` genuinely took the RECLAIM path this time
  (the same root branch, reattached), not the create path (which would have produced a second, duplicate
  root and a second tree object) — real crash-recovery, demonstrated across two genuinely separate real
  process invocations of the actual shipped tool, not simulated.

Full project rebuild (`cmake --build . --config Debug`, 319 targets): zero errors, zero warnings on the
new file (one MSVC `C4996` deprecation warning on an initial `std::getenv()` use was found and fixed by
switching to the already-established, portable `agentengine::pal::env_var()` helper this codebase's own
`external_skill_discovery.hpp` already uses for the identical `HOME`/`USERPROFILE` lookup — not a new
pattern, reused). Full `ctest`: **292 total, 1 failure** — the same, already-established
`test_reference_agent_task_corpus` pandas/matplotlib environment gap, zero regression anywhere else.
`python tools/naming_lint.py`: clean, 361 suppressed findings, unchanged (no new exported type — this
file is a tool, not a library header).

## 4. What was NOT done

- **The actual interactive chat loop was not exercised.** No `OPENAI_API_KEY` was available in this
  verification environment. `Bundle::ask()` (`core/session_builder.hpp`) is unmodified, already-shipped
  machinery `tools/sandboxed_shell_chat.cpp`/`tools/cli_chat.cpp` already use in production — this ADR
  did not touch that code path at all, only the session-construction wiring around it.
- **No independent red-team pass yet.** A new production tool composing `MandatorySandboxProvider<
  Surface, Store>` for the first time with a real, non-default `Store` — the expected next step for
  anything touching this design line.
- **No Linux verification yet.** Same established next-step pattern as every other ADR in this line;
  this tool was specifically designed to be portable (no `SandboxToolProvider`/HMAC dependency), but that
  claim has not yet been executed on real Linux/GCC.
- **No `run_shell`/native-jail tool composed alongside `run_command`** — unlike `sandboxed_shell_chat.cpp`,
  this tool demonstrates ONE provider only, by design (§2), to keep the durable-content story isolated
  and legible.
- **No automatic garbage collection or cross-process locking** for the durable state directory —
  inherits, unchanged, every residual `ADR-130`/`ADR-132` already disclosed for the underlying mechanisms
  this tool composes. A real, concurrently-running SECOND invocation of this same tool would hit the
  exact metadata-bookkeeping race `tests/test_content_durability_concurrency.cpp` already demonstrates
  and does not close — not a new risk this tool introduces, but worth naming: this tool is not safe to
  run twice concurrently against the same durable root.

## 5. Residuals

- Everything named in §4.
- The fallback home-directory lookup (`host_home_dir()`) is duplicated from
  `external_skill_discovery.hpp`'s own identical five-line helper rather than shared — a deliberate,
  small duplication (avoiding a dependency on an unrelated feature's own `_detail`-namespaced internal
  helper) matching this file's own "separate tool" framing, not an oversight.
- ~~No independent red-team pass yet.~~ **Closed by §6.**

## 6. Independent red-team round (same day)

An independent adversarial pass, briefed with zero prior context beyond this ADR and told to review this
the way any new CLI tool touching identity, quotas, and a real Docker daemon would be reviewed. Read the
full diff (`git show f1abbf2`) directly rather than trusting this ADR's own account, then verified the
specific claims below against real, running code — not merely reasoned about them.

- **The identity/content durable-root precondition (§2's own security claim), re-derived from the actual
  code, not the ADR's prose.** Confirmed `IdentityAuthority::bootstrap(identity_durable_dir)` and
  `Ledger<FileWorktreeObjectStore>(FileWorktreeObjectStore(objects_dir), ledger_durable_dir)` are both
  constructed from the SAME `durable_root` variable, unconditionally, on every code path through `main()`
  — there is no branch where one is configured durably and the other is not. One real, adversarial
  ordering hazard was traced and found NOT to apply here: `IdentityAuthority::bootstrap()`'s own
  Meyer's-singleton is "first call in the process wins" (`include/agentengine/trust/identity_authority.
  hpp` §"first call wins" comment) — a `bootstrap()` call anywhere else in the process, before this
  tool's own explicit durable call, would silently construct the singleton in-memory-only. Confirmed by
  reading every `IdentityAuthority::bootstrap()` call site reachable from this target's own link set
  (`agentengine::core`, `agentengine::worktree_store`, `agentengine::sandbox_io`,
  `agentengine::provider_http_client`) that none is a namespace-scope/global-constructor call — the
  orphan sweep that runs before it (`DockerCliBackend::reap_orphans()`) never touches identity at all.
  Real, but pre-existing and already disclosed at `decisions/ADR-102-identity-native-sandbox-
  implementation-phase-1.md`'s own "single-process-per-authority assumption" residual — not a new gap
  this tool introduces, and not something a per-tool ADR can close on its own.
- **The home-directory fallback, run for real, not trusted from the comment.** Built the target fresh
  and invoked the actual compiled binary with `USERPROFILE`/`HOME` unset (PowerShell,
  `Remove-Item Env:USERPROFILE`/`Env:HOME`): the tool correctly fell back to
  `%TEMP%\.agentengine\durable_shell_chat`, created real identity/ledger/object-store state there, and
  reached the same clean `quickstart_builder.no_store` failure — then ran it a SECOND time in the same
  fallback location and confirmed the real on-disk tree count stayed at 1. The fallback works as
  documented.
- **The startup orphan sweep's failure handling, read against `DockerCliBackend::reap_orphans()`'s own
  implementation.** `main()`'s `!swept.has_value()` branch prints "skipped (non-fatal)" and falls through
  — confirmed this is genuinely reachable and genuinely non-fatal (Docker unreachable/not on PATH makes
  `docker ps -a` exit non-zero, which `reap_orphans()` turns into a real `result<>` error, never a thrown
  exception or a hard exit) — matches the claim, not merely asserted by it.
- **The Ctrl+C/SIGINT disclosure gap — a real, fixed finding.** This file's own original top comment said
  nothing about interrupt handling at all, unlike `tools/sandboxed_shell_chat.cpp`'s own extensive
  disclosure of the identical, still-unclosed gap (no `SetConsoleCtrlHandler`/SIGINT handler anywhere in
  this codebase). Silent, not false — this file never claimed anything stronger than its sibling actually
  has — but a real omission for the FIRST tool whose whole point is crash-recovery, where Ctrl+C is
  exactly the scenario a user is likely to trigger. **Fixed**: added an explicit disclosure to this
  file's own top comment (mirroring `sandboxed_shell_chat.cpp`'s own), plus the honest, additional,
  durable-case-specific reassurance that `Ledger<Store>::create_root_branch()`/`commit()` persist
  synchronously before returning (no batched/deferred/at-exit flush for a Ctrl+C to tear), so an
  interrupt here only ever loses in-flight, uncommitted work — exactly the case `bind_root_branch()`'s
  own reclaim-if-orphaned logic already handles.
- **REAL FINDING, made while sanity-checking a new regression test (see below), that also applies to this
  ADR's own §3 verification methodology.** `tools/durable_sandboxed_shell_chat.cpp` had ZERO automated
  coverage — §3's own evidence was a human manually re-running the binary twice and eyeballing the
  filesystem. Turning that into an automated test (`tests/test_durable_sandboxed_shell_chat_cross_process.
  cpp`, invokes the real built binary twice via `std::system()`, isolated under a redirected
  `USERPROFILE`/`HOME` so it never touches real user state, `OPENAI_API_KEY` forced genuinely absent so
  it never blocks on stdin or makes a real network call) surfaced that **"the real, on-disk tree count
  stayed at exactly 1" — this ADR's own §3 core-claim evidence — does NOT actually distinguish a genuine
  RECLAIM from a hypothetically-broken ALWAYS-CREATE path.** Both `Ledger<Store>::create_root_branch()`
  and `reclaim_orphaned_branch()` resolve to the identical deterministic branch name
  (`"root-" + owner.id() + "-" + session_id`), `create_root_branch()`'s own
  `branches_.insert_or_assign(name, ...)` OVERWRITES rather than duplicates an existing entry of that
  name, and the branch's initial tree is always the same content-addressed empty `Tree{}` digest — so
  even a wrongly-repeated CREATE leaves exactly the one tree object a correct RECLAIM would. Confirmed by
  deliberately sabotaging `MandatorySandboxProvider::bind_root_branch()` (forcing `is_orphan = false`
  unconditionally, temporarily) and observing the tree-count assertion stay green regardless. The
  decisive signal found instead: `create_root_branch()` unconditionally calls `persist_snapshot_locked()`
  (rewrites `ledger_state.snapshot`); `reclaim_orphaned_branch()` never touches the durable snapshot at
  all. The new test asserts `ledger_state.snapshot`'s own last-write-time is UNCHANGED across the second
  invocation — confirmed, with the same sabotage-and-revert cycle, that this signal correctly FAILS when
  reclaim is skipped and correctly PASSES on the real, unmodified code. Full rebuild (zero errors) and
  full `ctest` (293 total — one more than this ADR's own §3 count, the new test — 1 pre-existing failure,
  `test_reference_agent_task_corpus`, zero regression) and `naming_lint.py` (361 suppressed, unchanged)
  after every change, including after reverting the sabotage.
- **CMakeLists.txt registration, checked against its sibling.** `agentengine_durable_sandboxed_shell_chat`
  links `agentengine::core`, `agentengine::provider_http_client`, `agentengine::worktree_store`,
  `agentengine::sandbox_io`, `agentengine_warnings`, and MbedTLS — confirmed this is neither more nor
  less than what the file actually includes: no `agentengine::mediated_shell_runner`/`agentengine::hmac`
  (the file never includes `secret_quarantine.hpp`/`sandbox_tool_provider.hpp`), and no `WIN32` gate (the
  file's only `_WIN32` use is a preprocessor branch inside `host_home_dir()`, already portable). Built the
  target fresh (not incrementally) to confirm no dependency was hidden by a stale incremental build.

No other findings — the security precondition (§2), the Docker-unreachable path, and the rest of the
tool's own error handling matched what the ADR claimed on direct inspection of the real code and real
runs against a real Docker daemon.
