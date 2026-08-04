# `docs/issues/`

Drafted GitHub issues for AgentEngine's remaining tracked work, written as plain markdown because
`gh` (GitHub CLI) wasn't available in the environment that authored them (2026-08-05) to create the
issues directly against `github.com/thnak/AgentEngine`.

Each file is one issue: the first line is the intended issue title, followed by a body written in
GitHub-flavored markdown, ready to paste into a new issue or feed to `gh issue create --title ...
--body-file ...` once `gh` is installed and authenticated.

**Source of truth stays the breakdown doc.** These issues summarize
[`docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md`](../planning/milestone-2-tools-capabilities-sandbox-breakdown.md)
at the time they were drafted — if the breakdown doc changes (scope, sizing, sequencing), the doc
wins; re-sync these files or the eventual real issues from it, not the other way around.

| File | Milestone phase |
|---|---|
| [m2-phase-c-native-jail-sandbox.md](m2-phase-c-native-jail-sandbox.md) | M2 Phase C — `native-jail` sandbox (008), Windows + Linux |
| [m2-phase-d-wasm-plugin-host.md](m2-phase-d-wasm-plugin-host.md) | M2 Phase D — WASM plugin host (009) |
| [m2-phase-e-agent-crtp-surface.md](m2-phase-e-agent-crtp-surface.md) | M2 Phase E — Agent CRTP surface (002) |
| [m2-phase-f-adr-track-tasks.md](m2-phase-f-adr-track-tasks.md) | M2 Phase F — cross-cutting ADR-track tasks (egress proxy, policy-reachability tool) |

Once filed as real issues, delete the corresponding file here (or leave it with a link back to the
issue) rather than maintaining the same task list in two places indefinitely.
