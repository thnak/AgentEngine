#!/usr/bin/env python3
"""Test-preset lint -- decisions/ADR-244-required-choices-step-1b.md gate G-C11 ("the test preset stays in tests").

ADR-244 makes every session choice stated: authority source, token budget, turn cap. Tests state them through a
small preset (tests/support/session_choices.hpp, tests/support/agent_options.hpp: `ae_test::trusted_unbounded()`,
`ae_test::declared_options<A>()`, ...) that keeps their old meaning -- no budget, no cap, the session's own grants.
That is an honest choice for a test and an implicit one anywhere else, so nothing under include/, src/, tools/ or
examples/ may reach it:

  - no `ae_test::` symbol, and
  - no include of anything under tests/support/.

Production code states its choices in full at the call site.

Usage:
  python tools/test_preset_lint.py              check the tree
  python tools/test_preset_lint.py --self-test  positive/negative controls on a synthetic tree

Exit code 0 when clean, 1 otherwise.
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SCANNED_ROOTS = ("include", "src", "tools", "examples")
SELF = "tools/test_preset_lint.py"
FORBIDDEN = (
    (re.compile(r"\bae_test::"), "uses the test preset (ae_test::)"),
    (re.compile(r'#\s*include\s*[<"][^>"]*support/(?:session_choices|agent_options)\.hpp[>"]'), "includes the test preset"),
)
SOURCE_SUFFIXES = {".hpp", ".cpp", ".h", ".cc", ".cxx", ".ipp"}


def strip_comment(line: str) -> str:
    # A mention in a `//` comment (this lint's own documentation, say) is not a use.
    at = line.find("//")
    return line if at < 0 else line[:at]


def scan(root: Path) -> list[str]:
    violations: list[str] = []
    for top in SCANNED_ROOTS:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            for i, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines()):
                code = strip_comment(line)
                for pattern, what in FORBIDDEN:
                    if pattern.search(code):
                        violations.append(f"{rel}:{i + 1}: {what}")
    return violations


def self_test() -> int:
    failures = 0

    def expect(cond: bool, what: str) -> None:
        nonlocal failures
        if not cond:
            failures += 1
            print(f"SELF-TEST FAIL: {what}")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        def put(rel: str, text: str) -> None:
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding="utf-8")

        # Negative controls: tests may use the preset; production states choices in full; comments are not uses.
        put("tests/rt/ok_test.cpp",
            '#include "../support/session_choices.hpp"\nauto c = ae_test::trusted_unbounded();\n')
        put("src/rt/ok.cpp", "auto c = SessionChoices{AuthoritySource::single_trusted_caller(), b, b};\n")
        put("include/agentengine/rt/ok.hpp", "// the ae_test:: preset lives in tests/support/session_choices.hpp\n")
        expect(scan(root) == [], "tests/, fully stated production code and comments are clean")

        # Positive controls: a planted use in each scanned root, in each forbidden form, is caught.
        put("include/agentengine/core/bad.hpp", '#include "../../../tests/support/session_choices.hpp"\n')
        put("examples/98_ok.cpp", '#include "support/run_task_sync.hpp"\n')  # a driving helper, not a choice
        put("src/rt/bad.cpp", "auto c = ae_test::trusted_unbounded();\n")
        put("tools/bad_tool.cpp", "opts = ae_test::declared_options<A>();\n")
        put("examples/99_bad.cpp", '#include "support/agent_options.hpp"\n')
        violations = scan(root)
        expect(len(violations) == 4, f"four planted uses are caught (got {len(violations)}: {violations})")
    if failures == 0:
        print("test_preset_lint self-test: OK")
        return 0
    return 1


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    violations = scan(ROOT)
    if violations:
        print("test-preset lint FAILED (ADR-244 G-C11): production code reaches the tests' session-choice preset:")
        for v in violations:
            print(f"  {v}")
        print("State the choices in full: SessionChoices{AuthoritySource::..., Bound<...>::..., Bound<...>::...}.")
        return 1
    print("test_preset_lint: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
