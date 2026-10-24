# ADR-243 — What is AgentEngine's public C++ API and layer structure, so that a developer builds a multi-user agent server in a few dozen lines, with every managed piece replaceable?

**Status:** Proposed (2026-10-20), revision 2 (2026-10-24). Red-team round 1 (2026-10-20) and round 2
(2026-10-24, a fresh reviewer: 2 FATAL, 8 MAJOR, §7) are folded in. The prove phase runs per migration
slice (§5). Implements [ADR-242](ADR-242-direction-reset-developer-engine-linux-server-explicit-choice.md)
§6 step 2 (R5, R6, R8, §2's "least developer code" measure, §9's line-budget question), over what
[ADR-244](ADR-244-required-choices-step-1b.md) (Judged) built, and consistent with
[ADR-245](ADR-245-sandbox-backends-in-a-separate-repository.md) (Proposed), whose amendments to this
ADR are folded in here (§2.1, §2.3, §4, §5).

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

Structural facts (re-measured at main @ a6e6709f, 2026-10-24):

- `AgentSession<ChatClientT, StateT, HistoryProviderT>` (`rt/agent_session.hpp:226`) and 4-parameter
  free functions are templates, although the turn loop is already non-template (`AgentSessionCore`) and
  model calls are already virtual. 132 files name its header.
- The slowest translation units on the build server (17–20 s each, `.ninja_log` of 2026-10-19, mixed
  incremental build, indicative only) instantiate the templated provider chat clients.
- Directories do not match layers: `rt/` (labelled L0) holds the L2 session and L3 workflows; `core/`
  (L2) holds V, L0 and L1 files. 87 per-file overrides make the layering lint pass, 8 upward edges are
  baselined, and directories form cycles (`rt → core` 123 edges; `core → trust` 29, `trust → core` 34).

## 2. Decision

### 2.1 Directories are layers

| Layer | Directory (`include/agentengine/`) | Contents |
|---|---|---|
| V | `vocab/` | `result`/`error`, JSON/YAML value, content model, run events, `Tainted<T>`, the `FilesystemAdapter` interface, `ToolSignature` (name, description, JSON schema as a value: what code mode needs from a tool, without the validator) |
| L0 | `runtime/` | `task`, channels, `Runtime`, lanes, stores, `pal/` (reactor, raw TLS stream) |
| L1a | `trust/` | capabilities, identity and authority, secrets, budgets and quotas |
| L1b | `worktree/` | worktree, ledger, mounts, staging and harvest (ADR-245 §2.3) |
| L1c | `sandbox/` | the `SandboxBackend` contract and `sandbox/backend_api/` (ADR-245 §2.2), `AnySandboxBackend` and its decisions, registry, egress proxy, the outbound HTTP client, the `ToolDispatch` callback; `sandbox/backends/` holds only `wasm` and `NoSandboxBackend` |
| L2 | `agent/` | `AgentSpec`, `Session`, tool pipeline, context providers and their runtime composer, the model plane (`ChatClient`, `AnyChatClient`), memory, YAML agent compiler, `agent_spawn_capability`, the `agent.*` code generation (it reads `ToolDescriptor`) |
| L2 | `providers/` | outbound conformers: OpenAI, Anthropic, embedders, vector indexes |
| L3 | `orchestration/` | workflows, multi-agent, projects, evaluation |
| L4 | `protocol/` | MCP, A2A, AG-UI dispatchers (typed, transport-agnostic) |
| L5 | `engine/` | the developer facade: `Engine`, presets, `agentengine.hpp` umbrella |
| L6 | `server/` | optional `agentengine_server` target: transports, listener, principal verifiers (ADR-242 R4); `server.hpp` umbrella |

Rules:

- A file's layer is its directory, with **zero** per-file overrides and **zero** inline
  `ae-layering-lint: allow` comments.
- A file includes only its own directory and directories of a **strictly lower** layer. Within L1 the
  order is `trust` < `worktree` < `sandbox`; `agent/` and `providers/` are peers, and `providers/`
  includes `agent/` only (never the reverse).
- **No cycles between directories** (G-A4). The 8 baselined upward edges are fixed by splitting the
  files named in `tools/layering_baseline.txt`. Edges resolved by intent today get a named target:
  `content.hpp`'s use of `json_schema.hpp` moves the schema *value* to `vocab/` and leaves the validator
  in `agent/`; `secret_quarantine` goes to `trust/`; `session_builder.hpp`'s provider-client includes go
  with the quickstart fold (§2.4) into `engine/`.
- **Code mode is an injected callback, not an include.** `sandbox/` defines `ToolDispatch` over V types
  (`ToolSignature`, `json::Value`); `agent/` passes it in when it builds the sandbox, and the
  wrapper's `agent.*` dispatch (ADR-245 §2.2 item 5) calls it. The sandbox never names the pipeline.
  The `agent.*` code generation reads `ToolDescriptor`, so it lives in `agent/`.
- **Test code stays test code.** `testing/` holds fakes at the layer of what they fake, and keeps
  today's rule as a lint: nothing outside a test target includes `testing/`.
- **`detail/` directories live inside a layer directory** (for example `sandbox/detail/`) and take its
  layer; today's root-level `detail/` files are assigned file by file in slice 3.
- **Backends.** The reactor and mbedTLS backends go under `runtime/backends/`; the Vulkan vector index
  under `providers/`. Backend mechanisms (native-jail, Kata) are not moved by slice 3: ADR-245 carves
  them out of the repository.

### 2.2 The developer surface: four handles

- **`Engine`** (L5): one per process; it holds the Runtime, registries, secrets and stores. It is the
  only place a `Runtime` is created (ADR-242 R6; ADR-244 §6 hands it `RuntimeConfig` sizing).
- **`AgentSpec`** (L2): what one agent is. `Agent<Derived,…>` sugar, the YAML compiler (I6), workflow
  nodes and protocol dispatchers all produce or consume the same `AgentSpec`.
  **Per-session seams are factories.** The model, the context providers and the history provider
  carry per-session state today (each session owns its client; the OpenAI client keeps a
  `session_id_`, `openai/chat_client.hpp:471`; an `InteractiveChatClient` keeps pending interactions,
  `core/chat_client.hpp:331`). `AgentSpec` therefore holds a *factory* for each, and every session gets
  its own instance; sharing one across sessions or tenants is impossible through the API.
- **`Session`** (L2): one conversation. It may run several agents (ADR-242 R8 rule 1), each in its own
  sandbox.
- **`Server`** (L6), if linked.

All four are ordinary non-template classes.

The reference application, which G-A1 measures (a sketch; names freeze in slice 1). Under ADR-245,
backends come from `agentengine-sandboxes`; this program is the **external** variant, counted there.
The in-repository variant names `backends::none()` and has no `providers::codeact()` (§4, G-A1).

```cpp
#include <agentengine/agentengine.hpp>
#include <agentengine/server.hpp>
#include <agentengine_sandboxes/kata.hpp>
using namespace agentengine;

task<result<RequestAuthority>> grants(VerifiedPrincipal const& who) {     // counted by G-A1
    if (who.claim("role") == "analyst")
        return ready(RequestAuthority{who, tenant(who.claim("org")), caps::exec() | caps::net_out("api.example.com:443")});
    return ready(RequestAuthority{who, tenant(who.claim("org")), caps::none()});
}

int main() {
    auto engine = Engine::create({
        .runtime   = runtime::all_cores(),
        .secrets   = secrets::env("AE_SECRET_"),
        .sandboxes = {sandboxes::kata({.image   = "registry.example/ae-python@sha256:...",
                                       .runtime = "io.containerd.kata.v2",
                                       .network = "ae-kata-net"})},
        .store     = stores::file("/var/lib/agentengine"),
    });
    if (!engine) return fail(engine.error());

    auto analyst = engine->agent({
        .name      = "analyst",
        .model     = models::openai("gpt-5", secret("openai-api-key")),     // a factory: one client per session
        .sandbox   = "kata",
        .ceiling   = caps::exec() | caps::net_out("api.example.com:443"),   // what any request may get, at most
        .limits    = presets::server_limits(),
        .policy    = presets::server_policy(),
        .providers = {providers::shell(), providers::codeact()},
        .tools     = {tools::web_fetch()},
    });
    if (!analyst) return fail(analyst.error());

    auto server = server::http(*engine, {
        .listen    = "0.0.0.0:8443",
        .tls       = tls::files("cert.pem", "key.pem"),
        .verifier  = verifiers::bearer_jwt("https://idp.example/.well-known/jwks.json"),
        .authority = grants,                      // root of trust, ADR-242 R7
        .tenancy   = tenancy::per_principal(),
        .admission = presets::server_admission(),
        .agents    = {*analyst},
        .protocols = {protocol::mcp, protocol::a2a, protocol::agui},
    });
    if (!server) return fail(server.error());
    return server->run();
}
```

Every ADR-242 required choice is present; leaving one out is a named startup error listing the options
(ADR-242 G-R13). `unbounded` is an explicit value wherever a bound is required. **Presets** are
explicit bundles: every narrowing default ADR-244 §6 left to this ADR (for example the history window,
output caps, approval modes per tool) is a named value inside the preset, printed in startup
diagnostics; there is no hidden default under a preset. Telemetry follows ADR-244: optional, and stated
in the preset when chosen.

**Authority** (ADR-242 R7, ADR-244 §2.3, I2/I3):

- `AuthorityPolicy` is `task<result<RequestAuthority>>(VerifiedPrincipal const&)`. It is the
  developer's **root of trust**, host root authority (ADR-242 R7). It is not an ADR-070 Delegated
  Decision Seam.
- `VerifiedPrincipal` is constructed only by a `PrincipalVerifier`: a bearer-JWT or mTLS verifier from
  `server/`, or one the host writes for its own socket (the ADR-039/061 path stays). Every construction
  is audited with the verifier's identity. Request bodies and model output never reach a verifier's
  input. This is an audited guarantee, not a structural one: host code can write a verifier.
- The engine compares the policy's result with the agent's `.ceiling`. A result wider than the ceiling
  is **refused** with `run.authority_exceeds_ceiling` (ADR-244 §2.3; `agent_session_core.cpp:995`;
  `mcp/server.hpp:116`), never narrowed.
- The server module accepts only the per-request authority source; `AuthoritySource::single_trusted_caller()`
  is refused there (ADR-244). A session is bound to the principal and tenant that created it; another
  principal resuming it by id is refused.

A single-user program is the same `Engine` and `AgentSpec` plus
`engine->session(analyst, AuthoritySource::single_trusted_caller())` and `session.ask(...)`, using
ADR-244's types; the single trusted caller is a named choice, never a default.

### 2.3 Managed, but every piece is replaceable

Each seam has a **concept** and one **type-erased holder**.

| Seam | Concept / interface today | API holder | Engine options |
|---|---|---|---|
| Model | `ChatClient` | a factory of `AnyChatClient` (new) | `models::openai`, `models::anthropic`, decorators (failover, recording, replay) |
| Execution | `SandboxBackend` over `backend_api/` (ADR-245) | `AnySandboxBackend`, built by ADR-245's contract slice and adopted here | `backends::none()` (`NoSandboxBackend`), `wasm`; others from `agentengine-sandboxes` |
| Context | `ContextProvider` | a factory of `AnyContextProvider` (new), composed at run time by `AnyComposedContextProvider`, which keeps ADR-066 attribution and ADR-116 `bind_owner` | `providers::shell`, `codeact`, RAG, memory |
| Tool | none (data only) | `ToolDescriptor` (already erased) | `tools::*`, MCP-imported tools |
| Secrets | `SecretStore` concept; `SecretSource` virtual base (`trust/secret.hpp:123`) | `SecretSource` (already erased) | `secrets::env`, `secrets::file` |
| Stores | `SessionStore`, `AppendLogStore`, `WorktreeObjectStore` | `Any…Store` | `stores::file`, `stores::memory` |
| Authority | `AuthorityPolicy` (§2.2) | the same function | none: the developer's root of trust |
| Principal | new: `PrincipalVerifier` | `AnyPrincipalVerifier` | `verifiers::bearer_jwt`, `verifiers::mtls` |
| Approval | `PolicyDecider`, a `std::function` (`tool_pipeline.hpp:179`) | the same | in `presets::*_policy` |
| Transport | new: `Transport` (server module) | `AnyTransport` | `server::http`, or the host's own socket |
| Workflow node | new: a node behaviour concept (today's `Executor`, `graph.hpp:106`, is a data struct) | `AnyNode` | agents, sub-workflows |

**Erasure keeps every compile-time facet that carries an invariant.** `rt/agent_session.hpp` chooses
these at compile time today, and each holder probes them when built, stores them, and answers them at
run time with the same behaviour:

- the gateway call path, and separately the gateway **stream** path (`ModelCallGatewayStreamLike`),
  versus the plain chat path (`:407-430`);
- whether the client has `chat()` (the buffered path) or only a stream (the drain path): ADR-198's
  undeclared-tool-call check (I3) runs in **both**, at different places (`:430-466`);
- `capabilities().tool_calling`, which gates that check (`:437`, `:462`);
- whether a client exists at all (`is_default_constructible_v`, `:472`);
- the outbound media gate (`validate_outbound_media_capabilities`, `:391`);
- `HasProducerChatClientId` (`:497`) and `InteractiveChatClient` (ADR-230, `:511`);
- `HasOnRunEnd` and ADR-116's `bind_owner` on the history provider (`:261`, `:487`);
- `fork_from`'s child-branch copy (an explicit `clone()` verb);
- **clear**, which today resets state and history to fresh values (`agent_session.hpp:369-370`): an
  explicit `reset()` verb on every holder.

G-A5 has one test per facet, including "an undeclared tool call through `AnyChatClient` is refused" on
the buffered and on the drain path.

`task<T>`, `stream<T>` and cancellation (through `EffectContext::cancellation`) pass through erasure
unchanged. A task returned by a holder borrows the holder, and the holder must outlive it.

**Cost.** A seam crossing that adapts the inner task costs an indirect call **and a coroutine frame
allocation**; G-A3 measures both. A seam on a measured 023 hot path may keep a template fast path
(ADR-242 R5).

### 2.4 What goes away

- **The session template goes.** `Session` replaces `rt::AgentSession<ChatClientT, StateT,
  HistoryProviderT>`. Typed state becomes an in-memory `AnyState`, fixed when the session is built,
  read with `session.state<T>()`, which returns `result<std::reference_wrapper<T>>` (`result` cannot
  hold a reference) and checks the type id. Clear resets it to a fresh `T` (as `state_ = StateT{}`
  does today); fork copies it; restore builds a fresh `T`, since state is not in the snapshot
  (`save_agent_session_snapshot` writes only `AgentSessionRecord`). No ambient store access (I2).
- **The quickstart builders fold in** to `Engine`/`AgentSpec`, with `session_builder.hpp`'s provider
  includes; `ComposedQuickstartSessionBuilder<...>` goes.
- **Developers stop touching the internals.** `Ledger`, `IdentityAuthority`, `AsyncQuota`, staging
  and `block_on` are owned by the engine per agent and sandbox; they remain public at their own layer
  for anyone building a custom seam.
- **CRTP becomes sugar** that produces an `AgentSpec`.

## 3. Alternatives

- **A. A facade only, over today's templates.** Every custom seam still fits a template parameter
  list, compile cost stays, and the directory/layer mismatch stays. Rejected.
- **B. A C ABI first, C++ on top.** Makes the C++ surface second-class. Rejected for now; the `Any…`
  holders are the natural C ABI boundary later.
- **C. Shared seam instances in `AgentSpec`** (revision 1). One model client per agent serves every
  tenant's sessions and leaks per-session state across them. Rejected, round 2.
- **D. Chosen: §2.**

## 4. Gates

Each has a positive control (spec 022 §5).

| Gate | Claim | Disproof / positive control |
|---|---|---|
| **G-A1** | Least developer code | A `.clang-format` (column limit 100) is committed in slice 1 and frozen; changing it needs an ADR amendment. CI formats the reference programs, then counts every non-blank, non-comment line the developer writes, helpers included. `grants` must map at least two claims to distinct tenants and capability sets (a body that returns one constant fails a check). The model is injected through the `models::` factory seam, so the measured program is the shipped one, run against a scripted model at no line cost. Budgets: hello agent ≤ 15; single-user shell chat ≤ 30; multi-user server ≤ 60. In this repository the server runs over `backends::none()` without CodeAct; the code-interpreter variant (ADR-242 G-R6) is counted in `agentengine-sandboxes` (ADR-245 §2.6). Control: padding a program past its budget fails; a `grants` returning one constant fails |
| **G-A2** | Compile time does not regress | Clean `ninja` build on the build server (gcc and clang, all cores), total and top-20 TU times, before and after each slice, recorded in the PR. A total regression over 10 % **fails** the slice. Control: a planted heavy include in a hot header shows in the measurement and fails |
| **G-A3** | Erasure is cheap where claimed | A bench of one seam crossing through each holder in the slice, against the direct call, in ns, allocation counted. A crossing over 250 ns or more than one allocation fails, unless it is off every 023 hot path and the PR shows that. Control: a planted slow holder (an extra allocation and a 1 µs spin) fails |
| **G-A4** | Directories are layers | The layering lint runs with an empty `[files]` override table, an empty baseline and no inline allows; a directory-cycle check fails on any cycle; the `testing/` rule holds. Controls: a planted upward include, a planted same-layer cycle, a planted inline allow and a planted `testing/` include from a library target each fail |
| **G-A5** | Every seam is replaceable, every facet kept | For each row of §2.3, a call-counting custom implementation passed through the public API where the engine's option goes is called. One test per facet in §2.3's list. Two tenants on one `AgentSpec` get distinct client instances, and one tenant's pending interaction is invisible to the other. Control: a facade that silently uses the engine's option, or shares one client, fails |
| **G-A6** | Nothing implicit survives the API | ADR-242 G-R13's cases through `Engine::create`, `engine->agent`, `engine->session` and `server::http`; a missing choice is a named error listing the options; a preset prints every value it sets. Control: a test that starts with one choice unset must fail |
| **G-A7** | I6: YAML and C++ are equivalent | The same agent built from YAML and from C++ yields equal `AgentSpec`s (a field-by-field comparison over every field). Control: a YAML field the compiler drops is caught |
| **G-A8** | R6: one Runtime, no private threads | After building an `Engine` and 1,000 idle sessions across the seams of §2.3, the process thread count equals the Runtime's configured size (helper processes counted separately). Control: a provider client that builds a private Runtime fails |
| **G-A9** | The server module keeps per-request authority (G-R5's slice-5 part) | A server dispatch without a `RequestAuthority` fails to compile (`try_compile` beside a compiling one); the HTTP parser and every verifier input are fuzzed to ADR-015's criteria; a resumed session under another principal is refused. Control: the compiling dispatch; a resume by the owning principal succeeds |

## 5. Migration, in slices (each its own PR, each proves its gates)

**Order** (this amends ADR-242 §6, whose "steps 2–4 … sequenced" is kept, in this order):

1. ADR-244 (§6 step 1b): its items that need no live infrastructure. Its items that wait on Kata
   hosts (Kata `create`'s remaining checks, `UnmediatedShell`, the time-limit row) stay ADR-244's, are
   finished wherever Kata lives (here, or in `agentengine-sandboxes` after ADR-245), and do not block
   this ADR.
2. ADR-245's contract slice (the contract half of ADR-242 §6 step 3): `backend_api/`,
   `AnySandboxBackend`, `NoSandboxBackend`, deletion of `execution_surface`, `persistent_shell` and
   the Docker/containerd surfaces.
3. ADR-237 (ADR-242 §6 step 4): no private Runtime in provider clients (`openai/chat_client.hpp:473`).
4. This ADR's slices:
   1. **Facade over today's code.** `Engine`, `AgentSpec`, `Session` (wrapping the current types),
      presets, the authority and verifier types, `agentengine.hpp`; the `.clang-format`; G-A1, G-A6,
      G-A7 in CI; names freeze.
   2. **Type erasure.** The factories, `AnyChatClient`, `AnyContextProvider` with
      `AnyComposedContextProvider`, a non-template `Session`; G-A2, G-A3, G-A5 (facets), G-A8.
   3. **Directory moves, one layer per PR**, bottom up: `vocab`, `runtime`, `trust`, `worktree`,
      `sandbox` (with `ToolDispatch`), `agent`/`providers`, `orchestration`, `protocol`; G-A4.
   4. **Seams.** The remaining holders and G-A5's rows.
   5. **Server module** per ADR-242 R4, with tenancy, admission, verifiers and session binding; G-A9.
5. ADR-245's carve and extraction (its steps 5–6).

**Moves while other work is in flight.** Each slice-3 move PR is generated from a move map
(`tools/layer_moves.toml`, old path → new path) by a script that also rewrites includes, CMake and
`tools/` paths. Branches in flight rebase by running the same script against the map; a move PR is
announced and merged within a day, and no two layer moves are open at once.

**Forwarding headers.** None while this repository is the only consumer. That ends at ADR-245's
extraction: after it, a moved contract header is a contract change (ADR-245 §2.5's ABI tag).

**Citations.** Each move PR rewrites the `path:line` citations in the RFCs, the spec, CONVENTIONS and
the live ADRs (ADR-242 §3a's G-R12 checks them). Archived ADRs stay unedited (decisions/README);
`decisions/archive/PATHS.md` maps their old paths to new ones.

## 6. Residuals

- **A large, repo-wide churn.** Directory moves rewrite most include lines; `git blame` across moves
  needs `--follow`.
- **Names are provisional** until slice 1 freezes them; the §2.2 sketch is a target.
- **The line budgets are a choice**, not an external measurement; the column limit chosen with them
  is frozen with them.
- **Compile-time gain is a hypothesis** until G-A2 measures it.
- **The verifier guarantee is audited, not structural** (§2.2): host code can write a verifier.
- **The directory map is re-checked by script, not proved.** Round 2 re-mapped today's include graph
  onto §2.1; the remaining edges are named in §2.1. Slice 3's lint is the proof.
- **The in-repository server has no code interpreter** until a backend with the interpreter facet is
  linked from `agentengine-sandboxes` (ADR-245).

## 7. Red-team record

Every cited location was re-checked before folding in.

**Round 1** (2026-10-20): F1 layer order (`server/` below `engine/`, `AgentSpec` at the top) → `agent/`
holds `AgentSpec`/`Session`, `engine/` L5, `server/` L6. M1 L1 cycles → L1 order, `FilesystemAdapter`
to `vocab/`, `agent_spawn_capability` to `agent/`. M2 backends and code mode → `ToolDispatch`, backend
sub-map. M3 erasure dropped facets → facet list and tests. M4 store-backed state → in-memory `AnyState`.
M5 authority underspecified. M6 server sketch incomplete. M7 gate loopholes. Minor: seam table, wording,
order, citation rewrite.

**Round 2** (2026-10-24, fresh reviewer): not ready, 2 FATAL, 8 MAJOR.
- **F1** "narrowed and audited" contradicted ADR-244 §2.3 and the code, which refuse; `require_authority`
  is gone; `single_trusted_caller(caps)` did not match ADR-244's type → §2.2 restated on ADR-244.
- **F2** `result<T&>` cannot compile; clear was unspecified → `reference_wrapper`, `reset()` (§2.3–§2.4).
- **M1** one shared model client per agent mixes per-session state across tenants → factories (§2.2),
  G-A5's two-tenant test, alternative C.
- **M2** facet list incomplete; no runtime composer → five more facets, `AnyComposedContextProvider`.
- **M3** G-A1 unfalsifiable (no `.clang-format`, trivial `grants`, scripted model, no in-repo backend)
  → G-A1 rewritten.
- **M4** G-A2 and G-A3 could not fail; cost mislabelled → thresholds, planted controls, frame
  allocation stated.
- **M5** missing I6, R6 and G-R5 gates; ADR-244 §6's delegations unanswered → G-A7–G-A9; presets
  carry the narrowing defaults; the PDF tool's backend follows ADR-245.
- **M6** "produced only by the bridge" unbuildable and killed the host-socket path; seam wording
  contradicted R7 → `PrincipalVerifier`, audited guarantee, R7 wording.
- **M7** ADR-243 and ADR-245 blocked each other → ADR-245's amendments folded in (§2.1, §2.3, G-A1,
  §5).
- **M8** order interleaved ADR-242 §6 without amending it; open-ended "1b first"; no concurrency
  protocol; archive citations → §5's order, infrastructure-bound 1b items no longer block this ADR,
  move map and rebase script, `PATHS.md`.
- **Minor:** figures re-measured; the `testing/` rule kept as a lint; `detail/` placement; telemetry
  per ADR-244; README status.
