# ADR-121 — Real Linux verification of ADR-116: the `ComposedContextProvider` cross-session identity fix holds on GCC 14.2.0, ABA scenario reproduced for real

- **Status:** Proposed — real verification pass, no production code changed. The pre-existing
  `/root/ae-verify` WSL2 Ubuntu 24.04 checkout (ADR-115, reused by ADR-118/120) fast-forwarded to this
  branch's exact HEAD (`35fe8dd`, the ADR-120 commit) and rebuilt/retested with the same real GCC
  14.2.0 toolchain.
- **Date:** 2026-08-30.
- **Scope:** No files changed. This ADR records a verification run, not a code change.
- **Related specs:** Closes ADR-116's own disclosed "not yet Linux-verified" gap (never previously
  closed by any ADR — ADR-118/120 Linux-verified ADR-117/119, but ADR-116 landed on Windows/MSVC only
  and was never independently re-checked on Linux until now). Reuses ADR-118/120's own environment and
  methodology directly.

## 1. The question

ADR-116 fixed `ComposedContextProvider<Ms...>`'s cross-session move-assignment aliasing bypass with an
owning-session identity tag, and its own same-day independent red-team round (ADR-116 §7) found and
fixed two further real bugs: a tag-laundering bypass through an untagged intermediate variable, and a
genuine ABA hole from tagging with `AgentSession`'s own raw address (empirically reproduced on MSVC's
CRT allocator — heap addresses get reused after destruction). All of this landed and was verified on
Windows/MSVC only. Does the fix — including the ABA-hardened, monotonic-counter-based identity tag —
actually hold on a real, independently-obtained GCC 14.2.0 toolchain, and does the ABA scenario the
fix specifically guards against actually reproduce on a DIFFERENT allocator (glibc's, not MSVC's CRT)?

## 2. What was done

Reused ADR-120's own `/root/ae-verify` checkout (confirmed clean, sitting at `f2c66e6`) and
fast-forwarded it (`git fetch` + `git merge --ff-only`) to `35fe8dd` — a documentation-only fast-forward
at the code level (ADR-116's own code had already been fast-forwarded into this checkout during the
ADR-118 pass, which jumped from `bfdfe6e` all the way past both ADR-116 and ADR-117 in one fetch; this
pass is the first to actually exercise `test_session_builder`'s ADR-116-specific checks on Linux).

`test_session_builder` is gated behind `AGENTENGINE_WITH_HTTPS` (it links `agentengine::provider_
http_client`, transitively required by `quickstart::ComposedQuickstartSessionBuilder`) — reconfigured
with `-DAGENTENGINE_WITH_HTTPS=ON` (the same toggle ADR-120 used to reach `containerd_shell_chat.cpp`)
and built `tests/test_session_builder` directly: compiles clean (one benign GCC `-Wself-move` warning
on line 808, which is B21a's OWN deliberate `hp = std::move(hp)` self-assignment regression test —
expected, not a defect; MSVC has no equivalent warning for this pattern, which is why this is the first
time it was ever seen).

**Ran `test_session_builder` directly: `ALL PASS`**, including every check the ADR-116 line of work
added or depends on:
- **B20** — the original cross-session move-assignment refusal: both sessions' own content stays
  completely untouched.
- **B23** — the two-hop bypass through an untagged intermediate local variable (the red-team's first
  MUST-FIX): closed.
- **B24** — the three-hop bypass through an `operator=`-populated relay variable (proving `operator=`'s
  own propagation fix is independently necessary, not redundant with the move constructor's): closed.
- **B25** — THE ABA REGRESSION, reproduced for real on a SECOND, independent allocator: this check
  heap-allocates a session, extracts its tagged provider, destroys the session, heap-allocates an
  unrelated new session, and asserts the two share the identical address (`AsyncQuota`'s glibc-backed
  `new`/`delete` pattern) BEFORE checking that the new session does not inherit the old one's content.
  **The ABA precondition held on Linux/glibc, exactly as it held on Windows/MSVC** — confirmed by the
  test's own `ok:` line, not merely assumed to generalize from the original Windows-only finding. The
  monotonic-counter identity tag (not a raw address) correctly prevents the leak on this second,
  independently-obtained allocator too.
- **B21a/b/c/d, B22** — every other pre-existing regression check in this file (self-assignment,
  move-construction chains, double-build, the engage()-retry-after-failure fix) — all pass unchanged.

Reverted `AGENTENGINE_WITH_HTTPS` to `OFF` afterward for an apples-to-apples `ctest` comparison against
ADR-118/120's own baseline: **182/182 total, identical 5 failures** — `test_composed_sandbox_providers_
live`, `test_sandbox_runtime`, `test_docker_orphan_reap`, `test_mandatory_sandbox_provider`,
`test_task_branch_tools` — the same, already-diagnosed, pre-existing Docker-CLI-reachability gap
ADR-115/118/120 already named, unrelated to `ComposedContextProvider` entirely. Zero regression from
anything in this pass.

## 3. What this closes, and what it does not

**Closes for real, unconditionally**: ADR-116's own Linux-verification gap, for both the original fix
and both of its same-day red-team follow-on fixes. The ABA scenario specifically — the part most likely
to be allocator-dependent and therefore the least safe to assume "probably also true on Linux" — was
independently reproduced and confirmed closed on glibc's allocator, not merely inferred from the MSVC
CRT finding.

**Does NOT close**: nothing new — `test_session_builder` has no Docker/containerd dependency at all, so
this is a complete, unconditional closure with no environment-caused residual of its own.

## 4. What was NOT done

- No new test was written — this pass ran the existing, already-comprehensive `test_session_builder`
  suite as-is; ADR-116's own test additions (B20/B23/B24/B25) were judged sufficient on inspection and
  needed no Linux-specific supplement.
- `AGENTENGINE_WITH_HTTPS` was toggled ON only long enough to build and run this one test target, then
  reverted to `OFF` for the `ctest` baseline comparison, matching ADR-120's own established pattern for
  this exact class of HTTPS-gated-but-not-network-dependent test.

## 5. Residuals

- None specific to `ComposedContextProvider`/ADR-116 — this pass found nothing new to disclose.
- The same pre-existing, disclosed Docker-CLI-reachability gap (ADR-115/118/120) remains unrelated and
  unchanged.
