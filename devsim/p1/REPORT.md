# devsim/p1 — building a product on AgentEngine, and what the docs did and did not give me

Consumer-side simulation: a small-business order-desk CLI. One declared tool, one declared agent,
a real Anthropic backend whose API key comes from the environment, one turn, engine errors surfaced
verbatim. Nothing in this directory is part of AgentEngine; no existing repo file was modified.

Deliverables in this directory:

| File | What it is |
| --- | --- |
| `main.cpp` | The product. Declared agent + real provider + one turn + engine error reporting. |
| `order_assistant_decl.hpp` | The tool and agent declaration, shared with the verifier below. |
| `CMakeLists.txt` | The consumer build. |
| `verify_tool_call.cpp` | Dev-aid target (off by default): drives the *same* declaration with a scripted in-process client, proving the tool really runs without a credential. |
| `out/` | My build tree (Visual Studio 18 2026, x64, Release; ~150 MB, safe to delete). |

## 0. A note on the state of the docs during this session — read this first

**The documentation site source was being edited by someone else in this same workspace while I was
working.** `git status` shows 7 modified files under `web/marketing/` that are not mine (I wrote only
inside `devsim/p1/`), plus several untracked ones. Concretely:

* `web/marketing/src/components/ApiFirstAppReference.tsx` and `web/marketing/api/first-app.html` did
  **not exist when I started** (`apiPages` in `apiContent.ts` had no `first-app` entry when I read it)
  and appeared mid-session. That page documents exactly the two things I had reverse-engineered as my
  biggest doc failures: consumer CMake wiring (`add_subdirectory` + `agentengine::core` +
  `AGENTENGINE_BUILD_TESTS/EXAMPLES OFF` before `add_subdirectory`), and a complete program with its
  `#include` lines and its error handling.
* `apiContent.ts` grew ~41 lines above line 561 in that window, so every line number I cite for
  `apiContent.ts` is from the revision I read. Other cited files (`builderContent.ts`,
  `providerContent.ts`, `content.ts`, `ApiPizzaOrderReference.tsx`) were untouched and their line
  numbers are current.

**So the report below has two readings.** The fallback log is what was true against the docs as they
stood when I started; several entries are closed by `first-app` as it stands now. I flag each one.

I also verified the new page's own real-model snippet and it does not work as printed — see
"Defects in the new `first-app` page" below. That finding is empirical, not a reading of prose.

## 1. What I actually verified (so the confidence section means something)

| Run | Result |
| --- | --- |
| `order_assistant.exe`, no `ANTHROPIC_API_KEY` | exit 1, `ask failed: secret not in environment: ANTHROPIC_API_KEY (code: secret.not_found)` |
| `order_assistant.exe`, `ANTHROPIC_API_KEY=sk-ant-dummy…` | exit 1, `ask failed: invalid x-api-key (code: anthropic.http_401)` |
| `order_assistant.exe`, dummy key, `chat_client_grants` omitted | exit 1, `ask failed: secret 'ANTHROPIC_API_KEY' resolved without a granted Secret<name> capability (code: secret.not_granted)` |
| `order_assistant.exe` linked against `agentengine::core` only | **compile error**: `'anthropic' is not a class or namespace name` |
| `order_assistant_verify_tool_call.exe` | exit 0, prints `Order 1042 is shipped.`; schema, `Described<…>` description, direct `invoke()`, session bind and the tool round trip all pass |

The dummy-key run is the strongest evidence available here: the key was resolved from the
environment, a real TLS connection to `api.anthropic.com` was made, Anthropic answered HTTP 401, and
the engine's own error surfaced with its own code. The one thing I could not do is complete a
successful model turn — this machine has no valid credential (I checked the environment; none of
`ANTHROPIC_API_KEY` / `OPENAI_API_KEY` / `AGENTENGINE_*_API_KEY` is set). Everything up to and
including the HTTP exchange is proven; the model's answer is not.

## 2. Getting started

### (a) Include the engine — **had to reverse-engineer**
No doc page showed a single `#include` when I started (`grep '#include'` over
`web/marketing/src/data/*.ts` and `components/Api*.tsx` found only prose about a lint rule over
`include/` and `src/`). **The new `first-app` page closes this**: `ApiFirstAppReference.tsx:11-17` is
a complete include list. What I fell back to: `examples/01_hello_agent.cpp:27-33` (the include block
of the engine's own smallest example), then `glob include/agentengine/**` to find
`protocol/anthropic/chat_client.hpp`, `core/agent_session_bridge.hpp`, `trust/secret.hpp`.

### (b) Construct a session — **shape found in docs; the part that makes it work reverse-engineered**
Found in docs:
* `apiContent.ts:602-608` (`minimalAgentSnippet`) and `:613-622` (`helloAgentRunSnippet`) — the
  declared-agent path: `auto agent = make_agent_session<Joker>(JokerChatClient{});` then
  `agent->ask("…")`. This is the only construction path in the docs that carries the agent's declared
  tools, so it is the one I had to use.
* `apiContent.ts:624-640` (`multiTurnSnippet`) — the other path, a raw
  `agentengine::rt::AgentSession<ChatClientT>` with `initialize(...)`, `emplace_chat_client()`,
  `set_capabilities()`.
* `builderContent.ts:301-323` (`quickstartWorkedExampleSnippet`) — `OpenAiSessionBuilder(model)
  .endpoint(...).api_key_from_env(...).declare_capabilities(caps).build()`. Attractive, but it has no
  way to attach declared tools, so it cannot satisfy this product.

Reverse-engineered: the **second parameter of `make_agent_session`**. `AgentSessionOptions` occurs
**zero times** in the entire docs site, and so does `chat_client_grants` — yet without a
`cap::Secret` in `chat_client_grants` a real provider fails closed at run time (I proved this; see
the table above). Fell back to `include/agentengine/core/agent_session_bridge.hpp:176-205` (the
struct) and `:415-439` (the two entry points), plus `:180-193` for the rule that
`chat_client_grants` may carry `cap::Secret` and nothing else, and `:29-31` for "the session's
capability set is the caller's grants narrowed to the agent's ceiling".

### (c) Declare a tool — **found in docs**
The docs are genuinely good here, and this is the half of the product that came straight from them:
* `apiContent.ts:752-761` (`minimalToolSnippet`) — the five required members.
* `apiContent.ts:649-750` (`toolStaticMembers`) — the required/optional member table, including that
  `declared_capabilities()`/`declared_approval()`/`declared_effect_class()` come from
  `Tool<Derived>` itself.
* `apiContent.ts:790-816` (`toolSchemaSnippet`) and `:818-837` (`toolSchemaOutput`) — `AE_JSON_SCHEMA`
  and the exact JSON Schema it produces (I used the same shape: nested `AE_JSON_SCHEMA` struct +
  `std::vector<T>`).
* `apiContent.ts:902-912` (`describedFieldSchemaSnippet`) — `Described<T, "…">` for a field
  description.

The one gap: the docs show `Described<…>` being *declared* and never being *read*. Reading it cost me
a real compile error (`operator==` template deduction does not consider the wrapper's implicit
conversion). Fell back to `include/agentengine/core/json_schema.hpp:99-122`.

### (d) Wire a real provider — **partly in docs; the working sequence reverse-engineered**
Found in docs:
* `providerContent.ts:759-784` (`credentialSnippet`) — the real construction shape:
  `OpenAIChatClient<AgentEngineSecretStore> client{"api.openai.com", 443, "gpt-5",
  SecretRef{"OPENAI_API_KEY"}, caps, store};` and that resolution happens inside `chat()`.
* `providerContent.ts:434-451` (`credentialSteps`) — resolution at the point of use; step 4 names
  `EnvSecretSource` as "reads an environment variable"; step 3 shows it fails closed with
  `secret.not_granted` before the source is touched.
* `providerContent.ts:347-360` (`conformerRows`) — `AnthropicChatClient<Store>` "holds
  host/port/model/SecretRef and a Store const&".

Reverse-engineered / contradicted by the docs:
1. The only *snippet* that constructs an Anthropic client is
   `apiContent.ts:1770-1776`: `AnthropicChatClient<InMemorySecretStore> live{secret_store};` — one
   argument. That does not compile; the real constructor takes six with no defaults.
   `protocol/anthropic/chat_client.hpp:456-468`.
2. The doc cites `protocol/anthropic/chat_client.hpp:1071` as the constructor's file position;
   the file is 565 lines long and the constructor is at `:456`. A citation that cannot be followed.
3. `AgentEngineSecretStore` is named three times and never constructed. `EnvSecretSource` is named
   once, in prose, with no convention. Its env-var spelling is `QUARK_SECRET_<name>` by default
   (`trust/secret.hpp:137-170`) — a prefix left over from the removed Quark dependency, which is
   exactly the kind of fact a consumer needs and cannot get from the docs. I used
   `EnvSecretSource("")` so `SecretRef{"ANTHROPIC_API_KEY"}` maps to the environment variable of that
   name.
4. Nothing in the docs says a real backend requires `AGENTENGINE_WITH_HTTPS=ON`, or that this option
   is OFF by default, or that the option alone is not enough. Fell back to
   `CMakeLists.txt:674-822` and `examples/CMakeLists.txt:246-256`, then to compiling my target to find
   out that `agentengine::core` is insufficient (see 1(f) and the defect below).
5. Nothing says which capability a provider call needs to egress. Fell back to
   `tools/cli_chat.cpp:1712-1725`, whose grant list contains only `cap::Secret` plus fs caps —
   concluded no `cap::NetOut` is needed, and the dummy-key run confirmed a socket was made.

### (e) Handle errors — **found in docs**
`builderContent.ts:315-319` is the whole pattern and it is the one I shipped:
`built.error().message.c_str(), built.error().code.c_str()`. `apiContent.ts:1050-1066`
(`capabilityDenialErrorSnippet`) confirms codes are stable strings and are meant to be branched on.
Two gaps: the failure type is never named, and `apiContent.ts:1061`'s `agentengine::Error` is a
different type (a content-item variant member, `core/content.hpp:139`, with a message and no code) —
the failure type is lowercase `agentengine::error` (`core/error.hpp:78`) and `result<T>` is
`std::expected<T, error>` (`core/error.hpp:92`). The docs never state that a *failed run* (as opposed
to a failed build) surfaces through the same `error()`, which is what requirement 4 needs. I confirmed
it by running.

### (f) Build/link — **had to reverse-engineer** (partly closed mid-session by `first-app`)
When I started, the only build documentation was `content.ts:315-349` (`buildSteps`): clone the repo,
`cmake -S . -B build`, `cmake --build build --config Release -j 4`, `ctest`. That is how to build
**the engine**, not how to build **against** it. There is no consumer CMake page in
`apiContent.ts`'s `apiPages` as I read it, and no `find_package`, no exported target list, no install
story. What I fell back to:

* `CMakeLists.txt:92-103` — `agentengine::core` is the INTERFACE target carrying the include dir;
  `:115-262` shows it transitively links the compiled libraries a session needs.
* `CMakeLists.txt:1433-1444` — `AGENTENGINE_BUILD_EXAMPLES` / `AGENTENGINE_BUILD_TESTS`, both ON by
  default, must be set before `add_subdirectory`.
* `CMakeLists.txt:185-210, 674-822` — the HTTPS gate and the pinned mbedTLS/Asio/CA-bundle fetches,
  each with a `*_VENDOR=OFF` + local-path escape hatch (I used all three to configure offline).
* `examples/CMakeLists.txt:11-13, 246-256` — the link line for a live example:
  `agentengine::core` + `agentengine::provider_http_client` + the three `MbedTLS::*` targets, all
  inside `if(AGENTENGINE_WITH_HTTPS)`.
* `cmake/AgentEngineWarnings.cmake:14` — `agentengine_warnings` is `/W4 /WX` and
  `-Wall -Wextra -Werror`. I deliberately did **not** link it into the application.

### Defects in the new `first-app` page (verified, not inferred)

The page appeared mid-session and is a large improvement, but its real-model step
(`ApiFirstAppReference.tsx:96-104, 210-216`) will not work as printed:

1. **It will not compile/link.** The snippet says the only change is `-DAGENTENGINE_WITH_HTTPS=ON`,
   and the CMake snippet links exactly one target. Linking `agentengine::core` alone leaves
   `AGENTENGINE_WITH_HTTPS` undefined for your TU (`anthropic/chat_client.hpp:94` is `#ifdef`-gated),
   so the include is empty and you get `error C2653: 'anthropic' is not a class or namespace name`.
   The definition is propagated as `PUBLIC` by `agentengine_net_egress_proxy`
   (`CMakeLists.txt:804`), which only arrives via `agentengine::provider_http_client`. I reproduced
   this by building my `main.cpp` against `agentengine::core` alone. The correct line is
   `target_link_libraries(myapp PRIVATE agentengine::core agentengine::provider_http_client)`.
2. **Even then it fails at run time.** `make_agent_session<Greeter>(std::move(client))` passes no
   `AgentSessionOptions`, so no `cap::Secret` is granted and the first model call fails closed with
   `secret.not_granted` — I reproduced this too, by removing only that one line from my program. The
   page needs the `chat_client_grants` line; the concept exists in `agent_session_bridge.hpp:185-193`
   and nowhere in the docs site.
3. Minor: the page's model id is `"claude-sonnet-4-20250514"`; the repo's own live CLI uses
   `"claude-sonnet-5"` (`tools/cli_chat.cpp:304`). Docs and code disagree on which model string is
   current.

## 3. Fallback log

Each line is one departure from the docs. `[open]` = still a doc gap after the mid-session edit;
`[closed]` = the new `first-app` page now covers it.

1. `[closed]` Consumer CMake: target names, `add_subdirectory` idiom, the two options → `CMakeLists.txt`, `examples/CMakeLists.txt`.
2. `[closed]` Any `#include` line at all → `examples/01_hello_agent.cpp:27-33`, then `glob include/agentengine/**`.
3. `[open]` `make_agent_session`'s second parameter and how a session gets any authority → `agent_session_bridge.hpp:176-205, 415-439`.
4. `[open]` `AgentSessionOptions::chat_client_grants` (0 occurrences in the docs) and the "`cap::Secret` only" rule → `agent_session_bridge.hpp:185-193`.
5. `[open]` `AgentEngineSecretStore`'s constructor (type named 3× in docs, constructed 0×) → `trust/secret.hpp:296-307`.
6. `[open]` `EnvSecretSource`'s env-var convention `QUARK_SECRET_<name>`, and that an empty prefix gives a bare name → `trust/secret.hpp:137-170`.
7. `[open]` `AnthropicChatClient`'s real parameter list → `protocol/anthropic/chat_client.hpp:456-468` (docs' cited line `:1071` does not exist).
8. `[open]` That `AGENTENGINE_WITH_HTTPS` is ON/OFF, that it gates the class's existence, and that `agentengine::core` alone does not define it → `CMakeLists.txt:674-822`, `examples/CMakeLists.txt:246-256`, and a deliberate failed compile.
9. `[open]` That a provider call needs `agentengine::provider_http_client` + MbedTLS → `examples/CMakeLists.txt:249-251`.
10. `[open]` How to configure offline (vendored mbedTLS/Asio/CA bundle) → `CMakeLists.txt:185-210, 674-764`.
11. `[open]` The failure type: lowercase `agentengine::error`, `result<T> = std::expected<T, error>` → `core/error.hpp:78, 92`.
12. `[open]` That a *failed run* surfaces its message/code through `Bundle::ask()`'s `error()` → confirmed empirically, not from docs.
13. `[open]` How to read a `Described<T, "…">` field → `core/json_schema.hpp:99-122` (after a compile error).
14. `[open]` `pal::env_var` for optional model/host overrides → `pal/env.hpp:46`; the docs' examples use an undefined `env_or(...)`.
15. `[open]` `drive(...)` is not an engine symbol → docs say so (`ApiPizzaOrderReference.tsx:296-298`); its real body is `rt::block_on` + `while(!done()) resume()`, `rt/block_on.hpp`.
16. `[open]` `user_message(...)`'s actual home, `quickstart::detail::user_message` (a `detail` namespace helper used unqualified in 8 doc snippets) → `session_builder.hpp:386`.
17. `[open]` Which model id string to use → `tools/cli_chat.cpp:304` (docs' `first-app` page disagrees).
18. `[open]` `agentengine_warnings` is `/W4 /WX`; whether a consumer should inherit it → `cmake/AgentEngineWarnings.cmake:14`.

## 4. Undefined identifiers in the snippets I relied on

For each snippet: every identifier it uses without defining it, and where it actually comes from.
`agentengine::` is usually omitted in the snippets, which is itself part of the problem.

**`minimalToolSnippet` (`apiContent.ts:752-761`)** — undefined: `SearchArgs`, `SearchReply` (defined
in a *different* snippet, `toolSchemaSnippet`), `result<Reply>` and `EffectContext` (unqualified;
`agentengine::result` from `core/error.hpp:92`, `agentengine::EffectContext` from
`core/effect_context.hpp:37`), and `AE_JSON_SCHEMA`, a macro from `core/json_schema.hpp:499` that no
doc snippet ever shows being included.

**`toolSchemaSnippet` (`apiContent.ts:790-816`)** — self-contained for the types, but uses
`std::optional`/`std::vector` with no includes, and closes with a reference to
`json_schema_of<SearchReply>()` without saying what it returns (`std::string`,
`json_schema.hpp:286`).

**`minimalAgentSnippet` (`apiContent.ts:602-608`)** — undefined: `SearchTool` (named, never defined
anywhere in the docs), `Agent`, `ChatClientId`, `Tools`, `register_agent`. Prose says "same 8 checks
below", i.e. it depends on a table that lives on a different page.

**`helloAgentRunSnippet` (`apiContent.ts:613-622`)** — undefined: `JokerChatClient` (exists only in
`examples/01_hello_agent.cpp:93-129`), `TokenBudget`, `MaxTurns`, `Tools`, `RollDie` (defined in the
same snippet's own example file, not in the docs). `make_agent_session` and `ask()` are used here and
documented nowhere.

**`quickstartWorkedExampleSnippet` (`builderContent.ts:301-323`)** — undefined: `check(...)` (a
file-local helper; `durabilityContent.ts:149` is the only place that admits this), `OpenAiSessionBuilder`
(defined in `quickstartAliasesSnippet`, a different page section), `model`, `host`, `kHttpsPort`,
`kPathPrefix`, `kSecretName` (all four are example-local constants that appear in no doc file), and
`ChatClientCapabilities`.

**`credentialSnippet` (`providerContent.ts:759-784`)** — undefined: `AgentEngineSecretStore`,
`store`, `caps`, and the members `store_`, `api_key_ref_`. Its earlier half shows a construction that
works; its later half is a method body with no enclosing class.

**`chatClientSwapSnippet` (`apiContent.ts:1770-1776`)** — undefined: `secret_store`,
`InMemorySecretStore`, `recorded_run`, `Guarded`. As noted, the one-argument
`AnthropicChatClient<InMemorySecretStore>` construction does not compile.

**`agentSessionEventStreamSnippet` (`apiContent.ts:1795-1820`)** — undefined: `drive(...)`,
`user_message(...)`, `Principal`, `CapabilitySet`, `ScriptedChatClient`, `joined_deltas`,
`AgentSession` (unqualified).

**`capabilityDenialExampleSnippet` (`apiContent.ts:1052-1068`)** — undefined: `session`, `session2`,
`CapabilitySet::grant_root`, `Capability`, `cap::FsWrite`, `drive`, `user_message`.

**`composedProviderExampleSnippet` (`apiContent.ts:1497-1520`)** — undefined: `HistoryProvider`,
`Window<0>`, `SkillsProvider`, `NoSessionState`, `secret_store`.

Where the helpers really live, for the record:
`drive` — not an engine API; local to each example, using `rt::block_on` (`rt/block_on.hpp:3`).
`check` — local to each test/example `main()`.
`user_message` — `agentengine::quickstart::detail::user_message`, `core/session_builder.hpp:386`.
`StartRun` — `agentengine::rt::StartRun`, `rt/agent_session_core.hpp:106`.
`text_of` — `core/tool_call_extraction.hpp:83`.

## 5. Blockers, ranked

1. **The authority wiring is undocumented and non-obvious.** `AgentSessionOptions` /
   `chat_client_grants` appear zero times in the docs. Without that one field a real provider fails
   at run time with `secret.not_granted`, and nothing in the docs explains why — the failure looks
   like a broken key, not a missing grant. This cost the most time and is the one I would fix first.
2. **No consumer-side build story.** Building *the engine* is documented four times; building
   *against* it, zero. The link set (`agentengine::core` + `agentengine::provider_http_client` +
   MbedTLS), the `AGENTENGINE_WITH_HTTPS` gate, its default-OFF state, its propagation as a *compile
   definition* via a target `core` does not link, and the configure-time network fetches were all
   discovered by reading the engine's own build files and by deliberately breaking the build.
3. **No `#include` lines anywhere.** Every snippet is header-less. Mapping ~15 snippet identifiers to
   headers was pure search work.
4. **Real-provider construction is documented by description, not by example.** The Anthropic client
   constructor is described in prose with a wrong line citation, and the only construction snippet in
   the docs does not compile. A reader following the docs literally cannot construct this object.
5. **`Described<T, "…">` is documented only on the declaration side.** Reading it produces a
   template-deduction error that gives no hint the wrapper is the cause.
6. **The docs were changing underneath me.** A new page appeared mid-session covering items 2 and 3
   above and part of 1 — useful, but it means any line-number-level claim about `apiContent.ts` is
   revision-specific, and it leaves two verified defects in its own real-model step.

## 6. Confidence

The brief lists five product requirements; I read the sixth as the deliverables themselves.

| # | Requirement | Verdict |
| --- | --- | --- |
| 1 | One custom tool ("look up an order by id", small JSON reply), declared the engine's way | `certain` — patterned on `minimalToolSnippet`/`toolSchemaSnippet`; compiled, and the offline verifier confirms the generated schema, the `Described<…>` description, `invoke()`'s reply, and a real invocation through the tool pipeline. |
| 2 | An agent using that tool, bound to a real provider, key from an env var, never compiled in | `certain` for the binding and the credential mechanism — the dummy-key run resolved the key from the environment and completed a real HTTPS exchange with `api.anthropic.com`; `code` for a *successful* turn is unverified: no valid credential exists on this machine. |
| 3 | Run one turn, send a user message, print the answer | `certain` for the code path (`Bundle::ask()` is the documented one-shot); the *printed answer* is unverified live, and verified against a scripted client in the offline target. |
| 4 | On build or run failure, print the engine's message + code and exit non-zero | `certain` — executed three distinct engine failures (`secret.not_found`, `secret.not_granted`, `anthropic.http_401`), each printed verbatim with its code, each exit 1. |
| 5 | Buildable and runnable by me | `certain` — configures, builds and runs here (MSVC 19.51, VS 18 2026 generator, x64 Release). |
| 6 | Deliverables in `devsim/p1/`, no existing file modified | `certain` — all writes were inside `devsim/p1/`; `git status`'s 7 modified files under `web/marketing/` are someone else's concurrent edit, not mine. |

Note on requirement 2's "bound to a real model provider": `AGENTENGINE_WITH_HTTPS` must be ON and
`agentengine::provider_http_client` linked. I judged that acceptable for a product that needs a live
model; a product that must build with the default OFF cannot have a real backend at all, and the docs
never say so.

## 7. Verdict

**Not yet — but it is close, and only for the surface this product needs.** Everything about
*declaring* things is genuinely documentation-grade: a tool, its JSON Schema, an agent's policies,
the error-code vocabulary, and the error-handling idiom all come straight from the docs and compiled
without argument. Everything about *running* things against a real model fails. The docs will let you
declare a tool and an agent; they will not let you include the engine, link it, construct a real
backend, or authorize that backend's credential — and of those four, the credential one is invisible
rather than merely missing: you get a plausible-looking `secret.not_granted` at run time with nothing
in the docs to explain it. The docs also never show an `#include` line, which for a C++ library is
the first thing a reader needs. If the new `first-app` page that appeared while I was working is the
direction of travel, then (a) include, (b) session, (e) errors and (f) build/link are now genuinely
covered, and the remaining holes are narrow and specific: the authority-grant field, the real
provider's link set, `Described` reads, and a two-line correction to `first-app`'s own real-model
step. With those fixed I would say yes — a competent C++ developer could build exactly this product
from the docs alone, in an afternoon.
