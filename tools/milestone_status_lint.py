#!/usr/bin/env python3
"""Milestone-status drift lint (ADR-026, docs/planning/2026-08-10-full-codebase-adr-gap-audit.md
gap 23). Same failure mode as 027's naming-lint (tools/naming_lint.py) applied to a different
artifact: CLAUDE.md, README.md, and the marketing site's protocol-status copy all independently
claimed "Milestone(s) N have not started" for a milestone that had already landed real, committed
code, and none of the three caught the drift until a full-codebase audit found it by hand. This
script is CLAUDE.md's own existing anti-staleness rule ("Do not hardcode a milestone/phase table
here — it goes stale on the next commit") turned into an enforced CI check instead of prose.

What it checks: for each SENTENCE (not each file, not each paragraph) in a scanned file that
contains the phrase "have not started", extract every milestone number that sentence names, and
fail if any of those numbers has a real, landed `docs/planning/milestone-N-*-breakdown.md` (a file
containing at least one "**Outcome" marker, this project's own convention for a phase that actually
shipped code — see e.g. milestone-7-protocol-conformance-breakdown.md's `**Outcome, C1 (...)`
lines).

Sentence-scoped, not paragraph-scoped, on purpose: the corrected prose this lint is meant to
protect reads "Milestone 7 ... is in progress: ... not yet met. Milestones 8-9 have not started."
in the same paragraph. A paragraph- or file-scoped match would associate "Milestone 7" with the
LATER, unrelated "have not started" clause belonging to 8-9, and — because milestone-7's own
breakdown doc genuinely has real Outcome markers — falsely fail on the very sentence that just
fixed the drift. This is not a hypothetical: it was found and named as a real defect in this
script's first proposed version during design review, before it was ever run. The fix is to
sentence-split first (on ". ", after flattening all whitespace/newlines to single spaces so a
blockquote's or JSX text node's line-wrapping never bleeds into it) and only search for milestone
numbers WITHIN the same sentence that contains "have not started" — never across sentences.

Like naming_lint.py, this is deliberately not a full parser: line-oriented would-be false
negatives (a rewording that doesn't match the literal phrase "have not started") are the accepted
failure mode, not false positives — see the file-top comment of naming_lint.py for the same
argument applied to C++ declarations instead of prose.

Exit code 0 if nothing flagged, 1 otherwise.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PLANNING_DIR = REPO_ROOT / "docs" / "planning"

# Every location this project has, twice now, independently let this exact claim go stale in.
# Add a new location here the moment prose describing milestone status appears somewhere else —
# that is precisely the class of gap this lint exists to prevent from recurring a third time.
SCANNED_FILES = [
    REPO_ROOT / "CLAUDE.md",
    REPO_ROOT / "README.md",
    REPO_ROOT / "web" / "marketing" / "src" / "components" / "ApiProtocolStatus.tsx",
]

_HAVE_NOT_STARTED = re.compile(r"have not started", re.IGNORECASE)
_MILESTONE_NUMBER = re.compile(r"[Mm]ilestones?\s+(\d+)(?:\s*[-–—]\s*(\d+))?")


def _sentences(text: str) -> list[str]:
    """Flattens all whitespace (so line-wrapped blockquotes/JSX text nodes don't fragment a
    sentence) then splits on ". " — a period followed by whitespace. Deliberately simple: see this
    file's own top comment for why paragraph- or file-level scoping is actively wrong here, not
    merely less precise."""
    flat = re.sub(r"\s+", " ", text).strip()
    return [s.strip() for s in re.split(r"(?<=\.)\s+", flat) if s.strip()]


def _milestone_numbers_in(sentence: str) -> set[int]:
    numbers: set[int] = set()
    for m in _MILESTONE_NUMBER.finditer(sentence):
        lo = int(m.group(1))
        hi = int(m.group(2)) if m.group(2) else lo
        numbers.update(range(min(lo, hi), max(lo, hi) + 1))
    return numbers


def _breakdown_has_landed_work(milestone_n: int) -> tuple[bool, Path | None, str | None]:
    for path in sorted(PLANNING_DIR.glob(f"milestone-{milestone_n}-*-breakdown.md")):
        text = path.read_text(encoding="utf-8")
        idx = text.find("**Outcome")
        if idx != -1:
            first_outcome_line = text[idx : text.find("\n", idx)].strip()
            return True, path, first_outcome_line
    return False, None, None


def main() -> int:
    failures: list[str] = []

    for path in SCANNED_FILES:
        if not path.exists():
            continue
        for sentence in _sentences(path.read_text(encoding="utf-8")):
            if not _HAVE_NOT_STARTED.search(sentence):
                continue
            for n in sorted(_milestone_numbers_in(sentence)):
                landed, breakdown_path, outcome_line = _breakdown_has_landed_work(n)
                if landed:
                    rel = path.relative_to(REPO_ROOT)
                    rel_breakdown = breakdown_path.relative_to(REPO_ROOT)
                    failures.append(
                        f"{rel}: claims 'have not started' for Milestone {n}, but "
                        f"{rel_breakdown} has real landed work:\n"
                        f"    {outcome_line}\n"
                        f"    stale sentence: \"{sentence}\""
                    )

    if failures:
        print(f"milestone-status-lint: {len(failures)} stale 'have not started' claim(s):\n")
        for f in failures:
            print(f"  {f}\n")
        print(
            "Update the claim to reflect real, landed work (see docs/planning/"
            "v1-implementation-roadmap.md and the relevant milestone-N-*-breakdown.md for current "
            "status), or add a new breakdown-file naming convention exception here if this is a "
            "genuine false positive — never silence by deleting the claim without correcting it."
        )
        return 1

    print("milestone-status-lint: OK — no scanned file claims 'have not started' for a milestone "
          "that already has landed work.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
