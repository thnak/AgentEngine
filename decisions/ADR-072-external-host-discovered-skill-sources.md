# ADR-072 — Should AgentEngine auto-scan the host machine for skills belonging to OTHER installed
# AI coding tools (Claude Code, GitHub Copilot, OpenAI Codex), and if so, how?

**Status:** Proposed (design → red-team → prove phases complete; awaiting project-owner "Judged"
sign-off, matching ADR-070/071's own flow).

**Relates to:** `009-Plugin-and-Extension-System.md` §8 (owns the `SkillSource`/`SkillsProvider`
machinery this ADR reuses unchanged, §8g cross-references this ADR), `decisions/ADR-024-skill-
scoped-tool-and-mount-wiring.md` (on-demand mounting, unaffected), `decisions/ADR-066-context-
provider-attribution-provenance.md` (the provenance mechanism this ADR's residual falls short of,
§7), `decisions/ADR-070-host-configurable-responsibility-boundary.md` (the Delegated Decision Seam
pattern this ADR is checked against, §4), `decisions/ADR-071-native-unsandboxed-process-execution-
providers.md` (the sibling "expand the responsibility-shift surface" ADR from the same session —
cited throughout for what does and does NOT carry over), `docs/research/2026-08-21-external-ai-tool-
skill-conventions.md` (the dated, sourced research behind `kKnownRoots`, §2).

## 1. The question

**Stated so it has a wrong answer:** a host building on AgentEngine as an SDK may already have
Claude Code, GitHub Copilot, and/or OpenAI Codex CLI installed on the same machine, each with its
own library of `SKILL.md`-format skills the user has accumulated over time. Should AgentEngine give
that host a way to surface those same skills into an AgentEngine session without hand-authoring a
duplicate copy — and if so, does doing so require new parsing, a new provider type, a new capability
kind, or a sandbox-style gate the way ADR-071's native-exec providers needed one? Getting this wrong
by under-building (inventing a bespoke parser or provider per tool) adds needless surface area for a
problem that, on inspection, does not exist; getting it wrong by over-building (a native-exec-style
capability gate, or worse, automatic engine-internal scanning) either adds ceremony with no
corresponding risk to gate, or violates ADR-070 property 1 ("explicit opt-in") outright.

## 2. Prior art surveyed, and what it already proves

`core/skill_source.hpp`'s `DiskSkillSource` (009 §8) already loads every `skill-name/SKILL.md`-
shaped subdirectory of ANY real host directory a caller points it at — origin-agnostic by design,
with no assumption baked in about who authored the directory. `core/skill_provider.hpp`'s
`SkillsProvider` already resolves N such sources, rejects (fail-closed) two sources declaring the
same skill name rather than silently letting one shadow the other, mounts every resolved skill
read-only into the session's own virtual worktree (`core/worktree.hpp`'s real Tree/Ref/Mount, never
a raw host path handed to the agent), and contributes a "name: description" advertisement message.
Confirmed by reading `MountedSkillsState`'s own top comment and `tool_pipeline.hpp`'s `invoke_tool`
(`:508-522`): mounting a skill grants **no new capability** — the capability-bind step is entirely
independent of skill/mount state, so a skill's `allowed-tools` frontmatter can only ever *narrow*
which already-capability-authorized tools are declared/invocable while mounted, never widen. Dated,
sourced research (`docs/research/2026-08-21-external-ai-tool-skill-conventions.md`) found GitHub
Copilot added `SKILL.md` support in April 2026 specifically so the same files work unmodified across
Claude Code, Cursor, Codex CLI, and 20+ other agents; GitHub's own docs recognize three equivalent
project-level directories (`.github/skills/`, `.claude/skills/`, `.agents/skills/`) plus per-tool
user-level directories (`~/.copilot/skills/`, `~/.codex/skills/`, `~/.claude/skills/`, and the
tool-agnostic `~/.agents/skills/`). This machine's real environment corroborates the research is
describing a live convention, not just documentation: `~/.codex/skills/` exists (a real, installed
Codex CLI), and `~/.claude/skills/*` are real NTFS symlinks resolving into
`~/.agents/skills/<name>/SKILL.md`.

**What this means for scope**: because the wire format converged, this ADR's entire job is finding
the right real paths — `parse_skill_md` (`core/skill.hpp:155-218`), `DiskSkillSource`, and
`SkillsProvider` need zero changes.

## 3. The competing designs

**Design A — per-tool bespoke parser/adapter, converting each tool's native format into `Skill`.**
Steelman: would be necessary if the tools' formats had NOT converged. Rejected: the research in §2
establishes the premise is false for all three in-scope tools — building adapter code for a format
divergence that doesn't exist is pure unnecessary surface area, and every line of bespoke parsing is
a line that could silently drift from `parse_skill_md`'s own validation rules (§8a: "1-64 chars,"
directory-name match, etc.) if maintained separately.

**Design B (chosen) — a locator function producing `(origin_id, path)` pairs, reusing
`DiskSkillSource`/`SkillsProvider` unchanged.** `core/external_skill_discovery.hpp`'s
`discover_external_skill_locations()` knows only the conventional real paths per tool/scope, checks
directory *existence* (never file content), and returns `ExternalSkillLocation` values a host feeds
directly into `make_skill_source_descriptor(DiskSkillSource(loc.origin_id, loc.path))` — the exact
existing call a host already makes for its own project skills. Steelman: smallest possible surface
area, zero changes to already-tested, security-relevant code (`SkillsProvider`'s collision/mount/
capability behavior), and the discovery function itself is trivially auditable (a static table plus
an `is_directory` check, no parsing, no I/O beyond `stat`).

**Design C — engine-internal automatic scanning at session/engine startup, no host call required.**
Rejected outright, without a prove phase: this violates ADR-070 property 1 ("explicit opt-in... never
reachable ambiently") directly. A host that never intended to expose its Claude Code skill library to
a completely different AgentEngine-based application would have no way to opt out — the opposite of
"transfer responsibility to the consumer dev," which requires the consumer dev to make an active
choice, not inherit one silently.

**Design D — gate discovery behind a new capability kind (`cap::ExternalSkillRead`), ADR-071-style.**
Considered because ADR-071's `cap::NativeExec` was this session's most recent precedent for "host
machine access." Rejected after checking the actual reason ADR-071 needed a capability: `cap::
NativeExec` exists because PATH-scanning there could become an oracle the MODEL exploits via a tool
call to discover/invoke reachable executables — a live I2 concern, since the model's own tool calls
are what drive native process execution. Here, `discover_external_skill_locations()` is called by
**host code**, never the model (the model has no tool that calls it) — the same trust category as a
host already being free to point today's `DiskSkillSource` at any path it chooses, which carries no
capability gate today either. Adding one here would gate a host-only code path against a model-facing
threat model it does not share — ceremony without a corresponding risk.

## 4. Required properties (checked against ADR-070 §4's five)

1. **Explicit opt-in.** `discover_external_skill_locations()` is called nowhere in the engine
   automatically; a host must call it and then separately choose to construct a `DiskSkillSource`
   from each returned location. Two independent host decisions, not one implicit one.
2. **Fails closed/safe when unset.** A host that never calls this function sees zero behavior
   change from before this ADR — `SkillsProvider` behaves identically whether or not this header is
   even included.
3. **Narrows or decides among already-possessed authority only.** Vacuously satisfied in a different
   sense than ADR-070/071's capability-narrowing instances: this function never touches the
   capability model at all (§2's `invoke_tool` evidence) — a discovered skill's `allowed-tools` can
   only narrow tool declaration, never grant. The one thing this design does NOT narrow is *content
   exposure*: see §6 item 1 for the honestly-scoped residual this leaves.
4. **Host code, never model output.** `project_root: std::optional<std::filesystem::path>` is the
   only input; nothing tainted (`Tainted<T>`) can reach this function's signature.
5. **Always audited (I4).** Not directly applicable — this function performs no effect requiring
   `EffectContext`/audit attribution (it is a pure filesystem query, same category as `DiskSkillSource`
   itself, which is also unaudited). What IS attributable: `SkillsProvider`'s existing collision error
   names the colliding `origin_id`s, so a host inspecting a failure can always tell which two real
   directories collided — proven in §5c below.

## 5. Falsifiable claims, evidence, and per-claim verdicts

### 5a. Discovery correctness (`tests/test_external_skill_discovery.cpp`, D1-D5c)

| Claim | Disproving experiment | Verdict |
|---|---|---|
| An isolated environment with nothing installed and no project root discovers nothing. | Redirect `HOME`/`USERPROFILE` to an empty scratch directory, call with no `project_root`; assert empty. | **CORRECT** — D1. |
| Each of the four known tool/scope conventions is discovered when, and only when, a real directory exists there. | Create exactly one synthetic root per tool/scope (Claude Code user, Codex project, Copilot user, generic_agents project); assert each is found, every OTHER tool/scope combination with nothing on disk is absent. | **CORRECT** — D2-D5. |
| `origin_id` follows the documented `external:<tool>:<scope>` shape and `path` is the exact real directory. | String/path equality check against the synthetic fixture. | **CORRECT** — D2. |
| `project_root = std::nullopt` skips every project-scope check while leaving user-scope discovery unaffected. | Call with no `project_root` against a fixture with both scopes populated; assert only user-scope rows returned. | **CORRECT** — D5b. |
| A REAL candidate path segment (`~/.copilot`) existing as a plain FILE, not a directory, fails closed for that one candidate without crashing or suppressing discovery of every other, unrelated real candidate. | Create `~/.copilot` as a file (not directory) in the scratch tree; assert the Copilot user-scope location is absent AND every other genuinely-real candidate is still found. | **CORRECT** — D5c (found-during-review: the original version of this check used an arbitrary non-candidate filename, `fake_claude_home`, which never actually exercised a path-segment collision at all — corrected to collide with a real `kKnownRoots` entry, see §8). |

### 5b. Real-machine evidence (`test_external_skill_discovery.cpp`, D6) — opportunistic, not required

| Claim | Disproving experiment | Verdict |
|---|---|---|
| On THIS actual development machine (no HOME override), the real, already-installed `~/.claude/skills` and `~/.codex/skills` directories are genuinely discovered. | Run discovery against the real, unmodified environment; assert both are present in the result. | **CORRECT** — confirmed independently via `ls` earlier this session (`~/.codex/skills` exists; `~/.claude/skills` contains real symlinked entries) before the test was written, then proven programmatically. |
| `DiskSkillSource`'s `directory_iterator`-based walk actually FOLLOWS the real NTFS symlinks under `~/.claude/skills/*` (each one a real symlink into `~/.agents/skills/<name>/SKILL.md` on this machine) rather than silently skipping them. | Feed the real discovered `~/.claude/skills` location into `DiskSkillSource::load_skills()`; assert it succeeds AND returns at least one real skill. | **CORRECT** — this was a genuine open question going in (Windows/MSVC `std::filesystem` symlink-following behavior was not assumed, per this project's "execute and observe" discipline, and this exact category of platform assumption was wrong once already this session for `_dupenv_s` vs `SetEnvironmentVariableA`, ADR-071 §5c) — at least one real symlinked skill parsed successfully. |

### 5c. `SkillsProvider`'s existing safety properties are unaffected by discovered sources (`R-D7`)

| Claim | Disproving experiment | Verdict |
|---|---|---|
| Two DISCOVERED external locations declaring the same skill name still fail the whole `on_context()` call, exactly as two project-local sources already do (009 §8c) — no silent shadowing introduced by this feature. | Two synthetic `DiskSkillSource`s built from realistic `external:*` origin_ids, both declaring `shared-name`; assert failure, `skill.name_collision_across_sources`, and zero partial mount. | **CORRECT** — R-D7. |
| The collision error names BOTH real `origin_id`s, so a host can tell exactly which two external tools' installs collided. | Inspect the error message text for both origin_id substrings. | **CORRECT** — R-D7. |

### Evidence commands

Windows/MSVC build (the documented `vcvars`-equivalent env-var workaround — **note**: Visual Studio's
own install path changed since ADR-071 was written, from `...\Microsoft Visual Studio\2022\
BuildTools\...` to `...\Microsoft Visual Studio\18\BuildTools\...`; MSVC toolset `14.51.36231` and
SDK `10.0.26100.0` unchanged):

```
cmake --build . --target test_external_skill_discovery -- -j1
./tests/test_external_skill_discovery.exe
```
`test_external_skill_discovery`: **20/20 checks pass**, including D6's real-machine evidence.

Full tree: `cmake --build . -- -j4` — zero compile errors, all targets. Full `ctest --output-on-failure -j4`:
**228/228 tests passed, 0 failed** (100%, Total Test time 101.87s) — including `test_external_skill_
discovery` itself (20/20 checks, 0.08s) and every pre-existing skill-source/skill-provider test
(`test_skill_source_disk`, `test_skill_provider_mount`, `test_skill_source_inline`) unaffected. Zero
regressions anywhere in the tree, and unlike ADR-071's evidence run, no unrelated `-j4` flake occurred
this time.

## 6. The red-team attack

1. **Can a host that opts in unknowingly surface a third-party, unreviewed skill's instructions into
   an AgentEngine session's context?** Yes — this is the real, honestly-scoped residual this design
   accepts, not a bug it silently carries. `SkillsProvider`'s "resolve = advertise name+description
   unconditionally" behavior (unchanged, pre-existing) means simply discovering an external tool's
   skill library puts every discovered skill's name+description into context, even before any skill
   is explicitly mounted — a host that calls `discover_external_skill_locations()` and blindly wires
   every result into `SkillsProvider` inherits whatever third-party content (e.g. an `npm`-installed
   package that dropped a `SKILL.md` into `~/.codex/skills/`) happens to be on that machine. Mitigated
   by property 1 (explicit, two-step opt-in — a host can inspect `ExternalSkillLocation`s, e.g. by
   logging or filtering by `tool`/`scope`, before ever constructing a `DiskSkillSource` from one) and
   by the fact that `SkillsProvider`'s collision guard (§5c) still catches a *colliding* name — it
   does not, and structurally cannot, vet non-colliding new content for intent. This is exactly the
   risk category the user explicitly signed off transferring to the consumer dev this session; not
   treating it as solved would be dishonest, so it is named here rather than implied away.
2. **Can a discovered skill's `allowed-tools` frontmatter widen what's invocable beyond what the
   session already holds capability for?** No — §2's `tool_pipeline.hpp` evidence: the capability
   bind step (`held.bind(requirement)` against `tool->capability_ceiling`) is structurally
   independent of skill/mount state; `allowed_tool_names_for()` only ever narrows which
   already-authorized tools are DECLARED/accepted, confirmed by reading `skill_provider.hpp`'s own
   code, not assumed.
3. **Does symlink-following introduce a path-traversal-style escape (a symlink pointing OUTSIDE the
   discovered root, onto arbitrary host content)?** `DiskSkillSource`'s walk only ever reads file
   *content* at whatever path the OS resolves a symlink to — it has no notion of "confine to root"
   (unlike ADR-071's `native_worktree_bridge`, which had to build exactly that for a spawned child's
   argv). A symlink under `~/.claude/skills/firecrawl` pointing to `~/.agents/skills/firecrawl` is
   itself just another location on the same machine the host's own OS-level file permissions already
   govern — no new privilege is exercised reading through it that a plain `cat` from that same host
   account couldn't already do. Named as a non-issue for THIS ADR's scope (no privilege boundary is
   crossed), not silently ignored.
4. **Does this design create two divergent notions of "where do my skills come from" the way a
   second Python runtime would have for ADR-071?** No — deliberately avoided by construction: there
   is exactly one skill-loading pipeline (`DiskSkillSource`/`SkillsProvider`), regardless of whether
   a location came from a host's own project configuration or from `discover_external_skill_
   locations()`. A skill mounted this way is indistinguishable, once mounted, from any other skill —
   which is the intended reuse, not a gap.
5. **Does the discovery table itself risk staleness (a tool changes its convention, or a fifth tool
   should be added)?** Named as an explicit, accepted maintenance surface, not solved: the table in
   `external_skill_discovery_detail::kKnownRoots` is a plain, small, easily-editable static array with
   the citing research inline in the header's own top comment; a wrong or stale entry fails safe (the
   `is_directory` check simply finds nothing), never unsafe.

No FATAL finding. No design change resulted from this pass.

## 7. The decision

**Design B is adopted and implemented.** It binds `009-Plugin-and-Extension-System.md` §8g (new,
cross-references this ADR) — no other spec requires amendment, since `SkillsProvider`/`DiskSkillSource`
themselves are unchanged. Full-tree evidence: `test_external_skill_discovery` 20/20; full `ctest`
228/228 (100%), zero regressions.

**Explicitly out of scope, named rather than left implied:**
- Antigravity — no confirmed, dated convention was found for it; adding it later is a small,
  additive change to `kKnownRoots` (one more row), not a redesign.
- Per-skill provenance rendered into the "name: description" advertisement text itself
  (`PromptSkillSummary`) — `origin_id` is visible today only in `SkillsProvider`'s own
  collision-error text and in the `ExternalSkillLocation` a host receives before ever constructing a
  source from it. Extending `PromptSkillSummary` would touch roughly nine existing tests asserting
  exact advertisement text (`test_reference_agent_prompt.cpp`, `test_context_provenance.cpp`,
  `test_skill_provider_mount.cpp`, and others) for a blast radius out of proportion to this feature's
  actual scope — named as an accepted residual (ADR-041 precedent), not silently worked around.
- Vetting or scoring discovered third-party skill content in any way (§6 item 1) — the explicitly
  accepted risk this ADR's whole design posture assumes the host either accepts or filters itself.

## 8. Found and fixed during code review, not left as residual

A code-review pass (this project's `/code-review` skill) against the initial implementation found
four real issues, all fixed before this ADR's evidence was finalized:

1. **CI-breaking (the most severe finding).** D6's original checks hard-asserted
   `~/.claude/skills`/`~/.codex/skills` presence unconditionally — correct on this dev machine, but
   this project's `windows-msvc`/`windows-clang-cl` CI jobs (`.github/workflows/ci.yml`) run the full
   `ctest` suite on fresh `windows-latest` GitHub-hosted runners, where neither tool is installed;
   `test_external_skill_discovery` would have failed in CI on every run. Fixed by making D6
   opportunistic (matching `tests/CMakeLists.txt`'s own established "gated on environment, SKIP when
   unset, stays green" convention for other machine-dependent evidence, e.g. the `live-network`
   tests): the real-evidence assertions now run and prove something ONLY when the real directories
   are present, and print an informational skip line — never a failure — otherwise.
2. **Uncited research.** §2's claims were dated but not sourced to a `docs/research/*.md` file,
   violating CLAUDE.md's "research is dated and cited" rule. Fixed by writing
   `docs/research/2026-08-21-external-ai-tool-skill-conventions.md` with the real search queries and
   URLs behind `kKnownRoots`, and re-pointing §2 and this ADR's own header at it.
3. **Misleading comment.** `host_home_dir()`'s comment claimed to mirror `pal/env.hpp`'s
   `#if defined(_MSC_VER)` split, but the code branches on `#if defined(_WIN32)` — a different axis
   (which env var NAME the OS defines, not which CRT function MSVC deprecates). Left uncorrected, a
   future maintainer trusting the comment could "fix" the code to match it, silently breaking
   home-directory resolution under a non-MSVC Windows toolchain (MinGW/clang, where `_MSC_VER` is
   undefined but `USERPROFILE` is still the right variable). Fixed by correcting the comment to state
   the actual, deliberate reason for `_WIN32`.
4. **A test that didn't test what its comment claimed.** D5c's original fixture
   (`fake_claude_home`) was not any real `kKnownRoots` path segment, so it never exercised the
   file-where-a-directory-is-expected scenario its comment described — a real regression in that
   exact scenario would have gone undetected. Fixed by making `~/.copilot` itself the colliding file
   (a genuine candidate path from the table), so the test now proves the claim it makes.

`test_external_skill_discovery` re-verified after all four fixes: still 20/20 (D5c and D6 rewritten,
same count). Full tree rebuilt and full `ctest --output-on-failure -j4` re-run: **228/228 passed, 0
failed** (100%, Total Test time 101.32s) — identical to the pre-fix run, confirming the fixes changed
test behavior/robustness without changing pass/fail outcome on this machine.

**Residual risks:**
- §6 item 1 (unreviewed third-party skill content exposure) is the primary one, restated here rather
  than only in the red-team section per this project's own documentation discipline: a host that
  wires every discovered location without inspection inherits whatever is on that machine's other
  AI-tool skill libraries, name+description unconditionally advertised the moment `SkillsProvider`
  resolves.
- The known-roots table (§6 item 5) requires manual maintenance as tool conventions evolve; a stale
  entry fails safe (nothing found) rather than unsafe.
