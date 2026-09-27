#!/usr/bin/env python3
"""Include-direction (layering) lint -- issue #120 S10.

AgentEngineSpecification.md §3: "a layer may depend only on layers below it, and only through the
seam that layer publishes." CONVENTIONS.md adds that `agentengine::core` never holds an `mcp::` or
`a2a::` type. The directory tree does not map onto the layers one to one (core/ holds both shared
vocabulary and L2 machinery; rt/ holds L0, L2 and L3 code), so this lint reads an explicit layer
map, tools/layers.toml, instead of trusting directory names.

Layers, low to high: V (shared vocabulary), L0, L1, L2, L3, L4, and TEST (test support).

Rule: a file at layer N may include only headers at layer <= N, or V. A V file may include only V.
Nothing outside TEST may include a TEST header; a TEST file may include anything.

Scope: include/agentengine/**/*.hpp and src/**/*.{hpp,cpp}. An include is followed when it
resolves to one of those files: `agentengine/...` against include/, any other quoted path against
the including file's directory and then src/. Everything else (std, OS, third-party) is ignored.

Ratchet: violations that already exist are listed in tools/layering_baseline.txt, one edge per line
(`includer -> included  # category: note`). The lint fails on any violation not in the baseline,
and on any baseline entry that no longer matches a violation, so the baseline can only shrink.

Suppression: an include whose line (or the line above it) carries
`ae-layering-lint: allow <reason>` is not counted as a violation but is still reported as
suppressed. The reason is required; a bare marker does not suppress.

Like naming_lint.py this is line-oriented, not a preprocessor: it sees every #include, including
ones inside #if blocks that a given platform never compiles. That is deliberate -- a layering edge
is a problem on the platform that does compile it.

Usage:
  python tools/layering_lint.py              check against the baseline
  python tools/layering_lint.py --list       print every current violation in baseline format
  python tools/layering_lint.py --self-test  run the positive/negative controls on a synthetic tree

Exit code 0 when clean, 1 otherwise.
"""

from __future__ import annotations

import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    print("layering-lint: needs Python 3.11+ (tomllib).")
    sys.exit(1)

REPO_ROOT = Path(__file__).resolve().parent.parent
LAYER_MAP = Path("tools/layers.toml")
BASELINE = Path("tools/layering_baseline.txt")

# Rank order. V sits below L0: everything may include it, it may include nothing but itself.
RANK = {"V": -1, "L0": 0, "L1": 1, "L2": 2, "L3": 3, "L4": 4, "TEST": 99}

_INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
_ALLOW = re.compile(r"ae-layering-lint:\s*allow\s+\S")


@dataclass(frozen=True)
class Violation:
    includer: str  # repo-relative, forward slashes
    line: int
    included: str
    includer_layer: str
    included_layer: str

    @property
    def edge(self) -> tuple[str, str]:
        return (self.includer, self.included)

    def render(self) -> str:
        return (f"{self.includer}:{self.line}: {self.includer} ({self.includer_layer}) -> "
                f"{self.included} ({self.included_layer})")


@dataclass
class Result:
    violations: list[Violation]
    suppressed: list[Violation]
    errors: list[str]  # configuration problems: unassigned files, stale overrides, bad layers


def scanned_files(root: Path) -> list[Path]:
    out: list[Path] = []
    inc = root / "include" / "agentengine"
    if inc.is_dir():
        out += sorted(inc.rglob("*.hpp"))
    src = root / "src"
    if src.is_dir():
        out += sorted(p for p in src.rglob("*") if p.suffix in (".hpp", ".cpp") and p.is_file())
    return out


def load_layer_map(root: Path, map_path: Path) -> tuple[dict[str, str], dict[str, str], list[str]]:
    data = tomllib.loads((root / map_path).read_text(encoding="utf-8"))
    errors: list[str] = []
    dirs: dict[str, str] = {}
    files: dict[str, str] = {}
    for section, target in (("directories", dirs), ("files", files)):
        for key, entry in data.get(section, {}).items():
            layer = entry.get("layer") if isinstance(entry, dict) else None
            reason = entry.get("reason", "").strip() if isinstance(entry, dict) else ""
            if layer not in RANK:
                errors.append(f"{map_path}: [{section}] {key}: unknown layer {layer!r}")
                continue
            if not reason:
                errors.append(f"{map_path}: [{section}] {key}: missing reason")
            target[key.rstrip("/")] = layer
    for key in files:
        if not (root / key).is_file():
            errors.append(f"{map_path}: [files] {key}: no such file (stale override)")
    for key in dirs:
        if not (root / key).is_dir():
            errors.append(f"{map_path}: [directories] {key}: no such directory (stale default)")
    return dirs, files, errors


def layer_of(rel: str, dirs: dict[str, str], files: dict[str, str]) -> str | None:
    if rel in files:
        return files[rel]
    best = None
    for d, layer in dirs.items():
        if rel.startswith(d + "/") and (best is None or len(d) > len(best[0])):
            best = (d, layer)
    return best[1] if best else None


def resolve(root: Path, includer: Path, bracket: str, target: str) -> Path | None:
    if target.startswith("agentengine/"):
        p = root / "include" / target
        return p if p.is_file() else None
    if bracket != '"':
        return None
    for base in (includer.parent, root / "src"):
        p = (base / target).resolve()
        if p.is_file():
            return p
    return None


def include_lines(path: Path):
    """Yields (line_no, bracket, target, suppressed) for each #include outside a comment."""
    prev = ""
    in_block = False
    for n, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        line = raw
        if in_block:
            end = line.find("*/")
            if end == -1:
                prev = raw
                continue
            line = line[end + 2:]
            in_block = False
        start = line.find("/*")
        if start != -1 and line.find("*/", start) == -1 and not line.lstrip().startswith("#"):
            in_block = True
        m = _INCLUDE.match(line)
        if m:
            suppressed = bool(_ALLOW.search(raw) or _ALLOW.search(prev))
            yield n, m.group(1), m.group(2), suppressed
        prev = raw


def check(root: Path, map_path: Path = LAYER_MAP) -> Result:
    root = root.resolve()
    dirs, files, errors = load_layer_map(root, map_path)
    violations: list[Violation] = []
    suppressed: list[Violation] = []
    for path in scanned_files(root):
        rel = path.relative_to(root).as_posix()
        src_layer = layer_of(rel, dirs, files)
        if src_layer is None:
            errors.append(f"{rel}: no layer assigned (add a directory default or a [files] entry)")
            continue
        for line_no, bracket, target, allow in include_lines(path):
            dst = resolve(root, path, bracket, target)
            if dst is None:
                continue
            dst_rel = dst.relative_to(root).as_posix()
            dst_layer = layer_of(dst_rel, dirs, files)
            if dst_layer is None:
                continue  # reported once as an unassigned file by its own scan
            if src_layer == "TEST":
                continue
            if RANK[dst_layer] <= RANK[src_layer]:
                continue
            v = Violation(rel, line_no, dst_rel, src_layer, dst_layer)
            (suppressed if allow else violations).append(v)
    return Result(violations, suppressed, errors)


def load_baseline(path: Path) -> tuple[dict[tuple[str, str], int], list[str]]:
    """Returns ({edge: baseline line number}, parse errors)."""
    edges: dict[tuple[str, str], int] = {}
    errors: list[str] = []
    if not path.is_file():
        return edges, errors
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        body = raw.split("#", 1)[0].strip()
        if not body:
            continue
        parts = [p.strip() for p in body.split("->")]
        if len(parts) != 2 or not all(parts):
            errors.append(f"{path.name}:{n}: expected 'includer -> included', got {raw!r}")
            continue
        if "#" not in raw:
            errors.append(f"{path.name}:{n}: baseline entry needs a '# category: note' comment")
        edge = (parts[0], parts[1])
        if edge in edges:
            errors.append(f"{path.name}:{n}: duplicate entry (first at line {edges[edge]})")
        edges[edge] = n
    return edges, errors


def run(root: Path, baseline_path: Path, map_path: Path = LAYER_MAP, out=print) -> int:
    result = check(root, map_path)
    baseline, base_errors = load_baseline(root / baseline_path)
    errors = result.errors + base_errors
    present = {v.edge for v in result.violations}
    new = [v for v in result.violations if v.edge not in baseline]
    stale = sorted((n, e) for e, n in baseline.items() if e not in present)

    for e in errors:
        out(f"layering-lint: config error: {e}")
    if result.suppressed:
        out(f"layering-lint: {len(result.suppressed)} suppressed edge(s) (ae-layering-lint: allow).")
    if new:
        out(f"layering-lint: {len(new)} new layering violation(s):")
        for v in new:
            out(f"  {v.render()}")
        out("  Fix the include (depend downward, or move the shared piece down a layer), or add an "
            "inline `// ae-layering-lint: allow <reason>` if the edge is a deliberate seam. "
            "Do not add new edges to the baseline.")
    if stale:
        out(f"layering-lint: {len(stale)} stale baseline entr(y/ies) -- the edge is gone, delete the line:")
        for n, (a, b) in stale:
            out(f"  {baseline_path.as_posix()}:{n}: {a} -> {b}")
    if errors or new or stale:
        return 1
    out(f"layering-lint: OK -- no new violations ({len(baseline)} baselined edge(s) remain).")
    return 0


# ---------------------------------------------------------------------------------------------
# Self-test: a synthetic tree whose expected verdict is known, so the checks above can fail.
# ---------------------------------------------------------------------------------------------

_SELF_TEST_MAP = """
[directories]
"include/agentengine/core"    = { layer = "L2", reason = "t" }
"include/agentengine/rt"      = { layer = "L0", reason = "t" }
"include/agentengine/testing" = { layer = "TEST", reason = "t" }
"src"                         = { layer = "L1", reason = "t" }
[files]
"include/agentengine/core/error.hpp" = { layer = "V", reason = "t" }
"""


def _write(root: Path, rel: str, text: str) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


def self_test() -> int:
    failures: list[str] = []

    def expect(cond: bool, what: str) -> None:
        print(f"  {'ok  ' if cond else 'FAIL'} {what}")
        if not cond:
            failures.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _write(root, "tools/layers.toml", _SELF_TEST_MAP)
        _write(root, "include/agentengine/core/error.hpp", "#pragma once\n#include <string>\n")
        _write(root, "include/agentengine/core/session.hpp",
               '#pragma once\n#include "agentengine/rt/task.hpp"\n#include "agentengine/core/error.hpp"\n')
        # The one bad edge: L0 including L2.
        _write(root, "include/agentengine/rt/task.hpp",
               '#pragma once\n#include "agentengine/core/error.hpp"\n'
               '// #include "agentengine/core/session.hpp"  (commented out: must be ignored)\n'
               '#include <agentengine/core/session.hpp>\n')
        _write(root, "include/agentengine/testing/fake.hpp", '#include "agentengine/core/session.hpp"\n')
        _write(root, "src/impl.cpp", '#include "agentengine/rt/task.hpp"\n#include "local.hpp"\n')
        _write(root, "src/local.hpp", "#pragma once\n")

        print("layering-lint self-test: rule check")
        r = check(root)
        edges = [v.edge for v in r.violations]
        bad = ("include/agentengine/rt/task.hpp", "include/agentengine/core/session.hpp")
        expect(not r.errors, f"synthetic tree has no config errors {r.errors}")
        expect(edges == [bad], f"exactly the one bad include is flagged (got {edges})")
        expect(bool(r.violations) and r.violations[0].line == 4, "it is reported at the right line")
        expect(bool(r.violations) and r.violations[0].render() ==
               "include/agentengine/rt/task.hpp:4: include/agentengine/rt/task.hpp (L0) -> "
               "include/agentengine/core/session.hpp (L2)", "report format is path:line: a (Lx) -> b (Ly)")

        _write(root, "include/agentengine/core/error.hpp",
               '#pragma once\n#include "agentengine/rt/task.hpp"\n')
        r = check(root)
        expect(("include/agentengine/core/error.hpp", "include/agentengine/rt/task.hpp")
               in [v.edge for v in r.violations], "V including L0 is flagged (V may include only V)")
        _write(root, "include/agentengine/core/error.hpp", "#pragma once\n")

        _write(root, "include/agentengine/core/uses_test.hpp", '#include "agentengine/testing/fake.hpp"\n')
        r = check(root)
        expect(("include/agentengine/core/uses_test.hpp", "include/agentengine/testing/fake.hpp")
               in [v.edge for v in r.violations], "non-test code including TEST is flagged")
        (root / "include/agentengine/core/uses_test.hpp").unlink()

        print("layering-lint self-test: suppression")
        task = root / "include/agentengine/rt/task.hpp"
        good = task.read_text(encoding="utf-8")
        task.write_text(good.replace("#include <agentengine/core/session.hpp>",
                                     "// ae-layering-lint: allow self-test seam\n"
                                     "#include <agentengine/core/session.hpp>"), encoding="utf-8")
        r = check(root)
        expect(not r.violations and len(r.suppressed) == 1, "an allow with a reason suppresses the edge")
        task.write_text(good.replace("#include <agentengine/core/session.hpp>",
                                     "#include <agentengine/core/session.hpp>  // ae-layering-lint: allow"),
                        encoding="utf-8")
        r = check(root)
        expect(len(r.violations) == 1, "an allow without a reason does not suppress")
        task.write_text(good, encoding="utf-8")

        print("layering-lint self-test: baseline ratchet")
        quiet = lambda *_: None  # noqa: E731
        base = Path("tools/baseline.txt")
        _write(root, base.as_posix(), "")
        expect(run(root, base, out=quiet) == 1, "an unbaselined violation fails")
        _write(root, base.as_posix(),
               "include/agentengine/rt/task.hpp -> include/agentengine/core/session.hpp  # a: test\n")
        expect(run(root, base, out=quiet) == 0, "the same violation passes once baselined")
        _write(root, "src/new.cpp", '#include "agentengine/core/session.hpp"\n')
        expect(run(root, base, out=quiet) == 1, "a NEW violation fails even with a baseline present")
        (root / "src/new.cpp").unlink()
        task.write_text('#pragma once\n#include "agentengine/core/error.hpp"\n', encoding="utf-8")
        expect(run(root, base, out=quiet) == 1, "a baseline entry whose edge is gone fails as stale")
        task.write_text(good, encoding="utf-8")
        _write(root, base.as_posix(),
               "include/agentengine/rt/task.hpp -> include/agentengine/core/session.hpp\n")
        expect(run(root, base, out=quiet) == 1, "a baseline entry without a category comment fails")

        print("layering-lint self-test: layer map hygiene")
        _write(root, "include/agentengine/unmapped/x.hpp", "#pragma once\n")
        expect(any("no layer assigned" in e for e in check(root).errors), "an unassigned file is an error")
        (root / "include/agentengine/unmapped/x.hpp").unlink()
        _write(root, "tools/layers.toml",
               _SELF_TEST_MAP + '"include/agentengine/core/gone.hpp" = { layer = "L2", reason = "t" }\n')
        expect(any("stale override" in e for e in check(root).errors), "an override for a missing file is an error")

    if failures:
        print(f"layering-lint self-test: FAILED ({len(failures)} check(s))")
        return 1
    print("layering-lint self-test: OK")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    if "--list" in argv:
        result = check(REPO_ROOT)
        for e in result.errors:
            print(f"# config error: {e}")
        for edge in sorted({v.edge for v in result.violations}):
            print(f"{edge[0]} -> {edge[1]}")
        return 0
    return run(REPO_ROOT, BASELINE)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
