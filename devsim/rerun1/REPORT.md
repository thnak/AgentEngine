# Building a product on AgentEngine from the docs alone — re-run 1

**Product built:** `order_assistant`, a one-shot CLI assistant for a small business with one custom
tool (`lookup_order`) bound to the real Anthropic backend, credential read from `ANTHROPIC_API_KEY`.

**Deliverables in this directory:** [`main.cpp`](main.cpp), [`CMakeLists.txt`](CMakeLists.txt), this file.

**Environment:** Windows 11 x86-64, MSVC 19.51.36260 (Visual Studio 18 2026 Community), CMake 4.3.2,
Ninja (from the VS install), C++23 via the engine's own `-std:c++latest`. Engine vendored at
`C:\Users\thanh\Git\AgentEngine`. Build directories were kept off the workspace
(`C:\ae-rr1`, `C:\ae-rr2`) per the on-ramp page's own advice about `CMAKE_OBJECT_PATH_MAX`
(`web/marketing/src/components/ApiFirstAppReference.tsx:186-195`).

**Primary doc sources used:** `web/marketing/src/components/ApiFirstAppReference.tsx` (the
`api/first-app.html` on-ramp), `web/marketing/src/components/ApiToolReference.tsx`,
`web/marketing/src/data/apiContent.ts` (the tool/agent snippets and tables it renders),
`web/marketing/src/data/providerContent.ts`, `web/marketing/src/data/builderContent.ts`,
`web/marketing/src/components/ApiSnippetConventions.tsx`, `ApiAgentReference.tsx`,
`ApiHowItWorksReference.tsx`.

---

## 0. What actually ran (verified, verbatim output)

Build (the two commands the on-ramp page prescribes, plus the HTTPS flag; `C:\ae-rr1` was the
iteration directory, `C:\ae-rr2` the clean run timed below):

```
cmake -S C:\Users\thanh\Git\AgentEngine\devsim\rerun1 -B C:\ae-rr2 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build C:\ae-rr2 --target order_assistant -j 4
```
→ configure 7.4 s ("Configuring done (7.4s)"), 161 ninja build edges, clean link;
**whole fresh configure + build = 37.8 s** (measured). Measured incremental rebuild of `main.cpp` +
link in the iteration directory: **12.8 s**.

Tool schema the run would hand the model (`order_assistant.exe --schema`) — this is the engine's
own emission from `AE_JSON_SCHEMA` over my `Args`/`Reply` types:

```
lookup_order args:  {"type":"object","properties":{"order_id":{"type":"string","description":"The order id to look up, e.g. \"A-1042\""}},"required":["order_id"]}
lookup_order reply: {"type":"object","properties":{"order_id":{"type":"string"},"status":{"type":"string"},"total_cents":{"type":"integer"},"eta":{"type":"string"}},"required":["order_id","status","total_cents","eta"]}
```

That matches every claim the Tool page makes about the emission: `Described<T,"...">` produces the
field `"description"`, `std::string`→`"string"`, `int`→`"integer"`, non-optional fields are
`"required"`, and the emission is compact single-line
(`ApiToolReference.tsx:48-66`, `apiContent.ts:769-788,818-837,898-912`).

A run with no credential in the environment (the only run I could complete — see §6):

```
$ ./order_assistant.exe "Where is order A-2001?"
> Where is order A-2001?
run failed: secret not in environment: ANTHROPIC_API_KEY (error code: secret.not_found)   [exit 1]
```

Positive control for requirement 1 (that `Tools<...>` is really read, not ignored): I temporarily
added a second tool with the same `name` to `Tools<...>` and rebuilt:

```
bind failed: two declared tools share the name 'lookup_order' (error code: agent.tool_name_collision)   [exit 1]
```

— the exact code and meaning the on-ramp page's error table claims
(`ApiFirstAppReference.tsx:280`, `apiContent.ts:536`). The control was reverted; the shipped
`main.cpp` is the one that builds and prints the schema above.

---

## 1. Getting started — found in docs vs. reverse-engineered

### (a) Include the engine — **found in docs**

| What | Where |
|---|---|
| No `find_package`, no installed release; vendor the repo and `add_subdirectory` it | `ApiFirstAppReference.tsx:80-96` (in `api/first-app.html#depend`) |
| `set(AGENTENGINE_BUILD_TESTS OFF CACHE BOOL "" FORCE)` / same for `..._BUILD_EXAMPLES`, before `add_subdirectory` | `ApiFirstAppReference.tsx:83-87` |
| Why it matters (2,336 → 159 build statements, 49 s → 4 s configure) | `ApiFirstAppReference.tsx:174-185` |
| Link one target, `agentengine::core` | `ApiFirstAppReference.tsx:99-101` |
| Reaching a real model needs a second target `agentengine::provider_http_client` **and** `-DAGENTENGINE_WITH_HTTPS=ON` (defaults OFF), or you get the misleading `C2653: 'anthropic' is not a class or namespace name` | `ApiFirstAppReference.tsx:103-108`, `:245-255`; also `providerContent.ts:568` |
| The six `#include`s a consumer program needs for an offline program + a real backend | `ApiFirstAppReference.tsx:14-17` and `:112-113` |
| Keep the build directory shallow (Windows 250-char object path) | `ApiFirstAppReference.tsx:186-195` |

The `FORCE`-cache-variable idiom worked exactly as advertised: configure took 9.2 s, no test
targets, no example targets, tests genuinely absent (161 edges, not thousands).

**Not in docs:** which header declares `Tool<...>`, `Capabilities<...>`, `EffectClass<...>`,
`effect_class`, `AE_JSON_SCHEMA`, `error`, `failure_class`. The Tool page — the page whose job is
the tool's full declaration surface — contains **no `#include` line at all**; its only path
reference is a GitHub citation to `include/agentengine/core/json_schema.hpp:5-19`
(`ApiToolReference.tsx:369-370`). There is no umbrella header anywhere in the API section (grepped
`#include <agentengine...>` / `agentengine/agentengine` across `web/marketing/src`: no matches).
→ **fallbacks 1 and 2.**

Also **not in docs:** the on-ramp CMake snippet hardcodes
`add_subdirectory(third_party/AgentEngine agentengine)`. It gives no variable and no statement
about where to point it when the engine is vendored somewhere else — the ordinary case for a
consumer whose checkout is not a child of the app. → **fallback 6.**

### (b) Construct a session — **found in docs**

| What | Where |
|---|---|
| `make_agent_session<A>(client, opts)` runs `register_agent<A>()`'s validation and binds the compiled policy set to a real session | `ApiFirstAppReference.tsx:60-62`, `:69`; `apiContent.ts:611-622`; `ApiAgentReference.tsx:127-137` |
| `AgentSessionOptions` fields: `session_id`, `principal`, `chat_client_grants` | `ApiFirstAppReference.tsx:132-135` |
| One call runs a turn and returns `result<std::string>`: `agent->ask("...")` | `ApiFirstAppReference.tsx:69-77` |
| The alternative raw surface `rt::AgentSession<ChatClientT>::start_run(StartRun{...})` | `apiContent.ts:624-639` |

The header is never named for `AgentSessionOptions` or `make_agent_session`, but the on-ramp
program already includes `agentengine/core/agent_session_bridge.hpp`
(`ApiFirstAppReference.tsx:15`), and that is in fact where both live
(`include/agentengine/core/agent_session_bridge.hpp:176` and `:430`). So the docs carried me here
by implication; I confirmed the linkage by grepping the header. `Bundle::ask()` is documented on the
builder page (`builderContent.ts:325-332`) and used on the on-ramp page without its type ever being
named.

### (c) Declare a tool — **mostly found in docs, two hard gaps**

| What | Where |
|---|---|
| `Tool<Derived, Policies...>`; five required members `name`, `description`, `Args`, `Reply`, `invoke`; the three optional accessors default sensibly | `ApiToolReference.tsx:37-47`, `:350-360`; `apiContent.ts:649-699` |
| Full minimal shape | `apiContent.ts:752-761` |
| `Args`/`Reply` are structs paired with `AE_JSON_SCHEMA(Type, field1, ...)` | `apiContent.ts:790-816`, `:919-938` |
| Per-field `"description"` via `Described<T, "...">`, never a macro argument; a C++ enum flattens to `"integer"` | `apiContent.ts:898-912`; `ApiToolReference.tsx:54-66`, `:377-388` |
| The schema is exactly what the model sees; Anthropic `{name, description, input_schema}`, OpenAI `{type:"function", ...}` | `ApiToolReference.tsx:81-98` |
| Attaching tools to an agent: the `Tools<Ts...>` policy on `Agent<Derived, Policies...>`; most agents write only `ChatClientId` + `Tools` | `ApiAgentReference.tsx:80-96`; `apiContent.ts:602-608`, `:613-622` |
| Duplicate names across `Tools<...>` are rejected with `agent.tool_name_collision` | `ApiFirstAppReference.tsx:280`; `apiContent.ts:536` |
| Capability ceiling / effect class as optional policy tags | `apiContent.ts:863-889`, `:914-938`; `ApiToolReference.tsx:133-150` |

Verified end-to-end: the schema dump in §0, and the `agent.tool_name_collision` positive control.

**Gap 1 (documentation failure):** no page shows how `invoke()` reads a field that was declared
`Described<T, "...">`. The natural C++ fails:

```
main.cpp(60): error C2678: binary '==': no operator found which takes a left-hand operand of type
  'agentengine::Described<std::string, agentengine::fixed_string<39>{...}>'
main.cpp(68): error C2677: binary '+': no global operator found which takes type
  'agentengine::Described<...>'
```

→ **fallback 3** (read the header; the public `value` member or the conversion operators are the
answer).

**Gap 2 (documentation failure):** the Tool page writes `json_schema_of<SearchReply>()`
(`ApiToolReference.tsx:323`, `apiContent.ts:836`) as if it were reachable; it is
`agentengine::schema::json_schema_of<T>()` (`include/agentengine/core/json_schema.hpp:286`), and I
had to switch to `Tool<Derived>::args_schema()/reply_schema()`. → **fallback 5.**

### (d) Wire a real provider with a credential — **found in docs, with one hard gap**

| What | Where |
|---|---|
| Store resolving from the environment; `EnvSecretSource`'s prefix defaults to the historical `QUARK_SECRET_`, so `""` gives a bare `ANTHROPIC_API_KEY` | `ApiFirstAppReference.tsx:115-117` |
| `Client<Store> client{host, port, model, SecretRef{"ANTHROPIC_API_KEY"}, caps, *store}` — templated on the **store type** | `ApiFirstAppReference.tsx:119-125`; `providerContent.ts:759-764` |
| `AgentSessionOptions::chat_client_grants = {Capability{cap::Secret{...}}}`; omitting it **compiles** and fails the first run with `secret.not_granted`, which reads like a broken key | `ApiFirstAppReference.tsx:127-136` |
| The credential is resolved at the point of use inside `chat()`, never at construction; fails closed, null capability pointer is a denial | `providerContent.ts:434-441`, `:775-784` |
| `caps` as deployment-declared capability set | `providerContent.ts:642-646` |

Verified: my program resolves the credential from the environment only — no key appears in
`main.cpp`, and the observed failure with none set is the engine's own
`secret not in environment: ANTHROPIC_API_KEY (secret.not_found)`.

**Gap 3 (documentation failure):** the class name in every doc snippet is unqualified
(`AnthropicChatClient<AgentEngineSecretStore>`) and the on-ramp page's own snippet only has
`using namespace agentengine;`. The class is `agentengine::anthropic::AnthropicChatClient`
(`include/agentengine/protocol/anthropic/chat_client.hpp:118,444`). MSVC reports it as
`error C7568: argument list missing after assumed function template 'AnthropicChatClient'`, which
does not hint at a namespace problem. → **fallback 4.** The same pattern applies to
`agentengine::openai::OpenAIChatClient` (`protocol/openai/chat_client.hpp:74`), which
`providerContent.ts:761` also writes unqualified.

**Gap 4 (documentation gap):** the on-ramp §4 snippet sets only `caps.streaming` and
`caps.max_output_tokens`. `ChatClientCapabilities::tool_calling` defaults to **false**
(`include/agentengine/core/chat_client.hpp:37`), and nothing in the docs says what it gates. For an
agent that declares `Tools<...>` this is exactly the flag you would want set; the builder page's
example does set it (`builderContent.ts:305`). → **fallback 7.**

### (e) Handle errors — **found in docs**

| What | Where |
|---|---|
| `result<T>` is literally `std::expected<T, agentengine::error>`; check `has_value()`, use `error()` | `ApiFirstAppReference.tsx:260-275` |
| Three fields: `code` (branch on this), `message` (host-facing prose), `klass` (`failure_class`: transient / policy / contract / resource / fatal / canceled); only transient and resource are retryable | `ApiFirstAppReference.tsx:266-275` |
| Five real, stable codes including `agent.chat_client_id_missing`, `agent.tool_name_collision`, `tool.capability_not_held` | `ApiFirstAppReference.tsx:277-284` |
| The worked example's own failure printing (`fprintf(stderr, "...%s (%s)", message, code); return 1;`) | `ApiFirstAppReference.tsx:63-74`; `builderContent.ts:315-320` |
| Constructing an `error` is `error{failure_class, message, code}` | `providerContent.ts:778-780` |

My program follows the doc's shape exactly, in one place (`fail()`), for both the bind failure and
the run failure, and exits 1. Verified against two real engine errors (§0).

**Not in docs:** the header for `error` / `failure_class` / `result` (→ fallback 2); and the five
`failure_class` enumerators are named but the docs never show which header defines them.

### (f) Build/link — **found in docs**

Covered under (a). Two additional honest notes:

- The page's step-count claim ("30 build steps in total for the program above",
  `ApiFirstAppReference.tsx:222-227`) does **not** hold for a real-model consumer: with
  `AGENTENGINE_WITH_HTTPS=ON` and `provider_http_client` linked, my configure generates **161**
  build edges (nine engine static libraries plus the vendored TLS objects). The page's own §4 says
  two extra requirements come with a real backend but does not revisit the count. I did not
  independently reproduce the 2,336 / 159 comparison (that needs a second full configure without
  the two cache variables).
- `-G Ninja` requires Ninja **and** `cl` to be on `PATH`. On a stock Windows 11 box neither is,
  until a VS developer environment is entered (`vcvars64.bat`). Nothing in the docs mentions this
  — see blocker 8.

---

## 2. Fallback log

**Total fallbacks: 7.** A fallback = a point where the documentation did not answer the question and
I read engine source (`include/`, `src/`) or the header tree to proceed. Five of the seven were
required to make the program compile or to know which header to include; one is CMake wiring the
docs do not address at all; one is a semantics question I could not answer from the docs and
resolved by reading the header.

1. **Tool-plane headers not supplied.** The Tool page names no `#include`, so the header for
   `Tool<Derived, Policies...>`, `Capabilities<...>`, `EffectClass<...>`, `effect_class` and
   `AE_JSON_SCHEMA` was unknown. *Fell back to:* listing `include/agentengine/core/` and picking
   `tool.hpp` (+ `json_schema.hpp`, which the page cites by path in a GitHub link).
   *Cost:* header lookup only — my first guess was right.
2. **Error-model header not supplied.** `result<T>`, `error`, `failure_class` are described in prose
   on the on-ramp page and never tied to a header. *Fell back to:* the same directory listing,
   `core/error.hpp`. *Cost:* header lookup only.
3. **Reading a `Described<T,"...">` field inside `invoke()`.** No page shows it, and the obvious
   `args.order_id == "A-1042"` / `"..." + args.order_id` does not compile (C2678/C2677 — the
   wrapper's conversion operators are not considered for those operators). *Fell back to:*
   `include/agentengine/core/json_schema.hpp:100-117` → public `value` member. **Cost: one full
   build+link cycle (≈13 s after the first build) and the usual MSVC wall of candidate notes.**
4. **Namespace of the provider client class.** Docs write `AnthropicChatClient<Store>` unqualified;
   the real name is `agentengine::anthropic::AnthropicChatClient`. *Fell back to:*
   `protocol/anthropic/chat_client.hpp:118,444`. **Cost: one full build+link cycle**, and the error
   (`C7568: argument list missing after assumed function template`) actively misdirects — it reads
   like a template-arity problem, not a missing qualifier.
5. **`json_schema_of<T>()` is not reachable as written.** The Tool page's flow diagram and prose use
   the bare name; it lives in `agentengine::schema`. *Fell back to:* grepping the tree, then
   switching to `Tool<Derived>::args_schema()/reply_schema()` (which the flow diagram does name,
   though the member table never lists them). **Cost: one full build+link cycle.**
6. **CMake for an engine vendored anywhere other than `third_party/AgentEngine`.** The on-ramp
   snippet hardcodes the path with no variable and no guidance. *Fell back to:* writing my own
   `AGENTENGINE_ROOT` default (`get_filename_component("${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)`).
   *Cost:* ordinary CMake knowledge, but the snippet is not usable as printed by a consumer whose
   checkout is elsewhere.
7. **`ChatClientCapabilities::tool_calling` — what it gates, and what its default is.** No doc page
   states either. My agent has a tool and the on-ramp §4 snippet does not set this flag, so I needed
   to know whether copying that snippet would silently disable tool dispatch. *Fell back to:*
   `core/chat_client.hpp:35-55` (defaults `false`) and a grep for its uses. Result: the only uses I
   found are the output-schema strategy (`chat_client.hpp:367`) and the response-format leak scan
   (`rt/agent_session.hpp:437,462`) — neither is the tool-dispatch path, so the omission looks
   survivable on the Anthropic backend, but the declared capability is then wrong and I could not
   determine the full consequence from the docs. My program sets it `true`.

Not counted as fallbacks (they were friction but not documentation failures, and none required
reading engine source or tests): Ninja/`cl` not being on `PATH`; the engine's own configure time
(7.4-9.2 s across my two build directories); `Copy-Item` restoring an older mtime so Ninja skipped a
rebuild (a tooling mistake of mine).

---

## 3. Did the first-app page help?

**Yes — it is the single most valuable page in the set, and three of its claims are load-bearing and
were verified true:**

1. The build recipe. The `FORCE` cache variables before `add_subdirectory`, the single
   `agentengine::core` target, and — critically — the `AGENTENGINE_WITH_HTTPS=ON` +
   `agentengine::provider_http_client` pair *with the warning that the failure mode is a compile
   error naming `anthropic` as an unknown namespace*. Without that note I would have read a C2653 as
   a code problem, not a link-flag problem. I did not have to discover this the hard way, which is
   exactly the kind of thing a page like this is for.
2. The real-model wiring, especially "the grant is the one that surprises people… omitting it
   compiles cleanly and then fails the *first run*". Accurate in shape: my first run failed on the
   credential path, not on anything structural, and the engine's error was legible.
3. The error model. `result<T>` = `std::expected`, `code` to branch on, `message` host-facing,
   `klass` for policy. The five codes are real; `agent.tool_name_collision` fired with exactly the
   documented string in my positive control.

**Questions the page left unanswered, all of which I hit:**

- **How to add a tool.** The page's agent is `Agent<Greeter, ChatClientId<...>, MaxTurns<4>>` — no
  `Tools<...>` — and its §4 "only change" keeps that tool-less agent. There is no example anywhere
  of *a tool plus a real provider*, which is the combination my product needs. I had to combine the
  Agent page (`Tools<Ts...>`) with the Tool page (the declaration surface) myself. With two hard
  compile failures waiting in that combination (fallbacks 3 and 4), this is the page's most
  consequential omission.
- **Which header declares anything but the six includes it lists.** `AgentSessionOptions`,
  `make_agent_session`, `Tool`, `AE_JSON_SCHEMA`, `error` — none is tied to a header, and for the
  tool plane the header is nowhere in the API section.
- **`ChatClientCapabilities` for a tool-using agent.** §4 sets two fields; `tool_calling` is not one
  of them and is not explained.
- **How to point `add_subdirectory` at a vendored engine that is not at `third_party/AgentEngine`.**
- **`Principal`'s second field.** `Principal{"p-demo", ""}` — the empty string is unexplained on
  this page; I inferred "tenant" from the pizza-order walkthrough
  (`ApiPizzaOrderReference.tsx:103`: `Principal{"customer-4711", "tenant-pizzapalace"}`).
- **The 30-build-step claim**, which does not survive adding a real backend (161 edges for me).

The new shared "How to read the code on this page" note
(`ApiSnippetConventions.tsx`, rendered on every API detail page via `ApiDetailLayout.tsx:40`,
including `first-app` — `FirstAppDetailApp.tsx:7` passes `active="first-app"`) is genuinely useful
and it did answer one question in advance — `drive(task)` is example-local and `rt::block_on` is the
engine equivalent — and its `foot` link back to "Your first program" is the right instinct. What it
does *not* cover is the class of problem I actually hit: it explains example-local *helpers*
(`drive`, `check`, `user_message`, page-local aliases) and the values a snippet assumes you built
(`caps`, `store`, `held`), but not unqualified *engine names* (`AnthropicChatClient`,
`json_schema_of`) or wrapper types whose read-side is undocumented (`Described`). A second row in
that panel — "engine names in snippets are unqualified; the real ones are
`agentengine::anthropic::AnthropicChatClient`, `agentengine::openai::OpenAIChatClient`,
`agentengine::schema::json_schema_of<T>()`" — would have saved two of my three failed builds.

---

## 4. Undefined identifiers

Names used by doc snippets without a definition on the page that uses them, and where they come from.
("Same page" means the identifier is defined elsewhere in the same doc file.)

**From the on-ramp page's offline program (`ApiFirstAppReference.tsx:11-78`):**

| Identifier | Where it comes from |
|---|---|
| `task`, `result`, `error` | Never defined on any doc page as a *type*; described in prose as `std::expected<T, agentengine::error>` (`:262-265`). Header `core/error.hpp` — **not in docs (fallback 2)** |
| `ChatResponse`, `ChatRequest`, `Message`, `role::assistant`, `ContentItem`, `content_origin::assistant`, `Text`, `ChatResponseUpdate`, `Usage` | Never defined; `Usage{1, 1, 0, 0, 0.0}` has five unnamed fields. Not needed for my program; **unresolved**. `ChatRequest` is shown field-by-field on `providerContent.ts:630-646` |
| `EffectContext` | Prose only; header cited on the orientation page (`ApiHowItWorksReference.tsx:183`: `core/effect_context.hpp`) |
| `make_stream<ChatResponseUpdate>` | Named as real engine API by the shared conventions note (`ApiSnippetConventions.tsx:28-30`: `core/stream.hpp`) |
| `ChatClient` (the concept) | Referenced as a concept and `static_assert`ed in the same snippet, but its required members are never listed. `providerContent.ts:347-385` gives the conformer shapes |
| `ChatClientCapabilities` | Used as `caps`; constructed on `providerContent.ts:643-646` and `builderContent.ts:302-305` |
| `Agent`, `ChatClientId`, `MaxTurns` | Defined by the Agent page (`ApiAgentReference.tsx:80-96`; `apiContent.ts:602-608`) |
| `make_agent_session`, `ask` | Only a comment on this page (`:60-62`). `make_agent_session` is named on the Agent page (`ApiAgentReference.tsx:129-131`); `Bundle::ask()` is documented on the builder page (`builderContent.ts:325-332`) |
| `AgentSessionOptions`, `Principal`, `Capability`, `cap::Secret` | Never defined. Only implicit header is `agent_session_bridge.hpp` (`:15`) — true for the first two (`include/agentengine/core/agent_session_bridge.hpp:176,430`); `cap::Secret`/`Capability` are cited to `include/agentengine/trust/capability.hpp` on the orientation page (`ApiHowItWorksReference.tsx:668-669`) and `apiContent.ts:949` |
| `SecretRef`, `AgentEngineSecretStore`, `EnvSecretSource` | Header given by the snippet itself (`:113` → `trust/secret.hpp`); semantics on `providerContent.ts:434-441` |
| `AnthropicChatClient` | Header given (`:112`); **namespace wrong in the docs** (fallback 4) |

**From the tool page (`ApiToolReference.tsx` + the snippets in `apiContent.ts`):**

| Identifier | Where it comes from |
|---|---|
| `Tool<Derived, Policies...>`, `Capabilities<...>`, `EffectClass<...>`, `effect_class::pure`, `AE_JSON_SCHEMA` | Never tied to a header anywhere (fallback 1). `effect_class::pure` also has no documented enumerator list |
| `SearchArgs`, `SearchReply`, `Hit` | Defined in `apiContent.ts:790-807`, but the *minimal* tool snippet (`:752-761`) references them only in a comment |
| `Described<T, "...">` | Header cited (`json_schema.hpp:5-19`); the **read** side is undefined anywhere (fallback 3) |
| `json_schema_of<T>()` | Used bare in prose and the flow diagram; namespaced `agentengine::schema` (fallback 5) |
| `ToolTable`, `ToolTable::from_tools<Ts...>()` | Used in `apiContent.ts:891-893`; `ToolTable` is never defined, and no page says whether it is needed when you use the `Tools<...>` policy instead. `ToolDescriptor` (the *entry* type) is defined at `apiContent.ts:839-855`. I did not need `ToolTable` at all |
| `cap::decl::FsWrite<"work">` | `cap::decl` namespace undefined on this page; declaration tags are listed on `apiContent.ts:948` |
| `EffectContext&`, `result<Reply>` | Signatures shown, headers unnamed (fallbacks 1 and 2) |

**Across the doc set:** no API page gives a header for `error`/`failure_class`/`result`, none gives
an umbrella include, and none names a header for `Tool<...>`. `AgentEngineSecretStore`,
`EnvSecretSource` and `cap::Secret` are the only credential-path names whose header *is* given.

---

## 5. Blockers ranked, worst first

1. **The on-ramp page's real-model snippet does not compile as printed** (fallback 4). §4 is the
   page's answer to "point it at a real model" and it is the one part of the page that is not part
   of the verified complete program. It is missing the `agentengine::anthropic::` qualifier, and the
   diagnostic (`C7568`) points away from the cause. For my product this is *the* page, so this is
   the worst defect I hit.
2. **No example anywhere of a tool plus a real provider.** The on-ramp is tool-less; the Tool page's
   live fixture is a test file (`tests/rt/agent_session/test_rt_agent_session_live_multitool_e2e.cpp`)
   that sets up its own client and never shows a consumer's `main()`. A consumer building exactly the
   thing the task asked for has to synthesise the combination, and two of the three compile failures
   are waiting precisely in that synthesis.
3. **`Described<T>`'s read side is undocumented and the obvious code does not compile**
   (fallback 3). The docs push you to `Described` for field descriptions — correctly, it is the only
   supported way — and then never show the inside of an `invoke()` that reads one.
4. **The tool plane names no header at all** (fallback 1). The page that documents the tool's "full
   shape, field by field" does not tell you what to `#include`. Combined with the absence of an
   umbrella header anywhere, every consumer assembles the include set by trial.
5. **The Builder API, which the Agent page recommends for app code, cannot express my product.**
   `ApiAgentReference.tsx:53-61` sends app authors to `QuickstartSessionBuilder`, and
   `builderContent.ts:47-94` is where `.api_key(...)`, `.store(...)` and `.build() -> result<Bundle>`
   live — the friendliest credential path in the docs. But nothing in its documented method table
   attaches a `Tools<...>` set, so an app with a custom tool must leave that surface and hand-wire
   the CRTP path. The docs never say this, and the two pages read as if the choice were free.
6. **`ChatClientCapabilities::tool_calling` is undocumented** (fallback 7), and the on-ramp's
   real-model snippet omits it. I could not determine the full consequence from the docs.
7. **CMake wiring for an engine vendored outside `third_party/AgentEngine`** (fallback 6).
8. **Toolchain prerequisites are not stated.** `-G Ninja` needs Ninja and `cl` on `PATH`; on a
   stock Windows 11 install neither is, and `cmake --version` succeeding tells you nothing. This
   cost me a discovery cycle before the first configure, and it is the first thing a Windows
   consumer meets.
9. **No credential in this environment**, so the successful live turn could not be verified (§6).
   Not a doc defect, but it is the one acceptance criterion I cannot sign off.

Minor, not blocking: the 30-build-step claim does not hold once a real backend is linked; the
on-ramp page's step-count and configure-time figures (2,336 → 159 statements, 49 s → 4 s) are
plausible but I did not reproduce them (my fresh configure was 7.4 s, my target 161 edges);
`Principal`'s second field is never explained on the page that introduces it.

---

## 6. Confidence per requirement

| Requirement | Confidence | Basis |
|---|---|---|
| 1. One custom tool declared the engine's way | **Certain** | Compiled; `--schema` prints the engine's own emission for `Args`/`Reply` and it matches every documented rule; the `agent.tool_name_collision` control proves `Tools<...>` is really read |
| 2. Agent bound to a real provider, credential from an env var, never compiled in | **Certain for construction, guessed for a live call** | `make_agent_session` succeeded; the credential path is exercised (it failed *at* `ANTHROPIC_API_KEY` resolution, i.e. after the bind). That a real HTTP turn then succeeds is unverified — no key available |
| 3. Run one turn and print the answer | **Could not determine** | No `ANTHROPIC_API_KEY` (or any of the other provider names) is set in this environment, and I did not have one to set. The failure path is verified; the success path is not |
| 4. On failure print the engine's message + code, exit non-zero | **Certain** | Two distinct real engine errors observed, both printed verbatim with their codes, both exiting 1 (`secret.not_found` on a run, `agent.tool_name_collision` on a bind) |
| 5. Buildable and runnable by me | **Certain** | Fresh-directory configure + build + link clean on MSVC 19.51 / CMake 4.3.2 / Ninja; binary runs and exits 0 on `--schema`, 1 on the credential failure |

What I could **not** verify: a successful live model turn; that the model actually calls
`lookup_order` and that the reply round-trips; anything about streaming; and the two configure-time
figures quoted on the on-ramp page.

---

## 7. Verdict

**Could a competent C++ developer build *this product* from the docs alone? No — not without
reading engine headers.** They could build the *offline* program on the first-app page verbatim; that
page is genuinely good. But the moment the product needs a custom tool on a real backend, the docs
run out: three hard compile failures (the provider class's namespace, the `Described` read side, and
`json_schema_of`'s namespace), two headers that no page supplies (`tool.hpp`, `error.hpp`), one
capability flag whose semantics are undocumented, and CMake wiring that assumes a fixed vendor path.
Every one of those is recoverable in minutes *by reading the headers* — which is why the honest
answer is "not from the docs alone", not "not at all".

**How long would it take?** Measured on this machine: a fresh directory from nothing —
configure **7.4 s**, full build of the consumer target (nine engine static libraries plus the
vendored TLS objects, `AGENTENGINE_WITH_HTTPS=ON`, 161 edges) **37.8 s** total; every later edit is
a **12.8 s** rebuild of one translation unit plus link. So the machine cost is small; the cost is all
in the reading. A competent C++ developer who reads every API page and is willing to open headers
when a snippet fails lands this in **roughly two hours**, of which perhaps 30 minutes is the offline
program and the rest is closing the gaps above. A developer who insists on staying inside the
documentation — the fair reading of "from the docs alone" — **cannot finish it**: fallback 4 stops
the build outright, and the docs give no path around it.

**The two changes that would move the needle most, in order:** (1) put a header and a namespace on
every engine name the Tool page and the real-model snippet use, ideally as a copy-pasteable
`#include` block plus a two-line "the real names are `agentengine::anthropic::AnthropicChatClient`
and `agentengine::schema::json_schema_of<T>()`" note in the shared conventions panel; (2) publish one
complete program that has both a `Tools<...>` policy and a real backend — the on-ramp page's own §4
snippet, made to compile, would do it.
