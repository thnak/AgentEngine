# Codebase quality audit — 2026-10-10

**Base score 100, final score 85/100** — for the **PR 169 integration branch**, which is what was
measured (see §5 for why that scope matters and what it means for `main`).

An evidence-based audit of the engine as it stood on branch `adr237-async-design` (HEAD `033525a`;
slice 7 of the ADR-237 §5.2 tool-seam migration landed *during* this audit and `rt/agent_spawn.hpp`
was being edited concurrently, so this is a snapshot of a moving tree, not a frozen release).

Every number below was measured on this machine, not estimated. Where a metric is a proxy rather
than ground truth, it says so.

> **Correction, same day.** An earlier revision of this file scored 83/100 and claimed "no TSan
> anywhere". That claim was wrong — TSan *is* used, with a positive control, and the deduction was
> reduced from −3 to −1 accordingly (§3, D2). The other deductions stand.

---

## 1. What the codebase is

| Measure | Value |
|---|---|
| Git-tracked files | 1,704 |
| Tracked C++ (`*.hpp`/`*.cpp`/`*.h`/`*.cc`) | 1,006 files, 264,774 lines |
| Public headers (`include/agentengine/`) | 249 |
| Production translation units (`src/`) | 124 |
| Test translation units (`tests/`) | 498 files, 138,041 lines |
| Test : source line ratio | 1.38 : 1 |
| RFC specs | 30 (`001`–`030`) |
| ADRs | 217 files, highest number 237 |
| Markdown | 471 files, 116,719 lines |
| Registered CTest tests (`dev` preset) | 415 |
| Commits, last 30 days / last 7 days | 243 / 49 |

Configuration measured: the `dev` preset, which has `WITH_HTTPS`, `WITH_WASM`, `BUILD_PYTHON_RUNNER`,
`WITH_PDF`, `WITH_VULKAN`, `WITH_NATIVE_PROCESS` and `BUILD_KATA_BACKEND` all OFF. Roughly 60
HTTPS-dependent tests (provider parsers, TLS, embedder, Qdrant) are therefore **not** in this audit's
test run; CI covers them on two legs.

---

## 2. Build and test health (measured)

**Build:** the tree is fully built and warning-free. `cmake --build build-dev -j4` reports
`ninja: no work to do` — and this build enforces `/W4 /WX` (MSVC) / `-Wall -Wextra -Werror` (GCC,
Clang) through `cmake/AgentEngineWarnings.cmake`, whose own policy is *no* `-Wno-*`/`/wd*` entry ever.
A clean `-Werror` build of 264k lines is a real, verified property.

**Test run (415 tests, full suite, `-j4`, 55 s wall):**

| Outcome | Count |
|---|---|
| Failed | 34 |
| Passed | 379 |
| Skipped | 2 |

All 34 failures were then classified by re-running each class under corrected conditions:

| Class | Count | Verdict |
|---|---|---|
| `compile_fail_*` failing with `C1083: Cannot open include file: 'algorithm'` | 20 | **Environmental.** `cl.exe` was invoked outside a VS developer shell (no `INCLUDE`). Re-run inside `vcvars64.bat`: **33/33 compile-fail tests pass, 100%.** |
| Docker-daemon tests (`test_docker_*`, `test_sandbox_runtime`, `test_mandatory_sandbox_provider`, `test_task_branch_*`, `test_live_shell_sandbox_provider_live`, …) | 13 | **Environmental.** Docker daemon is not running (`failed to connect to the docker API at npipe:////./pipe/dockerDesktopLinuxEngine`). CI excludes the *same 13 names by hand* on Windows legs. |
| `test_external_skill_discovery` D6 | 1 | **Genuine test-gating bug** (see §5, D1). |

**Net: 399 passed, 2 skipped, 0 genuine failures — 401/401 environment-independent tests green.**

---

## 3. Deduction ledger

| # | Finding | Evidence | Points |
|---|---|---|---|
| D1 | **Environment-dependent tests fail instead of skipping.** 14/415 (3.4%) are red on a stock dev box: 13 need a Docker daemon, and `test_external_skill_discovery` D6 gates on "`~/.claude/skills` exists" but asserts on its *contents* (symlinked skills), so it is red right now because that directory exists but holds no symlinked skills. CI's 13-name exclusion list is hand-maintained, and CI's own comment admits the same class of miss "has now happened twice … nothing checks it against `RESOURCE_LOCK docker_daemon`". | §2; `ci.yml:165-193`, `ci.yml:343-346`; `tests/core/skills/test_external_skill_discovery.cpp:203-216` | −5 |
| D2 | **TSan evidence is real but not reproducible from the repo's declared surfaces.** TSan *is* used seriously: `tools/test_driver/c2_tsan.sh` builds the test **twice** — normal, and a deliberately racy control (`AGENTENGINE_TEST_DRIVER_C2_RACE`) that *must* produce a report — which is the project's own "a test that cannot fail proves nothing" rule applied to a race detector. ADR-237 records per-step TSan runs (5/5, 3/3, "145 reports → 0"), including races TSan found **in the tests**. What is missing is the plumbing: there is **no TSan preset** in `CMakePresets.json` and **no TSan CI leg** (the only `tsan` string in `ci.yml` is a comment deferring the matrix). So the invocation is reconstructed by hand from a bash script, and nothing gates a TSan regression on a merge. | `tools/test_driver/c2_tsan.sh` (positive control); `ci.yml:12`; grep of `CMakePresets.json` → 0 hits | −1 |
| D3 | **No coverage measurement of any kind.** No `gcov`, `llvm-cov`, or `OpenCppCoverage` reference anywhere in CMake, presets, CI or docs; no coverage target and no coverage gate, across 249 public headers and 265k lines. For a project whose culture is "a gate, not trust", untested-line count is the one gate that does not exist. | grep for `coverage\|gcov\|llvm-cov` → 0 hits | −2 |
| D4 | **RFC-027 vocabulary gate is green by suppression, not by satisfaction.** `naming_lint.py` reports "OK — every exported type appears in 027's vocabulary tables", but prints *619 suppressed findings* alongside 681 inline `ae-naming-lint: allow` sites. Of those: 220 are one "ADR-025 §4c deferred bulk reconciliation", 68 + 17 are "ADR-237 new reactor/runtime vocabulary, 027 §4 row added when the ADR [lands]", 55 are "pre-existing M0 scaffolding, reconcile at owning milestone", ~40 more are "027 not yet updated". The lint is honest (it prints the count, and every deferral carries an ADR tag) — but RFC 027's tables are not current, so this gate currently enforces materially less than its OK implies. | `tools/naming_lint.py:44-49,172-184`; suppression sites grouped by reason | −2 |
| D5 | **Integration completeness.** 40/249 (16%) public headers are implemented and tested but have **no production consumer** anywhere in `include/`, `src/`, `tools/`, `examples/` or `samples/` — including the entire new reactor surface (`rt/runtime`, `rt/scope`, `rt/sleep`, `rt/process`, `rt/multi_agent`, `rt/http_tls`, `rt/effect_journal`, `rt/project_manifest`) and all of A2A (`client`, `server`, `streaming`), AG-UI (`projection`, `sse`), the OpenAI/Qdrant embedders and the PDF image/metadata/TOC tools. Separately, `conformance/` — the designated home of the I7 gate — contains only a `README.md`; so do `samples/` and `plugins/`. (There are **zero** truly orphan headers: every header is either consumed or directly included by a test.) | path-level `#include` scan across 249 headers; `conformance/`, `samples/`, `plugins/` listings | −2 |
| D6 | **Documentation drift at the front door.** `README.md`'s status block still reads "73 ADRs in `decisions/`" (actual: 217), and "Milestones 8-9 … have not started" while ADR-237 (Draft, 2026-10-03) drives 49 commits in the last 7 days. Separately, ADR numbering has 21 gaps in the range 0–237 with no in-band marker (the retirement policy is documented in `decisions/README.md`, but a reader cannot distinguish a retired number from an unwritten one without git archaeology). | `README.md:20-21`; ADR filename scan | −1 |
| D7 | **A second error idiom in the newest hot path.** CONVENTIONS.md: "No exceptions for control flow. Hot paths are `noexcept`." The `rt::` substrate throws at ~20 real sites — `AsyncMutex` lock refusal and `block_on` refusal (`lock_refused`/`block_on_refused`), `Runtime` shutdown and empty-task validation, and `task_scope` strand-contract violations. ADR-237 §4.3 backs these deliberately, but the codebase now signals failure two ways: 1,616 `result<>`-returning APIs plus 2,209 `std::unexpected` sites, and a coroutine path that throws. Two idioms is precisely what a reviewer cannot check mechanically. | real (non-comment) `throw` scan: 35 lines, ~20 in `include/agentengine/rt/` | −1 |
| D8 | **No formatting or static-analysis automation in the loop.** No `.clang-format`, `.clang-tidy`, `.editorconfig` or pre-commit config. 24 `NOLINT(...)` suppression sites exist in production code with no analyzer configured to honour or verify them. (Observed style consistency is nonetheless high — see §4.) | root-config probe | −1 |
| D9 | **CI automation gaps.** Worth recording even where it overlaps D1–D3: no TSan CI leg (see the corrected D2), no coverage job, the Docker exclusion list is hand-maintained and has regressed twice, and fuzzing is a single target (`mediated_shell_parser_fuzz`). | `.github/workflows/ci.yml` | (folded into D1–D3) |
| — | Observed but **not** scored: untracked `.obj`/`.pdb`/build-log litter in the repo root and an untracked `.commandcode/` absent from `.gitignore`; HEAD mid-migration with an edit in flight, so there is no frozen green reference point today. All are normal for active development. | `git status`, root listing | 0 |
| | **Total deducted** | | **−15** |

**100 − 15 = 85/100.**

D9 in the original ledger ("no TSan job") has been merged into the corrected D2.

---

## 4. What is genuinely strong (verified, not assumed)

1. **Warning-free `-Werror` build** of 264k lines across MSVC *and* clang-cl, with a documented,
   enforced no-suppression policy (`cmake/AgentEngineWarnings.cmake`).
2. **Test depth and falsifiability.** 415 tests spread across 22 labelled areas; 33 `compile_fail`
   **negative-compilation** tests, 100% passing; 32 security-invariant test files including
   explicitly-named positive controls (`capability_set_grant_root_positive_control.cpp`,
   `tainted_declassify_positive_control.cpp`,
   `native_capability_announcer_distinct_families_positive_control.cpp`) — the project actually
   applies its own rule that "a test that cannot fail proves nothing".
3. **Three bespoke static-analysis gates, all green,** and all self-testing:
   `tools/layering_lint.py` (15-check `--self-test` covering rule detection, suppression semantics,
   baseline ratchet and layer-map hygiene → `OK -- no new violations (8 baselined edges remain)`),
   `tools/naming_lint.py`, `tools/milestone_status_lint.py`, plus
   `cmake/check_testing_includes.cmake` (→ `ok`). The layering baseline is a **true shrinking
   ratchet**: a new violation fails, and a stale baseline entry also fails.
4. **Layering discipline with teeth.** Every `#include` in `include/` and `src/` is checked against a
   layer map. 89 `_WIN32` preprocessor sites exist and essentially all sit inside the sanctioned
   seams (`pal/`, `sandbox/`, `src/backends/`); the three outside them
   (`core/external_skill_discovery.hpp`, `trust/secure_random.hpp`, `tools/extract_pdf_text.hpp`)
   each carry an explicit written rationale, and the core one still routes the actual environment
   read through `pal::env_var`.
5. **Convention compliance is measurable and near-total.** `#pragma once` on **249/249** public
   headers. **Zero** RTTI use — the five `typeid` hits are all comments explaining the prohibition.
   No `using namespace std`; the 12 `using namespace` sites are all function-scoped
   `using namespace <module>_detail;`. No raw `new`/`delete` problem (the 35 flagged lines are the
   word "new" in prose). Error model is consistent: 1,616 `result<>`, 2,209 `std::unexpected`,
   1,042 `noexcept`.
6. **Hostile-test machine safety, as required.** Abuse corpora and resource-cap tests are real and
   present on both platforms: `test_native_jail_abuse_corpus_{windows,linux}.cpp`,
   `test_job_object_limits.cpp`, `test_docker_isolation.cpp`, `test_containerd_isolation.cpp`,
   `test_worktree_mount_fs_escape_corpus*.cpp`.
7. **Zero dead public headers, and honest labels for the unwired ones.** The 40 unwired headers are
   not hidden — the ADR record names this class explicitly (ADR-005's own residual: "both designs
   are real and tested, but neither has a production consumer yet").
8. **Documentation that admits what is missing.** `decisions/README.md` defines when an ADR is
   required, what it must contain (competing designs, a falsifiable claim per design, a red-team
   attack, executed evidence with positive controls, per-claim verdicts including `INCONCLUSIVE`),
   and a policy for *retiring* documents that turn out not to be decisions. The README discloses
   unmet gates (MCP auth 3/49) rather than claiming success. `OpenQuestions.md` tracks 1 open /
   25 resolved. Dated, cited research records exist under `docs/research/`.
9. **Git hygiene.** Comprehensive `.gitignore` (build dirs, artifacts, `*.pem`, `*.key`, `api-test.txt`,
   `deep-seek.txt`); **zero** build artifacts tracked; conventional, descriptive commit messages with
   no co-author trailer; 9 focused local branches.

---

## 5. Scope: this audit measured PR 169, not `main`

This matters, because the two are different codebases right now.

**PR #169 — "ADR-237: async extension points and I/O reactor (foundation; conversion in progress)"**
is an open **Draft**, head `adr237-async-design` → base `main`, **182 files, +23,352 / −1,213**. Its
own body states: *"`main` stays frozen until this merges (ADR-237 §9 D2: one integration branch,
merged once)."* Every measurement in this audit — the 415-test run, the header census, the lints —
was taken on that branch, because that is what the working tree is checked out to.

Three consequences:

1. **85/100 describes the integration branch.** `main` was not measured, and is frozen and older
   (it predates the whole `rt::` reactor surface). A separate run against `main` would produce a
   different number, most likely *higher* on completeness-of-what-exists and *lower* on test count.
2. **PR 169's own evidence independently corroborates this audit's test findings.** Its body records
   "Merged tree, Windows (clang): **405/418, failures are the 13 Docker-dependent sandbox tests (no
   Docker daemon on the test machine)**" — the same 13 tests, for the same reason, as D1 here — plus
   "Layering and naming lints clean", matching §4. Two independent measurements agreeing is the
   strongest evidence in this report.
3. **The unwired surface in D5 is a declared work-in-progress, not an accident.** PR 169 is explicit:
   *"Everything above is additive: existing callers still use the synchronous paths"*, followed by an
   unchecked list of the remaining conversion (the pipeline itself async, chat clients, context
   providers, memory stores, sandbox/shell providers, `BatchBackend`, the deciders, eval seams,
   MCP/A2A, ~130 test drivers, then delete `block_on`-at-edge / `drive_leaf_task` / `ThreadPool` /
   `multi_agent::parallel` / detached threads). The `rt::` headers with no production consumer in
   D5 are exactly that list. The deduction is still real — 16% of the public surface has no consumer
   *today* — but it is priced as in-flight work, not as design debt.

---

## 6. Documentation at risk when PR 169 merges

PR 169 changes ~180 files and touches `web/marketing/` itself (for the `Described<T,"...">` tool-field
convention and the RAG capability-ceiling fix). Anything that describes an **extension point's
signature or synchronicity** goes stale the moment the conversion lands. The concrete exposure:

| Surface | Why it goes stale | Risk |
|---|---|---|
| `web/marketing/api/tool.html`, `skill.html`, `codeact.html` | `Tool<>::invoke` becomes an instance `const task<result<Reply>>`; `make_tool_descriptor_with_invoke` / `make_sync_invoke` / the 15 static sentinels are deleted; `ToolTable::from` replaces the current authoring contract | **High** |
| `web/marketing/api/runtime.html`, `providers.html`, `streaming.html` | `chat()` → `get_response` / `get_streaming_response` (PR 169 "D4"); callers move onto `Runtime`/strands | **High** |
| `web/marketing/api/protocols.html`, `plugins.html` | MCP/A2A are on the conversion list; plugin/WASM host interfaces are a seam | Medium |
| `web/marketing/api/workflow*.html` (8 pages) | Workflow supervisor moves onto strands; `route_from` blame changes; ADR-160 parallel batches → `when_all` child strands | Medium |
| `web/marketing/api/trust-sandbox.html` | "D3: per-call capability requirement — parse once, bound set only, fail closed (`ctx.authorize(cap)`)" changes how a capability is checked | Medium |
| `web/marketing/api/events.html`, `hitl.html`, `durability.html` | New `failure_class::canceled`; canceled runs end `run.canceled`; A2A `TASK_STATE_CANCELED` | Medium |
| `rfcs/027-Vocabulary-and-Naming.md` | PR 169's own checklist: "apply the §10 spec changes and 027 vocabulary rows" | **High** |
| `README.md` | Already stale (§3 D6), and says nothing about the async programme | **High** |
| `docs/guides/*` | Any guide showing a synchronous tool/skill/provider implementation | Medium |
| This audit's D5/D7 rows | Both are resolved or reshaped by PR 169's completion | Low |

Two structural recommendations, both cheap:

- **Update the docs in the same PR that changes the contract**, since `main` is frozen until merge —
  a docs follow-up *after* merge would land on a moving `main` with no branch protection that
  requires it.
- **Generate, don't hand-maintain, the parts that drift**: `README.md`'s status block and the
  `027` vocabulary rows are both derivable from `decisions/README.md`, the roadmap and
  `tools/naming_lint.py`'s own output. Hand-maintaining them is why D6 exists.

The missing UI page is a separate gap and is not caused by PR 169 — see §8.

---

## 7. The five fixes that would move the number most

| Fix | Recovers | Effort |
|---|---|---|
| Make environment-dependent tests **skip** rather than fail: gate the Docker tests on a daemon probe, and gate `test_external_skill_discovery` D6 on *finding a symlinked skill* rather than on the directory existing. Then replace CI's hand-maintained 13-name exclusion list with a lint over `RESOURCE_LOCK docker_daemon` (CI's own comment asks for exactly this). | −5 → 0 | Low |
| Wire the **existing** TSan script into a `tsan` preset and one CI leg (the Linux job already exists as the natural home — `tools/test_driver/c2_tsan.sh` is the positive control, so this is plumbing, not new work). | −1 → 0 | Low |
| Add a **coverage** target and publish the number (llvm-cov on the clang-cl leg is the cheapest path). | −2 → 0 | Medium |
| Reconcile RFC 027's vocabulary tables against the 619 suppressed naming findings — starting with the 220-entry ADR-025 §4c block — so the gate's OK means what it says. | −2 → 0 | Medium |
| Refresh `README.md`'s status block (and generate it from `decisions/README.md` + the roadmap instead of hand-maintaining it). | −1 → 0 | Low |

Fixing all five: **96/100**. The remaining ~4 points are structural (16% unwired surface, an
in-progress migration, the new `rt::` exception idiom) and are the expected cost of a codebase that
is honestly mid-flight rather than finished.

---

## 8. Open gap: the UI docs have no "how AgentEngine works" page

`web/marketing/` ships two distinct surfaces, and the conceptual explanation only exists on one of them.

**What exists (verified by reading the composed trees):**

- The **landing page** (`src/App.tsx` → `index.html`) *does* explain the shape of the system:
  `Hero` → `Pillars` → `Invariants` (the eight core invariants) → `Architecture` (five layers, one
  dependency direction, L1 as sole OS-capability holder) → `SpecDriven` → `GettingStarted`.
- The **API docs** (`api.html` + 26 `api/*.html` pages, `src/ApiApp.tsx` hub) are a pure **reference**:
  a hero, then a card grid — *"Each one is its own page — full field-by-field detail, not a summary
  card"* — and each child page is a three-rail field-by-field reference (`api/agent.html`,
  `api/tool.html`, `api/runtime.html`, `api/trust-sandbox.html`, `api/workflow-*.html`, …).

**What is missing:** there is no page in the docs section that explains how the engine *works* as a
system. Specifically, nothing covers:

1. **The request lifecycle** — session → run → turn → model call → tool call → effect → ledger
   commit / checkpoint, and where streaming, HITL suspension and cancellation enter that path.
2. **How authority flows through one call** — capability set → `EffectContext` → per-call
   authorization → sandbox → effect, i.e. I2/I3/I4 read as a *flow* rather than as type references.
3. **Where the invariants are enforced** — each of the eight mapped to the mechanism and the test
   that gates it, rather than listed as prose on the landing page.
4. **Who owns what** — engine vs. host: what a consumer must implement (provider, sandbox backend,
   store, decider) versus what the engine owns (runtime, session, turn loop, dispatch, durability).
5. **The mental model of a `run`** — one session/one executor (I1), strands and in-flight calls, and
   after PR 169, what "asynchronous extension point" means for an implementer.

**Why it matters for the score, not just for readers:** §4 credits this project with 217 ADRs, 30
RFCs and 117k lines of documentation, and §3 D6 charges it for front-door drift. A reference that
documents 26 type families field-by-field, with no companion page explaining how they compose, is the
same failure mode as the README status block: depth everywhere, orientation nowhere. It is a
**documentation-completeness** gap, so it is recorded here rather than as a new deduction — the
traceability dimension already carries D6.

**Fit and cost, before anyone starts:** the site is bilingual (`en`/`vi` — every component follows
the `const copy = { en: {...}, vi: {...} }` pattern, and nav comes from `apiPages[lang]`). A new page
therefore means `src/data/apiContent.ts` (nav entry + group), a new `api/<id>.html` and its
`src/<id>-main.tsx`, a page component following `ApiDetailLayout`, and a `vite.config.ts` multi-page
input — roughly six files, all of them also touched by PR 169, which is a merge-conflict argument for
either landing it *inside* PR 169 or waiting until `main` is unfrozen.

---

## 9. Bottom line

| View | Score |
|---|---|
| **Overall (100 base, all dimensions), PR 169 branch** | **85 / 100** |
| Engineering quality of what is built, tested and documented | ~93 / 100 |
| Portfolio completeness / integration of what is specified | ~74 / 100 |

The gap between those two rows is the whole story: this is an unusually disciplined codebase —
`-Werror` clean, self-testing custom gates, a shrinking layering ratchet, positive controls on
security claims (including on the race detector itself), ADRs that publish their own residuals —
whose score is held down not by bad code but by **unmeasured and unskippable edges**: no coverage
number, no TSan *gate* despite real TSan evidence, tests that go red on a clean machine, a vocabulary
gate that passes by deferral, and documentation with depth everywhere but no orientation page.

Four of those five are plumbing — wiring up things that already exist. None requires rethinking the
architecture.

