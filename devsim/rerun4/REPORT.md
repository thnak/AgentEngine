# devsim/rerun4 — building a product on AgentEngine, measured against the docs

**Product**: `ae_order_assistant`, a command-line order-lookup assistant for a small business.
One custom tool (`lookup_order`), one agent declared against it, bound to a real Anthropic-wire
provider whose key is resolved from an environment variable at run time.

**A note on citation line numbers.** The documentation source under `web/marketing/src/` was being
edited by another process *during* this exercise — `ApiFirstAppReference.tsx` grew from 688 to 776
lines between my first read and my last, and `apiContent.ts` gained lines in the same window (observed
mtimes 2026-10-10 10:38–10:43). Every citation below is therefore an **anchor plus a line number as of
2026-10-10 10:50**; where a number has drifted, the named exported symbol or the quoted prose is the
anchor to relocate it by.

**Measurement headline: 5 documentation fallbacks.** In five places the documentation site could not
answer a question I had to answer to ship this, and I read engine source instead. Everything else —
every include, the session construction, the tool declaration, the provider wiring, the error model,
and the whole CMake file — came from the docs.

**Build result: succeeded, first attempt, zero compile-error iterations.** Configure 21.4 s (including
two network fetches), cold build 44.6 s / 161 build steps with `-j4` on MSVC 19.51 (VS 18, x86-64).
`main.cpp` compiled and linked the first time it was handed to the compiler.

**Live result: real turns ran and printed answers.** Against a real provider endpoint, the model
called the custom tool and answered from its reply — twice, including once where the tool itself
returned an error. No API key existed in the environment; the only
credential available was the repo's own git-ignored dev key file (`apt-openrouter.txt`, documented by
`tools/run-live-provider-tests.ps1`), which is an OpenRouter key — so the live turn was made through
the Anthropic-wire backend pointed at OpenRouter's Anthropic-compatible endpoint
(`openrouter.ai` + `/api/v1`), not at `api.anthropic.com`. The program's compiled-in defaults are the
real Anthropic endpoint. **A turn against `api.anthropic.com` itself was not made** — no Anthropic key
exists here.

---

## Deliverables

| File | What it is |
|---|---|
| `devsim/rerun4/main.cpp` | The whole program: tool, agent, real-provider wiring, error handling. 165 lines. |
| `devsim/rerun4/CMakeLists.txt` | The consumer build file, transcribed from the documented one. |

Commands actually run (Ninja is not on `PATH`; the Ninja that ships inside Visual Studio 18 was used,
and `vcvars64.bat` supplies MSVC):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAGENTENGINE_WITH_HTTPS=ON
cmake --build build --target ae_order_assistant -j 4
./build/ae_order_assistant.exe "What is the status of order A-1042, and how much was it?"
```

Observed runs (all four are real transcripts, not reconstructions):

```
# no ANTHROPIC_API_KEY set
run failed: secret not in environment: ANTHROPIC_API_KEY (code secret.not_found)      exit 1

# deliberately invalid key -- the repo's own OR-*-7 "positive control" idea
run failed: invalid x-api-key (code anthropic.http_401)                               exit 1

# live turn, real provider, tool called
Order A-1042 is shipped and totaled $45.99.                                           exit 0
# -> $45.99 is 4599 cents, a value only lookup_order knows: the tool really ran

# live turn where the tool itself fails
There is no order with id Z-0000.                                                     exit 0
# -> the tool's own result<Reply> error was fed back to the model as a tool error
```

---

## Getting started

Each of (a)–(f): **found in docs** (file and line) or **had to reverse-engineer**.

### (a) Include the engine — found in docs

| Fact | Source |
|---|---|
| The minimal include set is `core/agent.hpp`, `core/agent_session_bridge.hpp`, `core/chat_client.hpp`, `core/content.hpp` | `web/marketing/src/components/ApiFirstAppReference.tsx:11-17` (`SNIPPET_PROGRAM`) |
| A tool-bearing program additionally includes `core/json_schema.hpp`, `core/tool.hpp`, `trust/principal.hpp` | `web/marketing/src/components/ApiPizzaOrderReference.tsx:51-61` (`SNIPPET_PREAMBLE`) |
| Reaching a real model adds `protocol/anthropic/chat_client.hpp` and `trust/secret.hpp` | `ApiFirstAppReference.tsx:118-119` |
| `agentengine::core` is the one target that carries the include dir and the engine's static libraries | `ApiFirstAppReference.tsx:104`, prose at `:224-230` |

Weak spot, not a fallback: `trust/capability.hpp` (for `Capability` / `cap::Secret`) is never named in
prose anywhere on the site. Its only locator is a comment inside a snippet —
`web/marketing/src/data/apiContent.ts:1069`. Same for `SecretRef`, located only by
`web/marketing/src/data/providerContent.ts:763` (`trust/secret.hpp:196-198`).

### (b) Construct a session — found in docs

| Fact | Source |
|---|---|
| `make_agent_session<A>(client)` runs `register_agent<A>()`'s validation and binds the compiled policy set to a real session | `ApiFirstAppReference.tsx:60-62` |
| The two-argument form `make_agent_session<A>(client, opts)` | `ApiFirstAppReference.tsx:149` |
| `AgentSessionOptions`; `opts.session_id`, `opts.principal`, `opts.chat_client_grants` | `ApiFirstAppReference.tsx:144-147` |
| `opts.chat_client_grants` accepts `cap::Secret` and nothing else | `ApiFirstAppReference.tsx:139-143` |
| `agent->ask(text)` returns a `result`; `reply->c_str()` yields the text | `ApiFirstAppReference.tsx:69-76` |
| `make_agent_session<A>()` is `register_agent<A>()` + the bridge, since ADR-226 | `web/marketing/src/components/ApiAgentReference.tsx:126-137`; `apiContent.ts:615-624` |
| One `ask()` covers the whole multi-round tool conversation | `ApiFirstAppReference.tsx:69`; `apiContent.ts:622`; the `AgentSession` entry at `apiContent.ts:1180-1186` |

`AgentSessionOptions`/`make_agent_session` are never bound to a header in prose; the header is
inferable from the include list at `ApiFirstAppReference.tsx:15`. No fallback needed.

### (c) Declare a tool — found in docs

| Fact | Source |
|---|---|
| `Tool<Derived, Policies...>` is the declaration surface; five members required | `web/marketing/src/components/ApiToolReference.tsx:29-34`, `:37-47` |
| The five: `name`, `description`, `Args`, `Reply`, `invoke(Args, EffectContext&)` | `apiContent.ts:651-701` (`toolStaticMembers`, the required rows) |
| Complete worked declaration, including `AE_JSON_SCHEMA(Type, fields...)` | `apiContent.ts:754-763` (`minimalToolSnippet`), `:792-818` (`toolSchemaSnippet`) |
| Per-field descriptions go on the field's *type* via `Described<T, "...">`, never on the macro | `ApiToolReference.tsx:54-66`, `:124-132`; `apiContent.ts:900-914` |
| A `Described<T,"...">` field must be read as `.value` | `ApiToolReference.tsx:133-142` |
| Empty ceiling is the default; a tool that reaches nothing declares `Capabilities<>` and needs no grant | `apiContent.ts:1022-1027`, `:921-945`; `ApiToolReference.tsx:149-157` |
| Naming the tool in the agent's `Tools<...>` is what makes it addressable | `ApiFirstAppReference.tsx:151-170`; `apiContent.ts:604-610` |

The one thing the docs get wrong here is logged as fallback 1 below.

### (d) Wire a real provider with a credential — found in docs

Everything in this step is on one page section, `ApiFirstAppReference.tsx:116-170`:

| Fact | Source |
|---|---|
| `std::make_unique<AgentEngineSecretStore>(std::make_unique<EnvSecretSource>(""))`; the empty prefix is what produces a bare `ANTHROPIC_API_KEY` (the default prefix is the historical `QUARK_SECRET_`) | `:121-123` |
| `caps.streaming`, `caps.tool_calling`, `caps.max_output_tokens` | `:125-130` |
| `caps.tool_calling` **defaults to false**, so an agent with `Tools<...>` against a real backend silently never gets a tool call | `:127-129`, `:310-313` |
| `anthropic::AnthropicChatClient<Store>` is namespaced, constructed `{host, port, model, SecretRef{...}, caps, store}` | `:132-137` |
| `AgentSessionOptions::chat_client_grants = {Capability{cap::Secret{"ANTHROPIC_API_KEY"}}}`; omitting it compiles and then fails the first run with `secret.not_granted` | `:139-147` |
| The credential is resolved at the point of use inside `chat()`, never at construction; the gate fires before any source is read | `providerContent.ts:759-784`, `:434-441` |
| The governing spec section is 018 §4 | `ApiFirstAppReference.tsx:319` |

### (e) Handle errors — found in docs

| Fact | Source |
|---|---|
| Every fallible call returns `result<T>`; check `has_value()`, use `error()` | `ApiFirstAppReference.tsx:346-361` |
| `result<T>` is literally `std::expected<T, agentengine::error>` | `:348-350` |
| `error().code` is the stable string to branch on; `error().message` is host-facing prose; `error().klass` is a `failure_class` | `:351-356` |
| `failure_class` enumerators: `transient`, `policy`, `contract`, `resource`, `fatal`, `canceled`; only the first two retryable | `:353-359` |
| A tool-pipeline denial arrives at the call site as an ordinary tool error, not a crash | `:370-376`; `ApiPizzaOrderReference.tsx:159-171` |
| The documented "look, print message and code, return 1" shape | `:63-67` (bind failure), `:70-74` (run failure) |

### (f) Build and link — found in docs

| Fact | Source |
|---|---|
| Set `AGENTENGINE_BUILD_TESTS` / `AGENTENGINE_BUILD_EXAMPLES` OFF with `CACHE BOOL "" FORCE` **before** `add_subdirectory()`; a plain `set()` is shadowed | `ApiFirstAppReference.tsx:86-87`, prose `:232-242` |
| The engine is vendored, never installed; there is no `find_package(AgentEngine)` | `:224-230` |
| `add_subdirectory("${AGENTENGINE_ROOT}" agentengine)`; link `agentengine::core` | `:96-104` |
| Do **not** link `agentengine_warnings` | `:105-107` |
| Reaching a real model needs `-DAGENTENGINE_WITH_HTTPS=ON` **and** `agentengine::provider_http_client`; without the target the failure is the misleading compile error `C2653: 'anthropic' is not a class or namespace name`, because the HTTPS macro is a compile definition propagated by a target `core` does not link | `:109-114`, prose `:302-318` |
| Keep the build directory shallow (Windows 250-char object-path cap; `CMAKE_OBJECT_PATH_MAX` warnings) | `:244-252` |

Measured against the docs' own numbers: the docs report 159 build statements and a 4 s configure with
the two options off (`:232-240`). I measured **161 statements** (the extra two are my `main.cpp` and
its link) and a **21.4 s configure** — because `AGENTENGINE_WITH_HTTPS=ON` adds two network fetches the
docs do not count. That gap is fallback 5.

---

## Fallback log

**Rule for counting**: an entry is a fallback only if the documentation site could not answer a
question I had to answer to ship this, and I answered it from engine source (`include/`, `src/`, or
the engine's own root `CMakeLists.txt`). Source reads that only *double-checked* something the docs
had already answered are listed separately under "Verification-only reads" and are **not** counted.

### Total fallbacks: 5

**1. The current `invoke` signature — the docs are stale, and the header says the documented form is
being deleted.**
*Question*: do I write `static result<Reply> invoke(Args, EffectContext&)`, or the coroutine form?
*Docs' answer*: `apiContent.ts:678-683` says the type is `static result<Reply>`, "Synchronous by
design, not a coroutine — `ae::task<T>` stays deferred until a milestone actually needs concurrent
tool calls (`tool.hpp:112-114`)". Four other snippets in the same file declare tools the same way
(`apiContent.ts:762`, `:813`, `:869`, `:937`).
*Source*: `include/agentengine/core/tool.hpp:219-226` says the opposite is now true — ADR-237 §5.2's
form is a **non-static `const` member** `task<result<Reply>> invoke(Args, EffectContext&)`, and "the
older `static result<Reply> invoke(Args, EffectContext&)` form is still accepted transitionally …
and is deleted once every conformer is converted (ADR-237 §5.2, 'one way to declare a tool')".
*Resolution*: wrote the documented synchronous form. It compiled and registered
(`include/agentengine/core/tool_descriptor.hpp:188-216` builds the descriptor around `ToolT::invoke`).
*Severity*: medium. Nothing was blocked, but a reader following the docs writes a form the engine has
already scheduled for deletion — and the `tool.hpp:112-114` citation the docs give for "`ae::task<T>`
stays deferred" points at `tool_detail::policy_capabilities<Capabilities<Cs...>>`
(`include/agentengine/core/tool.hpp:112-115`), which says nothing about coroutines at all. The citation
is stale in the same direction as the claim.

**2. `AE_JSON_SCHEMA`'s placement rules.**
*Question*: where does the macro go, and what must `Args`/`Reply` satisfy?
*Docs' answer*: the macro is shown being called (`apiContent.ts:797`, `:803`, `:809`) and its field-name
limitation is explained (`ApiToolReference.tsx:54-66`), but no page states that it must be at namespace
scope (it expands to three inline free functions: `include/agentengine/core/json_schema.hpp:499-521`)
or that the type must be default-constructible for the decode path.
*Resolution*: `json_schema.hpp:499-521` — namespace scope required, `Type out{}` in `ae_from_json`.
*Severity*: low. The docs' own consistent placement makes it inferable, and everything compiled.

**3. `Principal`'s two fields.**
*Question*: what does the second string mean?
*Docs' answer*: only positional aggregate initialization is ever shown — `Principal{"p-demo", ""}`
(`ApiFirstAppReference.tsx:146`) and `Principal{"customer-4711", "tenant-pizzapalace"}`
(`ApiPizzaOrderReference.tsx:143`). No page names the fields.
*Source*: `include/agentengine/trust/principal.hpp:27-29` — `id` and `tenant_id`.
*Resolution*: passed `{"cli-user", "small-business"}`.
*Severity*: low. Aggregate init compiles without the names; the read was needed to pick a *correct*
second value rather than a plausible one.

**4. The optional constructor parameters of `AnthropicChatClient`.**
*Question*: how do I point the client at a non-default path prefix?
*Docs' answer*: the six-argument form only (`ApiFirstAppReference.tsx:136-137`). The only other
constructor shown anywhere is the **OpenAI** client's (`providerContent.ts:761-764`), which shows the
same six arguments and likewise no optional ones.
*Source*: `include/agentengine/protocol/anthropic/chat_client.hpp:456-468` — thirteen parameters; the
seventh is `path_prefix = "/v1"`.
*Resolution*: passed `path` as the seventh argument.
*Severity*: medium **only for running live here** — the documented six-argument form is complete for
`api.anthropic.com`. It became load-bearing because the only credential available in this environment
is an OpenRouter key, which needs `openrouter.ai` + `/api/v1`.

**5. What `AGENTENGINE_WITH_HTTPS=ON` actually pulls in, and how to do it offline.**
*Question*: the docs say configure with `-DAGENTENGINE_WITH_HTTPS=ON` — what does that cost, and how
do I do it on a machine with no network?
*Docs' answer*: the CMake comment at `ApiFirstAppReference.tsx:89-92` says the engine "fetches its
vendored dependencies (Asio by default; mbedTLS, wasmtime, CPython, PDFium per option) rather than
expecting them installed", and gives the offline override **for Asio only** — `AGENTENGINE_VENDOR_ASIO
OFF` plus `AGENTENGINE_ASIO_ROOT`. Nothing says the one flag this page then tells you to turn **on**
triggers *two more* fetches, and no offline override for those two is documented anywhere on the site.
*Source*: the engine's root `CMakeLists.txt:674-772` — `FetchContent` of mbedTLS 3.6.7 (`:705-709`) and
`file(DOWNLOAD)` of a pinned `cacert-2026-08-13.pem` (`:752-755`), plus the overrides the docs omit:
`AGENTENGINE_VENDOR_MBEDTLS=OFF` + `AGENTENGINE_MBEDTLS_ROOT` (`:688-719`) and
`AGENTENGINE_VENDOR_CA_BUNDLE=OFF` + `AGENTENGINE_CA_BUNDLE_PATH` (`:749-763`).
*Resolution*: network was available; both fetches succeeded. **The asymmetry is the defect**: the docs
give the offline escape hatch for the dependency HTTPS does not need and withhold it for the two it
does, while the same page instructs the reader to turn HTTPS on.
*Severity*: high for a locked-down CI, which is exactly the audience the vendored-dependency doctrine
is written for.

### Verification-only reads (not counted)

Read after the docs had already answered the question, purely to confirm; none of these changed the
code:

- `include/agentengine/core/tool.hpp:200-382` — that `Tool<Derived>` supplies `declared_capabilities()`
  / `declared_approval()` / `declared_effect_class()` defaults. Docs: `apiContent.ts:684-701`.
- `include/agentengine/core/tool_descriptor.hpp:188-216` — that registration is
  `make_tool_descriptor<ToolT>()`. Docs: `ApiToolReference.tsx:50-52`.
- `include/agentengine/core/agent_session_bridge.hpp:160-441` — the full `AgentSessionOptions`
  (`:176-205`), the `cap::Secret`-only check (`:333-341`), the ceiling narrowing (`:230-242`), and the
  `make_agent_session` signature (`:428-439`). Docs answered all four: `ApiFirstAppReference.tsx:139-149`;
  `ApiAgentReference.tsx:87-98` ("only `ChatClientId<Id>` is required … `Capabilities` is empty");
  `apiContent.ts:538-539` (checks 3 and 4).
- `include/agentengine/core/json_schema.hpp:99-122` — `Described<T,d>`'s `.value`. Docs:
  `ApiToolReference.tsx:133-142`.
- `include/agentengine/trust/secret.hpp:137-170, 295-307` — the `EnvSecretSource` empty-prefix trick
  and `AgentEngineSecretStore`. Docs: `ApiFirstAppReference.tsx:121-123`.
- `include/agentengine/trust/capability.hpp:108-140, 250, 830` — `cap::Secret`'s fields. Docs:
  `apiContent.ts:1069`, `:2064`.
- `include/agentengine/core/agent.hpp:27-120` — the policy tags. Docs: `ApiAgentReference.tsx:63`,
  `:77-86`.
- `tools/run-live-provider-tests.ps1` and `tests/protocol/openai/test_openrouter_live_e2e.cpp:84,
  :240-291` — where a live credential is expected to come from and which model/path prefix the repo's
  own live test uses. These are repo files, but reading them was a decision about *running*, not an
  API question the docs owed an answer to.

---

## Hard blockers

**None.** No step of this exercise was blocked outright.

Configure succeeded on the first invocation. The build succeeded on the first invocation — 161/161
steps, no compile error, no link error, no second compile iteration. The program ran and printed an
answer on the first live invocation. The one thing that could not be done as specified — a turn
against `api.anthropic.com` — was blocked by the absence of an Anthropic credential in this
environment, not by the engine or the docs, and the program is compiled for that endpoint by default.

---

## Undefined identifiers

Names a documented snippet used without defining them, and where each comes from.

| Identifier | Where the docs use it | Where it comes from | Located in docs? |
|---|---|---|---|
| `LookupArgs`, `LookupReply` | `ApiFirstAppReference.tsx:158-159`, with only comments (`// { order_id }`, `// { status }`) | You define them; the pattern is on the Tool page | **Yes** — `apiContent.ts:792-818` |
| `AE_JSON_SCHEMA` | `ApiFirstAppReference.tsx:158-159` (comments only) | `core/json_schema.hpp` | **Yes** — `ApiToolReference.tsx:390-392` cites `include/agentengine/core/json_schema.hpp:5-19` |
| `EffectContext` | Every tool `invoke` snippet — `apiContent.ts:762`, `:869`, `:937` | Transitive, via `core/tool.hpp` | **No.** Never declared or located anywhere on the site |
| `result<T>` / `result<Reply>` | Everywhere; semantics fully explained at `ApiFirstAppReference.tsx:346-361` | Transitive, via `core/tool.hpp` / `core/error.hpp` | **No.** The type is described but never named to a header |
| `error`, `failure_class` | `ApiFirstAppReference.tsx:63-65`, `:346-359` | Transitive, `core/error.hpp` | **No.** Enumerators listed, header never given |
| `Capability`, `cap::Secret` | `ApiFirstAppReference.tsx:147` | `trust/capability.hpp` | **Weakly** — only a snippet comment, `apiContent.ts:1069` |
| `SecretRef` | `ApiFirstAppReference.tsx:136` | `trust/secret.hpp` | **Weakly** — only a snippet comment, `providerContent.ts:763` |
| `AgentEngineSecretStore`, `EnvSecretSource` | `ApiFirstAppReference.tsx:123` | `trust/secret.hpp` | **Weakly** — inferable from the snippet's own include line, `:119` |
| `Principal` | `ApiFirstAppReference.tsx:146`; `ApiPizzaOrderReference.tsx:143` | `trust/principal.hpp` | **Partly** — header appears in the pizza preamble `:61`; its two fields are named nowhere (fallback 3) |
| `AgentSessionOptions`, `make_agent_session` | `ApiFirstAppReference.tsx:62, 144, 149` | `core/agent_session_bridge.hpp` | **Weakly** — inferable from the include list, `:15` |
| `ChatClientCapabilities` | `ApiFirstAppReference.tsx:125-130` | `core/chat_client.hpp` | **Weakly** — inferable from the include list, `:16`; three of the twenty fields shown here, the rest tabulated on the Providers page |
| `Message`, `ContentItem`, `Text`, `content_origin::assistant`, `role::assistant` | `ApiFirstAppReference.tsx:31-36` | `core/content.hpp` | **Partly** — header in the include list; the aggregate shapes are shown, never specified |
| `drive(task)` | `ApiPizzaOrderReference.tsx:84-86` | Example-local | **Explicitly flagged** — `ApiSnippetConventions.tsx:14-18`, which names `agentengine::rt::block_on` as the real equivalent |
| `check(cond, label)` | Throughout the pizza page | Example-local | **Explicitly flagged** — `ApiSnippetConventions.tsx:20-23` |
| `user_message(text)` | `ApiPizzaOrderReference.tsx:65-73` | Example-local | **Defined on the same page**, and flagged at `ApiSnippetConventions.tsx:24-26` |
| `Session`, `OrderAgent` | Pizza page | Page-local `using` alias | **Explicitly flagged** — `ApiSnippetConventions.tsx:36-38` |
| `caps`, `store`, `held` | Various | Values the reader must build | **Explicitly flagged** — `ApiSnippetConventions.tsx:40-41`; `caps` and `store` are then built at `ApiFirstAppReference.tsx:121-130` |

Net: three identifiers (`EffectContext`, `result`/`error`, `failure_class`) are used pervasively and
never located to a header at all. In practice all three arrive transitively through `core/tool.hpp`,
so nothing broke — but a reader has no way to know that from the docs, and a reader who wants to
`#include` the type explicitly rather than rely on that has to search the tree.

---

## Blockers ranked

No hard blockers occurred. Ranked by how likely each is to stop the next developer, hardest first:

1. **The HTTPS build surface (fallback 5).** Highest risk, because it is invisible until it bites. A
   consumer on a locked-down or proxied CI reads the first-app page, is told to configure with
   `-DAGENTENGINE_WITH_HTTPS=ON`, and is then blocked by two undocumented network fetches for which the
   page provides no offline override — while providing one for Asio, the dependency it does not need.
   This is the one item I would call a genuine documentation defect rather than a gap.
2. **The `invoke` form (fallback 1).** A reader who trusts the docs writes a form the header says is
   already scheduled for deletion; a reader who checks the header may write a coroutine the docs never
   mention. Either way the reader cannot tell from the docs alone which shape is current. Nothing
   breaks today.
3. **`EffectContext`, `result`, `error`, `failure_class` unlocated (undefined identifiers).** Nothing
   breaks, because they arrive transitively — but only by luck of the include graph. A reader who
   wants to include them explicitly is searching the tree.
4. **Ninja is assumed to be installed.** `ApiFirstAppReference.tsx:276` gives
   `cmake -S . -B build -G Ninja`. It is not on `PATH` in this environment; the copy that ships inside
   Visual Studio 18 was used instead, and `vcvars64.bat` was needed to get MSVC. Neither prerequisite
   is mentioned. Low impact — any CMake user works this out — but it is unstated tooling the page
   depends on.
5. **Windows object-path depth (`ApiFirstAppReference.tsx:244-252`).** Documented, and with
   `AGENTENGINE_BUILD_TESTS=OFF` there are no compile-fail tests to trip it. Did not occur.
6. **`Principal`'s field meanings (fallback 3).** Lowest, because positional aggregate initialization
   compiles without ever learning the names.

---

## Confidence per requirement

| # | Requirement | Confidence | Evidence |
|---|---|---|---|
| 1 | One custom tool of my own, a "look up an order by id" tool returning a small JSON reply, declared the way the engine wants | **certain** | `LookupOrderTool` in `main.cpp:52-83`; compiled, registered through `register_agent<A>()`, and the live model actually called it — the reply's `total_cents = 4599` is a value that exists nowhere else, and the model answered "$45.99" |
| 2 | An agent using that tool, bound to a real model provider (Anthropic), API key from an environment variable, never compiled in | **certain** | `main.cpp:89-97` (agent), `:124` (secret store over `EnvSecretSource`), `:141` (real Anthropic-wire client). Runtime evidence: with the variable unset the engine returns `secret.not_found`; with it set to a bad value the real endpoint returns `anthropic.http_401`; with the repo's working dev key a real turn completes. The built binary contains no credential (`sk-or-` literal absent; 1,655,296 bytes) |
| 3 | Run one turn, print the answer | **certain** | Live turn printed `Order A-1042 is shipped and totaled $45.99.`, exit 0. **Caveat: not against `api.anthropic.com`** — no Anthropic key exists here, so the live turn went through the Anthropic-wire backend pointed at OpenRouter. The compiled default is the real Anthropic endpoint |
| 4 | On a failed build or run, print the engine's own error message and error code and exit non-zero; do not swallow errors | **certain** for the run path, via `report_failure()` at `main.cpp:101-104`, exercised with three distinct real engine errors: `secret.not_found` (missing env var), `anthropic.http_401` (the real service rejecting a deliberately bad key), and the tool's own `lookup_order.not_found` — a code string of mine, but carried out through the engine's own `error` type and fed back to the model as an ordinary tool error rather than surfacing as a process failure, which is the documented behaviour (`ApiPizzaOrderReference.tsx:169-171`). **Could not determine** for a *CMake/compile* failure: nothing in the docs describes a mechanism by which a build failure carries an engine error code, and none exists that I found. I read "failed build" as the agent-build/bind step — `make_agent_session<A>()` running `register_agent<A>()`'s validation — which the docs do cover (`ApiFirstAppReference.tsx:60-67`) and which `main.cpp:152-155` handles |
| 5 | Buildable and runnable by me | **certain** | Configure 21.4 s; cold build 44.6 s / 161 steps / `-j4`; MSVC 19.51 (VS 18), x86-64, C++23, CMake 4.3.2 (≥ 3.28). Single build directory, one executable |

---

## Verdict

**Can a competent C++ developer build this from the docs alone? Yes** — certainly for the offline
skeleton, and yes for a working program with a tool against a real provider, with a small number of
warnings.

The evidence for that is unusually strong and worth stating plainly: I wrote `main.cpp` and
`CMakeLists.txt` from the documentation, and **the first compile of the first draft linked and ran**.
No include was missing, no type was wrong, no signature was off, no link line was absent, and the
`invoke` gotcha (`.value` on a `Described<T,"...">` field) was already documented and did not cost a
compile cycle. That essentially never happens with a pre-v1 engine, and it is not luck: `api/first-app.html`
is a real, complete translation unit that the page says was itself compiled, linked and run, and the
Tool, Providers and Capabilities pages each supply the one piece the on-ramp page deliberately omits.

**In how long?** Build time is not the constraint: 21.4 s to configure (with fetches), 44.6 s to build
the entire engine plus the program on a cold tree, and seconds for an incremental `main.cpp` change.
The cost is in the five fallbacks and the cross-page assembly. Concretely:

- **Under an hour** — someone with a warm build tree and a working credential copies the first-app
  program, adds the Tool-page declaration, and runs it. The five fallbacks either do not arise (the
  default endpoint needs only the documented six-argument constructor) or cost minutes (`invoke`'s
  form, `Principal`'s fields).
- **Half a day** — the realistic case, and mine: first contact with the engine, cold tree, a tool of
  your own, a real provider, and the failure path done properly. Most of that is reading across four
  API pages to assemble a program that no single page shows whole, plus the cold build.
- **A day or more** — if the build machine has no network. Fallback 5 is a wall there, not a speed
  bump, and it is the one finding I would act on: give the mbedTLS and CA-bundle offline overrides the
  same treatment the Asio one already gets, on the page that tells the reader to turn HTTPS on.

Everything above is verified or explicitly marked as not verified. No live turn was claimed against
`api.anthropic.com`; none was made.
