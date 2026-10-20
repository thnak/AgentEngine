# ADR-243 — What is AgentEngine's public C++ API and layer structure, so that a developer builds a multi-user agent server in a few dozen lines, with every managed piece replaceable?

**Status:** Proposed (2026-10-20). Design; red-team pending; the prove phase runs per migration slice
(§5). Implements [ADR-242](ADR-242-direction-reset-developer-engine-linux-server-explicit-choice.md)
§6 step 2 (R5, R6, R8, §2's "least developer code" measure and §9's line-budget question).

## 1. The question, and the baseline

ADR-242 makes the C++ API the product: "managed by the engine, replaceable by the developer", nothing
chosen implicitly. The question, with a wrong answer: **does the developer keep composing engine
internals (ledgers, quotas, identity, staging paths, template parameter lists), or does the engine
expose a small set of type-erased handles over layers that match their directories?** Keeping today's
surface is wrong if the reference product cannot be written briefly, which is the case:

| Program (main @ 7f224166) | Code lines (no comments/blanks) | What the developer must know |
|---|---|---|
| `examples/30_session_builder_quickstart_live.cpp`, one question to a model | 66 | builder + capabilities struct |
| `tools/containerd_shell_chat.cpp`, single-user shell chat | 94 | orphan sweep; `Ledger`; `IdentityAuthority::bootstrap`; three `AsyncQuota::mint_root`; `block_on` a root branch; two staging directories; `bind_sandbox(...)` with 7 arguments; a 4-type builder template |
| A multi-user agent server with a code interpreter | does not exist | socket, HTTP, TLS, auth, wire binding, per-request authority, tenancy (ADR-242 §3a) |

And the structural facts behind it (from the 2026-10-20 review):

- `AgentSession<ChatClientT, StateT, HistoryProviderT>` (`rt/agent_session.hpp:226`) and 4-parameter
  free functions are templates, although the turn loop is already non-template (`AgentSessionCore`) and
  model calls are already virtual. Its header is referenced directly by 148 files.
- The slowest translation units on the build server (17–20 s each, `.ninja_log` of 2026-10-19, mixed
  incremental build, indicative only) are the ones that instantiate the templated provider chat clients.
- Directories do not match layers: `rt/` (labelled L0) holds the L2 session and L3 workflows; `core/`
  (L2) holds V, L0 and L1 files; about 60 per-file overrides make the layering lint pass, 8 upward
  edges are baselined, and directories form cycles (`rt → core` 111 edges and `core → rt` 35;
  `core ↔ trust` 27 each way).

## 2. Decision

### 2.1 Directories are layers

| Layer | Directory (`include/agentengine/`) | Contents |
|---|---|---|
| V | `vocab/` | `result`/`error`, JSON/YAML value, content model, run events, `Tainted<T>` |
| L0 | `runtime/` | `task`, channels, `Runtime`, lanes, stores, `http`, TLS, `pal/` |
| L1 | `trust/` | capabilities, identity and authority, secrets, budgets and quotas |
| L1 | `sandbox/` | `SandboxBackend` (with the worktree verbs, ADR-242 R8), registry, egress; `backends/` beside it |
| L1 | `worktree/` | worktree, ledger, mounts, harvest |
| L2 | `agent/` | `Session`, tool pipeline, context providers, the model plane (`ChatClient`, `AnyChatClient`), memory |
| L2 | `providers/` | outbound conformers: OpenAI, Anthropic, embedders, vector indexes |
| L3 | `orchestration/` | workflows, multi-agent, projects, evaluation |
| L4 | `protocol/` | MCP, A2A, AG-UI dispatchers (typed, transport-agnostic) |
| L5 | `server/` | optional `agentengine_server` target: transports, listener, auth bridges (ADR-242 R4) |
| L6 | `engine/` | the developer facade: `Engine`, `AgentSpec`, presets; `agentengine.hpp` umbrella |

Rules: a file's layer is its directory, with **zero** per-file overrides; a layer includes only lower
layers; **no cycles between directories** (a new lint, G-A4); the 8 baselined upward edges are fixed by
splitting the files named in `tools/layering_baseline.txt`. `src/` mirrors `include/`.

### 2.2 The developer surface: four handles

The developer works with **`Engine`** (one per process: the Runtime, registries, secrets, stores),
**`AgentSpec`** (what one agent is: model, tools, providers, sandbox kind, limits, policy),
**`Session`** (one conversation, one per user and agent), and, if linked, **`Server`**. All four are
ordinary non-template classes; none exposes a template parameter to the developer.

The reference application, which G-A1 measures (sketch; names are settled in slice 1):

```cpp
#include <agentengine/agentengine.hpp>
#include <agentengine/server.hpp>
using namespace agentengine;

int main() {
    auto engine = Engine::create({
        .runtime   = runtime::all_cores(),
        .secrets   = secrets::env("AE_SECRET_"),
        .sandboxes = {backends::kata({.image = "registry.example/ae-python@sha256:...",
                                      .network = "ae-kata-net"})},
        .store     = stores::file("/var/lib/agentengine"),
    });
    if (!engine) return fail(engine.error());

    auto analyst = engine->agent({
        .name      = "analyst",
        .model     = models::openai("gpt-5", secret("openai-api-key")),
        .sandbox   = "kata",
        .limits    = presets::server_limits(),   // budgets, turns, resource limits: an explicit bundle
        .policy    = presets::server_policy(),   // approval, concurrency, telemetry, strictness
        .providers = {providers::shell(), providers::codeact()},
        .tools     = {tools::web_fetch({"api.example.com:443"})},
    });
    if (!analyst) return fail(analyst.error());

    auto server = server::http(*engine, {
        .listen    = "0.0.0.0:8443",
        .tls       = tls::files("cert.pem", "key.pem"),
        .auth      = auth::bearer_jwt("https://idp.example/.well-known/jwks.json"),
        .authority = [](Principal const& who) { return my_grants_for(who); },  // root of trust, ADR-242 R7
        .agents    = {*analyst},
        .protocols = {protocol::mcp, protocol::a2a, protocol::agui},
    });
    if (!server) return fail(server.error());
    return server->run();
}
```

Every ADR-242 required choice is present: the backend, the authority source (`.authority`, per
request), budgets and limits and policy (as named presets the developer picks), the Runtime, the secret
source, the transport. Leaving one out is a named startup error listing the options (ADR-242 G-R13).

A single-user program is the same `Engine` and `AgentSpec` plus `engine->session(analyst, who)` and
`session.ask(...)`.

### 2.3 Managed, but every piece is replaceable

Each seam has a **concept** (for implementers who want the compile-time check) and an **`Any…`
type-erased holder** (what the API takes), so a custom implementation is passed exactly where the
engine's own option would go:

| Seam | Concept | API holder | Engine options |
|---|---|---|---|
| Model | `ChatClient` | `AnyChatClient` | `models::openai`, `models::anthropic`, decorators (failover, recording, replay) |
| Execution | `SandboxBackend` (+ worktree, ADR-242 R8) | `AnySandboxBackend` | `backends::kata`, `native_jail`, `wasm`, `none` (`NoSandboxBackend`) |
| Context | `ContextProvider` | `AnyContextProvider` | `providers::shell`, `codeact`, RAG, memory |
| Tool | `Tool` | `ToolDescriptor` (already erased) | `tools::*`, MCP-imported tools |
| Secrets | `SecretSource` | `AnySecretSource` | `secrets::env`, `secrets::file` |
| Stores | `SessionStore`, `AppendLogStore`, `WorktreeObjectStore` | `Any…Store` | `stores::file`, `stores::memory` |
| Authority | function `Principal → CapabilitySet` | `AuthorityPolicy` | none: this is the developer's root of trust (ADR-242 R7) |
| Approval | `PolicyDecider` (ADR-070) | same | `presets::*_policy` |
| Transport | `Transport` (server module) | `AnyTransport` | `server::http`, or the host's own socket (ADR-039 path) |
| Workflow node | `Executor` | `AnyExecutor` | agents, sub-workflows |

Erasure costs one indirect call per seam crossing, which is noise against a model or sandbox round
trip; a seam on a **measured** 023 hot path may keep a template fast path (ADR-242 R5), and must show
the measurement.

### 2.4 What goes away

- `Session` replaces `rt::AgentSession<ChatClientT, StateT, HistoryProviderT>`; typed session state
  becomes `session.state<T>()` over the session's store, not a class template parameter.
- `quickstart::*SessionBuilder` and `Bundle` are folded into `Engine`/`AgentSpec`; the 4-type
  `ComposedQuickstartSessionBuilder<...>` goes.
- Developers stop touching `Ledger`, `IdentityAuthority`, `AsyncQuota`, staging directories and
  `block_on`: the engine owns them per agent and sandbox (ADR-242 R8). They remain public at their own
  layer for anyone building a custom seam.
- `Agent<Derived, Policies...>` (CRTP) stays only as optional sugar that produces an `AgentSpec`.

## 3. Alternatives

- **A. A facade only, over today's templates.** Cheapest; but every custom seam still has to fit a
  template parameter list, compile cost stays, and the directory/layer mismatch stays. Rejected.
- **B. A C ABI first, C++ on top.** Good for bindings (deferred, ADR-242 R5), but makes the C++ surface
  second-class. Rejected for now; the `Any…` holders are the natural C ABI boundary later.
- **C. Chosen: §2** (directories are layers, four non-template handles, concept + `Any…` per seam).

## 4. Gates

| Gate | Claim | Disproof / positive control |
|---|---|---|
| **G-A1** | Least developer code | CI counts code lines of three reference programs, built and run against a scripted model: hello agent ≤ 15, single-user shell chat ≤ 30, the §2.2 multi-user server ≤ 60 (ADR-242 G-R6). A program over budget fails; positive control: padding one program past its budget fails the check |
| **G-A2** | Compile time improves | Clean `ninja` build on the build server (gcc and clang, all cores), total and top-20 TU times, before and after each slice; the slice that makes `Session` non-template must cut the time of TUs that include it; numbers recorded, not asserted |
| **G-A3** | Erasure is free where claimed | A bench of one seam crossing through `AnyChatClient` and `AnySandboxBackend` vs the direct call, reported in ns against a measured model round trip; any seam on a 023 hot path keeps a template path or shows the budget still holds |
| **G-A4** | Directories are layers | The layering lint runs with an empty `[files]` override table and an empty baseline; a new directory-cycle check fails on any cycle; positive control: a planted upward include and a planted cycle each fail |
| **G-A5** | Every seam is replaceable | For each row of §2.3, a test passes a custom implementation through the public API exactly where the engine's option goes, and it is used; positive control: the same test with the engine's option still passes |
| **G-A6** | Nothing implicit survives the API | The ADR-242 G-R13 cases expressed through `Engine::create` and `engine->agent`; a missing choice is a named error listing the options |

## 5. Migration, in slices (each its own PR, each proves its gates)

1. **Facade over today's code:** `Engine`, `AgentSpec`, `Session` (wrapping the current types),
   presets, `agentengine.hpp`; the three reference programs and G-A1 in CI. Freezes the names.
2. **Type erasure:** `AnyChatClient` and a non-template `Session`; G-A2 and G-A3 measured.
3. **Directory moves, one layer per PR**, bottom up (`vocab`, `runtime`, `trust`/`sandbox`/`worktree`,
   `agent`/`providers`, `orchestration`, `protocol`); overrides and baseline drop to zero; G-A4.
4. **Seams:** the remaining `Any…` holders and G-A5, aligned with ADR-242 step 3 (one execution seam).
5. **Server module:** `server/` per ADR-242 R4.

Slices 2 and 3 touch the same headers as ADR-237's remaining work (ADR-242 §6 step 4); they are
sequenced, not parallel. No forwarding headers are kept for moved files: this repository is the only
consumer, and every include is rewritten in the same PR.

## 6. Residuals

- **A large, repo-wide churn.** Directory moves rewrite most include lines; each slice is a big diff,
  and `git blame` across moves needs `--follow`.
- **Names are provisional** until slice 1 freezes them; the §2.2 sketch is a target, not an API.
- **The line budgets are a choice**, not a measurement of anything external; they are set where the
  sketch lands with some slack, and tightening them later is cheap.
- **Compile-time gain is a hypothesis** until G-A2 measures it; the 2026-10-19 timing data is
  incremental and indicative only.

## 7. What the red-team should attack

- Whether the four handles really hide the internals, or a realistic custom seam (a custom
  `SandboxBackend`, a custom authority policy) forces the developer back into `Ledger`/`AsyncQuota`.
- Whether `.authority` as a plain function keeps I2/I3: it must be host code only, never reachable from
  model output, and its result must still be checked by the existing capability machinery.
- Whether presets quietly become defaults (ADR-242 §2: a preset must be named to apply).
- Whether `session.state<T>()` loses the type safety `StateT` gave, or the snapshot/restore guarantees.
- Whether the directory map creates new cycles (for example `agent/` and `sandbox/` around the
  shell/CodeAct providers, or `worktree/` and `sandbox/` around harvest).
