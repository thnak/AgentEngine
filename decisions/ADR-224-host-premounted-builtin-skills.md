# ADR-224 — Host pre-mounted built-in skills, and two shell trust tiers in `cli_chat`

**Status:** **Proposed (2026-10-01).** Design (§3), one adversarial self red-team pass (§4, 7 findings
fixed, 7 residuals named), implemented and proven (§5). Awaiting Judge.

**Relates to:** GitHub issue #47. `009-Plugin-and-Extension-System.md` §8c (on-demand mounting, amended
here) and §8f (built-in skills). ADR-024 (skill-scoped tools and `mount_skill`; its "mounting grants no
authority" argument is reused unchanged). ADR-030 (cli_chat's session-scoped CodeAct wiring). ADR-096
(`SandboxToolProvider`), ADR-102 Phase 5 / ADR-119 (`run_command` and `cap::RunCommand`), ADR-208 (the
test driver's `run_shell`). `include/agentengine/core/mounted_skills_state.hpp`,
`include/agentengine/core/skill_premount.hpp`, `include/agentengine/core/builtin_skills.hpp`,
`tools/cli_chat_plan.hpp`, `tools/cli_chat.cpp`.

## 1. The question

A live `cli_chat` session (issue #47) showed a model probing the shell grammar one character at a
time across fifteen turns. The skill that documents that grammar, `shell-pipelines`, was resolved and
advertised the whole time; the model never called `mount_skill` for it. Two questions:

1. Should a host be able to mount the built-in skill that teaches a tool, before the first model call,
   when the host itself declares that tool? If so, how is that kept a host action — never something
   model output can trigger, never presented to the model as something it chose (I3)?
2. Which shell tool should `cli_chat` offer? It offered only `run_command` (a fresh Docker container per
   call: files persist, shell state does not), while the session-persistent mediated `run_shell` sat
   unused by it.

## 2. What exists (verified on `main` at `f19bbbd`)

Several of the issue's claims had moved:

- `run_shell` (`SessionShellSandbox`, `src/backends/native_jail/session_shell_wiring.hpp`) is NOT wired
  into nothing: `tools/sandboxed_shell_chat.cpp` reaches it through `SandboxToolProvider` (ADR-096), and
  the test driver through ADR-208. `cli_chat` alone did not.
- The `"%^` / `` "$` `` character rejection on `run_command` is gone (issue #50): `run_command` now
  passes an argv vector; only an embedded NUL is refused.
- The mediated shell treats a newline as a statement separator and returns every statement's output
  (issues #140/#141, `f19bbbd`).
- `shell-pipelines` was stale against the real shell. It promised "variable assignment and expansion"
  (a bare `X=1` is a parse error; `"$X"` does not expand), "`cd`, ... `cp`, and similar" (there are
  exactly ten builtins), and that a command such as `grep` resolves to a registered Tool (no session
  registers any). It did not say that `export` needs `cap::EnvWrite`, that only `cat` reads a pipe,
  that builtins take almost no flags, or which failures stop a script.
- `MountedSkillsState` recorded no provenance: a mount was a name, nothing more.

## 3. Decision

### 3.1 Provenance on every mount

`MountedSkillsState` gains `enum class skill_mount_origin { model, host }`. The model-reachable path,
`mount(name)`, records `model`; the new `mount_by_host(name)` records `host`. They are two methods, not
one method with an origin argument, so a `mount_skill` tool implementation names no origin and cannot
pass `host` by mistake. The FIRST mount of a name fixes its origin; a later mount by either party is a
no-op and does not rewrite it. `origin_of(name)` reads it. Provenance is attribution only: nothing may
read it to decide a permission, because mount state itself grants none (ADR-024).

### 3.2 Host pre-mount (`core/skill_premount.hpp`)

- `kBuiltinToolSkillPairings`: `run_shell` → `shell-pipelines`, `run_command` → `shell-pipelines`,
  `execute_code` → `using-the-code-interpreter`.
- `builtin_skills_for_tools(host_declared_tools)` — pure; input is the host's own tool list, never a
  `ContextContribution` or anything a model wrote.
- `premount_skills_by_host(state, skills, resolvable)` — mounts with `mount_by_host()`, only names the
  session's own skill sources resolved (the same check a model's `mount_skill` is held to), and reports
  what it mounted, what was already mounted (origin untouched) and what it refused.
- `render_mounted_skill_bodies(skills, state)` — the injected body of a host-mounted skill is introduced
  as `Skill 'X' (pre-mounted by the host for a tool this session offers -- not mounted by a mount_skill
  call):`; a model mount keeps cli_chat's existing `Mounted skill 'X':` heading.
- **Opt-in per host.** Nothing in the engine calls it. `cli_chat` (a host) opts in by default and
  `--no-premount-skills` turns it off. Every outcome goes to cli_chat's `actions.log`.

### 3.3 Two shell tiers in `cli_chat` (`tools/cli_chat_plan.hpp`)

They coexist, selected by `--shell`; neither replaces the other:

| Tier | Tool | Authority it needs | Approval |
|---|---|---|---|
| `mediated` (**default**) | `run_shell` — engine-native, ten builtins, no process creation, `cd` persists | FsRead/FsWrite on "work" — already minted for `execute_code`; nothing new | none (same reasoning as `execute_code`) |
| `container` | `run_command` — real `sh -c`, fresh container per call | `cap::RunCommand`, now minted **only** in this tier | every call |
| `both` / `none` | both / neither | union / none | as above |

The default is the tier that mints the least authority, needs no Docker daemon and creates no
process. `run_shell` is rooted at the same directory as `execute_code`'s "work" mount, so either tool
reads what the other wrote. Unknown arguments and tiers are errors, never a silent fallback. The
decision half (`parse_args`, `plan_tools`) lives in a header so it is testable without a model.

### 3.4 `shell-pipelines` v2

Rewritten against `mediated_shell_parser.cpp` / `mediated_shell_dispatch.cpp` as they are, plus a
closing section on how `run_command` differs. Every claim is executed by
`test_shell_pipelines_skill_grammar`, which also runs every ```` ```sh ```` block in the skill.

## 4. Self red-team (adversarial, one round)

Each finding was looked for by trying to break the design, not by re-reading it.

| # | Finding | Severity | Resolution |
|---|---|---|---|
| F1 | A single `mount(name, origin)` lets any later caller rewrite the origin: the model re-mounting a host skill would flip it to `model`, and host code could relabel a model mount as `host` | MAJOR | Fixed: two entry points, first mount fixes the origin (M2, M3, E3) |
| F2 | Injecting a host-mounted body under the same `Mounted skill 'X':` heading tells the model it chose a skill it never chose — the exact "looks model-earned" failure the issue warned about | MAJOR | Fixed: provenance-labelled heading (E2; mutant planted, E2/E3 failed) |
| F3 | A host pre-mount of a name the session did not resolve would leave a dangling mount, and would let the host mount what a model could not | MAJOR | Fixed: checked against the resolved names, refused names reported (M4) |
| F4 | cli_chat materialized skills under the "work" directory, so the "work" FsWrite grant could rewrite a skill's files although each skill's own mount is read-only — already true through `execute_code`, and `run_shell` would have made it a second, easier path | MAJOR | Fixed: skills now materialize to a sibling directory; only their read-only grants reach them (smoke run shows the new path) |
| F5 | Keeping `container` as the default kept `cap::RunCommand` minted in every session, used or not | MINOR | Fixed: minted only when `run_command` is offered (P1, P2). This changes cli_chat's default tool; stated in `--help` and the startup line |
| F6 | An unrecognised `--shell=containr` could fall back to some tier the user did not choose | MINOR | Fixed: hard error, help printed, exit 2 (P5) |
| F7 | Pairing `run_command` with a skill about the mediated grammar would teach a real `sh` user wrong limits | MINOR | Fixed: the skill's last section describes `run_command` separately |
| R1 | `run_shell` is not approval-gated; a prompt-injected model can `rm -r` the work directory | residual | Accepted: identical reach to the ungated `execute_code` (`shutil.rmtree`), no process creation; approving every call trains a reflexive `y` |
| R2 | `export` is refused in cli_chat (no `cap::EnvWrite` minted — the issue requires no new authority), while `RunShellTool`'s description says exported variables persist | residual | The pre-mounted skill states it. Whether cli_chat should mint `EnvWrite` for shell-local variables (they live only in the session's `ExecState`, never the process environment) is an owner decision |
| R3 | The host heading is text in a system message; an operator-supplied skill body (e.g. an ADR-072 external source) could contain a forged one | residual | Not a security boundary: provenance is attribution for honesty, nothing reads it to decide anything, and skill bodies are already operator-trusted system content |
| R4 | Pre-mounting `using-the-code-interpreter` makes `execute_code` declared from turn 1 in cli_chat, where it used to wait for the model's `mount_skill` | residual | Visibility only (ADR-024): grants unchanged. `--no-premount-skills` restores the old behaviour |
| R5 | The host pre-mount is recorded in the host's `actions.log`, not as an engine `RunEvent`; there is no mount event kind | residual | Out of scope; would need a 013 event-kind addition |
| R6 | cli_chat's startup orphan sweep still runs `docker` even in the mediated tier | residual | Pre-existing, best-effort, non-fatal; gating it on the tier would leave a prior container run's orphans until the next one |
| R7 | cli_chat's wiring (as opposed to its decision half) is proven by a build and a scripted smoke run, not a unit test | residual | `plan_tools` and the premount path are unit/E2E tested; the smoke run's `actions.log` shows `run_shell bound`, both host pre-mounts, and `[host]` provenance |

Not decided here (issue #47's own side question): whether `RunShellTool` joins 009 §7's generic tool
catalog. It stays `session_shell_wiring.hpp`-local.

## 5. Proof

- `tests/core/skills/test_skill_premount.cpp` — 23 checks: provenance (M1-M3), `premount_skills_by_host`
  (M4), and end to end through a real `rt::AgentSession` with the strict `ScriptedChatClient`: the
  `shell-pipelines` body is in the FIRST model request under the host heading (E1, E2), stays host
  after the model's own `mount_skill` of it (E3), a model mount of another skill is labelled as the
  model's (E4); positive control E5 — no pre-mount, no body.
- `tests/tools/test_cli_chat_plan.cpp` — 27 checks: default mediated tier mints no `cap::RunCommand`
  and needs no approval; container/both/none; `--no-premount-skills`; bad input fails closed; `--help`
  documents both flags and the default.
- `tests/backends/native_jail/test_shell_pipelines_skill_grammar.cpp` — 46 checks: G0 runs every
  ```` ```sh ```` block in the skill; G1-G14 each pin one claim of the skill text under cli_chat's exact
  mediated-tier grants, with positive controls (G5 with `EnvWrite` granted, G14's out-of-bounds read).
- Mutants: a bare `X=1` added to a skill example made G0 fail; forcing the model heading for host
  mounts made E2/E3 fail. Both reverted.
- `test_skill_source_inline` updated (the body now names `run_shell`, not the internal `ShellRunner`).
  Unchanged and green: `test_session_shell_wiring`, `test_on_demand_skill_mount`,
  `test_skill_mount_materializer`, `test_shell_python_shared_mount_prove`,
  `test_rt_agent_session_tooling_and_delegation`, `example_11_skill_mount`, `test_agentengine_test_driver`.
- `agentengine_cli_chat` builds; `--help`, a bad tier (exit 2), and two scripted startups
  (`--shell` default and `--shell=container --no-premount-skills`) behave as §3.3 says.
