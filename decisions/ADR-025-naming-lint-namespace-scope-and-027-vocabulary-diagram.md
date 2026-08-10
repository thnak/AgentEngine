# ADR-025 — 027 naming-lint scope was namespace-blind to trust/sandbox/workflow

**Status:** Judged — 2026-08-10. Scope note: this ADR covers only the scanner fix and the 027 §6
namespace-diagram correction (making G1 actually see the whole public core). It deliberately does
**not** attempt the bulk reconciliation of the corrected, now-larger violation/suppression set
against 027 §2-4's canonical-name tables — that is real, separately-scoped follow-up work, named in
§5 below, not silently folded in here.
**Relates to:** 027-Vocabulary-and-Naming.md §6 (namespaces), §7 (terminology debt), §9 (G1/G3
promotion gates); `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap 8 (the audit run
that surfaced this); I2 (no ambient authority) and I4 (every effect is attributable) — the modules
this gate could never actually check are exactly `trust/` and `sandbox/`, the ones those two
invariants bear on most directly.

## 1. The question

009 §8/027 §9's G1 gate claims: "a lint over public headers verifies every exported type appears in
this RFC's tables; an unlisted public name fails CI." **Stated so it has a wrong answer:** does
`tools/naming_lint.py` actually check every exported name in `agentengine`'s public core, or only a
subset the scanner happens to be able to see?

## 2. Finding — the gate was structurally blind to its own highest-risk modules

`find_declarations()`'s scope check compared the current namespace string against the exact literal
`"agentengine"`. A C++17 nested-namespace declaration — `namespace agentengine::trust { ... }` —
produces the namespace string `"agentengine::trust"`, which never equals `"agentengine"`. Every
top-level declaration inside such a block was silently skipped: not reported as a violation, not
counted as suppressed, invisible to both the tool's output and a human reading its exit code.

Independently re-verified against the current tree (not assumed from the audit that first surfaced
it):

```
grep -rln "^namespace agentengine::trust"     include/agentengine/   → 7 files
grep -rln "^namespace agentengine::sandbox"   include/agentengine/   → 4 files
grep -rln "^namespace agentengine::workflow"  include/agentengine/   → 9 files
grep -rln "^namespace agentengine::(schema|json|yaml|response_format_codec)"
                                               include/agentengine/   → 4 files
```

24 headers, ~70+ top-level declarations, entirely unaudited. This included 027 §4's own flagship
worked examples (`Executor`, `Edge`, `Workflow`, `EdgeFailurePolicy` — declared inside
`agentengine::workflow` in `graph.hpp`) and the public surface of the capability-token/bearer-secret
mechanism (`CapabilityToken`, `SecretKey`, `Caveat`, `HmacSha256` — `trust/capability_token.hpp`,
`trust/hmac.hpp`). `.github/workflows/ci.yml`'s `naming-lint` job (line 168-179) runs this script
with no `continue-on-error` — a real, required CI gate — but one that had never once evaluated the
modules I2/I4 most directly govern.

027 §6's own namespace diagram was part of the same blind spot: it listed `agentengine`, `::detail`,
`::mcp`/`::a2a`/`::agui`, and `::pal`, omitting `::trust`, `::sandbox`, `::workflow`, `::schema`,
`::json`, `::yaml`, `::response_format_codec`, `::openai`, and `::anthropic` even though all nine
already existed in the tree — the scanner appears to have been written against this same incomplete
picture, not against the real namespace layout.

## 3. Why this needed an ADR rather than a silent fix

The mechanical fix (widen the scope check) is small. What made it non-mechanical: fixing the scanner
*before* any bulk vocabulary-table reconciliation is a sequencing decision with a real failure mode
if skipped — reconciling today's *visible* 188 names first, then fixing the scanner, would let 027 §7
read "G3 satisfied" and G1 read as "genuinely enforced" while an entire, comparably-sized swath of
exported names in `trust`/`sandbox`/`workflow` remained structurally invisible to the gate, with no
CI failure and no suppression comment signaling anything was missing — a *more* misleading state than
today's honestly-failing CI. This is the same "loud failure traded for a silent one" pattern the
audit's own executive summary named as its most common finding. Fixing the scanner's scope first,
proving it against real before/after numbers, and explicitly deferring the reconciliation is the
sequencing this ADR records and commits to.

## 4. The decision

**4a. Scanner fix** (`tools/naming_lint.py`): a declaration is in-scope when its enclosing namespace
is exactly `agentengine`, OR starts with `agentengine::` and is not `agentengine::detail` (or
anything nested under it). `agentengine::mcp`/`::a2a`/`::agui`/`::openai`/`::anthropic` need no
special case here — those live under `include/agentengine/protocol/`, already excluded at the
directory level (`EXCLUDED_DIRS`) before `find_declarations()` ever runs on them, so this namespace
widening cannot accidentally pull wire-protocol types into the core vocabulary check.

**4b. 027 §6 diagram correction**: the namespace table now lists `::trust`/`::sandbox`/`::workflow`
and `::schema`/`::json`/`::yaml`/`::response_format_codec` as public-core module namespaces governed
by the same §2-4 rules as bare `agentengine`, and adds `::openai`/`::anthropic` to the
already-exempt wire-protocol row alongside `::mcp`/`::a2a`/`::agui`.

**4c. Explicitly deferred, not attempted here**: reconciling the corrected violation/suppression set
against 027 §2-4 (bucket-triage into real table rows vs. issue-referenced debt-table rows vs.
genuine renames), hardening the suppression-comment-vs-§7-debt-table cross-check, and any change to
027 §8's rules (a lowercase-trait-naming sub-rule was proposed during design and red-teamed as
silently reversing an existing recorded decision in `workflow/graph.hpp`'s `message_type` — that is
its own contested design question, not bookkeeping, and needs its own review). See §5.

## 5. Falsifiable claims and verdicts

| # | Claim | Evidence | Verdict |
|---|---|---|---|
| 1 | Before this fix, the scanner was silently blind to `trust`/`sandbox`/`workflow`/`schema`/`json`/`yaml`/`response_format_codec` | `git stash` the fix, ran `tools/naming_lint.py` in place against the unmodified tree: exit 1, `112 suppressed finding(s)`, `76 exported name(s) not in 027's vocabulary tables` — the same 76+112=188 figures independently cited by the audit | **CORRECT** |
| 2 | After the fix, previously-invisible names in exactly those modules are now surfaced | Ran the fixed script against the same tree: exit 1, `127 suppressed finding(s)`, `136 exported name(s) not in 027's vocabulary tables` (263 combined, up from 188). Spot-checked output directly contains `trust\capability_token.hpp:62: CapabilityToken`, `trust\hmac.hpp:22: HmacSha256`, `workflow\graph.hpp:121: EdgeFailurePolicy` — none of which appeared in claim 1's output | **CORRECT** |
| 3 | The fix does not pull wire-protocol (`mcp`/`a2a`/`agui`/`openai`/`anthropic`) types into scope | `EXCLUDED_DIRS` directory check runs in `iter_headers()` before any file under `include/agentengine/protocol/` reaches `find_declarations()` — confirmed by reading the call order; the namespace-scope change only affects files that already pass the directory filter | **CORRECT** |
| 4 | The gate is still a real, required CI check (not weakened) | `.github/workflows/ci.yml:168-179`'s `naming-lint` job runs `python tools/naming_lint.py` with no `continue-on-error`; both before and after this fix, that command exits 1 against the current tree — the gate still fails, honestly, on real (now more complete) violations | **CORRECT** |

## 6. Residuals — named, not silently deferred

- **The bulk reconciliation itself** (~263 combined names): triage into (a) settled names needing a
  mechanical row in 027 §2-4, (b) genuinely open naming questions needing an issue-referenced §7
  debt-table row, (c) real renames. This is the large, error-prone part of gap 8 the audit's judge
  explicitly sequenced *after* this fix — tracked as its own follow-up, not started here.
- **Suppression-vs-§7-debt-table cross-check hardening**: making "suppressed in code" and "listed as
  debt in 027 §7" the same fact mechanically, so the gap that let 112 (now 127) suppressions
  accumulate outside §7's tracking can't recur silently. Deferred until after the bulk reconciliation
  above, so it checks against the corrected, complete set rather than locking in a partial one.
- **027 §8's lowercase-trait-naming question**: whether policy-tag names like `message_type` are in
  scope for §2-4 rows is a real, live design question (one existing suppression comment already
  records the opposite decision) — out of scope for this ADR, needs its own review if pursued.
- **Gap 9** (027 §2's `UsageDetails` vs. code's `Usage` — a separate three-way RFC disagreement
  across 027/003/004) and **gap 23** (CLAUDE.md/README/marketing-site milestone-status claims) are
  related findings from the same audit but are independent decisions, each needing its own ADR —
  not addressed here.
