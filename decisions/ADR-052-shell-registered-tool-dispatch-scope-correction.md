# ADR-052 — Gap 13's "shell tool-pipeline bypass" is a stub's stated scope, not a live bug

**Status:** Proposed (2026-08-14). Re-grounded against current code, corrected, and documented at
the exact call site; awaiting the project owner's explicit "Judged" sign-off per this project's
governance (`decisions/README.md`; `OpenQuestions.md` OQ-11).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #13 (the finding this
ADR closes). `decisions/ADR-001-shellrunner-grammar-and-dispatch.md` (Judged — Design A, the design
this finding actually concerns). `decisions/ADR-040-fs-quota-capability-gate-fix.md` (this session's
earlier finding that `mediated_shell_dispatch.cpp`, not `shell_dispatch.cpp`, is the real, live
native-jail shell path — reused here, not re-derived).

## 1. The question

**Stated so it has a wrong answer:** does this codebase have a shell command dispatcher that
invokes a registered `Tool` while bypassing 006 §3's real capability/approval/provenance pipeline,
reachable from real agent execution?

**Before re-grounding: the audit's own framing implied yes** — its recommended approach ("route
shell tool calls through `bridge_tool_call` like Python's bridge already does — but fix a null-
`tool_bridge` crash, stop collapsing capability/approval denials into continuable errors, and give
`call_index` a real source") reads as fixing an existing, reachable integration.

**After re-grounding: the bypass is real CODE, but it has never been reachable, and the type
carrying it says so itself.** `command_registry.hpp`'s own `RegisteredTool` struct comment (present
before this ADR, not added by it) already states plainly: "the minimum type-erased shape needed to
make `CommandRegistry`'s three-way lookup real and testable (Sh-C2), **not an implementation of
006's ten-step pipeline**." `shell_dispatch.cpp`'s `dispatch_command()` does call
`resolved.tool->invoke(argv, ctx)` directly, with none of 006 §3's gates — but this is `shell_
dispatch.cpp`, the file this session's own earlier ADR-040 already confirmed by name is "the
untouched `shell_dispatch.cpp` spike," distinct from `mediated_shell_dispatch.cpp` (the real,
Judged, actually-used native-jail shell path). Confirmed directly, again, for this ADR: grepping the
whole tree for `shell_runner.hpp` (the header exposing this dispatch path) finds exactly two
includers, both test files (`test_shell_runner_proof.cpp`, `test_native_jail_runner_stubs.cpp`) —
zero production call sites. `mediated_shell_dispatch.cpp`, separately confirmed, has ZERO references
to "tool" of any kind anywhere in it — the real, live shell path doesn't dispatch registered Tools
by name AT ALL today, so there is no bypass to close there because there is no dispatch to bypass.

## 2. What re-grounding against current code found

- **`ADR-001` (Judged, "Design A accepted") never claimed `RegisteredTool` was a real invocation
  mechanism.** Its own §11 item 3 names "argv-to-typed-Args mapping" as explicitly undesigned — the
  struct exists solely to make the name-resolution PRECEDENCE claim (Sh-C2: does a builtin shadow a
  same-named Runner/Tool correctly?) testable, using a closure stand-in. The audit's own recommended
  fix (`bridge_tool_call`, `call_index`, capability/approval semantics) describes requirements for a
  REAL tool-invocation integration that has never existed here — building one is new feature work,
  not a bug fix, and out of this gap's own stated scope ("bypass," not "missing feature").
- **The audit's other named symptoms (a null-`tool_bridge` crash, denials collapsed into
  continuable errors, no real `call_index` source) do not exist anywhere in `shell_dispatch.cpp`
  either** — confirmed directly: zero occurrences of `tool_bridge`, `call_index`, or any null-guard
  pattern in that file. These concerns apply to `bridge_tool_call()`'s own real call site
  (`mediated_python_runner.cpp`) or to a hypothetical future shell integration built to route through
  it — not to any code that exists today.

## 3. The fix

No functional code changes — there is no live bypass to close. Two real, cheap, valuable additions:
a warning comment at the EXACT `resolved.tool->invoke(argv, ctx)` call site in `shell_dispatch.cpp`
(the reachable-in-principle line a future implementer would extend), and a matching one on
`RegisteredTool`'s own struct definition (`command_registry.hpp`) — both stating explicitly that a
real future Tool-dispatch integration, in EITHER the spike or the mediated production path, must
route through `invoke_tool()` (`core/tool_pipeline.hpp`) the same way `bridge_tool_call()` already
does for Python, never call a `RegisteredTool`'s bare closure directly. This closes the real risk
gap-13 protects against — a future maintainer copying this exact anti-pattern when eventually wiring
real shell-tool dispatch — without inventing unscoped feature work this gap never actually asked for.

## 4. Self-red-team findings

**Checked, not assumed: `mediated_shell_dispatch.cpp` genuinely has no tool-dispatch code path at
all**, rather than one that happens to already route correctly. A plain string search for "tool"
(case-insensitive) across the entire file returns zero hits — confirmed this isn't a case of "already
fixed, audit stale" (this session's own recurring pattern elsewhere), but a genuine "doesn't exist
yet" gap, distinct from "exists and bypasses."

**Considered and rejected: deleting `shell_dispatch.cpp` as genuinely dead code.** It is unreachable
from production, but it is the real, Judged (ADR-001) proof of Design A's own grammar/precedence
claims (Sh-C1/Sh-C2/Sh-C3/A-C2), backed by a real, still-passing test file
(`test_shell_runner_proof.cpp`). Removing it would delete a real, valuable regression proof to "fix"
a bug with zero live blast radius — a worse trade than documenting the hazard at its exact source.

## 5. What this ADR does not claim

- **Does not build real Tool dispatch for the mediated (production) shell path** — that is
  unscoped, undesigned future work this gap never asked for; named, not silently implied.
- **Does not touch `ADR-001`'s own Judged status or Design A's accepted scope** — `RegisteredTool`'s
  test-stub nature was already correctly scoped by that ADR; this ADR only makes the constraint
  impossible to miss at the two places a future implementer would look.
- **Does not claim the audit's own recommended symptoms (null-crash, collapsed denials, missing
  `call_index`) are fixed** — they were never real in the first place; named as inapplicable, not
  silently dropped.

## 6. Evidence

Grep-verified, not merely reasoned about: `shell_runner.hpp` has exactly two includers, both test
files; `mediated_shell_dispatch.cpp` has zero "tool"-related tokens anywhere. `test_shell_runner_
proof.cpp` (Design A's own real, existing regression proof) re-verified passing unchanged after the
comment-only edits. Full suite: green (`ctest`, this pass), zero regressions.
