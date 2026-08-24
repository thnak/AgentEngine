# Research record — Dify AI feature comparison against AgentEngine

**Compiled:** 2026-08-24 · **Status:** dated snapshot, not a living document

Ad hoc comparison requested in conversation, not tied to a specific RFC gap. Same discipline as
`2026-08-08-agent-framework-feature-comparison.md`: every Dify claim is sourced from a web search
result (cited per section), every AgentEngine claim is grounded in a direct read of the cited RFC
section or a `codegraph_explore`/`grep` check against current source — no claim from memory. This
record is a design-comparison, not a critique: entries state facts and differences, not
better/worse judgments, except where a gap is explicitly named as such.

## 0. Category framing — read this before the tables

Dify and AgentEngine are not peers. **Dify is a hosted/self-hostable no-code LLMOps *application*
platform** (138k+ GitHub stars, 1M+ deployed apps, per its own blog) — you run it and build inside
its web UI. **AgentEngine is a C++23 *library/engine*** with no UI, no hosting control plane, and
no drag-and-drop authoring surface, by explicit non-goal (`AgentEngineSpecification.md` §2: "Not a
UI," "Not a cloud service"). Every gap below where Dify has a web UI, dashboard, or hosted product
and AgentEngine doesn't is not a bug to fix — it's the difference between "a product" and "a thing
you'd build a product with." Gaps are still recorded plainly because the user asked what Dify has
that AgentEngine doesn't, not because they're all defects.

---

## 1. Sandbox, shell, code interpreter

| Feature | Dify | AgentEngine mechanism | Status | How it differs |
|---|---|---|---|---|
| Sandboxed code execution | **DifySandbox** — separate Go service; all code nodes (Code, Template Transform/Jinja2, LLM node, Code Interpreter tool) route through it; isolation via **seccomp syscall whitelisting** inside one Docker container (not container-per-task), filesystem namespacing, network only via proxy container | Sandbox is a seam with named profiles (`wasm`, `native-jail`, `remote`), same contract, different strength — `SandboxProfileDescriptor`/`ProfileTraits` (`include/agentengine/sandbox/sandbox.hpp`). No `microvm` profile by design (008 §1) | Built (M0-M3) | AgentEngine ties sandbox strength to capability-typed enforcement (I2) rather than syscall whitelisting alone; Dify's is a single-strength Docker+seccomp service with no equivalent of AgentEngine's `remote` profile for hardware-isolation workloads. |
| Code interpreter | Python and Node.js ("technical capacity to embrace more languages," per own docs); general locked-down scripting sandbox, no stated data-science-library emphasis | `PythonLockdownInterpreter` (`src/backends/native_jail/python_lockdown.hpp`) — embedded **native CPython**, meta-path-finder import allowlist (`allowed_top_level_modules`) + disjoint `caller_gated_modules` tier for names a granted package's internals need but guest code must not reach directly (`ctypes`, `subprocess`, `winreg`), closing sys.modules cache-hit and frame-walk bypasses (ADR-002, ADR-003) | Built (M3, Judged ADRs) | AgentEngine's design goal is a Python that can genuinely `import numpy`/`pandas`; Dify's is a general execution sandbox for workflow glue code, not positioned around a native library ecosystem. |
| Shell/terminal | **No first-class shell node.** No built-in "run a shell command" capability in core workflow; shell/remote-command access exists only via a community SSH plugin, and a full "External Agentic Node" (Claude-Code/Codex-style autonomous coding) is an open, unshipped feature request (#35940) | `ShellRunner`/`MediatedShellRunner` (`src/backends/native_jail/shell_runner.hpp`, `mediated_shell_runner.hpp`) — engine-native: whole-script-parses-before-anything-executes (ADR-001), dispatches only to registered builtins/`Runner`s/`Tool`s over a capability-scoped `FileSystemAdapter`; never resolves a name against `$PATH`, so there is no ambient exec surface to sandbox | Built (M3) | Genuine capability gap in Dify's favor of *availability* (a real shell is one plugin install away in Dify's ecosystem) but AgentEngine's is a first-class, equally-mediated engine subsystem — not a bolt-on. Python and Shell share one `ExecState` in AgentEngine (a `cd` or exported var is visible to both); no equivalent stated for Dify's Code node vs. any shell plugin. |

**Sources:** [Introduction to DifySandbox](https://dify.ai/blog/dify-ai-blog-introducing-difysandbox) ·
[DifySandbox Goes Open Source](https://dify.ai/blog/difysandbox-goes-open-source-secure-execution-of-code) ·
[Code - Dify Docs](https://docs.dify.ai/en/use-dify/nodes/code) ·
[langgenius/dify-sandbox](https://github.com/langgenius/dify-sandbox) ·
[Feature Request #35940](https://github.com/langgenius/dify/issues/35940) ·
[Agent - Dify Docs](https://docs.dify.ai/en/use-dify/nodes/agent)

---

## 2. Built-in / default tools

| Feature | Dify | AgentEngine mechanism | Status | How it differs |
|---|---|---|---|---|
| First-party built-in tools | **50+ shipped tools**: Google Search, DALL·E, Stable Diffusion, WolframAlpha, Wikipedia, Web Scraper, Yahoo Finance, YouTube, Chart Generator, Current Time, etc. — installable/enabled with a click, maintained by the Dify team via the Marketplace (all tools moved to plugins since v1.0.0) | `Tool` concept + 10-step invocation pipeline (006), capability-gated by construction | **Zero shipped built-in tools** — confirmed by grep: `WebSearch` appears only as an illustrative example (`Tools<WebSearch, CodeInterpreter, Handoff<Writer>>`) in README/spec, never as an implemented tool under `src/` or `include/` | Named a genuine gap in the M7-era comparison doc (`2026-08-08-agent-framework-feature-comparison.md` §2): "Web search: only an illustrative `WebSearch` pattern... not a shipped built-in." Design philosophy differs: AgentEngine treats a tool as an author-declared, capability-scoped WASM component (009 §7's C/C++ library track — SQLite, tree-sitter, ONNX Runtime via `wasi-sdk`), not a vendor-hosted black-box endpoint the engine calls for you. |
| Tool marketplace / ecosystem | Dify Marketplace — community + official plugins (tools, models, agent strategies, bundles) | WASM Component Model plugin ABI (009) is locked and spec'd | Spec'd, no ecosystem/marketplace exists | Sharpest concrete gap in this whole comparison: Dify gives "web search" with zero code; AgentEngine requires authoring the tool yourself against the `Tool` plane, and there's nowhere to install a pre-built one from yet. |

**Sources:** [Dify GitHub](https://github.com/langgenius/dify) ·
[Dify Tools - Dify Docs](https://docs.dify.ai/en/cloud/use-dify/workspace/tools) ·
[Dify Marketplace](https://marketplace.dify.ai/) ·
[Dify AI Agents Guide 2026](https://dify-hosting.com/en/guides/dify-agent/) ·
[langgenius/dify-official-plugins](https://github.com/langgenius/dify-official-plugins)

---

## 3. Platform/product-layer features

| Feature | Dify | AgentEngine mechanism | Status | How it differs |
|---|---|---|---|---|
| Visual workflow builder (no-code) | Drag-and-drop canvas; production API endpoint with no orchestration code | C++ CRTP (002) / declarative YAML (015) — a developer authoring surface | Built | No no-code/citizen-developer path exists or is planned — non-goal. |
| Hosted deployment / multi-tenant SaaS | One-click deploy as API/chatbot/webapp; Deployment Hub | Host code owns the socket/listener (ADR-061, Judged); AgentEngine does not implement HTTP networking itself | Not a hosting product, by locked decision | AgentEngine hands already-parsed requests to host code; no billing/tenancy product exists at the engine layer. |
| RAG as a **product** (ingestion UI, chunking pipeline, knowledge-base screen) | Built-in, covers PDF/PPT/doc text extraction through retrieval | Primitives exist: `corpus_source`, `corpus_chunk`, `vector_index`, `embedder`, `vector_rag_context_provider` (grep-confirmed under `include/agentengine/core/`); default memory retrieval is deterministic (salience×recency×tag, 029 §1), vectors are an explicit opt-in upgrade | Primitives built (M4); no ingestion UI/pipeline product | Reverse default emphasis: Dify is vector-first by default; AgentEngine's memory system deliberately rejects vector-first as a default on I3/I5 replay grounds. |
| Prompt IDE | Yes | None | Not built, not spec'd as a product | — |
| App archetypes with instant deploy (Chatbot/Agent/Text Generator/Workflow/Chatflow, each → web app + API + embeddable widget) | Yes, 5 types | No equivalent | Non-goal | AgentEngine has no "publish this agent as a web app" feature. |
| Annotation & human-curated dataset loop | Annotated replies — human edits responses in-UI, feeds back as high-confidence Q&A pairs | `require_approval` filter verdict (017 §4) + `Interaction`/`InputRequired` suspend points (001 §2, 014 §4) for HITL | Mechanism spec'd (017 is M8, not started); no UI to curate replies into a dataset | Structural: no UI exists to build a curation surface on top of the mechanism. |
| Evaluation & monitoring dashboard (logs, feedback, latency, usage) | Built-in dashboard | RFC 016 (Observability, OTel GenAI-conformant) + RFC 022 (Testing and Evaluation — deterministic simulation, golden traces) | **016 is Milestone 8, not started** | Even once built, 016 emits spans/audit records for a host's dashboard — AgentEngine does not ship a dashboard itself, by design. |
| App DSL YAML export/import, version-controllable configs | Yes — export/import from app sidebar, version check on import | **I6 invariant** (RFC 015, Declarative Agent Format) — YAML/JSON and C++ CRTP required to compile to *byte-identical* metadata, validated by the same validator | Tested equivalence gate (not merely export convenience) | Arguably a strength: AgentEngine's is a tested equivalence gate, not just a config-dump/reload convenience — though 015's full build status (loader, M7 binding into 014's validator) needs independent reconfirmation before citing as fully shipped. |
| Enterprise SSO (SAML/OIDC), SCIM provisioning | Yes (Dify Enterprise) | RFC 018 (Identity, Authorization and Secrets — principals, admission vs. effect authorization, multi-tenancy) | Engine primitives built (M5, complete per README) | No SSO integration, no team/workspace UI — host-application concern, pushed outward by design. |
| RBAC (workspace/team, workflow-level, custom roles) | Yes (Dify Enterprise) | Principal/capability model (007, 018) | Built at primitive level | No team/workspace product exists to attach roles to. |
| Audit logs streamed to SIEM, PII-redacted prompt history | Yes (Dify Enterprise) | I4 — every effect attributable, OTel span + audit record always | Mechanism built/spec'd; SIEM streaming is a host integration point | No SIEM-streaming product or redacted-history viewer ships — host wires the emitted records wherever it wants. |
| Enterprise deployment tooling (Helm charts, Terraform for AWS/GCP/Azure, per-business-unit quotas/billing on one cluster) | Yes (Dify Enterprise) | RFC 020 (Configuration and Hosting) specs hosting shapes | **Milestone 9 (hosting/platform) not started** | No deployment tooling exists yet at all. |

**Sources:** [Dify AI Review 2026](https://www.gptbots.ai/blog/dify-ai) ·
[Dify Explained (SandBase)](https://www.sandbase.ai/blog/dify-ai-platform-explained-2026/) ·
[Key Concepts - Dify Docs](https://docs.dify.ai/en/use-dify/getting-started/key-concepts) ·
[Dify Workflow](https://hellodify.com/en/docs/workflow) ·
[Manage Apps - Dify Docs](https://docs.dify.ai/en/use-dify/workspace/app-management) ·
[DSL apps import/export discussion #9007](https://github.com/langgenius/dify/discussions/9007) ·
[Dify Enterprise](https://dify.ai/dify-enterprise) ·
[Welcome to Dify Enterprise Docs](https://enterprise-docs.dify.ai/en/2.8.x/use/introduction) ·
[Dify.AI: Open-Source LLMOps Platform](https://dify.ai/blog/open-source-llmops-platform-define-your-ai-native-apps) ·
[Guide to Dify (Medium)](https://medium.com/@tubelwj/guide-to-dify-an-open-source-platform-for-developing-large-language-model-llm-applications-e6cc2d39ecdf) ·
[Dify v1.6.0: Built-in Two-Way MCP Support](https://dify.ai/blog/v1-6-0-built-in-two-way-mcp-support) ·
[Turn Your Dify App into an MCP Server](https://dify.ai/blog/turn-your-dify-app-into-an-mcp-server)

---

## Summary

**Genuine gaps (AgentEngine could plausibly close, none currently even spec'd as products):**
built-in/default tools (web search, image gen, etc.) and a tool marketplace; annotation/HITL
curation UI; evaluation/monitoring dashboard product; enterprise deployment tooling (Helm/Terraform).

**Spec'd but not built (M7+):** Observability (016), Testing and Evaluation (022) as a product
surface, Safety/Content Governance filters (017) including `require_approval` HITL UI hooks,
Configuration and Hosting (020) deployment shapes.

**Non-goals, not gaps (Dify is a hosted app; AgentEngine is a library):** no-code visual builder,
hosted multi-tenant SaaS, app-archetype instant web-app deployment, workspace/team UI, SSO/RBAC
product surface (the underlying identity primitives are built — 018/007 — just not a UI on top).

**Different by design, not a gap either way:** RAG default retrieval (deterministic vs. vector-first);
declarative config equivalence as a tested gate (I6/015) vs. an export convenience; shell/Python
sharing one mediated `ExecState` vs. Dify's separate sandbox service plus optional SSH plugin.
