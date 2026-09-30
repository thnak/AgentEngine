# New sandbox types and products that appeared in 2026 — a scan for ADR-209's tiers

- **Date**: 2026-09-29
- **Question**: What sandbox *types* (isolation mechanisms, lifetime models) and notable products appeared or reached
  availability during 2026, and which of ADR-209's shell tiers (one-shot / live per run / live per session / native)
  does each correspond to?
- **Scope**: a quick web scan, not a deep survey. It complements, and does not repeat,
  `2026-08-23-microvm-sandbox-backend-landscape.md`, `2026-08-23-sandbox-feature-parity-survey.md` and the parallel
  `2026-09-29-persistent-shell-sandbox-landscape.md`.
- **Verification levels**: **[P]** fetched from the vendor's own page on 2026-09-29; **[S]** a secondary article
  fetched on 2026-09-29; **[R]** search-result text only, not opened. Anything marked [R] needs a [P] check before an
  ADR relies on it.

## Answer in one paragraph

2026's new *types* are less about a new isolation primitive than about three shifts:

1. **Desktop-native microVMs for local agents.** Docker Sandboxes uses each OS's own hypervisor, including Windows
   Hypervisor Platform. Microsoft Execution Containers is an OS-provided, policy-driven containment layer on Windows.
2. **Stateful, checkpointable sandboxes as a product category.** Examples: Fly.io Sprites, Perplexity SPACE
   (pause/resume/fork), and AWS AgentCore (a session microVM up to 8 h plus session storage).
3. **Per-tool-call, kernel-enforced scoping** (Nono: Landlock/Seatbelt, with a credential proxy).

For ADR-209, the most consequential are **MXC**, a possible future isolation layer for the *native* tier, and the
**stateful-sandbox products**. None of the stateful ones, as far as this scan read, restores *running processes*.
Sprites explicitly does not.

## Findings

| Product / type | Mechanism | 2026 milestone | Lifetime model | ADR-209 tier it resembles | Level |
|---|---|---|---|---|---|
| **Microsoft Execution Containers (MXC)** | OS policy layer: *process isolation* (file/network policy within the user's environment; adopted by GitHub Copilot CLI) and *session isolation* (each agent runs as a separate, throwaway Windows user account, cut off from desktop/clipboard/input); micro-VM and WSL Linux containers on the roadmap | announced Build 2026 (2026-06-02), early preview "shortly after Build" | short-lived operations and "sustained workflows"; session isolation is non-interactive at first | **native tier**: a candidate isolation layer *under* `NativeShellProvider`/`NativeShellSessionProvider`, which ADR-071/209 run unsandboxed today | [P] |
| **WSL Containers** (`wslc`) | Linux containers on WSL, with a CLI plus projected C++ and C#/WinRT APIs for apps | public preview in WSL 2.9.3 (June 2026); GA targeted fall 2026 | container lifetime (app-controlled) | a Windows-hosted `ExecutionSurface` candidate that needs no Docker Desktop: Tier 0 or live tiers | [R] |
| **Docker Sandboxes** (`sbx`) | purpose-built VMM on Hypervisor.framework / **Windows Hypervisor Platform** / KVM; one microVM per agent session with a private Docker daemon | launch dated 2026-04-16 on Docker's blog [P]; other sources say January or March 2026 (conflicting) | "disposable by design"; no persistence or snapshot semantics in the architecture post | a per-session sandbox for a *whole agent*, not a shell API. The closest to ADR-209's live tier, but it wraps the agent process, not a tool call | [P] |
| **Docker Cloud Sandboxes** + Sandbox Kits | the same microVM model on Docker-managed cloud; Kits are YAML specs (tools, env, injected credentials, allowed domains, files, startup commands) | 2026-09-24 | hosted sessions | `remote` profile candidate; Kits resemble a declarative grant, which parallels I6 | [R] |
| **Fly.io Sprites** | persistent Linux VMs (Firecracker); copy-on-write checkpoints (~1 s); lifecycle exposed over MCP (create/exec/checkpoint/restore/destroy/list, March 2026) | launched January 2026 | persistent VM. **Restore replaces the writable filesystem and restarts the environment; it does not resume a process at its previous instruction** | **live per session** for files; processes lost on restore. The same trade ADR-209 §5 makes | [R] |
| **Perplexity SPACE** | Firecracker microVM per task; pause, resume, **session forking**, per-session credential isolation | all Perplexity Computer sessions on SPACE from 2026-07-15 | long-running, pausable | **live per session** with pause/fork: beyond ADR-209, which restores no process state | [R] |
| **AWS Bedrock AgentCore Runtime** | dedicated microVM per session; managed session storage (preview, March 2026) persists a mount path across microVMs (files, dirs, **symlinks**, ≤1 GB, 14-day idle retention); runtime reworked 2026-09-18 | 2026 | session up to **8 h**, terminated after **15 min idle** | **live per session** with an idle reap, the same shape as ADR-209's `PerSession` + `LiveShellLimits`. Its storage keeps symlinks, which our Ledger does not (#142) | [R] |
| **Nono** | kernel-enforced per-tool-call sandbox: Landlock (Linux), Seatbelt (macOS), Windows via WSL2 only; "phantom credential" and a trusted proxy that injects real secrets outside the sandbox | 2026-07 | authority expires when the call finishes | **Tier 0** (per call), with a credential-injection model worth comparing to our secret handling | [S] |
| **Microsandbox** (libkrun), **mvm** (libkrun), **Matchlock** (Firecracker), **SlicerVM**, **Unikraft Cloud** | self-hostable OCI-image-as-microVM runtimes | 2026 activity | per sandbox | possible `remote`/self-hosted surfaces; out of scope for local profiles by the locked "no `microvm` profile" decision | [S]/[R] |

## What this means for AgentEngine (observations, not decisions)

1. **MXC is the first OS-provided isolation layer that fits the native tier.** ADR-071 and ADR-209 §9 accept "no
   worktree confinement" for native shells. MXC's process isolation (file/network policy) and session isolation
   (a separate user account) are exactly what they lack, and they come from the OS, so they are not a second local
   isolation technology that the locked decisions forbid. Worth a dedicated check (preview status, API surface, whether
   a Job Object shell can run under it) before ADR-209's native half is built.
2. **WSL Containers could give the Windows host a Docker-free `ExecutionSurface`.** It has C++ APIs. Preview only, so
   it needs a [P] check.
3. **Prior art converges on ADR-209's restore trade.** Sprites restores files, not processes. AgentCore persists files
   and reaps on idle. Only SPACE (per the [R] text) claims pause/resume/fork, which suggests memory snapshots.
   ADR-209's "files + cwd/env, never processes" is the mainstream position, not an outlier.
4. **Symlinks.** AgentCore session storage keeps them; our Ledger skips or fails on them (#142). A link kind in the
   Ledger tree is a follow-on worth weighing.
5. **Locked decisions hold.** Everything microVM-based here stays a `remote`-profile target, not a local profile
   (CLAUDE.md, "No `microvm` sandbox profile").

## Sources (accessed 2026-09-29)

- [P] Windows Developer Blog, "Windows platform security for AI agents" (2026-06-02): https://blogs.windows.com/windowsdeveloper/2026/06/02/windows-platform-security-for-ai-agents/
- [P] Docker, "Why MicroVMs: The Architecture Behind Docker Sandboxes": https://www.docker.com/blog/why-microvms-the-architecture-behind-docker-sandboxes/
- [R] Docker, "Introducing Cloud Sandboxes": https://www.docker.com/blog/introducing-cloud-sandboxes-start-on-your-laptop-finish-in-the-cloud/
- [R] Help Net Security, Docker Cloud Sandboxes / Kits (2026-09-25): https://www.helpnetsecurity.com/2026/09/25/docker-launches-cloud-sandboxes/
- [R] InfoWorld, "Docker Sandboxes and microVMs, explained": https://www.infoworld.com/article/4177309/docker-sandboxes-and-microvms-explained.html
- [S] Help Net Security, "Nono: Open-source sandbox for AI agents" (2026-07-27): https://www.helpnetsecurity.com/2026/07/27/nono-open-source-ai-agent-sandboxing/ (repo: https://github.com/nolabs-ai/nono)
- [S] "Your Container Is Not a Sandbox: The State of MicroVM Isolation in 2026": https://emirb.github.io/blog/microvm-2026/
- [R] Windows Command Line blog, "WSL container is now available for public preview": https://devblogs.microsoft.com/commandline/wsl-container-is-now-available-for-public-preview/
- [R] Fly.io, Sprites: https://fly.io/sprites/ ; Simon Willison on Sprites (2026-01-09): https://simonwillison.net/2026/Jan/9/sprites-dev/ ; bex.co, "Sprites Speak MCP" (2026-08-06): https://bex.co/blog/2026/08/06/sprites-speak-mcp-agent-sandbox-tool
- [R] AWS, AgentCore isolated sessions: https://docs.aws.amazon.com/bedrock-agentcore/latest/devguide/runtime-sessions.html ; session storage (preview, 2026-03): https://aws.amazon.com/about-aws/whats-new/2026/03/bedrock-agentcore-runtime-session-storage ; Unite.AI on the reworked runtime: https://www.unite.ai/aws-reworks-bedrock-agentcore-runtime-for-elastic-memory-fast-cold-starts/
- [R] Perplexity Research, "Making SPACE: Secure Runtimes for Long-Running Agents": https://research.perplexity.ai/articles/making-space-secure-and-efficient-runtimes-for-long-running-agents ; SiliconANGLE (2026-07-15): https://siliconangle.com/2026/07/15/perplexity-launches-secure-sandbox-make-ai-agents-secure-powerful/
- [R] Microsandbox (Bright Coding, 2026-06-30): https://www.blog.brightcoding.dev/2026/06/30/microsandbox-self-hosted-sandboxes-that-boot-in-200ms ; mvm: https://github.com/adgaultier/mvm
