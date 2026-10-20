#!/usr/bin/env python3
"""Egress test-seam lint -- decisions/ADR-241-egress-proxy-async-path-on-rt-http.md section 5 and gate G15.

`TestEgressProxy` (include/agentengine/sandbox/testing/net_egress_proxy_testing.hpp) is the ONLY way to run the egress
proxy with a non-default host resolver, address policy or CA bundle, and
include/agentengine/sandbox/detail/net_egress_impl.hpp holds the functions that take those seams as PARAMETERS
(`egress_fetch(..., EgressSeams, ...)`). Host code that could include either would have a way around the SSRF boundary
without naming `TestEgressProxy`, so nothing under include/, src/ or examples/ may include them, except:

  - src/sandbox/                                   (the implementation)
  - include/agentengine/sandbox/testing/           (the test seam itself)
  - include/agentengine/sandbox/detail/            (the implementation header)

Tests under tests/ may include them (that is the point of the seam).

An include may be suppressed by `ae-egress-seam-lint: allow <reason>` on its line or the line above; the reason is
required and the suppression is reported.

Usage:
  python tools/egress_seam_lint.py              check the tree
  python tools/egress_seam_lint.py --self-test  positive/negative controls on a synthetic tree

Exit code 0 when clean, 1 otherwise.
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SCANNED_ROOTS = ("include", "src", "examples")
ALLOWED_PREFIXES = (
    "src/sandbox/",
    "include/agentengine/sandbox/testing/",
    "include/agentengine/sandbox/detail/",
)
FORBIDDEN = re.compile(r'#\s*include\s*[<"]([^>"]*(?:sandbox/testing/|sandbox/detail/net_egress_impl\.hpp)[^>"]*)[>"]')
SUPPRESS = re.compile(r"ae-egress-seam-lint:\s*allow\s+\S+")
SOURCE_SUFFIXES = {".hpp", ".cpp", ".h", ".cc", ".cxx", ".ipp"}


def scan(root: Path) -> tuple[list[str], list[str]]:
    violations: list[str] = []
    suppressed: list[str] = []
    for top in SCANNED_ROOTS:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            if rel.startswith(ALLOWED_PREFIXES):
                continue
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            for i, line in enumerate(lines):
                m = FORBIDDEN.search(line)
                if not m:
                    continue
                here = f"{rel}:{i + 1}: includes {m.group(1)}"
                prev = lines[i - 1] if i > 0 else ""
                if SUPPRESS.search(line) or SUPPRESS.search(prev):
                    suppressed.append(here)
                else:
                    violations.append(here)
    return violations, suppressed


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

        # Allowed places.
        put("src/sandbox/net_egress_fetch.cpp", '#include "agentengine/sandbox/detail/net_egress_impl.hpp"\n')
        put("include/agentengine/sandbox/testing/net_egress_proxy_testing.hpp",
            '#include "agentengine/sandbox/detail/net_egress_impl.hpp"\n')
        put("include/agentengine/sandbox/detail/net_egress_impl.hpp", "#pragma once\n")
        # Ordinary host code: clean.
        put("src/rt/ok.cpp", '#include "agentengine/sandbox/net_egress_proxy.hpp"\n')
        violations, _ = scan(root)
        expect(violations == [], "allowed locations and ordinary includes are clean")

        # Positive controls: each forbidden spelling in each scanned root is caught.
        put("include/agentengine/tools/bad_testing.hpp",
            '#include "agentengine/sandbox/testing/net_egress_proxy_testing.hpp"\n')
        put("src/rt/bad_impl.cpp", '#include "agentengine/sandbox/detail/net_egress_impl.hpp"\n')
        put("examples/demo/bad.cpp", '#include <agentengine/sandbox/testing/net_egress_proxy_testing.hpp>\n')
        put("src/sandbox_adjacent/bad.cpp", '#include "agentengine/sandbox/testing/other.hpp"\n')  # not src/sandbox/
        violations, _ = scan(root)
        expect(len(violations) == 4, f"four forbidden includes are caught (got {len(violations)})")

        # A suppression with a reason counts as suppressed; a bare marker does not.
        put("src/rt/suppressed.cpp",
            '// ae-egress-seam-lint: allow reviewed-by-owner\n#include "agentengine/sandbox/testing/x.hpp"\n')
        put("src/rt/bare.cpp", '// ae-egress-seam-lint: allow\n#include "agentengine/sandbox/testing/x.hpp"\n')
        violations, suppressed = scan(root)
        expect(len(suppressed) == 1, "a reasoned suppression is reported as suppressed")
        expect(any("bare.cpp" in v for v in violations), "a bare suppression marker does not suppress")
    if failures == 0:
        print("egress_seam_lint self-test: OK")
        return 0
    return 1


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    violations, suppressed = scan(ROOT)
    for s in suppressed:
        print(f"suppressed: {s}")
    if violations:
        print("egress test-seam lint FAILED (ADR-241 G15): these files include the testing/detail egress headers:")
        for v in violations:
            print(f"  {v}")
        print("Only src/sandbox/, sandbox/testing/ and sandbox/detail/ may include them; tests/ may too.")
        return 1
    print("egress_seam_lint: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
