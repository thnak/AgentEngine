#pragma once
// QuickstartSessionBuilder -- a convenience facade over AgentSession's wiring, promoted from
// prototype to a supported feature (2026-08-22) after three red-team rounds against this real code,
// all findings closed. Implements docs/planning/quickstart-session-builder-design-draft.md's §2a
// (client stack, gateway-wrapped by default)/§2b (history/context composition, `ComposedQuickstart
// SessionBuilder` -- resolved during a 4th pass, see finding 8 below; a real structural gap, not just
// unstarted work)/§2c (capability+secret sugar, generalized past the draft's own original sketch --
// see finding 3 below)/§2d (approval/policy sugar, corrected from its original sketch -- see finding 5
// below)/§3 (the Store-lifetime finding)/§4 (`.ask()`). Explicitly NOT implemented here, named rather
// than silently dropped: `.with_fallback()`/`.with_middleware()`/`.with_content_replay()`, and the
// draft's own `.raw_client_only()` escape hatch. A convenience layer over already-Reviewed RFCs (002,
// 004, 005, 006, 007, 018), not itself a new invariant or capability shape, so it has no ADR of its
// own -- round 1 found findings 1-2; round 2, specifically against finding 3's own fix, found finding
// 3's own `.store(Store)` shape was itself wrong (finding 4); round 3, specifically against finding
// 5's `.approve_tools()`/`.policy()` landing, found and LIVE-REPRODUCED a real hang (finding 7) plus a
// documentation overclaim; round 3's own fix (finding 7) has not itself been independently red-teamed
// a fourth time -- disclosed here as an open item, not silently treated as closed. §2b's own
// `ComposedQuickstartSessionBuilder`/`detail::LazyComposedContextProvider` (finding 8) then WAS
// red-teamed (round 4): found and fixed a real session-isolation gap on `AgentSession::fork_from()`
// (finding 9) and a diagnostic-quality gap on an empty `Ms...` pack (finding 10), plus two test gaps
// closed (on_turn_end fan-out, two-real-contributor wire order). Round 4's own fixes (findings 9-10)
// then WERE red-teamed (round 5): found finding 9's own fix was necessary but not sufficient (finding
// 11 -- see below). Round 5's own fix (finding 11) then WAS red-teamed (round 6, explicitly re-scoped
// to also cover finding 7, never independently re-examined since round 3): found no functional bug in
// either -- both held under deliberately harsher probing (self-move-assignment, move-construction,
// third-generation moves, a fresh `max_turns`/`token_budget` audit) -- only a real test-coverage gap,
// closed as B21a-d. This is the first round since round 1 to find no live bug; still not proof nothing
// remains, only that this specific, disclosed probing found none. Finding 7 THEN got its own dedicated
// round at last (round 7, finding 13): a real bug, but NOT in this file -- `AgentSession::
// resolve_codeact_ask()`'s own ask-pending branch (`rt/agent_session.hpp`) never advanced `turn_index`,
// so `.max_turns()`/`.token_budget()` were completely bypassed by a non-converging CodeAct ask loop.
// This facade's OWN `max_turns_`/`token_budget_` wiring was always correct; what it wires into had the
// gap. Fixed at the root, in `rt/agent_session.hpp` -- see that file's own comment at the fix site, and
// `decisions/ADR-057-agent-ask-suspend-without-deadlock.md` §8 for the addendum, and this facade's own
// design draft §0i for the full account. Regression-proofed: `tests/test_rt_agent_session_codeact_ask_
// max_turns.cpp`, not this file's own test file, since the bug and its fix are both in `AgentSession`.
//
// Round 8: findings 11 and 13 (round 7's own fix) both got their own dedicated, independent
// re-examination round at last. Finding 11 (this file's `LazyComposedContextProvider` move ctor/
// assignment) held up clean under live probing (self-move-assignment, move-assignment INTO an
// already-engaged target from a different source, both confirmed no leak/no double-invoke via a
// destructor-counting mock). But a NEW real bug was found in the SAME class: `engage()` was not
// exception-safe (finding 15, MEDIUM). `build_contributors()` used to `push_back` each contributor
// directly into `contributors_` inside a fold -- if a LATER `Ms`'s move constructor (or
// `make_context_provider_descriptor()`'s own internal `make_shared`) threw partway through,
// `contributors_` was left PARTIALLY populated while `engaged_` stayed `false` (the `engaged_ = true`
// assignment was never reached). Since `engaged_ == false`, the `already_engaged` guard did NOT block
// a retry -- and round 5's own fix comment explicitly frames retry-after-failure as safe recovery --
// but `reserve()` does not clear existing elements, so a retry's `build_contributors()` APPENDED onto
// the stale entries instead of starting fresh. LIVE-REPRODUCED: a throwing-provider mock, first
// `engage()` throws after 1 of 2 contributors pushed, a retry with fresh providers succeeds, then ONE
// `on_context()` call invoked the stale first-attempt contributor a SECOND, duplicate time -- exactly
// the "duplicate every contributor on the wire" hazard `already_engaged` exists to prevent,
// reintroduced via the exception path. **Fixed**: `engage()` now builds into a LOCAL vector and
// publishes into `contributors_` (and flips `engaged_`) only once every `Ms` has been constructed
// without throwing -- a strong exception guarantee, so a throw now leaves `contributors_`/`engaged_`
// completely untouched and a retry genuinely starts fresh. Regression-proofed: `tests/test_session_
// builder.cpp`'s "B22" -- verified to have teeth (reverting the fix reproduces the exact double-invoke
// failure). Round 8 also re-examined finding 13's surrounding mechanism and found two LOW findings in
// `rt/agent_session.hpp` (findings 16 and 17, both fixed there -- see that file's own comments at the
// fix sites and this facade's design draft §0j for the full account): a stale "should be unreachable
// in practice" comment on `resolve_codeact_ask()`'s missing-record guard (it IS reachable via a
// session restore mid-ask, and used to leave the interaction stuck open forever instead of erasing
// it), and `clear_in_process_state()` never clearing `pending_codeact_asks_` (a real leak for a
// pooled/reused session). Full suite 222/222 after all three fixes landed.
//
// Correction found during implementation, not anticipated by the design draft's own §3: `AgentSession`
// (rt/agent_session.hpp) holds `rt::AsyncMutex session_mutex_` directly as a data member;
// `AsyncMutex`'s copy constructor is explicitly deleted and it declares no move constructor of its own
// (rt/async_mutex.hpp) -- by the ordinary C++ rule ("a user-declared copy/move special member
// suppresses every implicitly-generated move member"), `AgentSession` is therefore neither movable nor
// copyable. The draft's own `Bundle` sketch assumed a plain, move-constructed `SessionT session_`
// member -- that does not compile. Fixed here: `Bundle` heap-allocates the `AgentSession` itself
// (`std::unique_ptr<SessionT>`), configured in place via `initialize()`/`emplace_chat_client()`/
// `set_capabilities()` and never moved afterward -- matching how every existing example/test already
// uses `AgentSession` (constructed once, in place, configured, never relocated). This also closes the
// draft's own §3 `Store`/`CapabilitySet` reference-lifetime finding as a side effect, via one uniform
// rule ("everything long-lived is heap-owned by `Bundle`, referenced by stable address") instead of a
// session-specific exception to it.
//
// Red-team pass (against this real code, not the draft's sketch) found two real issues, both fixed
// here -- see docs/planning/quickstart-session-builder-design-draft.md's own red-team addendum for the
// full report:
// 1. `Bundle::ask()` originally reused examples/*.cpp's naive `while(!done()) resume()` idiom against a
//    real `AgentSession`. That idiom is safe ONLY when `session_mutex_` (rt::AsyncMutex) is never
//    contended -- true by construction in every linear, single-threaded example `main()`, but NOT
//    guaranteed for `Bundle`, a reusable, potentially-shared object with no built-in single-caller
//    discipline. Under real contention, `AsyncMutex` parks the coroutine for a LATER, possibly
//    DIFFERENT thread's `unlock()` to resume directly (rt/async_mutex.hpp) -- a second, blind
//    `resume()` on that same handle is a genuine cross-thread double-resume race, undefined behavior
//    per the C++20 coroutine spec (this exact bug class, previously found and fixed in
//    `rt/thread_pool.hpp`/`rt/drive_leaf_task.hpp` -- see their own file-top comments). Fixed with two
//    layers: `ask_mutex_` serializes every `.ask()` call this Bundle ever makes (so `session_mutex_`
//    should never actually be contended from here), and the drive loop itself is now bounded to one
//    `resume()` with a fail-closed error if that alone did not finish -- matching `rt/drive_leaf_task.
//    hpp`'s own shape -- as defense in depth, not a substitute for the lock. NOT covered: a caller who
//    reaches the raw session via `Bundle::session()` and calls `start_run()`/`resolve_interaction()`
//    directly, concurrently with `.ask()` -- that bypasses `ask_mutex_` entirely and inherits
//    `AgentSession`'s own ordinary I1 single-executor obligation, same as any other direct use.
// 2. `api_key_from_env()` pushed a NEW `cap::Secret` grant into `grants_` on every call, unconditionally
//    -- calling it twice (e.g. to correct a typo'd env-var name) left a phantom, unusable grant for the
//    FIRST name behind. Fixed: the auto-derived secret grant is now tracked in its own single
//    `primary_secret_grant_` field, overwritten (not appended) each call, matching `api_key_ref_`'s own
//    last-call-wins semantics -- `grants_` is now exclusively the explicit `.grant()` escape hatch.
//
// 3. (Follow-up pass, generalizes the builder rather than fixing a bug) `build()` used to require
//    `Store` to be default-constructible and expose `.set(name, value)` -- properties
//    `InMemorySecretStore` has but this project's real production store, `AgentEngineSecretStore`
//    (trust/secret.hpp -- no default constructor, no `.set()`), does not. `.api_key_from_env()` was
//    the ONLY way to supply a Store at all, so a host could not use this builder with a real production
//    secret store, full stop -- not merely a hygiene warning, a hard capability gap. Fixed:
//    `.api_key(SecretRef)` separately declares which ref the `ChatClient` resolves against + auto-
//    grants its `cap::Secret`, independent of how the Store gets populated; `.api_key_from_env()`
//    still exists as TEST-ONLY convenience sugar, `requires`-gated (a hard compile error, not a silent
//    wrong assumption) to only exist when `Store` is default-constructible and exposes `.set()` -- i.e.,
//    genuinely `InMemorySecretStore`-shaped. `build()` now MOVES `store_` out of the builder (needed to
//    stay generic over non-copyable stores) -- calling `build()` a second time on the same builder
//    instance now fails closed with `quickstart_builder.no_store` instead of the prior red-team's
//    verified "produces two independent Bundles" non-finding. Not UB, not silently wrong -- just
//    single-use now, named here for the record. `.store(Store)` in this pass's FIRST version took a
//    Store BY VALUE, claimed (wrongly) to work "for any real SecretStore conformer" -- a SECOND
//    red-team pass, specifically against this fix, compiler-confirmed that claim false:
//    `trust/secret_quarantine.hpp`'s `QuarantineSecretStore` (a real, already-shipped conformer,
//    ADR-068) holds a `std::mutex` directly, making it neither movable nor copyable -- a by-value
//    `.store(Store)` could not even be CALLED with one. Also found: the test proving this path
//    (`test_session_builder.cpp`'s "B5") never actually called `.resolve()`/`chat()`, so the
//    design draft's "proven end to end" wording overclaimed what it actually tested (construction-only,
//    not the capability-name-match at resolve time).
// 4. `.store()` is now a variadic, forwarding EMPLACE (matches `AgentSession::emplace_chat_client`'s own
//    established idiom, rt/agent_session.hpp) -- constructs `Store` in place from whatever constructor
//    arguments it needs, never requiring `Store` to be movable OR copyable at all. Closes finding 3's
//    residual: `QuarantineSecretStore` now works too (regression-proofed, `test_session_builder.cpp`'s
//    "B7"). `.api_key_from_env()`'s internal use updated to match (default-emplace,
//    then mutate through the `unique_ptr` -- never moves a `Store` value either). B5 strengthened
//    ("B5b") to actually call `.resolve()` against the real `AgentEngineSecretStore`/`EnvSecretSource`
//    pair with the exact capability grant `.api_key()` produces, closing the overclaim -- this specific
//    fix (findings 3-4 combined) has now been through ONE red-team round of its own (this one); not a
//    second, independent one yet, same disclosure posture finding 3 originally had.
// 5. §2d's original sketch (`docs/planning/quickstart-session-builder-design-draft.md` §2d) named the
//    method `.require_approval_for(tool_names)` -- a misleading name against the REAL mechanism, caught
//    before implementation rather than after: `ApprovalDecider` (core/tool_pipeline.hpp) is consulted
//    ONLY for a call that a tool's OWN declared `approval_mode` already marked as needing a decision
//    (`always_require`, or `policy_driven` with no `PolicyDecider`) -- it cannot make MORE tools require
//    approval than their own declaration already does. Implemented instead as `.approve_tools(names)`:
//    installs a decider that auto-approves ONLY the named tools and denies every other already-gated
//    call (the safe default) -- narrows/decides among already-required decisions only, never widens
//    which calls need one (I2). If never called, no decider is installed at all, preserving
//    `AgentSession`'s own true unset-decider default exactly, not approximating it with an
//    always-false one. `.policy(PolicyDecider)` is a thin, unmodified pass-through to
//    `set_policy_decider` (ADR-070).
// 6. §2b (history/context composition) was explicitly NOT attempted in this pass, for a real
//    structural reason rather than lack of time: `AgentSession<ChatClientT, StateT, HistoryProviderT>`'s
//    `HistoryProviderT` is a COMPILE-TIME type parameter, exactly like `ChatClientT` (§2a) -- adding
//    `.with_skills(...)` would need to change `Bundle`'s/`QuickstartSessionBuilder`'s own C++ type
//    partway through a fluent chain, the same "no clean single return type for a runtime toggle between
//    two different types" problem §2a's own top-of-class comment already named for `Provider`. Unlike
//    `Provider` (one value, chosen once, at construction), history/context composition is naturally
//    MULTI-VALUED (`Ms...`, any subset of contributor TYPES, in any combination) -- a single extra
//    template parameter does not scale the way `Provider` did. **RESOLVED in a 4th pass -- see finding
//    8 below**: a distinct, separately-templated builder type (`ComposedQuickstartSessionBuilder<
//    Provider, Store, Ms...>`), the second of the two options this finding originally named. The other
//    option (a type-changing fluent builder, every setter `&&`-qualified) was not attempted -- finding
//    8's own mechanism made it unnecessary, not merely deferred a second time.
// 7. **A third red-team round, against finding 5's `.approve_tools()`/`.policy()` landing, found and
//    LIVE-REPRODUCED a real hang.** `build()` called `session->initialize(session_id_, principal_)`,
//    never passing `max_turns`/`token_budget` -- `AgentSession::initialize()`'s own raw default for both
//    is `std::nullopt`, and `run_rounds()`'s turn loop (`agent_session.hpp`) is genuinely unbounded when
//    `max_turns_` is unset. A live probe (a scripted `ChatClient` that keeps requesting a tool
//    `.approve_tools()` denies, never emitting a terminating text-only reply -- ordinary retry-on-denial
//    behavior, not adversarial) hung the calling thread indefinitely. `Bundle::ask()`'s "bounded to one
//    `resume()`" guard (finding 1) does **NOT** catch this: every `chat()` call in this engine runs
//    synchronously to completion within one `resume()` (finding 1's own comment already says so), so the
//    ENTIRE unbounded turn loop executes inside that single guarded `resume()`, invisible to the
//    done()-check the guard relies on. Not an I2/I3 violation (nothing gets approved that shouldn't) --
//    a real availability/DoS-class gap, reachable through entirely ordinary use of the sugar this file
//    exists to make easy. **Fixed**: `.max_turns(std::optional<uint64_t>)`/`.token_budget(...)` setters
//    added, and `max_turns_` now defaults to a FINITE value (25, an arbitrary but reasonable safety net)
//    instead of mirroring `AgentSession`'s own raw unbounded default -- a deliberate divergence, named
//    here rather than silently matching the lower-level API. `.max_turns(std::nullopt)` remains a
//    legitimate way to opt back into genuinely unbounded turns, an explicit host choice. Also found (test
//    -gap, not a functional bug): the design draft's account of `.policy()`/`.approve_tools()` claimed
//    "proven end to end," but B9/B10 only ever call the extracted `ApprovalDecider`/`PolicyDecider`
//    directly, never through a live `start_run()` round -- the SAME overclaim shape finding 3 already
//    caught once for `.store()`/B5. Wording corrected in the design draft; a live-round test remains a
//    named gap, not yet added.
// 8. **§2b resolved (4th pass, not yet independently red-teamed).** `ComposedQuickstartSessionBuilder<
//    Provider, Store, Ms...>` fixes `Ms...` at declaration (same reasoning as `Provider`, finding 6);
//    `.providers(tuple, budgets)` supplies the REAL, host-constructed values for each `Ms` (no per-type
//    setter -- see this file's own top-of-class comment on that class for why). The part finding 6 did
//    NOT anticipate: `AgentSession<...>::history_provider_` is a PLAIN value member, always default-
//    constructed (no user-declared `AgentSession` constructor exists to route around this), so
//    `HistoryProviderT` must be default-constructible regardless of which builder shape is chosen --
//    `core/composed_context_provider.hpp`'s own `ComposedContextProvider<Ms...>` only clears that bar
//    when EVERY `Ms` is default-constructible, which real `SkillsProvider`/`MemoryProvider`/
//    `VectorRagContextProvider` never are (confirmed empirically: `tests/test_composed_context_provider
//    .cpp`'s only real `AgentSession` proof uses three default-constructible MOCK providers, never one
//    of these three real types -- composing any of them into a real `AgentSession` had never actually
//    been exercised before this pass, a materially bigger gap than finding 6's own text captured).
//    Fixed with `detail::LazyComposedContextProvider<Ms...>` -- ALWAYS default-constructible (starts
//    with an empty `contributors_`, not a default-constructed `Ms{}...`), engaged with the real values
//    after `AgentSession` already exists, via `AgentSession::history_provider()`'s mutable-reference
//    accessor (`rt/agent_session.hpp:647`) -- an accessor that DOES exist despite `composed_context_
//    provider.hpp`'s own comment (written before it was added) still describing this slot as having
//    "no emplace_*/accessor pair"; that comment is now stale and was not relied on here. `engage()` is
//    single-use per instance (fails closed on a second call, matching this file's own established
//    idiom for anything that would otherwise silently duplicate a wire contribution); `build()` itself
//    fails closed with a new `quickstart_builder.no_providers` if `.providers(...)` was never called,
//    the same posture `.api_key()`/`.store()` already have for their own missing-input cases.
//    `ComposedQuickstartSessionBuilder` duplicates §2a's fluent setters rather than sharing them via a
//    CRTP base -- a deliberate, disclosed simplification (see this file's own top-of-class comment on
//    that class), named as a candidate follow-up if a red-team pass finds the duplication drifting.
// 9. **Round 4 red-team, HIGH, I1/I4-adjacent: `AgentSession::fork_from()` silently aliases stateful
//    composed providers across sessions.** `fork_from()` (`rt/agent_session.hpp:1022`) plain
//    copy-assigns `history_provider_`. `LazyComposedContextProvider<Ms...>`'s `contributors_` holds
//    `ContextProviderDescriptor`s whose closures capture a `shared_ptr<Ms>` BY VALUE
//    (`make_context_provider_descriptor`, `context_assembly.hpp`) -- an implicit memberwise copy
//    therefore aliases the SAME underlying provider instances, not independent ones. LIVE-VERIFIED
//    (round 4's own probe): copy-assigning a stateful fixture, mutating the ORIGINAL via `on_turn_end`,
//    then reading the COPY's `on_context` showed the mutation. A forked session was meant to diverge
//    independently; instead it shares live mutable state (a memory-provider write-back, a per-skill
//    counter, a RAG cache) with its source -- a turn-end effect attributed to one `Principal` becomes
//    visible through another session's identity. Pre-existing in `core/composed_context_provider.hpp`'s
//    `ComposedContextProvider<Ms...>` too (not this file's own bug to begin with), but THIS pass is
//    what first makes it reachable through documented, public API composing real stateful providers
//    into a real, fork-capable session. **Fixed**: `LazyComposedContextProvider` is now move-only (copy
//    ctor/assignment `= delete`d, move ctor/assignment explicit `= default`) -- `fork_from()` is an
//    ordinary, non-template member function, only compiled when actually called, so this turns the
//    silent runtime aliasing into a compile error at the exact call site, with zero effect on every
//    already-passing path. The underlying `shared_ptr`-aliasing mechanism in `ComposedContextProvider`
//    itself is UNCHANGED -- out of this file's scope, named here as a real, disclosed residual rather
//    than silently left for the next caller to rediscover.
// 10. **Round 4 red-team, MEDIUM, diagnostics.** `ComposedQuickstartSessionBuilder<Provider, Store>`
//    (an empty `Ms...` pack) used to fail with a confusing multi-error cascade -- `LazyComposedContext
//    Provider<>`'s own `requires (sizeof...(Ms) >= 1)` fired, but cascaded into an unrelated
//    "unspecialized class template" error and ~20 lines of failures deep inside `<expected>`/
//    `<type_traits>`. **Fixed**: the identical constraint now also sits directly on
//    `ComposedQuickstartSessionBuilder` itself, so the OUTER template rejects an empty pack before ever
//    reaching the inner type's instantiation -- one clear diagnostic instead of a cascade. Also closed
//    as test gaps (not bugs): `on_turn_end` fan-out to every wrapped provider (previously proven for
//    `ComposedContextProvider` but not this file's own `LazyComposedContextProvider`), and wire order
//    with TWO providers that both contribute a real message (B16 previously proved order only for one
//    empty + one non-empty contributor, which cannot distinguish "order preserved" from "only the
//    non-empty one shows up regardless of position").
// 11. **Round 5 red-team, I1/I4-adjacent: finding 9's move-only fix was necessary but not sufficient.**
//    Finding 9 deleted copy ctor/assignment to block `fork_from()`'s aliasing COPY at compile time, but
//    left move ctor/assignment `= default`ed. `contributors_` (a vector) is correctly drained by a
//    default move, but `engaged_` (a plain `bool`) is trivially copied, not reset -- so the MOVED-FROM
//    side kept `engaged_ == true` over an now-EMPTY `contributors_`. LIVE-REPRODUCED: `session2.
//    history_provider() = std::move(session1.history_provider())` (the exact bypass route `on_context()`
//    's own comment below already names) left session1's `on_context()` NOT hitting the `!engaged_`
//    guard -- it silently returned a SUCCESSFUL, empty contribution instead of the `not_engaged` error
//    that guard exists to produce, and `engage()`'s `already_engaged` guard then permanently blocked
//    ever re-engaging it (no recovery). This is worse than what finding 9 anticipated: finding 9's own
//    text called `fork_from()` "the one real place this bites" -- demonstrably not true, since the same
//    public `history_provider()` accessor reaches an ENGAGED instance too, and that path bypasses BOTH
//    of this class's own fail-closed guards rather than tripping either one. **Fixed**: move ctor/
//    assignment now explicitly reset the moved-from side's `engaged_` to `false` (and defensively clear
//    its `contributors_`) -- restores the invariant ("`engaged_` implies `contributors_` is populated")
//    across a move, so a moved-from instance correctly fails closed via the EXISTING `not_engaged` guard
//    and can be `engage()`d again. Regression-proofed: `tests/test_session_builder.cpp`'s "B20".
//    Deliberately NOT changed: a move-assignment INTO an already-engaged target still silently replaces
//    its contributors with no diagnostic -- ordinary `operator=` replacement semantics, identical to
//    `history_provider() = HistoryProviderT{}`'s own pre-existing silent-reset behavior, not a new
//    hazard finding 11 introduces or needed to close.
// 12. **Round 6 red-team: no functional bug found, in either finding 7 or finding 11.** Explicitly
//    re-scoped to re-examine both fixes with fresh, harsher probing (self-move-assignment, move-
//    CONSTRUCTION as a genuinely distinct path from move-assignment, a third-generation move-then-
//    re-engage-then-move-again, and a fresh `max_turns`/`token_budget` audit including the token-
//    budget-checked-after-not-before-a-call ordering and whether a slow/pathological `on_context()`
//    escapes the turn bound) -- all held. The first round since round 1 to find no live bug; not proof
//    nothing remains, only that this specific, disclosed probing found none. One real test-coverage gap
//    closed as B21a-d: B20 only proved the invariant survives ONE generation of move-ASSIGNMENT between
//    two distinct instances; `ComposedQuickstartSessionBuilder` also had no double-`.build()` regression
//    test at all (the base `QuickstartSessionBuilder` has had one, B6, since round 1) even though
//    finding 11 changed the exact move machinery `build()`/`engage()` depend on.

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/context_assembly.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/model_call_gateway.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

// protocol/{openai,anthropic}/chat_client.hpp are each wrapped, file-wide, in their own
// `#ifdef AGENTENGINE_WITH_HTTPS` guard -- `OpenAIChatClient`/`AnthropicChatClient` are not valid
// names at all in a build with that option OFF, regardless of whether this header's own templates are
// ever instantiated (namespace-qualified name lookup for `agentengine::openai::...` is non-dependent,
// resolved at parse time, not instantiation time). This header follows the same convention rather than
// only guarding the two backend-specific specializations below, since every other symbol here
// (`QuickstartSessionBuilder`, `Bundle`) exists only to wire those two backends up.
#ifdef AGENTENGINE_WITH_HTTPS
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"

namespace agentengine::quickstart {

enum class Provider { openai, anthropic };

namespace detail {

// examples/01_hello_agent.cpp's own user_message()/drive<T>() idiom, named once here instead of
// copy-pasted per app -- see the design draft §4.
[[nodiscard]] inline Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

template <Provider P, class Store>
struct primary_client;
template <class Store>
struct primary_client<Provider::openai, Store> {
    using type = agentengine::openai::OpenAIChatClient<Store>;
};
template <class Store>
struct primary_client<Provider::anthropic, Store> {
    using type = agentengine::anthropic::AnthropicChatClient<Store>;
};

// Well-known per-provider defaults, overridable via QuickstartSessionBuilder::endpoint(). Both real
// backend constructors accept the identical (host, port, model, api_key_ref, caps, store, path_prefix)
// positional prefix -- confirmed by reading both (protocol/openai/chat_client.hpp,
// protocol/anthropic/chat_client.hpp) -- which is what lets QuickstartSessionBuilder::build() below
// construct either `Primary` through one uniform call shape.
template <Provider P>
struct default_endpoint;
template <>
struct default_endpoint<Provider::openai> {
    static constexpr char const* host        = "api.openai.com";
    static constexpr std::uint16_t port      = 443;
    static constexpr char const* path_prefix = "/v1";
};
template <>
struct default_endpoint<Provider::anthropic> {
    static constexpr char const* host        = "api.anthropic.com";
    static constexpr std::uint16_t port      = 443;
    static constexpr char const* path_prefix = "/v1";
};

// A Store shape `.api_key_from_env()` can populate on its own: default-constructible, and exposing a
// plain `.set(name, value)` mutator -- neither is part of the real `SecretStore` concept
// (trust/secret.hpp only requires `.resolve()`), so this is deliberately narrower, matching exactly
// what `InMemorySecretStore` provides and this project's real production store (`AgentEngineSecretStore`
// -- no default constructor, no `.set()`) does not.
template <class S>
concept TestOnlyPopulatableSecretStore = std::default_initializable<S> && requires(S s, std::string n,
                                                                                      std::string v) {
    s.set(std::move(n), std::move(v));
};

// §2b's real fix -- see this file's own top comment, finding 8. `AgentSession<...>::history_provider_`
// is a plain, always-default-constructed value member (rt/agent_session.hpp:2059; no user-declared
// `AgentSession` constructor exists, so the compiler-generated default one runs, which default-inits
// EVERY member including this one) -- so whatever occupies `HistoryProviderT` must itself be
// default-constructible, unconditionally, regardless of what the *quickstart* builder does. The
// project's own existing N-way composite, `core/composed_context_provider.hpp`'s `ComposedContextProvider
// <Ms...>`, only clears that bar when EVERY `Ms` is itself default-constructible (its own default
// constructor is `requires`-gated on exactly that) -- fine for `HistoryProvider<Window<n>>` or a mock,
// but `SkillsProvider` (explicit ctor, requires a `vector<SkillSourceDescriptor>`, no default),
// `MemoryProvider`/`VectorRagContextProvider` (both take required reference parameters) are NEVER
// default-constructible, so `ComposedContextProvider<HistoryProvider<...>, SkillsProvider<...>>` cannot
// even be default-constructed, meaning `AgentSession<..., ComposedContextProvider<...>>` cannot compile
// for that combination at all -- confirmed empirically: `tests/test_composed_context_provider.cpp`'s own
// only real `AgentSession` proof (`ThreeWayProvider`) uses three deliberately-default-constructible mock
// providers (`static_assert(std::is_default_constructible_v<ThreeWayProvider>...)`), never a real
// `SkillsProvider`/`MemoryProvider`/`VectorRagContextProvider`. This type closes that gap the same way
// ADR-018 closed the analogous one for `chat_client_`: always default-constructible (an empty
// `contributors_`, not a default-constructed `Ms{}...`), engaged with the REAL, host-constructed `Ms...`
// values after `AgentSession` already exists -- reachable because `AgentSession::history_provider()`
// (rt/agent_session.hpp:647) returns a mutable reference (an accessor `composed_context_provider.hpp`'s
// own comment, written before that accessor existed, still describes as absent -- stale, not relied on
// here). Kept local to this file (`quickstart::detail`), not promoted to `core/`, since nothing outside
// the quickstart builder currently needs a lazily-engaged composite.
template <class... Ms>
    requires (sizeof...(Ms) >= 1) && (agentengine::ContextProvider<Ms> && ...)
class LazyComposedContextProvider {
public:
    static constexpr std::string_view name = "quickstart_composed";  // ae-naming-lint: allow name — ADR-033's HasMiddlewareName precedent, reused verbatim per ADR-066 §3

    LazyComposedContextProvider() = default;  // contributors_ starts empty -- see engage() below

    // Move-only, NOT copyable -- red-team finding 9 (header top comment): `make_context_provider_
    // descriptor()` (context_assembly.hpp) wraps each `Ms` in a `shared_ptr<Ms>` CAPTURED BY VALUE in
    // `contributors_`'s closures, so an implicit memberwise COPY of this type would copy those
    // `shared_ptr`s too -- aliasing the SAME underlying provider instances, not independent ones. The
    // one real place a plain COPY bites: `AgentSession::fork_from()` (rt/agent_session.hpp:1022) does a
    // plain `history_provider_ = source.history_provider_;` -- a fork of a session using this type would
    // silently start sharing live, mutable provider state (a memory-provider write-back, a per-skill
    // usage counter, a RAG cache) with its source, an I1/I4-adjacent session-isolation gap a caller has
    // no reason to expect. Deleting copy here turns that into a COMPILE ERROR at the exact `fork_from()`
    // call site instead -- `fork_from()` is an ordinary (non-template) member function, only compiled
    // when actually called, so this has zero effect on every already-passing build()/engage()/
    // on_context()/on_turn_end() path; it only surfaces where the real problem would have.
    //
    // Round 5 red-team, finding A (header top comment): the FIRST version of this fix left move ctor/
    // assignment `= default`ed -- `contributors_` (a vector) is correctly drained on move, but `engaged_`
    // (a plain `bool`) is trivially copied by the defaulted move, so it stayed `true` on the MOVED-FROM
    // side even though its `contributors_` was now empty. LIVE-REPRODUCED: `session2.history_provider() =
    // std::move(session1.history_provider())` (reachable through the same public `history_provider()`
    // accessor `on_context()`'s own comment below already names as a bypass route) left session1 with
    // `engaged_ == true`, `contributors_` empty -- `on_context()`'s `if (!engaged_)` guard did NOT fire,
    // so session1 silently returned a SUCCESSFUL, empty contribution instead of the `not_engaged` error
    // the guard exists to produce, and `engage()`'s own `already_engaged` guard permanently blocked ever
    // re-engaging it (no recovery). **Fixed**: move ctor/assignment now explicitly reset the moved-from
    // side's `engaged_` to `false` (and defensively clear its `contributors_`, rather than relying on a
    // vector move's typically-but-not-guaranteed-empty post-move state) -- restores the class's own
    // invariant ("`engaged_ == true` iff `contributors_` is populated") across a move, so a moved-from
    // instance correctly fails closed via the EXISTING `not_engaged` guard, and can be `engage()`d again
    // (a real recovery path, not a permanently bricked instance). Declared explicitly (not `= default`)
    // since declaring the copy pair suppresses implicit move generation -- `AgentSession::
    // clear_in_process_state()`'s own `history_provider_ = HistoryProviderT{};` (rt/agent_session.hpp:
    // 1070) needs move-assignment from a prvalue to keep working; that call site is unaffected by this
    // fix (assigning FROM a fresh, never-engaged temporary, so there is nothing for the reset logic to
    // observably change). NOT fixed, deliberately out of scope for this round: a move-assignment into an
    // ALREADY-engaged target still silently replaces its contributors with no diagnostic -- ordinary,
    // expected `operator=` semantics (identical to `history_provider() = HistoryProviderT{}`'s own
    // existing silent-reset behavior, not a new hazard this fix introduces), not the state-invariant
    // violation the `engaged_`/`contributors_` desync above was. The underlying `shared_ptr`-aliasing
    // mechanism itself remains unchanged and pre-existing in `core/composed_context_provider.hpp`'s
    // `ComposedContextProvider<Ms...>` too (not fixed here -- out of this file's scope, named as a real,
    // disclosed residual, not silently left for a caller to rediscover).
    LazyComposedContextProvider(LazyComposedContextProvider&& other) noexcept
        : contributors_(std::move(other.contributors_)), engaged_(other.engaged_) {
        other.engaged_ = false;
        other.contributors_.clear();
    }
    LazyComposedContextProvider& operator=(LazyComposedContextProvider&& other) noexcept {
        if (this != &other) {
            contributors_ = std::move(other.contributors_);
            engaged_      = other.engaged_;
            other.engaged_ = false;
            other.contributors_.clear();
        }
        return *this;
    }
    LazyComposedContextProvider(LazyComposedContextProvider const&) = delete;
    LazyComposedContextProvider& operator=(LazyComposedContextProvider const&) = delete;

    // Host-only, configuration-time call (`ComposedQuickstartSessionBuilder::build()`, never derived
    // from model output, I3) -- builds each `Ms`'s real `ContextProviderDescriptor` and appends it, in
    // `Ms...`'s declared order (005 §3 drop-order-determinism / final wire order, identical rule to
    // `ComposedContextProvider`'s own). May be called AT MOST ONCE per instance -- a second call would
    // silently duplicate every contributor on the wire, so it fails closed instead of accumulating.
    [[nodiscard]] agentengine::result<void> engage(
        std::tuple<Ms...> providers,
        std::array<agentengine::ContextBudget, sizeof...(Ms)> budgets = {}) {
        if (engaged_) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "LazyComposedContextProvider::engage() called twice on the same instance -- every "
                "contributor from the first call would otherwise be duplicated on the wire",
                "quickstart.composed_context.already_engaged"});
        }
        // Round 8 red-team, finding 15: build into a LOCAL vector first, publish into contributors_
        // (and flip engaged_) only once every Ms's descriptor has been constructed without throwing.
        // The previous shape push_back'd directly into contributors_ inside build_contributors()'s
        // fold -- if a later Ms's move constructor (or make_context_provider_descriptor()'s own
        // make_shared) threw partway through, contributors_ was left PARTIALLY populated while
        // engaged_ stayed false (the assignment below was never reached). Since engaged_ == false,
        // the already_engaged guard above does NOT block a retry -- and round 5's own fix comment
        // explicitly frames retry-after-failure as safe recovery -- but reserve() does not clear
        // existing elements, so a retry's build_contributors() APPENDED onto the stale entries
        // instead of starting fresh. LIVE-REPRODUCED: a throwing-provider mock, first engage() throws
        // after 1 of 2 contributors pushed, a retry with fresh providers succeeds, then ONE
        // on_context() call invoked the stale first-attempt contributor a SECOND, duplicate time --
        // exactly the "duplicate every contributor on the wire" hazard already_engaged exists to
        // prevent, reintroduced via the exception path. Building locally and swapping in only on
        // success gives a strong exception guarantee: a throw here now leaves contributors_/engaged_
        // completely untouched, so a retry genuinely starts fresh instead of accumulating wreckage.
        std::vector<agentengine::ContextProviderDescriptor> built;
        built.reserve(sizeof...(Ms));
        build_contributors(built, providers, budgets, std::index_sequence_for<Ms...>{});
        contributors_ = std::move(built);
        engaged_ = true;
        return {};
    }

    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& session_ctx, agentengine::EffectContext& ctx) {
        if (!engaged_) {
            // Not reachable through ComposedQuickstartSessionBuilder::build() (it always calls engage()
            // before returning a Bundle) -- only reachable if a caller reaches in via
            // Bundle::session().history_provider() and substitutes/reuses an unengaged instance.
            // Fail-closed, matching this project's convention for a used-before-configured contract
            // violation, rather than silently contributing nothing.
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "LazyComposedContextProvider::on_context() called before engage()",
                "quickstart.composed_context.not_engaged"});
        }
        agentengine::ContextAssemblyResult assembled =
            co_await agentengine::assemble_context(contributors_, session_ctx, ctx);
        co_return assembled.combined;
    }

    // Matches ComposedContextProvider::on_turn_end's own forwarding -- assemble_context() itself never
    // calls it on its contributors.
    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView turn, agentengine::EffectContext& ctx) {
        for (auto& contributor : contributors_) (void)co_await contributor.on_turn_end(turn, ctx);
        co_return std::monostate{};
    }

private:
    template <std::size_t... I>
    void build_contributors(std::vector<agentengine::ContextProviderDescriptor>& out,
                             std::tuple<Ms...>& providers,
                             std::array<agentengine::ContextBudget, sizeof...(Ms)> const& budgets,
                             std::index_sequence<I...>) {
        (out.push_back(agentengine::make_context_provider_descriptor(
             std::move(std::get<I>(providers)), budgets[I])),
         ...);
    }

    std::vector<agentengine::ContextProviderDescriptor> contributors_;
    bool engaged_ = false;
};

}  // namespace detail

// Move-only. Owns every long-lived object the constructed session's `ChatClientT` and
// `CapabilitySet const*` reference for as long as the session is used -- see this file's own top
// comment for why a plain move-constructed `AgentSession` member cannot work here.
//
// `HistoryProviderT` defaults to `AgentSession`'s own default (`HistoryProvider<Window<0>>`) so
// every existing `Bundle<ChatClientT, Store>` spelling (§2a's `QuickstartSessionBuilder`) is
// completely unaffected -- the third parameter exists only so `ComposedQuickstartSessionBuilder`
// (§2b, below) can supply its own `detail::LazyComposedContextProvider<Ms...>` instead.
template <class ChatClientT, class Store,
          class HistoryProviderT = agentengine::HistoryProvider<agentengine::Window<0>>>
class Bundle {
public:
    using SessionT = agentengine::rt::AgentSession<ChatClientT, agentengine::rt::NoSessionState,
                                                     HistoryProviderT>;

    // unified-streaming-design-draft.md §4 (Piece D). NEW lifetime contract, found during
    // implementation, not named by either red-team pass or the design draft itself: `ask_stream()`'s
    // driver/relay threads capture `this` (a `Bundle*`) for as long as they run, to reach `session_`/
    // `event_stream_` -- unlike `ask()` (purely synchronous, no cross-call lifetime concern), moving a
    // `Bundle` elsewhere WHILE its `ask_stream_driver_`/relay pair is still active leaves that thread
    // holding a dangling `this` once the OLD `Bundle` object is destroyed. A caller must not move a
    // `Bundle` after calling `ask_stream()` on it until that call's returned `stream<std::string>` has
    // reached a terminal state (closed/failed) -- narrow, disclosed, not structurally prevented, the
    // same class of choice `call_stream()`'s own `this`-capture residual makes (`model_call_gateway.hpp`).
    Bundle(Bundle&&) noexcept = default;
    Bundle(Bundle const&)     = delete;
    Bundle& operator=(Bundle const&) = delete;
    // Move-ASSIGNMENT deliberately deleted, not defaulted: the defaulted version would member-wise
    // move-assign in DECLARED order (store_, capabilities_, session_), which destroys the OLD store_/
    // capabilities_ before session_ (still referencing them) is itself reassigned -- the reverse of
    // what the destructor/move-construction path safely relies on (see the declaration-order comment
    // below). Currently nothing in the destructor chain would read through a dangling reference during
    // that window, but nothing enforces that staying true either -- deleting this closes the class of
    // risk outright rather than trying to hand-write a correct reverse-order assignment nothing here
    // actually needs (every real use only move-CONSTRUCTS a Bundle, e.g. from `build()`'s return).
    Bundle& operator=(Bundle&&) = delete;

    [[nodiscard]] SessionT& session() noexcept { return *session_; }
    [[nodiscard]] agentengine::CapabilitySet const& capabilities() const noexcept { return *capabilities_; }

    // Design draft §4 -- the one-shot round-trip sugar. Serializes against itself via `ask_mutex_` and
    // drives with a BOUNDED, single-resume(), fail-closed loop (matching rt/drive_leaf_task.hpp's own
    // shape) rather than the naive `while(!done()) resume()` idiom examples/*.cpp use -- that idiom is
    // only safe when `session_mutex_` (rt::AsyncMutex) is never contended, which a linear, single-
    // threaded example `main()` guarantees by construction but a reusable `Bundle` does not. See this
    // file's own top comment for the full red-team finding this fixes.
    [[nodiscard]] agentengine::result<std::string> ask(std::string text) {
        std::lock_guard<std::mutex> guard(*ask_mutex_);
        auto t =
            session_->start_run(agentengine::rt::StartRun{detail::user_message(std::move(text))});
        t.resume();
        if (!t.done()) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::fatal,
                "Bundle::ask() needed more than one resume() to complete -- the run suspended on "
                "something this synchronous helper will not drive further (most likely a concurrent "
                "caller holding session_mutex_ via the raw session() accessor, since ask() already "
                "serializes against itself). Stopping here rather than resuming again, to avoid a "
                "cross-thread double-resume race.",
                "quickstart_bundle.ask_would_block"});
        }
        auto r = t.take_value();
        if (!r) return std::unexpected(r.error());
        return agentengine::text_of(r->message);
    }

    // unified-streaming-design-draft.md §4 (Piece D), Rev 7. Reuses `ask()`'s OWN bounded-single-
    // `resume()` contract exactly (see that method's own comment for why one `resume()` is always
    // enough for a healthy, uncontended run) -- just moved onto a background thread, because this call
    // will legitimately block for the run's whole real-world duration and the caller wants to consume
    // text live, not because more than one `resume()` is ever needed. Two cooperating threads, not a
    // resume loop (the 4th red-team pass's Finding 3: a resume loop is what reintroduces the exact
    // double-resume hazard `ask()` was built to avoid):
    //   - the OUTER driver (`ask_stream_driver_`, a `std::jthread`) calls `start_run()` then `resume()`
    //     exactly once, matching `ask()`'s own fail-closed check if that alone doesn't finish;
    //   - a nested relay thread, spawned BEFORE `resume()` is called (so no event is ever missed --
    //     `emit_run_event()` pushes onto a real, backpressured channel, `core/stream.hpp:211`'s default
    //     256-item capacity, so nothing is silently dropped even if the relay is a poll or two behind,
    //     but it must already be polling to ever pop the run's own opening `run_started` event), drains
    //     the session's ONE persistent event stream and relays `ModelTextDelta` fragments into the
    //     caller-facing `stream<std::string>`.
    //
    // `event_stream_` is a REAL, new design point the original sketch never named: `enable_event_stream()`
    // (`rt/agent_session.hpp`) is a single-call API (it move-assigns the session's own producer, so a
    // second call would silently orphan any earlier consumer) -- created lazily, ONCE, on the first
    // `ask_stream()` call, and reused by every later one. Confirmed by grep: nothing ever calls
    // `.close()`/`.fail()` on the session's event producer -- it is session-scoped and outlives any
    // single run, so the relay below recognizes ITS OWN run's terminal event and stops there, never
    // relying on the shared event stream's own `.done()` (which never becomes true).
    [[nodiscard]] agentengine::result<agentengine::stream<std::string>> ask_stream(std::string text) {
        std::lock_guard<std::mutex> guard(*ask_mutex_);
        if (!event_stream_.has_value()) {
            event_stream_ = session_->enable_event_stream(std::pmr::get_default_resource());
        }
        // Found while writing the first real (live) test for this method, not by either red-team pass
        // or by hand-tracing: without this, `stream_model_calls_` stays at its default `false`, so
        // `run_model_call()`'s dispatch never reaches `call_stream()` at all (unified-streaming-design-
        // draft.md §3) -- the relay below would then see only `run_started`/the terminal event, never a
        // single `model_delta`, and `ask_stream()` would silently return an empty text stream. Idempotent
        // (a bool set to its own value on a later call is harmless); also flips `.ask()`'s own behavior
        // on this session to the streaming call_stream() path from then on, which is functionally
        // equivalent for `.ask()` (same final result, it just doesn't read the extra live events).
        session_->set_stream_model_calls(true);
        auto pair = agentengine::make_stream<std::string>(std::pmr::get_default_resource());
        // `ask_stream_driver_ = std::jthread(...)`: if a PRIOR driver is still joinable, `std::jthread`'s
        // move-assignment operator requests-stop-and-joins it first (standard-guaranteed) -- since this
        // whole statement runs under `ask_mutex_`, an overlapping `ask_stream()` call genuinely blocks
        // here until the prior call's driver+relay pair has fully finished, the same full-duration
        // serialization `ask()` already gives sequential callers (Finding 9-new, 5th red-team pass: the
        // OBSERVABLE behavior was always meant to be this; the REASONING for why used to be wrong -- see
        // the design draft's own correction).
        ask_stream_driver_ = std::jthread([this, text = std::move(text), producer = std::move(pair.producer)]() mutable {
            std::string current_run_id;
            bool run_id_known = false;
            std::jthread relay([this, &current_run_id, &run_id_known,
                                 &producer](std::stop_token stop_tok) {
                while (!stop_tok.stop_requested()) {
                    while (std::optional<agentengine::RunEvent> ev = event_stream_->next()) {
                        if (!run_id_known) {
                            if (ev->kind == agentengine::run_event_kind::run_started) {
                                current_run_id = ev->run_id;
                                run_id_known   = true;
                            } else {
                                continue;  // a stray event from before this run started -- ignore
                            }
                        }
                        if (ev->run_id != current_run_id) continue;  // not this run's own event
                        if (ev->kind == agentengine::run_event_kind::model_delta) {
                            auto const& d =
                                std::get<agentengine::run_event_payload::ModelDelta>(ev->payload);
                            if (auto const* t = std::get_if<agentengine::run_event_payload::ModelTextDelta>(
                                    &d.value)) {
                                if (producer.push(t->text) != agentengine::stream_push::ok) return;
                            }
                            // ModelToolCallArgumentDelta: skipped -- ask_stream()'s contract stays
                            // text-only, unchanged from the design's own scoping.
                        } else if (ev->kind == agentengine::run_event_kind::run_finished) {
                            producer.close();
                            return;
                        } else if (ev->kind == agentengine::run_event_kind::run_failed ||
                                   ev->kind == agentengine::run_event_kind::run_canceled) {
                            producer.fail(agentengine::error{
                                agentengine::failure_class::fatal,
                                "the run ended without completing successfully",
                                "quickstart_bundle.ask_stream_run_failed"});
                            return;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });

            auto t = session_->start_run(agentengine::rt::StartRun{detail::user_message(std::move(text))});
            t.resume();
            if (!t.done()) {
                producer.fail(agentengine::error{
                    agentengine::failure_class::fatal,
                    "Bundle::ask_stream() needed more than one resume() to complete -- the run "
                    "suspended on something this driver will not drive further (most likely a "
                    "concurrent caller holding session_mutex_ via the raw session() accessor, since "
                    "ask_stream() already serializes against ask()/ask_stream() via ask_mutex_). "
                    "Stopping here rather than resuming again, to avoid a cross-thread double-resume "
                    "race.",
                    "quickstart_bundle.ask_stream_would_block"});
            }
            // `relay`'s own destructor (end of this lambda's scope) requests-stop-and-joins it -- by
            // design it should already have stopped itself above, having observed this run's own
            // terminal event once resume() returned; this is a formality, not a real wait, in the
            // healthy case.
        });
        return std::move(pair.consumer);
    }

private:
    template <Provider, class>
    friend class QuickstartSessionBuilder;
    template <Provider, class, class... Ms>
        requires (sizeof...(Ms) >= 1)
    friend class ComposedQuickstartSessionBuilder;

    Bundle(std::unique_ptr<Store> store, std::unique_ptr<agentengine::CapabilitySet> capabilities,
           std::unique_ptr<SessionT> session)
        : store_(std::move(store)), capabilities_(std::move(capabilities)), session_(std::move(session)) {}

    // Declaration order matters: members destroy in REVERSE declared order, so `session_` (which holds
    // a `ChatClientT` referencing `*store_` and a `CapabilitySet const*` aliasing `*capabilities_`)
    // must be declared LAST, so it is destroyed FIRST -- never left referencing an already-destroyed
    // Store/CapabilitySet, even for the instant between them during ~Bundle(). `ask_mutex_` is heap-
    // owned (not a plain `std::mutex` member) purely so `Bundle` stays move-constructible -- `std::
    // mutex` itself is neither movable nor copyable, and a plain member here would make EVERY member
    // below it non-movable too, breaking the whole point of this type.
    std::unique_ptr<Store> store_;
    std::unique_ptr<agentengine::CapabilitySet> capabilities_;
    std::unique_ptr<SessionT> session_;
    std::unique_ptr<std::mutex> ask_mutex_ = std::make_unique<std::mutex>();
    // unified-streaming-design-draft.md §4 (Piece D). CORRECTED during implementation (not as the design
    // draft's own prose originally stated): `ask_stream_driver_` must be declared AFTER `event_stream_`,
    // not before -- members destroy in REVERSE declared order (see this comment's own opening sentence),
    // so declaring it last means it is destroyed FIRST, which is what stops/joins the driver+relay
    // threads BEFORE `event_stream_` (the stream they read from) is torn down. The reverse order would
    // destroy `event_stream_` while a thread might still be reading it.
    std::optional<agentengine::stream<agentengine::RunEvent>> event_stream_;
    std::jthread ask_stream_driver_;

    static_assert(std::is_same_v<decltype(store_), std::unique_ptr<Store>>,
                  "store_ must stay heap-owned via unique_ptr -- a plain value member here would "
                  "silently reintroduce the dangling-reference hazard this file's own top comment (§0 "
                  "of the design draft) exists to close");
    static_assert(std::is_same_v<decltype(capabilities_), std::unique_ptr<agentengine::CapabilitySet>>,
                  "capabilities_ must stay heap-owned via unique_ptr -- same reasoning as store_ above");
};

// Design draft §2a/§2c/§3/§4, scoped as this file's own top comment names. `Provider` is a compile-
// time (template, not fluent-runtime) choice -- deliberately, since `AgentSession<ChatClientT>` is
// already templated on backend choice and a runtime toggle between two different C++ types has no
// clean single `build()` return type; this builder does not fight that constraint, it shortens the
// ceremony around it.
template <Provider P, class Store = agentengine::InMemorySecretStore>
class QuickstartSessionBuilder {
public:
    using Primary     = typename detail::primary_client<P, Store>::type;
    using ChatClientT = agentengine::ModelCallGateway<Primary>;
    using BundleT     = Bundle<ChatClientT, Store>;

    explicit QuickstartSessionBuilder(std::string model) : model_(std::move(model)) {}

    QuickstartSessionBuilder& session_id(std::string id) {
        session_id_ = std::move(id);
        return *this;
    }
    QuickstartSessionBuilder& principal(agentengine::Principal p) {
        principal_ = std::move(p);
        return *this;
    }
    QuickstartSessionBuilder& declare_capabilities(agentengine::ChatClientCapabilities caps) {
        caps_ = caps;
        return *this;
    }
    QuickstartSessionBuilder& endpoint(std::string host, std::uint16_t port, std::string path_prefix) {
        host_        = std::move(host);
        port_        = port;
        path_prefix_ = std::move(path_prefix);
        return *this;
    }

    // Red-team finding 7 (header top comment) -- `AgentSession::initialize()`'s raw default is
    // `std::nullopt` (genuinely unbounded, `agent_session.hpp`'s own `run_rounds()` loop condition:
    // `!max_turns_.has_value() || turn_index < *max_turns_`). This builder deliberately does NOT mirror
    // that default -- see `max_turns_`'s own member default below for why -- so these two setters exist
    // to opt OUT of the safe default, not just to set a value the raw API already defaults to.
    // `std::nullopt` is a legitimate, explicit choice here (a host who has their own external timeout/
    // cancellation layer and genuinely wants unbounded turns), not a value that shouldn't be reachable.
    QuickstartSessionBuilder& max_turns(std::optional<std::uint64_t> n) {
        max_turns_ = n;
        return *this;
    }
    QuickstartSessionBuilder& token_budget(std::optional<std::uint64_t> n) {
        token_budget_ = n;
        return *this;
    }

    // Design draft §2c, generalized -- declares which `SecretRef` the constructed `ChatClient` will
    // resolve against, and auto-grants a matching `cap::Secret` (I2 -- without this, the constructed
    // session would reference a ref nothing authorizes it to resolve). Independent of `.store()` below
    // -- this only names the ref, it never touches Store content, so it works identically regardless of
    // what kind of `SecretStore` `.store()` ends up supplying.
    QuickstartSessionBuilder& api_key(agentengine::SecretRef ref) {
        api_key_ref_ = ref;
        // Overwritten, not appended -- last-call-wins. Calling this twice (e.g. to correct a typo'd
        // name) must never leave a phantom grant for the FIRST ref behind in the final CapabilitySet;
        // this file's own top comment names the finding this fixed.
        primary_secret_grant_ =
            agentengine::Capability{agentengine::cap::Secret{ref.name, std::chrono::seconds{0}}};
        return *this;
    }

    // The PRODUCTION path: construct a `Store` of any real `SecretStore` conformer IN PLACE, forwarding
    // whatever constructor arguments that type needs -- matching `AgentSession::emplace_chat_client`'s
    // own established idiom in this exact codebase (rt/agent_session.hpp), for the identical reason.
    // Deliberately NOT `store(Store store)` taking a value: a prior version did, and a red-team pass
    // found and compiler-confirmed a real counter-example -- `trust/secret_quarantine.hpp`'s
    // `QuarantineSecretStore` (a real, already-shipped SecretStore conformer, ADR-068) holds a
    // `std::mutex` directly, making it neither movable nor copyable; a by-value `store(Store)` could
    // not even be CALLED with one. Emplacing sidesteps the whole question -- it never needs `Store` to
    // be movable OR copyable, only constructible from the given arguments, so it is genuinely generic
    // over every real `SecretStore` conformer in this codebase, not just the movable ones. The host
    // remains responsible for whatever the constructor arguments themselves require (e.g. constructing
    // a `unique_ptr<SecretSource>` for `AgentEngineSecretStore`); this builder only takes ownership of
    // the resulting object from here (heap-owned) -- see this file's own top comment for why `Bundle`
    // needs that stability.
    template <class... Args>
    QuickstartSessionBuilder& store(Args&&... args) {
        store_ = std::make_unique<Store>(std::forward<Args>(args)...);
        return *this;
    }

    // TEST-ONLY convenience sugar, `requires`-gated: only exists at all (hard compile error otherwise,
    // never a silent wrong assumption) when `Store` is default-constructible and exposes `.set(name,
    // value)` -- i.e., genuinely `InMemorySecretStore`-shaped. Emplaces a default-constructed `Store`
    // via `.store()` above, then mutates it in place through the `unique_ptr` -- never requires `Store`
    // to be movable either, same reasoning as `.store()`'s own comment. With `Store` left at its
    // default (`InMemorySecretStore`), the value this reads sits in-memory, in plaintext, never
    // zeroized -- that type's OWN header comment (trust/secret.hpp) labels it "Test-only... Production
    // code constructs AgentEngineSecretStore... never this." For a real deployment, call `.api_key(...)`
    // + `.store(...)` directly with a real `AgentEngineSecretStore` instead.
    QuickstartSessionBuilder& api_key_from_env(std::string secret_name, char const* env_var)
        requires detail::TestOnlyPopulatableSecretStore<Store>
    {
        api_key(agentengine::SecretRef{secret_name});
        if (auto value = agentengine::pal::env_var(env_var); value.has_value()) {
            store();
            store_->set(secret_name, std::move(*value));
        }
        return *this;
    }

    // Escape hatch for anything else the constructed session needs authorized (FsRead, NativeExec,
    // ...) -- host-authored only; nothing on this builder's surface accepts a `Tainted<T>` or anything
    // derived from a `ChatResponse`/tool result (I3).
    QuickstartSessionBuilder& grant(agentengine::Capability cap) {
        grants_.push_back(std::move(cap));
        return *this;
    }

    // Design draft §2d, corrected during implementation from its original sketch (named
    // `.require_approval_for(...)`, which read backwards against the real mechanism -- see this file's
    // own top comment). `ApprovalDecider` (core/tool_pipeline.hpp) is consulted ONLY for a call that
    // ALREADY needs a decision (the tool itself declared `approval_mode::always_require`, or
    // `policy_driven` with no `PolicyDecider` installed) -- it does not, and cannot, make MORE tools
    // require approval than their own declared `approval_mode` already does (I2: narrows or decides
    // among already-possessed authority only, never widens it). `.approve_tools(names)` installs a
    // decider that auto-approves ONLY calls whose tool name is in `names`; every other already-gated
    // call is denied (`false`), the SAFE default -- this can only ever turn an already-required human
    // decision into an immediate deny or an immediate host-declared approve, never skip a decision that
    // was never required in the first place, and never turn a decision INTO an approval for a tool not
    // explicitly named. If never called, no `ApprovalDecider` is installed at all -- `AgentSession`'s
    // own true default (unset decider, ADR-070-Judged fail-closed `always_require` wins) is preserved
    // byte-for-byte, not merely approximated by an always-false decider.
    QuickstartSessionBuilder& approve_tools(std::vector<std::string> tool_names) {
        approved_tool_names_ = std::move(tool_names);
        return *this;
    }

    // Thin pass-through to `AgentSession::set_policy_decider` (ADR-070) -- host-authored graduated
    // resolution for `policy_driven` tools, unset (`nullptr`) by default, identical shape to the raw
    // API. No new default behavior; this only shortens the syntax for installing one.
    QuickstartSessionBuilder& policy(agentengine::PolicyDecider decide) {
        policy_decider_ = std::move(decide);
        return *this;
    }

    // Fails closed -- a `result<BundleT>` error, not a thrown exception or a silent partial build --
    // when `.api_key(...)`/`.api_key_from_env(...)` was never called, or when no `Store` ever reached
    // this builder (either `.store(...)` was never called, or `.api_key_from_env(...)`'s named
    // environment variable was unset at build time). MOVES `store_` out (see this file's own top
    // comment on why, and on the resulting single-use behavior change).
    [[nodiscard]] agentengine::result<BundleT> build() {
        if (!api_key_ref_.has_value()) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "no primary backend credential named -- call .api_key(...) (or .api_key_from_env(...), "
                "if Store supports it) before build()",
                "quickstart_builder.no_credential"});
        }
        if (!store_) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "no Store supplied -- call .store(...) with an already-populated store (or "
                ".api_key_from_env(...) with the named environment variable actually set) before "
                "build()",
                "quickstart_builder.no_store"});
        }

        // primary_secret_grant_ is guaranteed set here: it and api_key_ref_ are only ever written
        // together, in api_key(), and the has_value() check above already confirmed api_key_ref_.
        std::vector<agentengine::Capability> all_grants = grants_;
        all_grants.push_back(*primary_secret_grant_);
        auto capabilities = std::make_unique<agentengine::CapabilitySet>(
            agentengine::CapabilitySet::grant_root(std::move(all_grants)));

        Primary primary(host_, port_, model_, *api_key_ref_, caps_, *store_, path_prefix_);

        auto session = std::make_unique<typename BundleT::SessionT>();
        session->initialize(session_id_, principal_, token_budget_, max_turns_);
        // Constructs ModelCallGateway<Primary> IN PLACE from (Primary&&, std::tuple<>&&) -- §2a's "one
        // rung above bare" default (retry+circuit-breaker, empty fallback chain, default RetryPolicy/
        // BreakerConfig) -- never a move of an already-built ChatClientT.
        session->emplace_chat_client(std::move(primary), std::tuple<>{});
        session->set_capabilities(capabilities.get());

        // §2d: only touched if the host actually called .approve_tools()/.policy() -- an untouched
        // session keeps AgentSession's own true unset-decider default (see .approve_tools()'s own
        // comment for why this must be a conditional install, not an always-installed no-op decider).
        if (approved_tool_names_.has_value()) {
            std::vector<std::string> const allow = *approved_tool_names_;
            session->set_approval_decider(
                [allow](agentengine::Principal const&, std::string_view tool_name,
                        std::string const&) {
                    return std::find(allow.begin(), allow.end(), tool_name) != allow.end();
                });
        }
        if (policy_decider_) {
            session->set_policy_decider(policy_decider_);
        }

        return BundleT(std::move(store_), std::move(capabilities), std::move(session));
    }

private:
    std::string model_;
    std::string session_id_ = "s-quickstart";
    agentengine::Principal principal_{"p-quickstart", ""};
    agentengine::ChatClientCapabilities caps_{};
    std::optional<agentengine::SecretRef> api_key_ref_;
    std::optional<agentengine::Capability> primary_secret_grant_;
    std::unique_ptr<Store> store_;
    std::vector<agentengine::Capability> grants_;  // explicit .grant() calls only -- see api_key()
    std::optional<std::vector<std::string>> approved_tool_names_;
    agentengine::PolicyDecider policy_decider_;
    // Red-team finding 7 (header top comment): deliberately DIVERGES from `AgentSession::initialize()`'s
    // own raw default (`std::nullopt`, genuinely unbounded) -- a live-reproduced hang was found where a
    // model that keeps retrying an `.approve_tools()`-denied call never terminates the round, and
    // `Bundle::ask()`'s "bounded resume()" guard (§0b finding 1) does NOT catch this class of hang
    // (every `chat()` call here is synchronous, so the WHOLE unbounded turn loop runs inside that one
    // guarded `resume()`, invisible to it). 25 is an arbitrary but reasonable safety net for a
    // "quickstart" surface whose whole purpose is a safe, working default -- `.max_turns(std::nullopt)`
    // opts back into genuinely unbounded, an explicit, informed host choice, not the silent default.
    std::optional<std::uint64_t> max_turns_ = std::uint64_t{25};
    std::optional<std::uint64_t> token_budget_;
    std::string host_        = detail::default_endpoint<P>::host;
    std::uint16_t port_      = detail::default_endpoint<P>::port;
    std::string path_prefix_ = detail::default_endpoint<P>::path_prefix;
};

using OpenAiSessionBuilder    = QuickstartSessionBuilder<Provider::openai>;
using AnthropicSessionBuilder = QuickstartSessionBuilder<Provider::anthropic>;

// §2b -- history/context composition. `Ms...` is a compile-time pack chosen ONCE, at this builder's
// own declaration, the same reason `Provider` is (§2a's own top-of-class comment): a runtime toggle
// between different C++ types has no single, clean `build()` return type, and unlike `Provider` (one
// value, chosen once) this slot is naturally multi-valued -- but still fixed in SHAPE (which provider
// TYPES compose, and in what order) the moment a host writes the type out, same as `Provider`. What
// varies at runtime is each provider's own VALUE (a `SkillsProvider` needs real sources, a
// `MemoryProvider` needs a real store reference) -- supplied via `.providers(tuple, budgets)` below,
// not via one setter per provider type (there is no generic way to name "the SkillsProvider slot" in
// an arbitrary `Ms...` pack without either a `requires`-constrained lookup-by-type or exposing index
// positions -- both real options, neither attempted here; a single aggregate call is simpler and the
// underlying providers' own constructors are already the ergonomic surface, e.g. `SkillsProvider{
// sources}` directly in the tuple).
//
// Deliberately duplicates QuickstartSessionBuilder's session_id()/principal()/declare_capabilities()/
// endpoint()/max_turns()/token_budget()/api_key()/store()/api_key_from_env()/grant()/approve_tools()/
// policy() rather than sharing them through a common base -- a real, named simplification, not an
// oversight: extracting a CRTP mixin would mean touching the already-shipped, already-red-teamed §2a
// class too, a larger, separate-risk refactor. Named here as a candidate follow-up if red-team finds
// the duplication has drifted, not attempted in this pass.
//
// Red-team finding 10 (header top comment): `requires (sizeof...(Ms) >= 1)` declared HERE too, not
// left to surface only once `HistoryProviderT` (below) instantiates `LazyComposedContextProvider<>`'s
// own identical constraint -- an empty `Ms...` pack used to fail with a multi-error cascade (an
// unspecialized-class-template error, then unrelated failures deep inside <expected>/<type_traits>),
// not one clean, actionable diagnostic. This constraint is checked on the OUTER template first now,
// so the compiler rejects it before ever reaching the inner type's own instantiation.
template <Provider P, class Store, class... Ms>
    requires (sizeof...(Ms) >= 1)
class ComposedQuickstartSessionBuilder {
public:
    using Primary          = typename detail::primary_client<P, Store>::type;
    using ChatClientT       = agentengine::ModelCallGateway<Primary>;
    using HistoryProviderT = detail::LazyComposedContextProvider<Ms...>;
    using BundleT           = Bundle<ChatClientT, Store, HistoryProviderT>;

    explicit ComposedQuickstartSessionBuilder(std::string model) : model_(std::move(model)) {}

    ComposedQuickstartSessionBuilder& session_id(std::string id) {
        session_id_ = std::move(id);
        return *this;
    }
    ComposedQuickstartSessionBuilder& principal(agentengine::Principal p) {
        principal_ = std::move(p);
        return *this;
    }
    ComposedQuickstartSessionBuilder& declare_capabilities(agentengine::ChatClientCapabilities caps) {
        caps_ = caps;
        return *this;
    }
    ComposedQuickstartSessionBuilder& endpoint(std::string host, std::uint16_t port,
                                                 std::string path_prefix) {
        host_        = std::move(host);
        port_        = port;
        path_prefix_ = std::move(path_prefix);
        return *this;
    }
    ComposedQuickstartSessionBuilder& max_turns(std::optional<std::uint64_t> n) {
        max_turns_ = n;
        return *this;
    }
    ComposedQuickstartSessionBuilder& token_budget(std::optional<std::uint64_t> n) {
        token_budget_ = n;
        return *this;
    }
    ComposedQuickstartSessionBuilder& api_key(agentengine::SecretRef ref) {
        api_key_ref_ = ref;
        primary_secret_grant_ =
            agentengine::Capability{agentengine::cap::Secret{ref.name, std::chrono::seconds{0}}};
        return *this;
    }
    template <class... Args>
    ComposedQuickstartSessionBuilder& store(Args&&... args) {
        store_ = std::make_unique<Store>(std::forward<Args>(args)...);
        return *this;
    }
    ComposedQuickstartSessionBuilder& api_key_from_env(std::string secret_name, char const* env_var)
        requires detail::TestOnlyPopulatableSecretStore<Store>
    {
        api_key(agentengine::SecretRef{secret_name});
        if (auto value = agentengine::pal::env_var(env_var); value.has_value()) {
            store();
            store_->set(secret_name, std::move(*value));
        }
        return *this;
    }
    ComposedQuickstartSessionBuilder& grant(agentengine::Capability cap) {
        grants_.push_back(std::move(cap));
        return *this;
    }
    ComposedQuickstartSessionBuilder& approve_tools(std::vector<std::string> tool_names) {
        approved_tool_names_ = std::move(tool_names);
        return *this;
    }
    ComposedQuickstartSessionBuilder& policy(agentengine::PolicyDecider decide) {
        policy_decider_ = std::move(decide);
        return *this;
    }

    // The REAL, host-constructed provider values -- e.g. `.providers(std::make_tuple(
    // HistoryProvider<Window<8>>{}, SkillsProvider{sources}))`. Declared order == wire order (same
    // rule `ComposedContextProvider`/`LazyComposedContextProvider` themselves already enforce).
    // Last-call-wins if called more than once before build() (matching `.api_key()`/`.store()`'s own
    // semantics) -- `Ms...` is fixed at declaration time, so a second call has nothing incremental to
    // add, only a full replacement.
    ComposedQuickstartSessionBuilder& providers(
        std::tuple<Ms...> providers,
        std::array<agentengine::ContextBudget, sizeof...(Ms)> budgets = {}) {
        providers_ = std::move(providers);
        budgets_   = budgets;
        return *this;
    }

    // Fails closed exactly like §2a's build(): missing credential, missing Store, AND (new here)
    // missing `.providers(...)` all produce a `result<BundleT>` error, never a partially-wired session
    // silently missing its whole history/context slot.
    [[nodiscard]] agentengine::result<BundleT> build() {
        if (!api_key_ref_.has_value()) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "no primary backend credential named -- call .api_key(...) (or .api_key_from_env(...), "
                "if Store supports it) before build()",
                "quickstart_builder.no_credential"});
        }
        if (!store_) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "no Store supplied -- call .store(...) with an already-populated store (or "
                ".api_key_from_env(...) with the named environment variable actually set) before "
                "build()",
                "quickstart_builder.no_store"});
        }
        if (!providers_.has_value()) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "no history/context providers supplied -- call .providers(...) with a real value for "
                "every Ms... this builder was declared with before build()",
                "quickstart_builder.no_providers"});
        }

        std::vector<agentengine::Capability> all_grants = grants_;
        all_grants.push_back(*primary_secret_grant_);
        auto capabilities = std::make_unique<agentengine::CapabilitySet>(
            agentengine::CapabilitySet::grant_root(std::move(all_grants)));

        Primary primary(host_, port_, model_, *api_key_ref_, caps_, *store_, path_prefix_);

        auto session = std::make_unique<typename BundleT::SessionT>();
        session->initialize(session_id_, principal_, token_budget_, max_turns_);
        session->emplace_chat_client(std::move(primary), std::tuple<>{});
        session->set_capabilities(capabilities.get());

        // Move `providers_` out and clear the optional -- same single-use signal `store_` going null
        // gives the base builder's own double-build() check above, applied here since `providers_`
        // (unlike `store_`) is not itself a pointer that goes null on move.
        auto providers = std::move(*providers_);
        providers_.reset();
        auto engaged = session->history_provider().engage(std::move(providers), budgets_);
        if (!engaged) return std::unexpected(engaged.error());

        if (approved_tool_names_.has_value()) {
            std::vector<std::string> const allow = *approved_tool_names_;
            session->set_approval_decider(
                [allow](agentengine::Principal const&, std::string_view tool_name,
                        std::string const&) {
                    return std::find(allow.begin(), allow.end(), tool_name) != allow.end();
                });
        }
        if (policy_decider_) {
            session->set_policy_decider(policy_decider_);
        }

        return BundleT(std::move(store_), std::move(capabilities), std::move(session));
    }

private:
    std::string model_;
    std::string session_id_ = "s-quickstart";
    agentengine::Principal principal_{"p-quickstart", ""};
    agentengine::ChatClientCapabilities caps_{};
    std::optional<agentengine::SecretRef> api_key_ref_;
    std::optional<agentengine::Capability> primary_secret_grant_;
    std::unique_ptr<Store> store_;
    std::vector<agentengine::Capability> grants_;
    std::optional<std::vector<std::string>> approved_tool_names_;
    agentengine::PolicyDecider policy_decider_;
    std::optional<std::uint64_t> max_turns_ = std::uint64_t{25};
    std::optional<std::uint64_t> token_budget_;
    std::string host_        = detail::default_endpoint<P>::host;
    std::uint16_t port_      = detail::default_endpoint<P>::port;
    std::string path_prefix_ = detail::default_endpoint<P>::path_prefix;
    std::optional<std::tuple<Ms...>> providers_;
    std::array<agentengine::ContextBudget, sizeof...(Ms)> budgets_{};
};

}  // namespace agentengine::quickstart

#endif  // AGENTENGINE_WITH_HTTPS
