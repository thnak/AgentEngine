# Research record — the open-standards landscape, July 2026

**Compiled:** 2026-07-31 · **Status:** dated snapshot, not a living document

This is the evidence base for the protocol and isolation decisions in the RFCs. It is dated on
purpose: RFC claims cite *this* record, and when a standard moves, a new dated record supersedes
it rather than editing history. Every load-bearing claim below carries a source.

---

## 1. Model Context Protocol (MCP)

**Current revision: `2026-07-28`** — three days old at time of writing, and the largest revision
since MCP launched. It reshapes the protocol in ways that directly change how a host embeds it.

Load-bearing changes ([changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog)):

- **Stateless core.** The `initialize` / `notifications/initialized` handshake is gone, as is the
  protocol-level session and the `Mcp-Session-Id` header. Every request is self-contained and
  carries its protocol version and client capabilities in `_meta`
  (`io.modelcontextprotocol/protocolVersion`, `.../clientCapabilities`, `.../clientInfo`).
- **`server/discover`** is a required RPC advertising supported versions, capabilities, identity.
- **Multi Round-Trip Requests (MRTR)** replace server-initiated requests. A server returns
  `InputRequiredResult` (`resultType: "input_required"`) with `inputRequests`; the client retries
  the original request carrying `inputResponses`. All results now carry a required `resultType`.
- **`subscriptions/listen`** — one long-lived POST-response stream — replaces the HTTP GET endpoint
  and `resources/subscribe`/`unsubscribe`. Request-scoped notifications (progress, message) still
  flow on their own request's response stream.
- **SSE resumability removed** (`Last-Event-ID`, event IDs). A broken stream loses the in-flight
  request; the client re-issues with a new request ID.
- **Cacheable list results** — `ttlMs` + `cacheScope` (`public`/`private`) required on
  `tools/list`, `prompts/list`, `resources/list`, `resources/read`, `resources/templates/list`.
  Tools SHOULD be returned in deterministic order (prompt-cache friendliness).
- **Extensions framework** — `extensions` on client/server capabilities; **tasks moved out of core**
  into the official `io.modelcontextprotocol/tasks` extension (polling `tasks/get`, `tasks/update`).
- **Deprecated:** Roots, Sampling, Logging (12-month minimum window; migrate to tool params /
  direct provider APIs / stderr+OpenTelemetry respectively); HTTP+SSE transport; OAuth Dynamic
  Client Registration in favour of **Client ID Metadata Documents**.
- **Authorization hardening:** RFC 9207 `iss` validation, `application_type` in DCR, credentials
  keyed by issuer and never reused across authorization servers.
- **OpenTelemetry trace propagation is now documented** for `_meta` (`traceparent`, `tracestate`,
  `baggage`) — SEP-414.
- **Error code policy:** `-32000..-32019` implementation-defined, `-32020..-32099` reserved for the
  spec; resource-not-found moved to `-32602`.
- **Feature lifecycle policy:** Active → Deprecated → Removed, minimum 12 months between.

**Consequences for AgentEngine (RFC 011):** an MCP client must be built stateless-first, with
per-request `_meta` assembly, an MRTR retry loop as a *first-class* control-flow shape rather than
a callback, a client-side list cache honouring `ttlMs`/`cacheScope`, and no dependency on Sampling
or Roots. Our own MCP *server* surface must not use deprecated features at all.

## 2. Agent2Agent (A2A)

**v1.0, released April 2026**, governed by the Linux Foundation (hosted since June 2025), TSC with
AWS/Cisco/Google/IBM/Microsoft/Salesforce/SAP/ServiceNow, 150+ organizations
([LF announcement](https://www.linuxfoundation.org/press/a2a-protocol-surpasses-150-organizations-lands-in-major-cloud-platforms-and-sees-enterprise-production-use-in-first-year),
[spec](https://a2a-protocol.org/latest/specification/)).

- **Three functionally equivalent bindings:** JSON-RPC 2.0, gRPC, HTTP+JSON/REST.
- **Agent Card** at a well-known endpoint: identity, provider, skills, capabilities (streaming,
  push notifications, extended card), per-binding service endpoints, auth schemes (API key, OAuth2,
  mTLS, OIDC), security requirements and scopes. Cards may be **signed** and are cacheable.
- **Objects:** `Task` (id, context id, status, artifacts, history), `Message` (role, parts), `Part`
  (text / bytes / URL / structured JSON), `Artifact` (multi-part output).
- **Lifecycle:** `SUBMITTED → WORKING → COMPLETED | FAILED | CANCELED | REJECTED`, with the
  interrupted states `INPUT_REQUIRED` and `AUTH_REQUIRED`. Terminal states reject further messages.
- **Update delivery:** polling (`GetTask`), streaming, and webhook push notifications.

**Consequence (RFC 012):** A2A's `Task` state machine is very close to our `Run` lifecycle, and its
`INPUT_REQUIRED` maps onto the same human-in-the-loop request point as MCP's MRTR and the
workflow's request/response port. Unifying these three into one internal shape is a design goal,
not a coincidence.

## 3. AG-UI and the UI layer

AG-UI is an event-based agent↔frontend protocol (CopilotKit, MIT, 12k+ stars). AWS added AG-UI
support to Bedrock AgentCore Runtime in March 2026; MAF documents AG-UI integration
([events reference](https://docs.ag-ui.com/concepts/events)). Event categories: **lifecycle**
(RunStarted/Finished/Error, StepStarted/Finished), **text message** (Start/Content/End/Chunk),
**tool call** (Start/Args/End/Result/Chunk), **state** (StateSnapshot/StateDelta/MessagesSnapshot),
**activity** (ActivitySnapshot/Delta), **reasoning** (ReasoningStart/End, ReasoningMessage*,
ReasoningEncryptedValue), **special** (Raw, Custom).

Adjacent and deliberately **out of scope for v1**: **A2UI** (agent-generated declarative UI),
**AP2** (agentic payments over existing rails), **X42** (trust/governance for cross-boundary agent
calls). They are tracked in `OpenQuestions.md`, not designed against.

**Consequence (RFC 013):** AG-UI's event set is close to a superset of what our run emits. We emit
AG-UI as a *projection* of the internal run event stream, so adding A2UI later is another
projection rather than a second event model.

## 4. OpenTelemetry GenAI semantic conventions

As of **July 2026 nothing in the GenAI conventions is marked Stable** — they remain Development,
with no 1.0. They now live in their own repository, `open-telemetry/semantic-conventions-genai`;
the core semantic-conventions repo deprecated and moved all `gen_ai.*` content in v1.42.0
(2026-06-12) and ships none as of v1.43.0 (2026-07-03)
([state of the conventions, July 2026](https://john-hodge.com/blog/opentelemetry-genai-semantic-conventions/)).

Load-bearing names in current use: spans **`invoke_agent`**, **`chat`**, **`execute_tool`**;
attributes `gen_ai.request.model`, `gen_ai.usage.input_tokens`, etc. Coverage spans LLM client
spans, agent spans, prompt/completion content events, and aggregate metrics. Vendor support is
already broad despite the experimental status.

**Consequence (RFC 016):** we conform to the convention *and* pin its version in a single mapping
layer, because it will break. Content capture (prompts/completions) is off by default and
privacy-gated. MCP's `_meta` trace propagation (SEP-414) is the cross-process link.

## 5. WebAssembly: WASI 0.3 and the Component Model

- **WASI 0.3.0 released 2026-06-11**, adding **native async to the Component Model**: `async func`,
  `stream<T>`, `future<T>`. `wasi:io` is removed entirely — pollables and streams are absorbed into
  the Canonical ABI ([WASI 0.3](https://wasi.dev/releases/wasi-p3),
  [Bytecode Alliance](https://bytecodealliance.org/articles/WASI-0.3)).
- **Wasmtime 45** runs the release candidate; **Wasmtime 46 ships WASI 0.3.0 with Component Model
  Async enabled by default**.
- **WASI 1.0** — production-stable with LTS commitments — targets late 2026 / early 2027.
- Capability model: no ambient authority; file, network, and environment access must be explicitly
  granted.

**Consequence (RFCs 008/009):** the plugin ABI targets **WASI 0.3 components**, and the async
primitives map onto `agentengine::rt::task<T>`'s coroutine handlers rather than requiring a blocking
thread per call (historical: originally Quark's coroutine handlers, before ADR-037 removed Quark as
a dependency). Pinning to a Wasmtime version that ships 0.3 by default is a build-matrix requirement.

## 6. Python in WebAssembly — the decisive finding

This is the single research result that changed a design decision.

**The rich ecosystem is Emscripten-targeted and needs a JavaScript host.**
Pyodide (versioned in lockstep with CPython — `314.x` for 3.14) ships NumPy, pandas, SciPy,
Matplotlib, scikit-learn, cryptography, PyYAML, regex, plus any pure-Python wheel on PyPI via
`micropip` ([packages in Pyodide](https://pyodide.org/en/stable/usage/packages-in-pyodide.html)).
Since **April 2026 PyPI accepts WASM wheels** under the PEP 783 `pyemscripten_202*_wasm32` tags, so
maintainers publish Pyodide wheels directly — though adoption was still early (~28 packages) in
June 2026 ([Simon Willison, 2026-06-13](https://simonwillison.net/2026/Jun/13/publishing-wasm-wheels/)).
All of this is **Emscripten**: it runs in a browser or Node, not in a standalone wasmtime embedded
in a C++ process.

**The embeddable target is WASI, and its Python package story is not there yet.**
**PEP 816 was accepted** in 2026, fixing how CPython supports WASI and the WASI SDK from Python
3.15 (the versions supported at a release's beta 1 are frozen for that release's lifetime). But as
of March 2026 the roadmap still lists, as *not done*: **socket support** (blocked on WASI 0.3
plumbing), **threading** (a prerequisite for sockets), **a wheel platform tag for WASI builds**, and
**a bundling mechanism** ([State of WASI support for CPython, March 2026](https://snarky.ca/state-of-wasi-support-for-cpython-march-2026/)).
There is consequently no binary-wheel ecosystem: `wasm32-wasip1` is a CPython Tier 2 target, but
NumPy-class packages are experimental one-offs, not installable artifacts.

**Therefore:** a WASI-Python code interpreter embedded in a C++ host in July 2026 is
*stdlib + pure-Python wheels*. That is a genuinely useful sandbox tier — deterministic, instantly
snapshot-able, identical on all three OSes — but it cannot `import numpy`, which is most of why
users want a Python code interpreter at all.

This is why RFC 010 defaults the interpreter to a **jailed native CPython** and keeps WASM Python as
a profile, while RFC 009 still locks **WASM components as the plugin ABI** — where the ecosystem
question is entirely different (see §7).

## 7. The C/C++ → WASI ecosystem (why the plugin ABI is safe to lock)

The plugin ABI does not depend on the Python-on-WASM ecosystem at all. It depends on the
**C/C++/Rust/Go → WASI** toolchain, which is mature: `wasi-sdk` (clang + wasi-libc) compiles
ordinary C and C++ sources, and the component tooling wraps the result in a WIT-typed component.
That makes the large, stable, permissively-licensed C/C++ library ecosystem — SQLite, tree-sitter,
libarchive, image and audio codecs, tokenizers, ONNX Runtime, DuckDB, compression and crypto
primitives — available as **sandboxed, capability-scoped, cross-platform plugins** rather than host
dependencies that would inherit full process authority.

This inverts the usual dependency risk: a heavy library becomes *safer* to adopt as a plugin than
as a linked dependency, because a plugin holds only the capabilities the host hands it.

## 8. Sandboxing techniques — the 2026 trade space

| Technique | Boundary | Cold start | Portability | Python ecosystem |
|---|---|---|---|---|
| WASM + WASI (wasmtime) | Software, capability-based, no ambient authority | µs–ms | Identical on Win/Linux/macOS | stdlib + pure-Python only (§6) |
| MicroVM (Firecracker-class) | Hardware, own kernel per workload; ~125 ms boot, <5 MiB overhead, ~150 VM/s/host | 150 ms–2 s | Linux/KVM; Windows via WHP; macOS weak | Full |
| Hyperlight / Hyperlight Wasm | Hypervisor-backed micro-guest, no kernel/devices, <2 ms start; WHP on Windows, KVM/mshv on Linux; CNCF Sandbox project | <2 ms | Windows + Linux; **explicitly experimental, not production-grade** | Wasm workloads |
| gVisor | User-space kernel, syscall interception; 10–30 % I/O overhead | Fast | Linux only | Full |
| OS process jail (seccomp+namespaces / AppContainer+Job Object / sandbox-exec) | Kernel-enforced, per-OS | ms | Three separate implementations | Full |
| K8s **Agent Sandbox** (SIG Apps CRD) | Declarative API decoupled from isolation backend (gVisor, Kata); pause/resume, stable identity, scheduled deletion | n/a (cluster) | Cluster-side | Full |

Sources: [Zylos research](https://zylos.ai/research/2026-04-04-ai-agent-sandboxing-security-isolation/),
[Northflank](https://northflank.com/blog/how-to-sandbox-ai-agents),
[Hyperlight](https://github.com/hyperlight-dev/hyperlight),
[Agent Sandbox](https://agent-sandbox.sigs.k8s.io/).

**Consequence (RFC 008):** no single technique wins on all axes, which is precisely why isolation is
a seam with named profiles rather than a hardcoded choice — and why the *remote* profile targets the
Kubernetes Agent Sandbox CRD, a standard API that itself decouples from the isolation backend.

## 9. Inference API surface

The de-facto interoperable shapes are the OpenAI Chat Completions and Responses APIs (implemented
by a long tail of gateways and local servers), alongside first-party Anthropic and Google APIs.
Anthropic's current families are Claude 5 (Fable 5, Opus 5, Sonnet 5) and Haiku 4.5.

**Consequence (RFC 004):** the provider seam is modelled on *capabilities* (streaming, tool calling,
structured output, reasoning traces, caching, multimodal parts) rather than on one vendor's request
shape, with an OpenAI-compatible client as the widest-reach default backend.

## 10. What this record does not cover

Deliberately unexplored in revision 0.1, tracked in `OpenQuestions.md`: A2UI, AP2, X42, agent
identity standards beyond OAuth/OIDC (SPIFFE-style workload identity for agents), evaluation
standards, model-signing/provenance, and the emerging agent-package/registry formats.
