# ADR-136 — `agentengine_cli_chat` gets the ADR-108 §7 startup orphan-container sweep its siblings already have

- **Status:** Proposed — implemented, verified (Windows/MSVC, real Docker daemon), full rebuild (zero
  errors) and full `ctest` clean (293 total, 1 pre-existing unrelated failure, zero regression),
  `naming_lint.py` clean.
- **Date:** 2026-08-30.
- **Scope:** `tools/cli_chat.cpp` only (one new block at the top of `main()`).
- **Related specs:** `decisions/ADR-108-docker-orphan-reap.md` (the sweep mechanism this reuses
  verbatim), `decisions/ADR-119-run-command-capability-variant-widening.md` (the same pass that wired
  `run_command`/`DockerExecutionSurface` into `cli_chat.cpp` in the first place, and is the reason this
  gap existed at all).

## 1. The question

A final-review pass across this branch's own diff (independent of any single ADR) found that
`tools/cli_chat.cpp` was the one tool, of four real production CLI entry points using
`DockerExecutionSurface`, that never ran `DockerCliBackend::reap_orphans()` at startup —
`tools/sandboxed_shell_chat.cpp`, `tools/containerd_shell_chat.cpp`, and
`tools/durable_sandboxed_shell_chat.cpp` all carry the identical ADR-108 §7 sweep. `cli_chat.cpp`'s
own top comment (~line 1088) already discloses that a real container from `run_command` is orphaned on
**every** ordinary CLI exit (`std::_Exit(0)` deliberately skips `DockerExecutionSurface`'s own
destructor, to dodge a real CPython finalize-thread crash this same comment documents) — making
`cli_chat.cpp` the tool that most needed the self-healing sweep, and the one place it was missing.

## 2. Findings

Not a design question — a straightforward omission. `ADR-119` wired `cap::RunCommand`/
`DockerExecutionSurface` into `cli_chat.cpp` for the first time; the sweep should have been added in
the same pass (it was added to the OTHER three tools in that same ADR) but was not. No structural
reason it couldn't be — `docker_execution_surface.hpp` (which declares `DockerCliBackend`) was already
included.

## 3. What was built

Added the identical block `sandboxed_shell_chat.cpp` already carries, at the top of the real
`#ifdef AGENTENGINE_WITH_HTTPS` `main()` (the stub `main()` in the `#else` arm needs nothing, since it
never touches Docker):

```cpp
{
    DockerCliBackend orphan_sweep;
    auto swept = orphan_sweep.reap_orphans();
    if (swept.has_value() && (swept->reaped > 0 || !swept->reap_failures.empty())) {
        std::cerr << "startup orphan sweep: inspected " << swept->inspected << ", reaped "
                  << swept->reaped << ", " << swept->reap_failures.size()
                  << " destroy failure(s)\n";
    } else if (!swept.has_value()) {
        std::cerr << "startup orphan sweep skipped (non-fatal): " << swept.error().message << "\n";
    }
}
```

Best-effort, never fatal — matching every sibling tool's own established discipline: a briefly
unreachable daemon or a sweep that finds nothing must never block the CLI's own real purpose.

## 4. Verification

Full rebuild (zero errors, `agentengine_cli_chat.vcxproj` rebuilt clean). Ran `agentengine_cli_chat.exe`
directly to confirm the startup sweep prints nothing when there is nothing to reap and does not
crash/hang startup. Full `ctest`: 293 total, 1 pre-existing unrelated failure (`test_reference_agent_
task_corpus`, the disclosed matplotlib/pandas environment gap), zero regression. `naming_lint.py`
clean.

Independent red-team round (this session's own consolidated pass covering all six ADR-136–142 fixes
together): reviewed alongside the Docker `run_capture` rewrite (ADR-139) — confirmed the sweep is a
faithful copy of the existing sibling pattern, is silent when nothing needs reaping, and does not
block/crash startup. No fix needed here.

## 5. Not done

- No attempt to also add a sweep to any OTHER tool — the other three already have it.
- Does not address the underlying "every ordinary exit orphans a live `run_command` container" gap
  itself (disclosed since ADR-102 Phase 3/5) — this ADR only closes the self-healing half (a LATER
  invocation now cleans up what an EARLIER one left behind), not the leak itself.

## 6. Residuals

- Same as every sibling tool: no signal handler installed anywhere in this codebase, so Ctrl+C during
  a blocked read/network call skips cleanup — reclaimed on the next startup's own sweep, not
  synchronously.
