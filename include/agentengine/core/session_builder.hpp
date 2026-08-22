#pragma once
// QuickstartSessionBuilder -- a convenience facade over AgentSession's wiring, promoted from
// prototype to a supported feature (2026-08-22) after three red-team rounds against this real code,
// all findings closed. Implements docs/planning/quickstart-session-builder-design-draft.md's §2a
// (client stack, gateway-wrapped by default)/§2c (capability+secret sugar, generalized past the
// draft's own original sketch -- see finding 3 below)/§2d (approval/policy sugar, corrected from its
// original sketch -- see finding 5 below)/§3 (the Store-lifetime finding)/§4 (`.ask()`) ONLY.
// Explicitly NOT implemented here, named rather than silently dropped: §2b (history/context
// composition -- a real structural gap, not just unstarted work; see finding 6 below for why),
// `.with_fallback()`/`.with_middleware()`/`.with_content_replay()`, and the draft's own
// `.raw_client_only()` escape hatch. A convenience layer over already-Reviewed RFCs (002, 004, 005,
// 006, 007, 018), not itself a new invariant or capability shape, so it has no ADR of its own --
// round 1 found findings 1-2; round 2, specifically against finding 3's own fix, found finding 3's own
// `.store(Store)` shape was itself wrong (finding 4); round 3, specifically against finding 5's
// `.approve_tools()`/`.policy()` landing, found and LIVE-REPRODUCED a real hang (finding 7) plus a
// documentation overclaim. Round 3's own fix (finding 7) has not itself been independently red-teamed
// a fourth time -- disclosed here as the one open item, not silently treated as closed.
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
// 6. §2b (history/context composition) is explicitly NOT attempted in this pass, for a real structural
//    reason rather than lack of time: `AgentSession<ChatClientT, StateT, HistoryProviderT>`'s
//    `HistoryProviderT` is a COMPILE-TIME type parameter, exactly like `ChatClientT` (§2a) -- adding
//    `.with_skills(...)` would need to change `Bundle`'s/`QuickstartSessionBuilder`'s own C++ type
//    partway through a fluent chain, the same "no clean single return type for a runtime toggle between
//    two different types" problem §2a's own top-of-class comment already named for `Provider`. Unlike
//    `Provider` (one value, chosen once, at construction), history/context composition is naturally
//    MULTI-VALUED and incremental (`.with_skills()`, later maybe `.with_memory()`, `.with_rag()`, any
//    subset, in any combination) -- a single extra template parameter does not scale the way `Provider`
//    did. Doing this properly needs either a real type-changing fluent builder (every setter
//    `&&`-qualified, returning a new specialization -- a bigger refactor of this whole file, touching
//    `.api_key()`/`.store()`/`.grant()`/`.approve_tools()` too, not just the new methods) or a distinct,
//    separately-templated builder type for the composed-context case. Left undesigned here rather than
//    rushed -- a real follow-up, not a placeholder.
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

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "agentengine/core/chat_client.hpp"
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

}  // namespace detail

// Move-only. Owns every long-lived object the constructed session's `ChatClientT` and
// `CapabilitySet const*` reference for as long as the session is used -- see this file's own top
// comment for why a plain move-constructed `AgentSession` member cannot work here.
template <class ChatClientT, class Store>
class Bundle {
public:
    using SessionT = agentengine::rt::AgentSession<ChatClientT>;

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

private:
    template <Provider, class>
    friend class QuickstartSessionBuilder;

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

}  // namespace agentengine::quickstart

#endif  // AGENTENGINE_WITH_HTTPS
