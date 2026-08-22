#pragma once
// Prototype implementation of docs/planning/quickstart-session-builder-design-draft.md -- covers
// §2a (client stack, gateway-wrapped by default)/§2c (capability+secret sugar, generalized past the
// draft's own original sketch -- see finding 3 below)/§3 (the Store-lifetime finding)/§4 (`.ask()`)
// ONLY. Explicitly NOT implemented here, named rather than silently dropped: §2b (history/context
// composition), §2d (approval/policy sugar), `.with_fallback()`/`.with_middleware()`/
// `.with_content_replay()`, and the draft's own `.raw_client_only()` escape hatch. Not an ADR.
// Red-teamed once against this real code (findings 1-2 below); finding 3 is a same-session follow-up
// fix, not itself independently red-teamed yet.
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
//    secret store, full stop -- not merely a hygiene warning, a hard capability gap. Fixed: `.store(
//    Store)` now accepts an ALREADY-CONSTRUCTED, host-owned `Store` of any shape (works for
//    `AgentEngineSecretStore` over `EnvSecretSource`/`FileSecretSource`, proven in the test against the
//    real types, not a stand-in), and `.api_key(SecretRef)` separately declares which ref the
//    `ChatClient` resolves against + auto-grants its `cap::Secret` -- independent of how the Store gets
//    populated. `.api_key_from_env()` still exists as TEST-ONLY convenience sugar, now `requires`-
//    gated (a hard compile error, not a silent wrong assumption) to only exist when `Store` is
//    default-constructible and exposes `.set()` -- i.e., genuinely `InMemorySecretStore`-shaped --
//    calling `.api_key()`+`.store()` under the hood. Behavior change, disclosed rather than silently
//    reintroduced: `build()` now MOVES `store_` out of the builder (needed to stay generic over
//    non-copyable stores like `AgentEngineSecretStore`, which owns a `unique_ptr<SecretSource>`) --
//    calling `build()` a second time on the same builder instance now fails closed with
//    `quickstart_builder.no_store` instead of the prior red-team's verified "produces two independent
//    Bundles" non-finding. Not UB, not silently wrong -- just single-use now, named here for the record.

#include <concepts>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/model_call_gateway.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
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

    // The PRODUCTION path: hand over an already-constructed, already-populated `Store` of any real
    // `SecretStore` conformer -- `AgentEngineSecretStore` over `EnvSecretSource`/`FileSecretSource`
    // included, proven against those real types in the test for this file, not a stand-in. The host
    // remains responsible for constructing and populating it however fits their deployment; this
    // builder only takes ownership from here (heap-owned, moved in) -- see this file's own top comment
    // for why `Bundle` needs that stability.
    QuickstartSessionBuilder& store(Store store) {
        store_ = std::make_unique<Store>(std::move(store));
        return *this;
    }

    // TEST-ONLY convenience sugar, `requires`-gated: only exists at all (hard compile error otherwise,
    // never a silent wrong assumption) when `Store` is default-constructible and exposes `.set(name,
    // value)` -- i.e., genuinely `InMemorySecretStore`-shaped. Calls `.api_key()` + `.store()` under the
    // hood, so it composes with either. With `Store` left at its default (`InMemorySecretStore`), the
    // value this reads sits in-memory, in plaintext, never zeroized -- that type's OWN header comment
    // (trust/secret.hpp) labels it "Test-only... Production code constructs AgentEngineSecretStore...
    // never this." For a real deployment, call `.api_key(...)` + `.store(...)` directly with a real
    // `AgentEngineSecretStore` instead.
    QuickstartSessionBuilder& api_key_from_env(std::string secret_name, char const* env_var)
        requires detail::TestOnlyPopulatableSecretStore<Store>
    {
        api_key(agentengine::SecretRef{secret_name});
        if (auto value = agentengine::pal::env_var(env_var); value.has_value()) {
            Store s{};
            s.set(secret_name, std::move(*value));
            store(std::move(s));
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
        session->initialize(session_id_, principal_);
        // Constructs ModelCallGateway<Primary> IN PLACE from (Primary&&, std::tuple<>&&) -- §2a's "one
        // rung above bare" default (retry+circuit-breaker, empty fallback chain, default RetryPolicy/
        // BreakerConfig) -- never a move of an already-built ChatClientT.
        session->emplace_chat_client(std::move(primary), std::tuple<>{});
        session->set_capabilities(capabilities.get());

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
    std::string host_        = detail::default_endpoint<P>::host;
    std::uint16_t port_      = detail::default_endpoint<P>::port;
    std::string path_prefix_ = detail::default_endpoint<P>::path_prefix;
};

using OpenAiSessionBuilder    = QuickstartSessionBuilder<Provider::openai>;
using AnthropicSessionBuilder = QuickstartSessionBuilder<Provider::anthropic>;

}  // namespace agentengine::quickstart

#endif  // AGENTENGINE_WITH_HTTPS
