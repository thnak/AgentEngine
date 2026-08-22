#pragma once
// Prototype implementation of docs/planning/quickstart-session-builder-design-draft.md -- covers
// §2a (client stack, gateway-wrapped by default)/§2c (capability+secret sugar)/§3 (the Store-lifetime
// finding)/§4 (`.ask()`) ONLY. Explicitly NOT implemented here, named rather than silently dropped:
// §2b (history/context composition), §2d (approval/policy sugar), `.with_fallback()`/
// `.with_middleware()`/`.with_content_replay()`, and the draft's own `.raw_client_only()` escape
// hatch. Not an ADR. Self-red-team not yet run against this real code (the draft's own self-red-team
// was against the SKETCH) -- see the design draft's §7 for why that gap is acceptable at this stage.
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
// NOT fixed here, named as a real, disclosed gap rather than glossed over: `build()` requires `Store`
// to be default-constructible and expose `.set(name, value)` -- properties `InMemorySecretStore`
// (the default `Store=` and the ONLY type this file's own two call sites actually exercise) has, but
// which are neither part of the `SecretStore` concept nor present on this project's own real
// production store, `AgentEngineSecretStore` (trust/secret.hpp -- no default constructor, no `.set()`).
// `InMemorySecretStore` is that same file's own designated **test-only** backend ("Production code
// constructs `AgentEngineSecretStore`... never this" -- its own header comment); its values are a
// plain, never-zeroized `std::unordered_map`, a direct contradiction of 018 §4's secret-hygiene
// invariant the rest of `trust/secret.hpp` goes out of its way to uphold. A host following this
// facade's DEFAULT, documented path (`OpenAiSessionBuilder`/`AnthropicSessionBuilder`,
// `.api_key_from_env()`) therefore currently wires a real credential into this project's own
// test-only, non-hygienic store, with nothing here warning them at the API surface. This is a real
// residual, not yet closed -- do not present this facade as production-ready until it is (either by
// making `build()` work generically against any `SecretStore` conformer regardless of shape, or by
// requiring a non-default `Store=` explicitly and refusing to compile against `InMemorySecretStore`
// without an opt-in marker).

#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
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

}  // namespace detail

// Move-only. Owns every long-lived object the constructed session's `ChatClientT` and
// `CapabilitySet const*` reference for as long as the session is used -- see this file's own top
// comment for why a plain move-constructed `AgentSession` member cannot work here.
template <class ChatClientT, class Store>
class Bundle {
public:
    using SessionT = agentengine::rt::AgentSession<ChatClientT>;

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

    // Design draft §2c -- grants BOTH halves a secret needs to actually resolve at call time: the
    // Store's own value (`Store::set`) AND a matching `cap::Secret` grant (I2 -- without this second
    // half, the constructed session would hold a value nothing authorizes it to read). This is the
    // ONLY way this builder ever populates a secret value; `.grant()` below is for anything else, or a
    // hand-built `cap::Secret` with a non-default ttl.
    //
    // TEST-ONLY DEFAULT, disclosed rather than silent: with `Store` left at its default
    // (`agentengine::InMemorySecretStore`), the value this reads is stored in-memory, in plaintext,
    // never zeroized -- that type's OWN header comment (trust/secret.hpp) labels it "Test-only...
    // Production code constructs AgentEngineSecretStore... never this." `build()` also requires
    // whatever `Store` IS used to be default-constructible and expose `.set(name, value)` -- properties
    // `AgentEngineSecretStore` (this project's real production store) does not have. Do not wire a real
    // production credential through this method against the default `Store` -- see this file's own top
    // comment for the full, still-open finding.
    QuickstartSessionBuilder& api_key_from_env(std::string secret_name, char const* env_var) {
        if (auto value = agentengine::pal::env_var(env_var); value.has_value()) {
            pending_secret_values_[secret_name] = std::move(*value);
        }
        api_key_ref_ = agentengine::SecretRef{secret_name};
        // Overwritten, not appended -- last-call-wins, matching api_key_ref_ above. Calling this twice
        // (e.g. to correct a typo'd env_var) must never leave a phantom grant for the FIRST name behind
        // in the final CapabilitySet; see this file's own top comment for the finding this closes.
        primary_secret_grant_ =
            agentengine::Capability{agentengine::cap::Secret{secret_name, std::chrono::seconds{0}}};
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
    // when `.api_key_from_env(...)` was never called at all, or when the environment variable it named
    // was unset at build time (a real value never reached the Store, so the built session would fail
    // `secret.not_found` on its very first call instead of here, at construction).
    [[nodiscard]] agentengine::result<BundleT> build() {
        if (!api_key_ref_.has_value()) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "no primary backend credential named -- call .api_key_from_env(...) before build()",
                "quickstart_builder.no_credential"});
        }
        auto pending = pending_secret_values_.find(api_key_ref_->name);
        if (pending == pending_secret_values_.end()) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "the environment variable named for '" + api_key_ref_->name +
                    "' was unset at build time -- the built session would fail on its first call "
                    "instead of here",
                "quickstart_builder.credential_env_unset"});
        }

        auto store = std::make_unique<Store>();
        store->set(pending->first, pending->second);

        // primary_secret_grant_ is guaranteed set here: it and api_key_ref_ are only ever written
        // together, in api_key_from_env(), and the has_value() check above already confirmed
        // api_key_ref_.
        std::vector<agentengine::Capability> all_grants = grants_;
        all_grants.push_back(*primary_secret_grant_);
        auto capabilities = std::make_unique<agentengine::CapabilitySet>(
            agentengine::CapabilitySet::grant_root(std::move(all_grants)));

        Primary primary(host_, port_, model_, *api_key_ref_, caps_, *store, path_prefix_);

        auto session = std::make_unique<typename BundleT::SessionT>();
        session->initialize(session_id_, principal_);
        // Constructs ModelCallGateway<Primary> IN PLACE from (Primary&&, std::tuple<>&&) -- §2a's "one
        // rung above bare" default (retry+circuit-breaker, empty fallback chain, default RetryPolicy/
        // BreakerConfig) -- never a move of an already-built ChatClientT.
        session->emplace_chat_client(std::move(primary), std::tuple<>{});
        session->set_capabilities(capabilities.get());

        return BundleT(std::move(store), std::move(capabilities), std::move(session));
    }

private:
    std::string model_;
    std::string session_id_ = "s-quickstart";
    agentengine::Principal principal_{"p-quickstart", ""};
    agentengine::ChatClientCapabilities caps_{};
    std::optional<agentengine::SecretRef> api_key_ref_;
    std::optional<agentengine::Capability> primary_secret_grant_;
    std::unordered_map<std::string, std::string> pending_secret_values_;
    std::vector<agentengine::Capability> grants_;  // explicit .grant() calls only -- see api_key_from_env()
    std::string host_        = detail::default_endpoint<P>::host;
    std::uint16_t port_      = detail::default_endpoint<P>::port;
    std::string path_prefix_ = detail::default_endpoint<P>::path_prefix;
};

using OpenAiSessionBuilder    = QuickstartSessionBuilder<Provider::openai>;
using AnthropicSessionBuilder = QuickstartSessionBuilder<Provider::anthropic>;

}  // namespace agentengine::quickstart

#endif  // AGENTENGINE_WITH_HTTPS
