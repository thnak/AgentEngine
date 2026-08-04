#!/usr/bin/env python3
"""027 naming-lint stub (docs/planning/milestone-0-bootstrap-breakdown.md task 3).

Walks include/agentengine/ and flags every exported (public, top-level-in-`agentengine`) type,
enum, using-alias, or concept name that does not appear in 027-Vocabulary-and-Naming.md's §2-4
canonical-name tables. This is 027 G1 in miniature: "a lint over public headers verifies every
exported type appears in this RFC's tables; an unlisted public name fails CI."

Deliberately NOT a full C++ frontend (027's own promotion gate doesn't demand one; the M0 breakdown
names "deciding how strictly to parse C++ symbol names without a full compiler frontend" as the
actual work here). This is a line-oriented brace-depth tracker: it knows enough to find declarations
sitting directly inside `namespace agentengine { ... }` and to skip anything nested one level
deeper (a nested namespace like `agentengine::detail`, or a struct/enum member) or shallower. It
will misparse code that departs from this project's prevailing header style (one file, one
`namespace agentengine { }` block, declarations not split across dense one-line blocks) rather than
silently approving something it can't actually see — false negatives are the failure mode to watch
for on review, not false positives, since a name it fails to see simply never gets checked.

Scope: include/agentengine/{core,trust,sandbox,plugin,workflow}/**/*.hpp. Excluded:
  - include/agentengine/detail/  — private internals, not user-facing (CONVENTIONS.md layout table).
  - include/agentengine/protocol/**  — wire types live in agentengine::mcp / ::a2a / ::agui / ::openai,
    not bare agentengine::: they are exempt from the *core* vocabulary tables by 027 §6's namespace
    boundary rule, not by this script's own judgment call.

Suppression: a declaration whose line (or the line immediately above it) contains
`ae-naming-lint: allow <Name>` is skipped from failure but still reported as suppressed, so a
deferral stays grep-able instead of silent (CLAUDE.md's "explicit gaps not silent" pattern, e.g.
021 §7's own open questions).

Exit code 0 if nothing unsuppressed is flagged, 1 otherwise.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
VOCAB_RFC = REPO_ROOT / "027-Vocabulary-and-Naming.md"
SCAN_ROOT = REPO_ROOT / "include" / "agentengine"
EXCLUDED_DIRS = {
    REPO_ROOT / "include" / "agentengine" / "detail",
    REPO_ROOT / "include" / "agentengine" / "protocol",
}

# Matches a canonical-name table row's first cell, e.g. "| `Agent` |", "| **`Run`** |",
# "| **`Handoff`** | `Handoff<Writer>` exposes ... |". Bold/backtick wrapping is stripped; only the
# bare identifier before any `<...>`/`::...`/`/` is kept as the checkable name.
_TABLE_ROW = re.compile(r"^\|\s*\*{0,2}`([A-Za-z_][A-Za-z0-9_]*)")

_DECL = re.compile(
    r"^\s*(?:template\s*<[^>]*>\s*)?"
    r"(?:struct|class)\s+([A-Za-z_]\w*)\s*(?:[:{;]|$)"
    r"|^\s*enum\s+(?:class\s+)?([A-Za-z_]\w*)\s*(?:[:{;]|$)"
    r"|^\s*(?:template\s*<[^>]*>\s*)?using\s+([A-Za-z_]\w*)\s*="
    r"|^\s*(?:template\s*<[^>]*>\s*)?concept\s+([A-Za-z_]\w*)\s*="
)

_NAMESPACE_OPEN = re.compile(r"^\s*namespace\s+([\w:]+)\s*\{")
_ALLOW = re.compile(r"ae-naming-lint:\s*allow\s+([A-Za-z_]\w*)")


def load_vocabulary() -> set[str]:
    text = VOCAB_RFC.read_text(encoding="utf-8")
    lines = text.splitlines()
    # Only §2-4 are canonical-name tables; §5 (collision register) and later use a different shape
    # and are not "the tables" G1 means (027 §1-4 vs §5's differently-structured word list).
    start = next(i for i, ln in enumerate(lines) if ln.startswith("## 2. Canonical names"))
    end = next(i for i, ln in enumerate(lines) if ln.startswith("## 5. The collision register"))
    names: set[str] = set()
    for ln in lines[start:end]:
        m = _TABLE_ROW.match(ln)
        if m:
            names.add(m.group(1))
    return names


def strip_comment(line: str) -> str:
    idx = line.find("//")
    return line if idx == -1 else line[:idx]


def find_declarations(path: Path) -> list[tuple[int, str, bool]]:
    """Returns (line_no, name, suppressed) for every top-level-in-`agentengine` declaration."""
    depth = 0
    ns_stack: list[tuple[int, str]] = []  # (depth after opening, namespace name)
    out: list[tuple[int, str, bool]] = []
    prev_raw = ""
    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = strip_comment(raw)

        ns_match = _NAMESPACE_OPEN.match(line)
        depth_before = depth
        depth += line.count("{") - line.count("}")
        if ns_match:
            ns_stack.append((depth, ns_match.group(1)))
        while ns_stack and depth < ns_stack[-1][0]:
            ns_stack.pop()

        current_ns = ns_stack[-1][1] if ns_stack else ""
        # A declaration is "directly in agentengine" when, at the start of this line, we're
        # exactly at agentengine's own scope depth (not nested inside a struct/enum body, and not
        # inside a deeper namespace like agentengine::detail).
        if current_ns == "agentengine" and depth_before == ns_stack[-1][0] and not ns_match:
            m = _DECL.match(line)
            if m:
                name = next(g for g in m.groups() if g)
                suppressed = bool(_ALLOW.search(raw) or _ALLOW.search(prev_raw))
                out.append((line_no, name, suppressed))
        prev_raw = raw
    return out


def iter_headers():
    for path in sorted(SCAN_ROOT.rglob("*.hpp")):
        if any(excluded in path.parents or excluded == path.parent for excluded in EXCLUDED_DIRS):
            continue
        yield path


def main() -> int:
    vocabulary = load_vocabulary()
    violations: list[tuple[Path, int, str]] = []
    suppressed_count = 0

    for path in iter_headers():
        for line_no, name, suppressed in find_declarations(path):
            if name in vocabulary:
                continue
            if suppressed:
                suppressed_count += 1
                continue
            violations.append((path, line_no, name))

    if suppressed_count:
        print(f"027 naming-lint: {suppressed_count} suppressed finding(s) (ae-naming-lint: allow).")

    if not violations:
        print("027 naming-lint: OK — every exported type appears in 027's vocabulary tables.")
        return 0

    print(f"027 naming-lint: {len(violations)} exported name(s) not in 027's vocabulary tables:")
    for path, line_no, name in violations:
        rel = path.relative_to(REPO_ROOT)
        print(f"  {rel}:{line_no}: `{name}`")
    print(
        "\nEach of these needs either a row in 027-Vocabulary-and-Naming.md §2-4 (027 §8 rule 3), "
        "a rename to the name 027 already lists, or an inline `// ae-naming-lint: allow <Name> — "
        "<reason>` suppression if the deferral is deliberate."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
