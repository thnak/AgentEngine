# AgentEngine

**A C++23 engine for building agent applications: sandboxed by construction, standards-native,
cross-platform.**

AgentEngine gives you agents, sessions, tools and multi-agent workflows on its own runtime
(`agentengine::rt::`), with untrusted-code isolation and a Python code interpreter built in rather than
bolted on. It speaks the open agent protocols (MCP, A2A, AG-UI, OpenTelemetry GenAI) instead of a
proprietary API. The vocabulary is [Microsoft Agent Framework](https://github.com/microsoft/agent-framework)'s
(agent, session, tool, middleware, graph workflow), expressed as zero-cost C++ policy types instead of
runtime configuration objects.

> **Status: implementation under way, not a finished product.** The design is 30 reviewed RFCs; the code
> under `include/`, `src/` and `tests/` implements a large part of it, and each gate that was executed is
> recorded as an ADR in [`decisions/`](decisions/README.md). What is built, and what is not, changes
> weekly: read [`docs/planning/`](docs/planning/) for the live per-milestone status rather than trusting a
> summary here. One scope decision up front: AgentEngine does **not** run an HTTP listener. Your host code
> owns the socket and hands the engine parsed requests ([ADR-061](decisions/archive/ADR-061-host-provided-inbound-transport.md)).

## An agent is a type

```cpp
struct Researcher : Agent<Researcher,
        ChatClientId<"anthropic:claude-opus-5">,
        Tools<WebSearch, CodeInterpreter, Handoff<Writer>>,
        Capabilities<NetOut<"api.search.example">>,
        SandboxProfile<Strict>,
        MaxTurns<12>> {
    static constexpr std::string_view name = "researcher";
    static constexpr std::string_view instructions =
        "Research the question. Cite sources. Hand off to the writer when you have enough.";
};

AgentSessionOptions opts;
opts.grants = {/* the authority the caller holds; narrowed to Researcher's Capabilities<...> */};
auto researcher = make_agent_session<Researcher>(std::move(chat_client), opts);  // validates, then binds
auto stream     = researcher->ask_stream("Compare WASI 0.2 and 0.3.");
```

`make_agent_session<A>()` validates the declaration and returns a running session whose tools, turn and
token bounds, approval floor and capability ceiling are the ones `A` declares.
[`examples/01_hello_agent.cpp`](examples/01_hello_agent.cpp) runs one end to end, offline. The same agent
written in YAML compiles to byte-identical metadata; that equivalence is a tested invariant.

For a single-provider script that doesn't need the full `Agent<>` surface, a smaller builder sits over the
same session wiring:

```cpp
auto built = quickstart::OpenAiSessionBuilder("gpt-4o-mini")
                 .api_key_from_env("openai-api-key", "OPENAI_API_KEY")
                 .grant(Capability{cap::FsRead{"scratch", "", std::nullopt}})
                 .build();
if (built) { auto reply = built->ask("Compare WASI 0.2 and 0.3."); }
```

`build()` fails closed (no credential, no store) instead of failing at the first model call. See
`include/agentengine/core/session_builder.hpp` and the
[design notes](docs/planning/quickstart-session-builder-design-draft.md).

## Build and run

Requirements: CMake 3.28+ and a C++23 compiler. CI builds with MSVC, clang-cl (Windows) and gcc-14 (Linux).

```
cmake --preset dev          # tests, examples and test tools, unoptimized: the fast edit-build-test loop
cmake --build --preset dev
ctest --preset dev
```

Use `--preset release` for an optimized build; a change to engine templates should pass that one too. The
dev build directory is `build-dev/`. Run an example from it, for instance
`build-dev/examples/agentengine_example_01_hello_agent`.

- **Real providers need HTTPS.** `AGENTENGINE_WITH_HTTPS` is OFF by default; turn it on to build the OpenAI and
  Anthropic chat clients, the embedder and the Qdrant index (it vendors a pinned mbedTLS and CA bundle).
- **Other options:** `AGENTENGINE_WITH_WASM` (plugin host), `AGENTENGINE_BUILD_PYTHON_RUNNER` (the interpreter).
- **Tests marked `live-network`** call real services and skip without credentials; exclude them with
  `ctest -LE live-network`. A handful of sandbox tests also need a Docker daemon.
- **Parallelism limits and the other machine-safety rules** are in [CONVENTIONS.md](CONVENTIONS.md#build--test--machine-safety).
- A local compiler cache makes rebuilds several times faster: [docs/guides/local-build-cache.md](docs/guides/local-build-cache.md).

## Examples

Thirty-five programs in [`examples/`](examples/), most of them offline against a scripted fake model. A selection:

| Number | Shows |
|---|---|
| `01`-`03` | a first agent, adding tools, multi-turn |
| `05`, `06` | human approval; capabilities and denial |
| `07`, `29`, `32` | streaming, session events, the streaming session loop |
| `08`, `11`, `18` | memory, skill mounts, a custom context provider |
| `12`, `20`, `22` | session checkpoint, workflow checkpoint and resume, plan sign-off |
| `04`, `09`, `10`, `13`, `14` | workflows: a first one, concurrent, conditional routing, reflection, multi-selection |
| `15`, `21`, `23`, `27`, `28` | group chat, workflows as participants, handoff mesh, nested request ports |
| `16`, `17`, `19`, `25`, `26`, `30` | the `_live` ones: they call a real model and need a provider key |
| `35` | a harness agent |

## What makes it different

- **Isolation is the substrate, not a tool.** Every effect (filesystem, network, secrets, subprocess) is
  reachable only through an explicitly passed capability. Sandboxed code starts with none; there is no
  "allow everything" constructor.
- **Model output is data, never authority.** Nothing a model emits can widen a capability, approve an
  action or change policy.
- **Files outlive the sandbox.** Every session owns a content-addressed **worktree** the engine manages, so
  persistence, rewind and multi-agent sharing don't depend on which isolation backend is selected.
- **CodeAct is the action space.** The agent acts by writing a program against a host-backed library, so
  control flow and large intermediate data stay in the sandbox instead of passing through the context
  window.
- **A Python interpreter that can `import numpy`.** It is embedded native CPython, chosen on dated
  evidence ([research](docs/research/2026-standards-landscape.md) §6-8), not WASM.
- **Plugins are WASM components** (WASI 0.3): one signed artifact runs the same on Windows and Linux, holding
  only what it is granted.
- **Standards with a named revision and a conformance run.** MCP `2026-07-28`, A2A v1.0, AG-UI, OTel GenAI.
  The one number measured so far (2026-08, client role): MCP non-auth 75/75, auth 3/49 because there is
  no OAuth client yet, so the MCP gate is **not yet met**. "Supports MCP" without a passing suite is not a
  claim this project makes.
- **Runs replay offline.** Every nondeterministic input crosses a recorded seam.

## Layers

```
 L4  Protocol surfaces     MCP server + client · A2A · AG-UI · OpenAI-compatible HTTP
 L3  Orchestration         workflow graph, handoff, group chat, checkpoint / resume / time-travel
 L2  Agent core            Agent · AgentSession · tool plane · ChatClient plane · middleware
 L1  Trust & isolation     capability model · sandbox seam · WASM component plugin ABI
 L0  Runtime substrate     agentengine::rt:: async mutex, thread pool, session/append-log store,
                           circuit breaker, channel/stream backend, platform abstraction layer
```

Every agent runs inside a sandbox from a `SandboxBackend` that hosts its worktree. Execution options
(`microvm` via Kata, `native-jail`, `remote`, `wasm`, `NoSandboxBackend`) are identical in contract and
differ in strength; none is the default, and the developer chooses one and owns its disclosed risk
([ADR-242](decisions/ADR-242-direction-reset-developer-engine-linux-server-explicit-choice.md), 2026-10-20).

## Eight invariants

| | |
|---|---|
| **I1** | One session, one executor |
| **I2** | No ambient authority: every effect needs an explicitly passed capability |
| **I3** | Model output is data, never authority |
| **I4** | Every effect is attributable: span and audit record, always |
| **I5** | Nondeterminism crosses a recorded seam: runs replay offline |
| **I6** | Declarative and native authoring surfaces are equivalent |
| **I7** | Protocol conformance is a gate, at a named revision |
| **I8** | Budgets are enforced by a benchmark gate, not documented |

Full statements: [AgentEngineSpecification.md §4](AgentEngineSpecification.md).

## Where things are

| | |
|---|---|
| [`rfcs/`](rfcs/README.md) | the 30 RFCs: the authoritative design, with an index and maturity ladder |
| [`AgentEngineSpecification.md`](AgentEngineSpecification.md) | vision, layering, invariants, locked decisions |
| [`CONVENTIONS.md`](CONVENTIONS.md) | the binding coding contract |
| [`decisions/`](decisions/README.md) | ADRs: executed proofs behind specific gates, and the decision log |
| [`OpenQuestions.md`](OpenQuestions.md) | what is unresolved and what it blocks |
| [`docs/`](docs/) | research (dated, cited), planning and milestone status, guides, architecture |
| `include/agentengine/`, `src/`, `tests/` | the engine; tests are one per load-bearing invariant |
| `examples/`, `samples/` | runnable programs |
| `wit/`, `plugins/`, `schema/` | the plugin ABI, first-party plugins, declarative-format schemas |

## Contributing

Read [CONVENTIONS.md](CONVENTIONS.md) first. Every change follows the RFCs; when code and an RFC disagree,
the RFC wins, so fix the RFC first, backed by an ADR, then the code. Contested, hot-path or
security-critical designs go through `design → red-team → prove → judge` and produce an ADR, not an ad-hoc
change. Security reports: [SECURITY.md](SECURITY.md).

## Licence

MIT, see [`LICENSE`](LICENSE).
