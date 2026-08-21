# Where Claude Code, GitHub Copilot, and OpenAI Codex CLI store `SKILL.md` skills

**Date:** 2026-08-21 · **Sources:** cited inline, fetched via web search, not asserted from memory.
Requested while designing `decisions/ADR-072-external-host-discovered-skill-sources.md` — needed the
real, current per-tool/per-scope directory conventions before writing
`include/agentengine/core/external_skill_discovery.hpp`'s `kKnownRoots` table, per this project's own
"research is dated and cited" rule (`CLAUDE.md`).

## 1. GitHub Copilot

Search: "GitHub Copilot custom instructions skillset file format 2026 .github/instructions
.prompt.md".

- [GitHub Copilot Skills: SKILL.md Setup Guide for Agent Mode (2026)](https://www.agensi.io/learn/github-copilot-skills-setup-guide)
- [Copilot Configuration Files: What They Are, Where They Live, and When to Use Them](https://dfberry.github.io/2026-06-08-copilot-config-files-guide)
- [GitHub Copilot Agent Skills Guide [2026 Latest]](https://smartscope.blog/en/generative-ai/github-copilot/github-copilot-skills-guide/)
- [Supercharge Your Dev Workflows with GitHub Copilot Custom Skills](https://techcommunity.microsoft.com/blog/azuredevcommunityblog/supercharge-your-dev-workflows-with-github-copilot-custom-skills/4510012)
- [github/awesome-copilot — docs/README.skills.md](https://github.com/github/awesome-copilot/blob/main/docs/README.skills.md)

**Format:** a skill is a folder containing a `SKILL.md` file (YAML frontmatter `name`/`description` +
Markdown instructions body), plus optional `scripts`/`templates`/reference docs — the front matter
`name` must match the folder name. GitHub Copilot added `SKILL.md` support in April 2026 through its
agent mode, explicitly so "the same `SKILL.md` files that work across Claude Code, Cursor, Codex CLI,
and 20+ other agents" work unmodified with Copilot too.

**Locations:** GitHub's own docs support `.github/skills`, `.claude/skills`, and `.agents/skills` as
equivalent project-level conventions; "if you primarily use GitHub Copilot, placing skills in
`.github/skills/` is recommended." Personal/user-level: `~/.copilot/skills/` for Copilot specifically,
or `~/.agents/skills/` for generic cross-agent compatibility.

## 2. OpenAI Codex CLI

Search: "OpenAI Codex CLI skills directory format ~/.codex AGENTS.md 2026".

- [Build skills (developers.openai.com/codex)](https://developers.openai.com/codex/skills.md)
- [Codex CLI Skills Directory: Best Skills for OpenAI Codex (2026)](https://agentskillshub.dev/guides/codex-skills/)
- [Codex CLI Skills: Install & Use SKILL.md (2026 Guide)](https://www.agensi.io/learn/codex-cli-skills-install-skill-md)
- [Codex CLI Agent Skills — 2026 Install and Usage Guide](https://itecsonline.com/post/codex-cli-agent-skills-guide-install-usage-cross-platform-resources-2026)
- [Codex CLI Skills & AGENTS.md Setup Guide 2026](https://www.agensi.io/learn/codex-cli-agents-md-complete-guide)
- [Writing Effective SKILL.md Files for Codex CLI](https://codex.danielvaughan.com/2026/03/26/writing-effective-skillmd-files/)

**Format:** identical `SKILL.md` shape (a directory with a `SKILL.md` file plus optional
scripts/references; `name`/`description` required in frontmatter).

**Locations:** personal skills at `~/.codex/skills/`; project skills at `.codex/skills/` (repo root).
`AGENTS.md` is a separate, project-instructions mechanism (not a skill format) living at
`~/.codex/AGENTS.md` (global) or the repo root (project-specific, overrides global) — out of scope
for ADR-072, which is about `SKILL.md`-shaped skill *libraries*, not project instruction files.

## 3. Claude Code

Not separately searched — this project's own development environment already runs Claude Code, so
its convention (`~/.claude/skills/` for personal skills, `.claude/skills/` for project skills, each a
`skill-name/SKILL.md`-shaped subdirectory) is directly, contemporaneously observable rather than
inferred from a search result: confirmed via `ls ~/.claude/skills` during the same session this
research was gathered in, which additionally showed every entry there as a real NTFS symlink into
`~/.agents/skills/<name>/SKILL.md` — corroborating §1's "generic agent" `~/.agents/skills/`
convention as a live, not merely documented, directory on a real machine.

## 4. Antigravity

No search was run for Antigravity's own skill-directory convention (out of scope for ADR-072's first
version — see that ADR's §7 "explicitly out of scope"). Adding it later is a small, additive row to
`kKnownRoots`, once a dated source for its convention exists.

## 5. What this resolved into

`include/agentengine/core/external_skill_discovery.hpp`'s `kKnownRoots` table, built directly from
§1-§3 above:

| tool | user scope | project scope |
|---|---|---|
| `claude_code` | `~/.claude/skills` | `<root>/.claude/skills` |
| `codex` | `~/.codex/skills` | `<root>/.codex/skills` |
| `copilot` | `~/.copilot/skills` | `<root>/.github/skills` |
| `generic_agents` | `~/.agents/skills` | `<root>/.agents/skills` |

A wrong or stale row fails safe by construction (`discover_external_skill_locations()`'s
`is_directory` check simply finds nothing for it) — see ADR-072 §6 item 5.
