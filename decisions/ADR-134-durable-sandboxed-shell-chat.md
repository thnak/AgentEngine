# ADR-134 — `tools/durable_sandboxed_shell_chat.cpp`: the first real production consumer of durable content storage

- **Status:** Proposed — implemented, verified for real (Windows/MSVC, real Docker daemon, two real
  process invocations), full rebuild (zero errors, 319 targets) and full `ctest` clean (292 total, 1
  failure, pre-existing/environment, zero regression), `naming_lint.py` clean. NOT Judged — no
  independent red-team pass yet, no Linux verification yet. **Honestly disclosed, not a blocker**: the
  actual interactive chat loop (`Bundle::ask()` against a real OpenAI endpoint) was NOT exercised in this
  pass — no `OPENAI_API_KEY` was available in the verification environment. That code path is unmodified,
  already-shipped machinery `tools/sandboxed_shell_chat.cpp` already uses in production; everything this
  ADR actually built (durable identity/ledger/store wiring, crash-recovery reattachment) was verified for
  real, twice, end to end, on real disk, independent of that gap.
- **Date:** 2026-08-30.
- **Scope:** `tools/durable_sandboxed_shell_chat.cpp` (new), `CMakeLists.txt` (one new target registered).
  No existing file was modified.
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
