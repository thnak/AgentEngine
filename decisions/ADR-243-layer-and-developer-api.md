# ADR-243 — What is AgentEngine's public C++ API and layer structure, so that a developer builds a multi-user agent server in a few dozen lines, with every managed piece replaceable?

**Status:** Proposed (2026-10-20). Design and red-team (round 1, §7) done; the prove phase runs per
migration slice (§5). Implements
[ADR-242](ADR-242-direction-reset-developer-engine-linux-server-explicit-choice.md) §6 step 2 (R5, R6,
R8, §2's "least developer code" measure and §9's line-budget question).

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
| V | `vocab/` | `result`/`error`, JSON/YAML value, content model, run events, `Tainted<T>`, the `FilesystemAdapter` interface (today `sandbox/filesystem_adapter.hpp`, which includes only `error.hpp`) |
| L0 | `runtime/` | `task`, channels, `Runtime`, lanes, stores, `pal/` (reactor, raw TLS stream) |
| L1a | `trust/` | capabilities, identity and authority, secrets, budgets and quotas |
| L1b | `worktree/` | worktree, ledger, mounts, harvest |
| L1c | `sandbox/` | `SandboxBackend` (with the worktree verbs, ADR-242 R8), registry, egress, the outbound HTTP client (it needs egress), and the **tool-dispatch callback** the code-mode bridges call (below) |
| L2 | `agent/` | `AgentSpec`, `Session`, tool pipeline, context providers, the model plane (`ChatClient`, `AnyChatClient`), memory, YAML agent compiler, `agent_spawn_capability` |
| L2 | `providers/` | outbound conformers: OpenAI, Anthropic, embedders, vector indexes |
| L3 | `orchestration/` | workflows, multi-agent, projects, evaluation |
| L4 | `protocol/` | MCP, A2A, AG-UI dispatchers (typed, transport-agnostic) |
| L5 | `engine/` | the developer facade: `Engine`, presets, `agentengine.hpp` umbrella (L5) |
| L6 | `server/` | optional `agentengine_server` target: transports, listener, auth bridges (ADR-242 R4); `server.hpp` umbrella (L6) |

Rules:

- A file's layer is its directory, with **zero** per-file overrides and **zero** inline
  `ae-layering-lint: allow` comments.
- A file includes only its own directory and directories of a **strictly lower** layer. Within L1 the
  order is `trust` < `worktree` < `sandbox`; `agent/` and `providers/` are peers, and `providers/`
  includes `agent/` only (never the reverse).
- **No cycles between directories** (a new lint, G-A4). The 8 baselined upward edges are fixed by
  splitting the files named in `tools/layering_baseline.txt`.
- **Code mode is an injected callback, not an include.** Today `tool_bridge.hpp` and
  `agent_tools_codegen.hpp` (sandbox side) include `core/tool_pipeline.hpp` (agent side), a deliberate
  seam (010 §6; ADR-242 G-R8: dispatch "like a model tool call"). After this ADR, `sandbox/` defines a
  `ToolDispatch` callback type over V types, and `agent/` passes it in when it builds the sandbox. The
  sandbox never names the pipeline.
- **`backends/` and `src/` follow the same map by file, not by directory.** Each backend is split by
  layer: the reactor and mbedTLS backends go under `runtime/backends/`; native_jail, native_process,
  wasm, kata and `NoSandboxBackend` go under `sandbox/backends/`; the Vulkan vector index goes under
  `providers/`. `plugin/`, `tools/` (built-in tools: L2), `testing/` (the layer of what it fakes) and
  `detail/` (its parent's layer) are mapped in slice 3's PR, with no file left unassigned.

### 2.2 The developer surface: four handles

The developer works with:

- **`Engine`** (L5): one per process; it holds the Runtime, registries, secrets and stores.
- **`AgentSpec`** (L2): what one agent is (model, tools, providers, sandbox kind, limits, policy).
  `Agent<Derived,…>` sugar, the YAML compiler (I6), workflow nodes and protocol dispatchers all produce
  or consume the same `AgentSpec`. That is why it sits in `agent/`, not in the facade.
- **`Session`** (L2): one conversation. It may run several agents (ADR-242 R8 rule 1), each in its own
  sandbox.
- **`Server`** (L6), if linked.

All four are ordinary non-template classes; none exposes a template parameter to the developer.

The reference application, which G-A1 measures (sketch; names are settled in slice 1):

```cpp
#include <agentengine/agentengine.hpp>
#include <agentengine/server.hpp>
using namespace agentengine;

task<result<RequestAuthority>> grants(AuthenticatedPrincipal const& who);  // counted by G-A1: the developer writes it

int main() {
    auto engine = Engine::create({
        .runtime   = runtime::all_cores(),
        .secrets   = secrets::env("AE_SECRET_"),
        .sandboxes = {backends::kata({.image   = "registry.example/ae-python@sha256:...",
                                      .runtime = "io.containerd.kata.v2",
                                      .network = "ae-kata-net"})},
        .store     = stores::file("/var/lib/agentengine"),
    });
    if (!engine) return fail(engine.error());

    auto analyst = engine->agent({
        .name      = "analyst",
        .model     = models::openai("gpt-5", secret("openai-api-key")),
        .sandbox   = "kata",
        .ceiling   = caps::exec() | caps::net_out("api.example.com:443"),  // what any request may get, at most
        .limits    = presets::server_limits(),   // budgets, turns, resource limits: an explicit bundle
        .policy    = presets::server_policy(),   // approval, concurrency, telemetry, strictness
        .providers = {providers::shell(), providers::codeact()},
        .tools     = {tools::web_fetch()},
    });
    if (!analyst) return fail(analyst.error());

    auto server = server::http(*engine, {
        .listen    = "0.0.0.0:8443",
        .tls       = tls::files("cert.pem", "key.pem"),
        .auth      = auth::bearer_jwt("https://idp.example/.well-known/jwks.json"),
        .authority = grants,                     // root of trust, ADR-242 R7
        .tenancy   = tenancy::per_principal(),   // secrets, budgets and stores scoped per tenant
        .admission = presets::server_admission(),// sessions/runs per principal, request size, store bounds
        .agents    = {*analyst},
        .protocols = {protocol::mcp, protocol::a2a, protocol::agui},
    });
    if (!server) return fail(server.error());
    return server->run();
}
```

Every ADR-242 required choice is present: the backend and its runtime, the authority source, the
ceiling, budgets, limits and policy (as named presets the developer picks), tenancy, admission bounds,
the Runtime, the secret source and the transport. Leaving one out is a named startup error listing the
options (ADR-242 G-R13). `unbounded` is an accepted, explicit value wherever a bound is required
(ADR-242 §9 decision 1).

**Authority** (ADR-242 R7, ADR-070 §4a, I2/I3):

- `AuthorityPolicy` is `task<result<RequestAuthority>>(AuthenticatedPrincipal const&)`.
  `RequestAuthority` carries the capability set, the tenant and an expiry.
- `AuthenticatedPrincipal` is produced only by the transport's auth bridge (ADR-061). Nothing derived
  from a request body or from model output can construct it.
- The engine intersects the result with the agent's `.ceiling`. A result wider than the ceiling is
  narrowed and audited, never minted: the policy decides among authority the host already holds.
- The server forces `require_authority` on. A session is bound to the principal and tenant that created
  it; another principal resuming it by id is refused.

A single-user program is the same `Engine` and `AgentSpec` plus
`engine->session(analyst, authority::single_trusted_caller(caps))` and `session.ask(...)`. The single
trusted caller is a named choice (ADR-242 §2), never the default when the authority argument is left
out.

### 2.3 Managed, but every piece is replaceable

Each seam has a **concept** (for implementers who want the compile-time check) and one **type-erased
holder** (what the API takes), so a custom implementation is passed exactly where the engine's own
option would go. Where a seam is already erased, this ADR names it rather than adding a second holder.

| Seam | Concept / interface today | API holder | Engine options |
|---|---|---|---|
| Model | `ChatClient` | `AnyChatClient` (new) | `models::openai`, `models::anthropic`, decorators (failover, recording, replay) |
| Execution | `SandboxBackend` (+ worktree, ADR-242 R8) | `RegisteredSandboxBackend`, **extended** with the R8 verbs and renamed `AnySandboxBackend` | `backends::kata`, `native_jail`, `wasm`, `none` (`NoSandboxBackend`) |
| Context | `ContextProvider` | `AnyContextProvider` (new) | `providers::shell`, `codeact`, RAG, memory |
| Tool | none (data only) | `ToolDescriptor` (already erased) | `tools::*`, MCP-imported tools |
| Secrets | `SecretStore` concept; `SecretSource` virtual base (`trust/secret.hpp:123`) | `SecretSource` (already erased) | `secrets::env`, `secrets::file` |
| Stores | `SessionStore`, `AppendLogStore`, `WorktreeObjectStore` | `Any…Store` | `stores::file`, `stores::memory` |
| Authority | `AuthorityPolicy` (§2.2) | the same `task`-returning function | none: the developer's root of trust (ADR-242 R7) |
| Approval | `PolicyDecider`, already a `std::function` (`tool_pipeline.hpp:179`) | the same | `presets::*_policy` |
| Transport | new: `Transport` (server module) | `AnyTransport` | `server::http`, or the host's own socket (ADR-039 path) |
| Workflow node | new: a node behaviour concept (today's `Executor` in `graph.hpp:106` is a data struct) | `AnyNode` | agents, sub-workflows |

**Erasure keeps every compile-time facet that carries an invariant.** Today `rt/agent_session.hpp`
chooses these at compile time:

- the gateway call/stream path versus the plain chat path (`:407-430`);
- `capabilities().tool_calling`, which gates ADR-198's undeclared-tool-call check (I3, `:437`, `:462`);
- `HasProducerChatClientId` (`:497`) and `InteractiveChatClient` (ADR-230, `:511`);
- `HasOnRunEnd` and ADR-116's `bind_owner` on the history provider (`:261`, `:487`);
- `fork_from`'s child-branch copy.

Each `Any…` holder probes these facets when it is built, stores them, and answers them at run time with
the same behaviour. Forking is an explicit `clone()` verb on the holder. G-A5 has one test per facet,
including "an undeclared tool call through `AnyChatClient` is refused".

`task<T>`, `stream<T>` and cancellation (through `EffectContext::cancellation`) are concrete types and
pass through erasure unchanged. A task returned by a holder borrows the holder, and the holder must
outlive it.

Erasure costs one indirect call per seam crossing, which is noise against a model or sandbox round
trip. A seam on a **measured** 023 hot path may keep a template fast path (ADR-242 R5), and must show
the measurement.

### 2.4 What goes away

- **The session template goes.** `Session` replaces
  `rt::AgentSession<ChatClientT, StateT, HistoryProviderT>`. Typed state becomes an in-memory
  `AnyState`, fixed when the session is built, read with `session.state<T>()`, which returns
  `result<T&>` and checks the type id. It is not in the snapshot, as `StateT` is not today
  (`save_agent_session_snapshot` writes only `AgentSessionRecord`), and the session gains no ambient
  store access (I2). Durable typed state is a separate, explicit opt-in with its own codec, not part of
  this ADR.
- **The quickstart builders fold in.** `quickstart::*SessionBuilder` and `Bundle` are folded into
  `Engine`/`AgentSpec`, and the 4-type `ComposedQuickstartSessionBuilder<...>` goes.
- **Developers stop touching the internals.** `Ledger`, `IdentityAuthority`, `AsyncQuota`, staging
  directories and `block_on` are owned by the engine per agent and sandbox (ADR-242 R8). They remain
  public at their own layer for anyone building a custom seam.
- **CRTP becomes sugar.** `Agent<Derived, Policies...>` stays only as optional sugar that produces an
  `AgentSpec`.

## 3. Alternatives

- **A. A facade only, over today's templates.** Cheapest; but every custom seam still has to fit a
  template parameter list, compile cost stays, and the directory/layer mismatch stays. Rejected.
- **B. A C ABI first, C++ on top.** Good for bindings (deferred, ADR-242 R5), but makes the C++ surface
  second-class. Rejected for now; the `Any…` holders are the natural C ABI boundary later.
- **C. Chosen: §2** (directories are layers, four non-template handles, one erased holder per seam).

## 4. Gates

| Gate | Claim | Disproof / positive control |
|---|---|---|
| **G-A1** | Least developer code | CI formats three reference programs with the repo's `.clang-format`, then counts every non-blank, non-comment line the developer writes, including their own helpers (such as `grants`). The programs are built and run against a scripted model: hello agent ≤ 15, single-user shell chat ≤ 30, the §2.2 multi-user server ≤ 60 (ADR-242 G-R6). A program over budget fails. Positive control: padding one program past its budget fails the check |
| **G-A2** | Compile time is measured | Clean `ninja` build on the build server (gcc and clang, all cores), total and top-20 TU times, before and after each slice, recorded in the slice's PR. A slice that makes compile time worse by more than 10 % in total needs a written reason in its PR |
| **G-A3** | Erasure is cheap where claimed | A bench of one seam crossing through each holder that exists in that slice, against the direct call, reported in ns. A crossing over 1 µs fails, unless it is off every 023 hot path and the PR says so. Any seam on a 023 hot path keeps a template path or shows that the 023 budget still holds |
| **G-A4** | Directories are layers | The layering lint runs with an empty `[files]` override table, an empty baseline and no inline allows; a new directory-cycle check fails on any cycle. Positive control: a planted upward include, a planted same-layer cycle and a planted inline allow each fail |
| **G-A5** | Every seam is replaceable | For each row of §2.3, a test passes a call-counting custom implementation through the public API exactly where the engine's option goes and asserts that it was called. Positive control: a facade that silently uses the engine's option instead fails the test. One test per facet in §2.3's list |
| **G-A6** | Nothing implicit survives the API | The ADR-242 G-R13 cases expressed through `Engine::create`, `engine->agent`, `engine->session` and `server::http`; a missing choice is a named error listing the options |

## 5. Migration, in slices (each its own PR, each proves its gates)

Ordering with ADR-242 §6:

- **Step 1b first.** It closes the implicit permissive choices; G-A6 cannot pass before it.
- **ADR-242 step 3's deletions before slice 3.** `execution_surface`, `persistent_shell` and the
  docker/containerd surfaces are removed there, so slice 3 does not move them.
- **ADR-237 before slice 2.** ADR-237 removes the provider clients' private Runtime
  (`openai/chat_client.hpp:473`), which `AnyChatClient` would otherwise have to carry.

The slices:

1. **Facade over today's code.** `Engine`, `AgentSpec`, `Session` (wrapping the current types),
   presets, the authority types and `agentengine.hpp`. The three reference programs and G-A1 go into
   CI, and the names freeze.
2. **Type erasure.** `AnyChatClient`, `AnyContextProvider` and a non-template `Session`, with the facet
   tests; G-A2 and G-A3 measured.
3. **Directory moves, one layer per PR**, bottom up: `vocab`, `runtime`, `trust`, `worktree`, `sandbox`
   (with the `ToolDispatch` callback), `agent`/`providers`, `orchestration`, `protocol`. Overrides,
   baseline and allows drop to zero; G-A4.
4. **Seams.** The remaining holders and G-A5, aligned with ADR-242 step 3 (one execution seam).
5. **Server module.** `server/` per ADR-242 R4, with tenancy, admission and session-to-principal
   binding.

No forwarding headers are kept for moved files: this repository is the only consumer. Each move PR
rewrites, in the same PR, every include, the paths in `tools/` and CMake, and the `path:line` citations
in the RFCs, the spec, CONVENTIONS and ADR-242 §3a (whose G-R12 checks them).

## 6. Residuals

- **A large, repo-wide churn.** Directory moves rewrite most include lines; each slice is a big diff,
  and `git blame` across moves needs `--follow`.
- **Names are provisional** until slice 1 freezes them; the §2.2 sketch is a target, not an API.
- **The line budgets are a choice**, not a measurement of anything external; they are set where the
  sketch lands with some slack, and tightening them later is cheap.
- **Compile-time gain is a hypothesis** until G-A2 measures it; the 2026-10-19 timing data is
  incremental and indicative only.
- **The directory map is checked by hand, not proved.** The red-team's edge counts mapped today's
  `layers.toml` onto §2.1 by script, and the cited edges were checked by hand. Slice 3's lint is the
  proof.

## 7. Red-team record

Round 1 (2026-10-20, independent agent; every cited code location re-checked before folding in):

- **F1 (fatal): layer order.** `server/` sat below `engine/` although it takes an `Engine`, and
  `AgentSpec` sat at the top although `agent/`, YAML, workflows and protocols need it. Fixed:
  `AgentSpec`/`Session` moved to `agent/`, `engine/` is L5, `server/` is L6, and the umbrellas have
  layers.
- **M1: L1 cycles.** `effect_context` ↔ `filesystem_adapter`, `sandbox` → `ledger` → `async_quota`/
  `identity_authority`, and `agent_spawn_capability` → `worktree`/`agent_registry`. Fixed: the L1 order
  is stated, `FilesystemAdapter` goes to `vocab/`, and `agent_spawn_capability` goes to `agent/`.
- **M2: backends and the code-mode bridge.** Fixed: the `ToolDispatch` callback, the backend sub-map,
  the HTTP client in `sandbox/`, and the unmapped directories assigned.
- **M3: erasure dropped invariant facets.** Fixed: the holders keep them, with tests per facet.
- **M4: store-backed `state<T>()`.** It changed the design and broke I2. Fixed: in-memory `AnyState`.
- **M5: authority underspecified.** Fixed: asynchronous, authenticated principal only, intersected with
  the ceiling, `require_authority` forced, and an explicit single-trusted-caller choice.
- **M6: the server sketch was incomplete.** Fixed: tenancy, admission, the Kata runtime and session
  binding, all counted by G-A1.
- **M7: gate loopholes.** Fixed: no inline allows, call-counting replaceability tests, formatted line
  counts that include developer helpers, a G-A2 threshold, and a per-slice G-A3.
- **Minor.** Fixed: the seam table corrected (`SecretStore`, `PolicyDecider`, `Executor`,
  `RegisteredSandboxBackend`), the session-per-agent wording, the slice ordering, and the citation
  rewrite.
