# Quickstart session builder — a convenience facade over `AgentSession`'s wiring — design draft

**Status: prototyped (§2a/§2c/§3/§4 only), red-teamed once against the real code, all three findings
closed** (two fixed and re-verified by the pass itself; the third — generalizing `.api_key_from_env()`
past its original `InMemorySecretStore`-only shape — fixed in a same-session follow-up, real and
tested against the project's actual production `AgentEngineSecretStore`, but not yet independently
red-teamed the way the first two were). Matches this project's `design → red-team → prove → judge`
discipline (CLAUDE.md), same honesty level as `docs/planning/tool-optimizer-provider-design-draft.md`
and `docs/planning/model-call-gateway-routing-design-draft.md`. Real, compiling, passing code:
`include/agentengine/core/session_builder.hpp`, `tests/test_session_builder_prototype.cpp` (18/18
checks, Windows/MSVC, `AGENTENGINE_WITH_HTTPS=ON`). Not implemented: §2b (history/context composition),
§2d (approval/policy sugar), `.with_fallback()`/`.with_middleware()`/`.with_content_replay()`, and this
draft's own `.raw_client_only()` escape hatch — named in the header's own top comment, not silently
dropped.

## §0b. Red-team pass against the real code — two findings fixed, one still open

A fresh, adversarial pass (independent of the implementer, per this project's own red-team practice)
against `session_builder.hpp`/the test/the design draft's own §5 self-red-team found three real issues.
Full detail lives in the header's own top comment (kept next to the code it describes); summarized here
for the record:

1. **FIXED — real UB, not hypothetical.** `Bundle::ask()` originally reused `examples/*.cpp`'s naive
   `while(!done()) resume()` idiom against a real `AgentSession`. That idiom is safe only when
   `session_mutex_` (`rt::AsyncMutex`) is never contended — guaranteed by construction in a linear,
   single-threaded example `main()`, but NOT guaranteed for `Bundle`, a reusable, potentially-shared
   object with no built-in single-caller discipline. Under real contention, `AsyncMutex` parks the
   coroutine for a later, possibly different thread's `unlock()` to resume directly — a second, blind
   `resume()` on that same handle is a genuine cross-thread double-resume race, undefined behavior per
   the C++20 coroutine spec. This is the EXACT bug class `rt/thread_pool.hpp`/`rt/drive_leaf_task.hpp`
   already found and fixed elsewhere in this codebase (their own file-top comments name it explicitly)
   — the red-team's contribution was recognizing `Bundle::ask()` reintroduces it at a new call site the
   existing fixes don't cover. **Fixed** with two layers: a heap-owned `ask_mutex_` serializes every
   `.ask()` call one `Bundle` ever makes, and the drive itself is now bounded to one `resume()` with a
   fail-closed error if that alone doesn't finish (matching `drive_leaf_task.hpp`'s own shape) as
   defense in depth. **Not covered**: a caller reaching the raw session via `Bundle::session()` and
   calling `start_run()` directly, concurrently with `.ask()` — bypasses `ask_mutex_`, inherits
   `AgentSession`'s ordinary I1 obligation like any other direct use. Not proven by a live concurrency
   test (named gap in the test file's own top comment) — verified by code review against the cited
   precedent only.
2. **FIXED — real hygiene bug.** `.api_key_from_env()` pushed a new `cap::Secret` grant on every call,
   unconditionally — calling it twice (e.g. correcting a typo'd env-var name) left a phantom, unusable
   grant for the first name behind, an I4 attributability smell (an audit reading `capabilities()` back
   would see a grant with no corresponding usable secret). **Fixed**: the auto-derived grant now lives
   in its own `primary_secret_grant_` field, overwritten (not appended) each call — last-call-wins,
   matching `api_key_ref_`'s own semantics. Regression-proofed: `tests/test_session_builder_prototype.
   cpp`'s "B4" case.
3. **STILL OPEN — real, disclosed, not yet fixed.** `build()` requires `Store` to be default-
   constructible and expose `.set(name, value)` — properties `InMemorySecretStore` (the default, and
   the only type this file's own tests exercise) has, but which are neither part of the `SecretStore`
   concept nor present on this project's own real production store, `AgentEngineSecretStore` (no
   default constructor, no `.set()`). `InMemorySecretStore` is `trust/secret.hpp`'s own designated
   **test-only** backend ("Production code constructs `AgentEngineSecretStore`... never this" — its own
   header comment); its values sit in a plain, never-zeroized map, a direct contradiction of 018 §4's
   secret-hygiene invariant the rest of that file goes out of its way to uphold. A host following this
   facade's DEFAULT, documented path (`OpenAiSessionBuilder`/`AnthropicSessionBuilder`,
   `.api_key_from_env()`) currently wires a real credential into this project's own test-only,
   non-hygienic store, with nothing at the API surface stopping them.

**FIXED in a same-session follow-up pass** (not independently red-teamed yet — flagged, not glossed
over): `.store(Store)` now accepts an already-constructed, host-owned `Store` of ANY shape — proven
against the project's real `AgentEngineSecretStore` over `EnvSecretSource`, not a stand-in
(`tests/test_session_builder_prototype.cpp`'s "B5"). `.api_key(SecretRef)` separately declares which
ref the `ChatClient` resolves against and auto-grants its `cap::Secret`, independent of how `.store()`
got populated. `.api_key_from_env()` survives as `requires`-gated, test-only sugar — composing
`.api_key()` + `.store()` under the hood — that simply does not exist as a callable member (a hard
compile error, not a silent wrong assumption) when `Store` isn't default-constructible-and-`.set()`-
able. A real, disclosed behavior change came with this fix: `build()` now MOVES `store_` out of the
builder (required to stay generic over non-copyable stores like `AgentEngineSecretStore`, which owns a
`unique_ptr<SecretSource>`), so a SECOND `build()` call on the same instance now fails closed with
`quickstart_builder.no_store` rather than the prior red-team's verified "produces two independent
Bundles" non-finding (below) — proven, not just asserted, by `test_session_builder_prototype.cpp`'s
"B6". No formal `try_compile()` compile-fail gate was added for the `requires`-clause rejection itself
(this project's own established idiom for "must not compile" claims, e.g. `tests/compile_fail/`) —
skipped as disproportionate for a mechanically obvious constraint (a `requires`-clause checking
`.set()`/default-construction against a type that provably has neither cannot do anything but reject
it), unlike ADR-071's `native_provider_families_distinct` gate, which tested genuinely non-obvious
compile-time logic. Named here rather than silently omitted.

Two non-findings, verified rather than assumed at the time of the original pass: `build()` genuinely
fails closed on every branch (checked independent of the two the test exercises), and `build()` called
twice was safe before the finding-3 fix above (produced two fully independent `Bundle`s, no aliasing)
— that second property is now INTENTIONALLY changed by the finding-3 fix, not a regression the red-team
would have flagged had it reviewed the fix.

**Verdict from the pass, before the above fixes:** not safe to build §2b/§2d on top of without fixing
finding 1 first; finding 3 should be resolved or loudly disclosed before presenting this as production-
ready. **All three findings are now closed** — 1 and 2 fixed and red-teamed; 3 fixed in a same-session
follow-up, real and tested, but not yet independently red-teamed the way 1/2 were.

## 0. Correction found during implementation — §3's own fix does not compile as written

**`AgentSession` cannot be moved or copied at all.** It holds `rt::AsyncMutex session_mutex_`
(`agent_session.hpp`) directly as a data member; `AsyncMutex`'s copy constructor is explicitly deleted
and it declares no move constructor of its own (`async_mutex.hpp`) — by the ordinary C++ rule ("a
user-declared copy/move special member suppresses every implicitly-generated move member"),
`AgentSession` is therefore neither movable nor copyable. §3 below (kept verbatim, as originally
written) designed `Bundle` around a plain, **move-constructed** `SessionT session_` member — that does
not compile, full stop, not merely a lifetime risk.

**Fix, implemented:** `Bundle` heap-allocates the `AgentSession` itself (`std::unique_ptr<SessionT>`),
configured in place via `initialize()`/`emplace_chat_client()`/`set_capabilities()` and never moved
afterward — matching how every existing example/test already uses `AgentSession` (constructed once, in
place, configured, never relocated). This closes §3's own `Store`/`CapabilitySet` reference-lifetime
finding as a side effect, via one uniform rule ("everything long-lived is heap-owned by `Bundle` at a
stable address") instead of the session-specific exception §3 originally proposed. §3's *diagnosis*
(the reference-lifetime hazard) was correct; only its proposed *fix shape* was wrong. Left unedited
below for the historical record — read this correction first.

**Origin:** a same-session conversation started from "is the engine API too verbose to build a new
app with?" — verified concretely, not assumed: `examples/01_hello_agent.cpp` is ~125 lines for an
*offline, fake-client* hello-world; a real provider (`tools/cli_chat.cpp:1051-1076`) needs a hand-
assembled `SecretStore` + `Capability{cap::Secret{...}}` + `CapabilitySet::grant_root({...})` before a
`ChatClientT` can even be constructed. This draft is the concrete design for the convenience layer
that conversation converged on, extended per this session's own follow-up: AgentEngine's model-call
and context-provider surfaces are each a *stack* of composable wrapper types (`ModelCallGateway`,
`MiddlewareModelCallGateway`, `ContentReplayGateway`; `HistoryProvider<Window/Summarize>`,
`ComposedContextProvider<Ms...>`), not one type each — a builder that defaults to the *simplest*
member of each stack (a bare `OpenAIChatClient`, a bare `HistoryProvider<Window<0>>`) would be
strictly worse than what a careful integrator already builds by hand today.

## 1. What problem this solves, and what it explicitly does not touch

**Solves:** the *bootstrap/wiring* ceremony a host must perform once, at startup, to get from "I have
an API key and a model name" to a running `AgentSession` — secret storage, capability granting,
`ChatClientT` construction and stacking, `HistoryProviderT` composition, sending one message and
reading the reply back as plain text.

**Does not touch:** `Agent<Policies...>` authoring (CRTP policy composition + its declarative YAML/
JSON equivalent, I6) — CLAUDE.md's locked "v1 authoring surfaces are C++ CRTP and declarative YAML/
JSON" decision is about *how an agent's behavior is declared*, not about how a host constructs and
configures the runtime objects (`SecretStore`, `CapabilitySet`, `ChatClientT`, `AgentSession`) that
declaration eventually runs inside. This builder is a facade over the *second* thing only. Named
explicitly so a future reader doesn't mistake it for a second authoring surface and reopen a locked
decision that was never actually in scope.

## 2. Design — per-slot, matching what already exists, not inventing new mechanism

The house rule from `tool-optimizer-provider-design-draft.md` applies here too: prefer an ordinary
composite over existing, already-proven types to a new generic mechanism. Every builder method below
constructs exactly the same object graph a careful hand-written integration already would — it is a
facade, not a new runtime concept.

### 2a. `ChatClientT` slot — the stack, defaulted one rung above "bare client"

Verified via CodeGraph (`include/agentengine/core/model_call_gateway.hpp`,
`include/agentengine/protocol/{openai,anthropic}/chat_client.hpp`):

| Layer (outer → inner) | Type | ADR |
|---|---|---|
| discard-and-retry on policy violation | `ContentReplayGateway<Inner>` | 069 |
| cross-cutting middleware chain | `MiddlewareModelCallGateway<Inner, Ms...>` | 033/036 |
| retry + circuit-breaker + failover | `ModelCallGateway<Primary, Fallback...>` | 036 |
| raw backend | `OpenAIChatClient<Store>` / `AnthropicChatClient<Store>` | 004 |

```cpp
// core/session_builder.hpp (sketch, not implemented)
template <SecretStore Store>
class QuickstartSessionBuilder {
public:
    QuickstartSessionBuilder& openai(std::string model, SecretRef key, ChatClientCapabilities caps);
    QuickstartSessionBuilder& anthropic(std::string model, SecretRef key, ChatClientCapabilities caps);
    // ^ exactly ONE primary backend call, required -- picks host/port/path_prefix by well-known
    //   provider default (api.openai.com / api.anthropic.com), overridable via .endpoint(...).

    QuickstartSessionBuilder& with_fallback(/* same shape as the primary call, any backend type */);
    QuickstartSessionBuilder& with_retry(RetryPolicy);          // else ModelCallGateway's own default
    QuickstartSessionBuilder& with_middleware(auto... ms);      // wraps in declared order
    QuickstartSessionBuilder& with_content_replay(ContentReplayTrigger);

    // ...history/context (2b), capability/secret (2c), approval (2d)...

    auto build();   // returns a move-only Bundle -- see §3, the store-lifetime landmine
};
```

`build()` **always** produces at least `ModelCallGateway<Primary>` (empty fallback tuple, default
`RetryPolicy{}`/`BreakerConfig{}`) — never a bare `OpenAIChatClient`/`AnthropicChatClient` directly in
`AgentSession`'s `ChatClientT` slot. Justification, cited: `ModelCallGateway`'s own constructor
(`model_call_gateway.hpp:136-138`) accepts a **default-constructed** `RetryPolicy`/`BreakerConfig` and
an **empty** `std::tuple<>` fallback — the marginal ceremony over a bare client is one extra template
argument, not one extra concept the host must learn, so there is no honest "simple mode" that a raw
client is actually simpler than. `MiddlewareModelCallGateway`/`ContentReplayGateway` stay opt-in
(`with_middleware`/`with_content_replay`) since they need host-supplied types (middleware conformers,
a `ContentReplayTrigger`) the builder cannot default sensibly.

**Composition order is builder-owned, not caller-owned.** `.with_middleware(...)` and
`.with_content_replay(...)` may be called in any order the host finds natural; `build()` always nests
them `ContentReplayGateway<MiddlewareModelCallGateway<ModelCallGateway<...>, Ms...>>` — outer-to-inner
fixed, per `002-Agent-Model-and-Authoring.md:191-196`'s own stated reasoning for why this nesting order
is correct (review the final, post-retry outcome once, not once per retry attempt). A host calling
these methods in the "wrong" order gets the *correct* object graph anyway — the one property a facade
must guarantee, or it isn't saving the host from a real mistake class.

**Escape hatch, not a second default:** `.raw_client_only()` bypasses `ModelCallGateway` entirely and
installs the bare client directly — for a deterministic fake (`JokerChatClient`,
`examples/01_hello_agent.cpp`) or a test double, where retry/breaker semantics are meaningless noise.
Named explicitly as escape hatch, not a coequal option, so a reader doesn't reach for it by habit in
production code.

### 2b. `HistoryProviderT`/context slot — same "one rung above bare" rule, composed in a fixed, known-correct order

Verified via CodeGraph (`include/agentengine/core/composed_context_provider.hpp`,
`include/agentengine/core/skill_provider.hpp`): `ComposedContextProvider<Ms...>`'s own file-top comment
(`composed_context_provider.hpp:30-33`) states contributor order **is** wire-message order; separately,
`skill_provider.hpp`'s own history (cited in `history_and_skills_provider.hpp`) already established
that skills must precede history on the wire. This is exactly the class of detail a hand-rolled
integration gets right once by luck and a builder should get right by construction, every time.

```cpp
QuickstartSessionBuilder& window(std::size_t n = 0);          // HistoryProvider<Window<n>>, default
QuickstartSessionBuilder& with_skills(std::vector<SkillSourceDescriptor>);
QuickstartSessionBuilder& with_memory(/* MemoryProvider ctor args */);
QuickstartSessionBuilder& with_rag(/* VectorRagContextProvider ctor args */);
QuickstartSessionBuilder& with_tool_optimizer(/* once it exists -- see tool-optimizer-provider-design-draft.md */);
```

`build()` picks the `ComposedContextProvider<...>` template argument order internally: skills, then
memory/RAG, then history last — **not** the order the host called these methods in. If only `.window()`
was called (no skills/memory/RAG), `build()` collapses to the plain `HistoryProvider<Window<n>>` alone
rather than a one-element `ComposedContextProvider<HistoryProvider<Window<n>>>` — avoids paying the
composite's indirection for the common single-contributor case, matching `AgentSession`'s own
documented default-template-argument shape (`agent_session.hpp:559`, one-argument instantiation already
means "just history, nothing composed").

### 2c. Capability/secret slot — pure sugar, zero new default authority (unchanged from the prior survey)

`.openai(model, key, caps)` above does **not** itself grant `cap::Secret` — it only names which
`SecretRef` the constructed `ChatClientT` will resolve at call time (`chat_client.hpp:891-893`'s own
"resolution happens inside chat(), never at construction" rule, unchanged). Granting is a **separate,
mandatory** call:

```cpp
QuickstartSessionBuilder& secret_from_env(std::string secret_name, char const* env_var);
    // -> Store::set(secret_name, getenv(env_var)) + a queued Capability{cap::Secret{secret_name, ttl}}
QuickstartSessionBuilder& grant(Capability);   // escape hatch for anything else (FsRead, NativeExec, ...)
```

`build()` fails (a `result<Bundle>` error, not a thrown exception or a silent no-op) if `.openai(...)`/
`.anthropic(...)` named a `SecretRef` that no `.secret_from_env(...)`/`.grant(...)` call ever covered —
catching the exact "host forgot to grant the key it referenced" mistake at build time instead of at
first-call time deep inside a coroutine. This is a real ergonomic win the raw API does not offer today
(a missing grant currently surfaces as a `chat()`-time `failure_class::policy` error, mid-run).

### 2d. Approval/policy slot — thin sugar over `ApprovalDecider`/`PolicyDecider` (ADR-070), unchanged shape

```cpp
QuickstartSessionBuilder& require_approval_for(std::vector<std::string> tool_names);
QuickstartSessionBuilder& policy(PolicyDecider);
```

No new default behavior: `AgentSession`'s own existing fail-closed default (no decider installed ==
`approval_mode::always_require` wins, per ADR-070's already-Judged design) is preserved untouched;
these two methods only shorten the syntax for installing a host-authored decider, identical in spirit
to §2c.

## 3. The one integration point that must not be gotten wrong

**`OpenAIChatClient<Store>`/`AnthropicChatClient<Store>` hold `Store const&` — a reference member, not
a value** (confirmed: `chat_client.hpp:949`, `Store const& store_;`). A naive `build()` that
stack-allocates the `SecretStore` as a local inside the builder method and returns
`AgentSession<...>` **by value** produces an immediately-dangling reference the moment `build()`
returns — a real, silent memory-safety bug, not a hypothetical one, and exactly the kind of landmine a
convenience layer exists to remove rather than reintroduce under a friendlier name.

**Required shape:** `build()` must return a single move-only `Bundle` that owns the `Store` at a
stable address (heap-allocated inside `Bundle`, e.g. `std::unique_ptr<Store>`) *and* the
`AgentSession<...>` referencing it, constructed in that order and never separated — the same
"ownership and the reference into it travel together, or not at all" rule `Store const&` already
imposes on any hand-written integration today. `Bundle` is not copyable (matching `AgentSession` itself
being move-only through its `chat_client_`/`history_provider_` members) and exposes `.session()`/
`.ask(text)` (§4) as its only surface — the raw `Store`/`ChatClientT` are not meant to be reached back
out to individually, since doing so would let a caller separate them again.

## 4. `.ask(text)` — the one-shot round-trip sugar

```cpp
// Bundle::ask, sketch
result<std::string> ask(std::string text) {
    auto r = drive(session_.start_run(StartRun{user_message(std::move(text))}));
    if (!r) return std::unexpected(r.error());
    return text_of(r->message);
}
```

Directly mirrors `examples/01_hello_agent.cpp:83-101,114-120`'s own `user_message()`/`drive<T>()`/
`text_of()` sequence — no new mechanism, just named and owned once instead of copy-pasted per example/
app. `drive<T>()`'s own safety precondition (`agentengine/rt` task never genuinely suspends on external
I/O within one `resume()` loop) holds for a synchronous, non-gateway-streaming call exactly as it does
in every existing `rt::` test file — `Bundle::ask` does not change that precondition, it packages an
already-safe idiom.

## 5. Self-red-team

- **I2 (no ambient authority):** every `.grant(...)`/`.secret_from_env(...)`/`.require_approval_for(...)`
  call maps 1:1 to an explicit `Capability`/`ApprovalDecider` the host itself named — `build()` never
  synthesizes a capability the host didn't request (§2c). The `build()`-time "referenced secret has no
  grant" check (§2c) is a *stricter* failure mode than today's raw API, never a looser one — it can only
  reject a construction that would already have failed later, never admit one that wouldn't have.
- **I3 (model output never authority):** nothing in the builder's surface accepts a `Tainted<T>` or
  anything derived from a `ChatResponse`/tool result — every builder method's arguments are host-authored
  literals/config, matching ADR-071 §4 property 4's identical requirement for `Native*Provider`
  construction.
- **Does defaulting to `ModelCallGateway<Primary>` (§2a) change observable behavior vs. a bare client in
  a way a host must know about?** Yes, named honestly rather than hidden: retries change *timing* (a
  transient failure that would have surfaced immediately now retries per `RetryPolicy::default`'s own
  attempt count/backoff first) and `run_start`'s own event-stream warning
  (`agent_session.hpp:826-832`, "a gateway-routed round makes several real backend calls... before the
  per-run token budget is checked") applies unconditionally the moment `build()` installs a gateway by
  default. This is a real, disclosed trade this draft makes deliberately (§2a's own justification: the
  marginal ceremony is near-zero, so defaulting to the safer stack is the right call) — not a residual
  to fix later.
- **Does the `ComposedContextProvider` ordering choice (§2b) hide a decision the host might disagree
  with?** The ordering itself (skills before history) is not this design's own opinion — it is
  `history_and_skills_provider.hpp`'s already-shipped, already-correct wire-order rule, just applied
  generically instead of re-derived per app. A host who genuinely needs a different order has the
  existing `ComposedContextProvider<Ms...>` (or `AgentSession`'s raw template parameter) as an escape
  hatch — the builder narrows the *easy* path, it does not remove the general one.
- **Store-lifetime landmine (§3):** the one finding in this pass with real memory-safety consequences if
  gotten wrong — called out as its own numbered section rather than folded into the self-red-team list,
  matching this project's convention of giving a load-bearing implementation risk its own visible home
  (`tool-optimizer-provider-design-draft.md §3`'s identical treatment of its own highest-risk point).
- **I6 (declarative/native equivalence):** does not apply — reconfirmed in §1, this facade sits below
  `Agent<Policies...>`/YAML authoring, never inside it. No declarative-surface analogue is owed by this
  design; flagged so a future reader doesn't manufacture a YAML-builder-parity requirement that was
  never actually implied.

## 6. Open questions — not designed here

- Should `Bundle` also expose a streaming `.ask_stream(text) -> stream<std::string>` sugar over
  `chat_stream()`/`stream_model_calls_`? Real demand likely (see `examples/07_streaming.cpp`), but adds
  its own event-plumbing surface — deliberately deferred to keep this draft's first cut to the
  synchronous case only. **Confirmed, during this same conversation, to be structurally impossible on
  top of §2a's current default `ChatClientT` (`ModelCallGateway<Primary>`) as things stand today** — a
  gateway-typed `AgentSession` emits zero `model_delta` events regardless of `stream_model_calls_`
  (`agent_session.hpp`'s own `ModelCallGatewayLike` branch). Tracked separately, deliberately not
  designed here or fixed in isolation: `docs/planning/quickstart-builder-streaming-gap.md`.
- Whether a `.dev_defaults()` convenience (wires an in-memory, non-persistent `SecretStore` +
  `Window<0>` + no approval gate, for a scratch/prototype session) is worth naming as a *second*,
  explicitly-labeled preset distinct from the production default in §2a/§2b — flagged, not designed,
  since a mislabeled "dev" preset that quietly becomes someone's production config is a real, seen-
  before failure mode this draft would rather not invent casually.
- Interaction with the `Native*Provider` family (ADR-071, Judged): should `QuickstartSessionBuilder`
  gain `.with_native_shell(...)`/`.with_native_python(...)` sugar over `cap::NativeExec` +
  `NativeCapabilityAnnouncer<Ps...>`? Deferred — that family's own compile-time family-distinctness
  guarantee (ADR-071 §5g) needs its `Ps...` pack fixed at the builder's own template-instantiation
  point, which interacts with this builder's currently-runtime, fluent-chain shape in a way this draft
  has not worked out yet.

## 7. What this draft is not

Not an ADR. §2a/§2c/§3(as corrected in §0)/§4 are now real, tested code, red-teamed once (§0b) with all
three findings closed — §2b/§2d and the fallback/middleware/content-replay/raw-client-only surface
remain design-only, unimplemented. This facade's own risk profile is genuinely low (§5: no new
capability semantics, no new authority path — every real finding §0/§0b surfaced was an ordinary C++
lifetime/concurrency/hygiene bug, not an I2/I3 mechanism breach) — proportionate next step is real code
for §2b/§2d following this same design → prototype → red-team → fix cycle, not the full
`design → red-team → prove → judge` gauntlet ADR-070/071-class changes require, since nothing here
touches I2/I3's own mechanisms, only how conveniently a host can drive them correctly. Finding 3's fix
itself has not been independently red-teamed yet (only findings 1/2 got a genuine adversarial pass) —
a small, disclosed residual worth a light second look before or alongside §2b/§2d, not a blocker to
starting them. If a later pass surfaces a finding that *does* touch I2/I3's own mechanisms, that finding
gets its own escalation at that point, matching this project's established practice — not decided in
advance here.
