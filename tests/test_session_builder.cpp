// Prove pass for docs/planning/quickstart-session-builder-design-draft.md / core/session_builder.hpp
// -- covers §2a (gateway-wrapped-by-default client stack)/§2b (history/context composition, B14-B17,
// added in a 4th pass -- see session_builder.hpp's own top comment, finding 8)/§2c (capability+secret
// sugar, fail-closed at build() time, generalized to any real SecretStore conformer -- proven against
// the project's own production AgentEngineSecretStore, not just InMemorySecretStore)/§3 (Store/
// CapabilitySet lifetime safety across a move), matching the header's own scope comment.
//
// Deliberately NOT exercised here, named as a real scope limit rather than silently skipped: no
// `.ask()`/`start_run()` call, so no real (or even attempted) network exchange happens in this test --
// `OpenAIChatClient::chat()`'s body is therefore never instantiated, which is also why this test needs
// no MbedTLS/provider_http_client link (see tests/CMakeLists.txt's own comment on this target). A
// live, real-network proof of `.ask()` end to end is separately scoped future work, matching this
// project's own "live-network" label convention for that class of test. B14-B17 (§2b) similarly never
// drive a live `start_run()` through `ComposedQuickstartSessionBuilder`'s own session -- it has no
// `.raw_client_only()` escape hatch either (§2a, still unimplemented) -- so `ComposedContextProvider
// ::on_context()` is driven DIRECTLY instead, the same scope limit `tests/test_composed_context_
// provider.cpp`'s own "Part 1" uses for the equivalent reason.
//
// Also NOT exercised: a genuine multi-threaded proof of the red-team's #1 finding fix (`ask_mutex_`
// serializing concurrent `.ask()` calls against a real, contended `session_mutex_`). That needs a
// ChatClient whose `chat()` coroutine genuinely suspends (this test's scope has none), plus real
// threads racing `.ask()` on one `Bundle` under repeated runs -- a real, larger test, not designed
// here. The fix is currently verified by code review against `rt/thread_pool.hpp`'s/`rt/drive_leaf_
// task.hpp`'s own documented precedent for this exact bug class, not by a live concurrency test.

#include <array>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/session_builder.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/trust/secret_quarantine.hpp"
#include "support/run_task_sync.hpp"

namespace {

// A minimal, LOCAL ContextProvider conformer with NO default constructor -- exists purely to prove
// §2b's real fix unambiguously (session_builder.hpp finding 8): a provider that genuinely cannot
// occupy `AgentSession`'s plain, always-default-constructed `HistoryProviderT` slot on its own now
// composes fine through `ComposedContextProvider`, since that slot is never asked to
// default-construct THIS type -- only `ComposedContextProvider<Ms...>` itself, which always can.
// Round 4 red-team findings 9-10 (session_builder.hpp's own top comment) -- `turn_end_calls` (a
// shared counter, not a private member, so a test can read it after the provider moves into
// ComposedContextProvider's own contributors_) lets B18/B19 prove `on_turn_end` fan-out reaches
// every wrapped provider, not just the first -- the same thing test_composed_context_provider.cpp's
// own FixedMessagesProvider already proves for ComposedContextProvider, previously untested here.
struct RequiredArgProvider {
    static constexpr std::string_view name = "required-arg";  // ADR-066 §3

    RequiredArgProvider(std::string text, std::shared_ptr<std::size_t> turn_end_calls)
        : text_(std::move(text)), turn_end_calls_(std::move(turn_end_calls)) {}

    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext&, agentengine::EffectContext&) {
        agentengine::ContextContribution c;
        agentengine::Message m;
        m.role = agentengine::role::system;
        agentengine::ContentItem item;
        item.origin = agentengine::content_origin::system;
        item.value  = agentengine::Text{text_};
        m.content.push_back(item);
        c.messages.push_back(std::move(m));
        co_return c;
    }
    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        ++*turn_end_calls_;
        co_return std::monostate{};
    }

private:
    std::string text_;
    std::shared_ptr<std::size_t> turn_end_calls_;
};
static_assert(!std::is_default_constructible_v<RequiredArgProvider>,
              "the whole point of B14-B17: this type must genuinely have no default constructor");
static_assert(agentengine::ContextProvider<RequiredArgProvider>,
              "RequiredArgProvider must satisfy ContextProvider (005 §5) to be usable at all");

// B22 (round 8 red-team, finding 15): proves ComposedContextProvider::engage() is exception-safe.
// CountingProvider never throws -- pushed first, its on_context() call count proves whether a stale
// copy from a FAILED engage() attempt survives into a later successful retry.
struct CountingProvider {
    static constexpr std::string_view name = "counting-provider";  // ADR-066 §3
    static inline int on_context_calls = 0;

    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext&, agentengine::EffectContext&) {
        ++on_context_calls;
        co_return agentengine::ContextContribution{};
    }
    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        co_return std::monostate{};
    }
};

// Throws on its Nth move-construction (1-indexed, counted across the WHOLE type, not per-instance) --
// lets a test fire the throw at a precise point inside engage()'s own machinery rather than merely
// "at construction time." set_throw_on_move(-1) disarms it (used for the retry, which must succeed).
struct ThrowingProvider {
    static constexpr std::string_view name = "throwing-provider";  // ADR-066 §3
    static inline int move_count = 0;
    static inline int throw_on_move_number = -1;

    ThrowingProvider() = default;
    ThrowingProvider(ThrowingProvider&&) {
        ++move_count;
        if (move_count == throw_on_move_number) {
            throw std::runtime_error("B22: simulated move failure mid-engage()");
        }
    }
    ThrowingProvider(ThrowingProvider const&) = default;

    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext&, agentengine::EffectContext&) {
        co_return agentengine::ContextContribution{};
    }
    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        co_return std::monostate{};
    }
};

}  // namespace

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

}  // namespace

int main() {
    using namespace agentengine;
    using quickstart::ComposedQuickstartSessionBuilder;
    using quickstart::OpenAiSessionBuilder;

    // §2b's own env-var setup, reused for B14-B17 (same value B3+ already use).
#if defined(_WIN32)
    _putenv_s("AE_TEST_QUICKSTART_KEY", "sk-test-value-123");
#else
    setenv("AE_TEST_QUICKSTART_KEY", "sk-test-value-123", 1);
#endif

    // ---- B1: no .api_key_from_env() at all -- build() fails closed, not a thrown exception --------
    {
        auto built = OpenAiSessionBuilder("gpt-4o-mini").build();
        check(!built.has_value(), "no credential named at all: build() fails");
        if (!built.has_value()) {
            check(built.error().code == "quickstart_builder.no_credential",
                  "the failure is specifically 'no_credential', not a different error");
        }
    }

    // ---- B2: .api_key_from_env() named, but the env var was never set -- build() still fails closed,
    // catching the mistake HERE instead of at the built session's first chat() call -----------------
    {
        auto built = OpenAiSessionBuilder("gpt-4o-mini")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_DEFINITELY_UNSET")
                         .build();
        check(!built.has_value(), "credential named but env var unset: build() fails");
        if (!built.has_value()) {
            check(built.error().code == "quickstart_builder.no_store",
                  "the failure is specifically 'no_store' -- api_key_from_env() named the ref (so "
                  "no_credential does NOT fire) but never reached store() since the env var was unset");
        }
    }

    // ---- B3: the real happy path -- env var set, build() succeeds, the session is fully wired ------
    {
#if defined(_WIN32)
        _putenv_s("AE_TEST_QUICKSTART_KEY", "sk-test-value-123");
#else
        setenv("AE_TEST_QUICKSTART_KEY", "sk-test-value-123", 1);
#endif
        auto built = OpenAiSessionBuilder("gpt-4o-mini")
                         .session_id("s-proto")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                         .grant(Capability{cap::FsRead{"scratch", "", std::nullopt}})
                         .build();
        check(built.has_value(), "credential named and set: build() succeeds");
        if (built.has_value()) {
            check(built->session().has_chat_client(),
                  "the built session has a ChatClientT emplaced (ModelCallGateway<OpenAIChatClient>)");
            check(built->capabilities().size() == 2,
                  "capabilities() holds BOTH the implicit cap::Secret from api_key_from_env() AND the "
                  "explicit .grant() -- neither silently dropped, neither silently duplicated");
            check(built->capabilities().contains(
                      Capability{cap::Secret{"openai-api-key", std::chrono::seconds{0}}}),
                  "the api_key_from_env()-derived cap::Secret grant is really present, matching the "
                  "SecretRef name the constructed OpenAIChatClient will resolve against at call time");

            // ---- §3's own load-bearing claim: MOVE the Bundle, then dereference through the new
            // location. If Store/CapabilitySet were stack-owned (the design draft's original, WRONG
            // sketch) rather than heap-owned via unique_ptr, this would be a dangling-reference read --
            // real undefined behavior, not a hypothetical one. Passing here does not prove the absence
            // of UB under every allocator/sanitizer configuration, but a heap-owned unique_ptr's
            // pointee address is provably unaffected by moving the ownership handle around it, which is
            // the actual property this design leans on (not merely "the test happened not to crash").
            auto moved = std::move(*built);
            check(moved.session().has_chat_client(),
                  "after moving the Bundle, the session (heap-owned via unique_ptr) is still wired");
            check(moved.capabilities().size() == 2,
                  "after moving the Bundle, CapabilitySet (heap-owned, stable address) still reads back "
                  "correctly -- the session's internal CapabilitySet const* still points at live memory");
        }
    }

    // ---- B4: red-team finding #5's regression proof -- calling .api_key_from_env() a SECOND time
    // (e.g. correcting a typo'd env_var name) must not leave a phantom grant for the FIRST name behind.
    {
#if defined(_WIN32)
        _putenv_s("AE_TEST_QUICKSTART_KEY2", "sk-test-value-456");
#else
        setenv("AE_TEST_QUICKSTART_KEY2", "sk-test-value-456", 1);
#endif
        auto built = OpenAiSessionBuilder("gpt-4o-mini")
                         .api_key_from_env("wrong-name", "AE_TEST_QUICKSTART_DEFINITELY_UNSET")
                         .api_key_from_env("right-name", "AE_TEST_QUICKSTART_KEY2")
                         .build();
        check(built.has_value(), "correcting the credential name on a second call: build() still succeeds");
        if (built.has_value()) {
            check(built->capabilities().size() == 1,
                  "exactly ONE auto-derived cap::Secret grant survives -- the first call's 'wrong-name' "
                  "grant was overwritten, not left behind as a phantom, unusable second entry");
            check(built->capabilities().contains(
                      Capability{cap::Secret{"right-name", std::chrono::seconds{0}}}),
                  "the surviving grant is for the SECOND (corrected) name");
            check(!built->capabilities().contains(
                      Capability{cap::Secret{"wrong-name", std::chrono::seconds{0}}}),
                  "the FIRST (wrong) name's grant is genuinely gone, not merely shadowed");
        }
    }

    // ---- B5: the GENERIC path (.api_key() + emplacing .store()) works with the REAL production store
    // type, AgentEngineSecretStore over EnvSecretSource -- not merely InMemorySecretStore-shaped. This
    // closes the finding named in the header's own top comment (finding 3): AgentEngineSecretStore has
    // no default constructor and no .set(), so .api_key_from_env() cannot even be NAMED against it (a
    // compile-time `requires` gate, not a runtime check) -- .api_key()+.store() is the only path for
    // this Store. This proves construction/wiring succeeds; B5b right below proves the actual secret
    // resolves, which this block alone (per a second red-team pass) did NOT prove.
    {
#if defined(_WIN32)
        _putenv_s("QUARK_SECRET_prod-api-key", "sk-prod-value-789");
#else
        setenv("QUARK_SECRET_prod-api-key", "sk-prod-value-789", 1);
#endif
        quickstart::QuickstartSessionBuilder<quickstart::Provider::openai, AgentEngineSecretStore>
            builder("gpt-4o-mini");
        builder.api_key(SecretRef{"prod-api-key"});
        // .store(...) forwards its args to Store's own constructor, in place -- AgentEngineSecretStore
        // itself is still movable (only QuarantineSecretStore, B7 below, genuinely needs the emplace
        // shape), so passing an already-constructed instance here works too, exercised deliberately to
        // prove the emplace-style `.store()` still accepts this call shape, not just default-args ones.
        builder.store(AgentEngineSecretStore(std::make_unique<EnvSecretSource>()));
        auto built = builder.build();
        check(built.has_value(), "the generic .api_key()+.store() path builds successfully against the "
                                  "REAL production AgentEngineSecretStore, not just InMemorySecretStore");
        if (built.has_value()) {
            check(built->session().has_chat_client(),
                  "the built session (real production Store type) has a ChatClientT emplaced");
        }
    }

    // ---- B5b: red-team round 2's finding #2 regression proof -- B5 alone never called .resolve(), so
    // it never proved the capability-name match between .api_key(SecretRef{name})'s auto-granted
    // cap::Secret and what EnvSecretSource/AgentEngineSecretStore actually check/look up at resolve
    // time. This calls .resolve() directly against the SAME real types, independent of the full
    // session/Bundle machinery, closing that gap for real.
    {
#if defined(_WIN32)
        _putenv_s("QUARK_SECRET_prod-api-key-2", "sk-prod-value-abc");
#else
        setenv("QUARK_SECRET_prod-api-key-2", "sk-prod-value-abc", 1);
#endif
        AgentEngineSecretStore store(std::make_unique<EnvSecretSource>());
        CapabilitySet const held =
            CapabilitySet::grant_root({Capability{cap::Secret{"prod-api-key-2", std::chrono::seconds{0}}}});
        EffectContext ctx;
        ctx.principal    = Principal{"p-test", ""};
        ctx.capabilities = borrow_capabilities(held);
        auto lease = store.resolve(SecretRef{"prod-api-key-2"}, ctx);
        check(lease.has_value(),
              "resolve() succeeds against the REAL AgentEngineSecretStore/EnvSecretSource pair, using "
              "the EXACT capability-grant shape .api_key(SecretRef) produces -- proves the grant name "
              "and the store's own lookup name genuinely match, not merely assumed to");
        if (lease.has_value()) {
            check(lease->reveal_text() == "sk-prod-value-abc",
                  "and it resolves to the actual env-var value, not a stale or wrong one");
        }
    }

    // ---- B6: build() called a second time on the same builder instance now fails closed --
    // documented behavior change from the earlier red-team's "build() called twice is safe" non-finding
    // (see the header's own top comment, finding 3): build() now MOVES store_ out to stay generic over
    // non-copyable stores.
    {
        quickstart::OpenAiSessionBuilder builder("gpt-4o-mini");
        builder.api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY");
        auto first = builder.build();
        check(first.has_value(), "first build() call on this builder succeeds");
        auto second = builder.build();
        check(!second.has_value(), "second build() call on the SAME builder instance fails closed, not "
                                    "UB and not a silently-broken second Bundle");
        if (!second.has_value()) {
            check(second.error().code == "quickstart_builder.no_store",
                  "specifically because store_ was already moved out by the first build() call");
        }
    }

    // ---- B7: red-team round 2's finding #1 regression proof -- .store()'s ORIGINAL, by-value shape
    // (`store(Store store)`) required Store to be movable/copyable, which a real, already-shipped
    // SecretStore conformer in this codebase, QuarantineSecretStore (trust/secret_quarantine.hpp,
    // ADR-068), genuinely is NOT (it holds a std::mutex directly). This builds a real session against
    // it, proving the current emplace-style `.store(Args&&...)` genuinely works where the old shape
    // could not even be CALLED -- if `.store()` ever regresses back to taking `Store` by value, this
    // block stops compiling, not merely stops passing.
    {
        quickstart::QuickstartSessionBuilder<quickstart::Provider::openai, QuarantineSecretStore>
            builder("gpt-4o-mini");
        builder.api_key(SecretRef{"quarantined-key"});
        builder.store();  // default-constructs QuarantineSecretStore(nullptr) IN PLACE -- never moves one
        auto built = builder.build();
        check(built.has_value(),
              "the builder wires a real, non-movable SecretStore conformer (QuarantineSecretStore) via "
              "the emplace-style .store() -- the by-value shape a prior version had could not do this");
        if (built.has_value()) {
            check(built->session().has_chat_client(),
                  "the built session (non-movable Store type) has a ChatClientT emplaced");
        }
    }

    // ---- B8: §2d, no-decider default is genuinely untouched when .approve_tools()/.policy() are
    // never called -- not merely "behaves the same," the underlying std::function member itself stays
    // empty (false in a boolean context), matching AgentSession's own true unset-decider state exactly.
    {
        auto built = OpenAiSessionBuilder("gpt-4o-mini")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                         .build();
        check(built.has_value(), "B8 setup: build() succeeds");
        if (built.has_value()) {
            check(!static_cast<bool>(built->session().approval_decider()),
                  "no .approve_tools() call: the session's ApprovalDecider is genuinely unset, not an "
                  "always-false decider standing in for 'unset'");
            check(!static_cast<bool>(built->session().policy_decider()),
                  "no .policy() call: the session's PolicyDecider is genuinely unset");
        }
    }

    // ---- B9: §2d, .approve_tools() installs a decider that auto-approves ONLY the named tools and
    // denies everything else -- the safe default (I2: narrows/decides among already-required decisions,
    // never widens which calls need one).
    {
        auto built = OpenAiSessionBuilder("gpt-4o-mini")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                         .approve_tools({"safe_tool"})
                         .build();
        check(built.has_value(), "B9 setup: build() succeeds");
        if (built.has_value()) {
            auto const& decide = built->session().approval_decider();
            check(static_cast<bool>(decide), ".approve_tools() installs a real ApprovalDecider");
            if (decide) {
                check(decide(Principal{"p", ""}, "safe_tool", "{}"),
                      "the named tool is auto-approved");
                check(!decide(Principal{"p", ""}, "other_tool", "{}"),
                      "an UN-named tool is denied, not silently auto-approved too");
            }
        }
    }

    // ---- B10: §2d, .policy() is a thin, unmodified pass-through to set_policy_decider().
    {
        auto built = OpenAiSessionBuilder("gpt-4o-mini")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                         .policy([](Principal const&, ToolDescriptor const&, bool) {
                             return policy_decision::auto_deny;
                         })
                         .build();
        check(built.has_value(), "B10 setup: build() succeeds");
        if (built.has_value()) {
            auto const& policy = built->session().policy_decider();
            check(static_cast<bool>(policy), ".policy() installs a real PolicyDecider");
            if (policy) {
                ToolDescriptor td{};
                check(policy(Principal{"p", ""}, td, false) == policy_decision::auto_deny,
                      "the host-authored decision function is genuinely reachable, unmodified, through "
                      "the builder");
            }
        }
    }

    // ---- B11/B12/B13: red-team round 3's finding #7 regression proof -- `max_turns_` genuinely
    // defaults to a FINITE value (25), not `AgentSession::initialize()`'s own raw `std::nullopt`
    // (unbounded) default, and `.max_turns(...)` genuinely reaches the constructed session. Scope limit,
    // named rather than silently skipped: this proves the builder's own WIRING is correct (the value it
    // passes to `initialize()` reads back correctly through `AgentSession::max_turns()`); it does NOT
    // re-run the red-team's own live hang-reproduction probe against a scripted, always-denies
    // ChatClient -- this builder has no `.raw_client_only()` escape hatch (§2a, still unimplemented) to
    // substitute a fake client for that without real network. The deeper guarantee ("does a finite
    // `max_turns_` actually bound `run_rounds()`'s loop") is `AgentSession`'s own responsibility, not
    // reproven here -- what IS this builder's own responsibility, and what these three prove, is that it
    // actually gets threaded through instead of silently staying at the dangerous raw default.
    {
        // B11: default -- finite, not unbounded.
        auto built = OpenAiSessionBuilder("gpt-4o-mini")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                         .build();
        check(built.has_value(), "B11 setup: build() succeeds");
        if (built.has_value()) {
            check(built->session().max_turns() == std::uint64_t{25},
                  "default max_turns() is a FINITE value (25), not AgentSession's own raw unbounded "
                  "(nullopt) default -- the fix for the red-team's live-reproduced hang");
        }
    }
    {
        // B12: .max_turns(std::nullopt) -- an explicit, informed opt-in to unbounded, still reachable.
        auto built = OpenAiSessionBuilder("gpt-4o-mini")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                         .max_turns(std::nullopt)
                         .build();
        check(built.has_value(), "B12 setup: build() succeeds");
        if (built.has_value()) {
            check(!built->session().max_turns().has_value(),
                  ".max_turns(std::nullopt) genuinely reaches the session as unbounded -- the safe "
                  "default is overridable, not hardcoded");
        }
    }
    {
        // B13: .max_turns(n) -- a custom finite bound reaches the session unchanged.
        auto built = OpenAiSessionBuilder("gpt-4o-mini")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                         .max_turns(3)
                         .build();
        check(built.has_value(), "B13 setup: build() succeeds");
        if (built.has_value()) {
            check(built->session().max_turns() == std::uint64_t{3},
                  ".max_turns(3) genuinely reaches the session, not silently ignored or clamped");
        }
    }

    // ---- §2b: ComposedQuickstartSessionBuilder<Provider, Store, Ms...> -- session_builder.hpp's own
    // finding 8. The pack mixes a default-constructible provider (HistoryProvider<Window<0>>) with a
    // genuinely NON-default-constructible one (RequiredArgProvider, this file's own local fixture) --
    // proving the actual gap finding 8 closes, not just that composition works for already-easy types.
    using ComposedBuilder =
        ComposedQuickstartSessionBuilder<quickstart::Provider::openai, InMemorySecretStore,
                                           HistoryProvider<Window<0>>, RequiredArgProvider>;
    static_assert(std::is_default_constructible_v<ComposedBuilder::HistoryProviderT>,
                  "ComposedContextProvider<Ms...> itself must stay default-constructible -- the "
                  "whole mechanism finding 8 relies on -- even though one of ITS Ms (RequiredArgProvider) "
                  "deliberately is not");

    {
        // B14: build() without .providers() fails closed, not a partially-wired session silently
        // missing its whole history/context slot.
        auto built = ComposedBuilder("gpt-4o-mini")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                         .build();
        check(!built.has_value(), "no .providers() call: build() fails");
        if (!built.has_value()) {
            check(built.error().code == "quickstart_builder.no_providers",
                  "the failure is specifically 'no_providers', not a different error");
        }
    }
    {
        // B15: build() succeeds once .providers() supplies real values for BOTH Ms -- including the
        // non-default-constructible one, which is exactly the case finding 8 says was never possible
        // before this pass (AgentSession<..., ComposedContextProvider<H, RequiredArgProvider>> could
        // not even be default-constructed, since ComposedContextProvider's own default ctor requires
        // every Ms default-constructible).
        auto turn_end_calls = std::make_shared<std::size_t>(0);
        auto built = ComposedBuilder("gpt-4o-mini")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                         .providers(std::make_tuple(HistoryProvider<Window<0>>{},
                                                     RequiredArgProvider{"required-arg-text",
                                                                           turn_end_calls}))
                         .build();
        check(built.has_value(), "credential + providers set: build() succeeds even with a "
                                  "non-default-constructible Ms in the pack");
        if (built.has_value()) {
            check(built->session().has_chat_client(),
                  "the built session has a ChatClientT emplaced, same as every other builder path");

            // B16: the composed provider's on_context() -- driven DIRECTLY (see this file's own top
            // comment for why, matching test_composed_context_provider.cpp's own "Part 1" scope) --
            // actually returns RequiredArgProvider's real, host-supplied contribution, in declared
            // order (HistoryProvider<Window<0>> first, contributing nothing for an empty history;
            // RequiredArgProvider second).
            Principal principal{"p-composed", ""};
            std::vector<Message> empty_history;
            SessionContext session_ctx{"s-composed", principal, empty_history};
            EffectContext effect_ctx{};
            auto contribution = agentengine::test_support::run_task_sync<
                result<ContextContribution>>(built->session().history_provider().on_context(
                session_ctx, effect_ctx));
            check(contribution.has_value(), "B16: on_context() succeeds once engaged");
            check(contribution.has_value() && contribution->messages.size() == 1,
                  "B16: exactly one message -- HistoryProvider<Window<0>> contributes nothing for an "
                  "empty history, RequiredArgProvider contributes exactly one");
            check(contribution.has_value() && contribution->messages.size() == 1 &&
                      std::get<Text>(contribution->messages[0].content.front().value).text ==
                          "required-arg-text",
                  "B16: the message is genuinely RequiredArgProvider's own host-supplied text, not a "
                  "placeholder or a default-constructed empty one");
        }
    }
    {
        // B17: engage() called a second time on the SAME ComposedContextProvider instance fails
        // closed (defense-in-depth, independent of the builder-level "second build() fails at
        // no_providers" guard) -- calling it twice would otherwise silently duplicate every
        // contributor on the wire.
        auto counter_a = std::make_shared<std::size_t>(0);
        auto counter_b = std::make_shared<std::size_t>(0);
        auto built = ComposedBuilder("gpt-4o-mini")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                         .providers(std::make_tuple(HistoryProvider<Window<0>>{},
                                                     RequiredArgProvider{"first-engage", counter_a}))
                         .build();
        check(built.has_value(), "B17 setup: build() succeeds");
        if (built.has_value()) {
            auto second_engage = built->session().history_provider().engage(std::make_tuple(
                HistoryProvider<Window<0>>{}, RequiredArgProvider{"second-engage", counter_b}));
            check(!second_engage.has_value(),
                  "a second engage() call on the same instance fails closed");
            if (!second_engage.has_value()) {
                check(second_engage.error().code == "composed_context.already_engaged",
                      "the failure is specifically 'already_engaged', not a different error");
            }
        }
    }
    {
        // B18/B19: round 4 red-team findings 9-10's own test-gap closures -- TWO providers that BOTH
        // contribute a real message (unlike B16's history+skills-style pairing, where only one ever
        // contributes anything) prove declared order genuinely holds, not just "the one non-empty
        // contributor shows up regardless of position"; and on_turn_end() fans out to BOTH, not just
        // the first, matching what test_composed_context_provider.cpp already proves for its sibling
        // ComposedContextProvider.
        using TwoRealBuilder =
            ComposedQuickstartSessionBuilder<quickstart::Provider::openai, InMemorySecretStore,
                                               RequiredArgProvider, RequiredArgProvider>;
        auto calls_first  = std::make_shared<std::size_t>(0);
        auto calls_second = std::make_shared<std::size_t>(0);
        auto built = TwoRealBuilder("gpt-4o-mini")
                         .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                         .providers(std::make_tuple(RequiredArgProvider{"first", calls_first},
                                                     RequiredArgProvider{"second", calls_second}))
                         .build();
        check(built.has_value(), "B18/B19 setup: build() succeeds with two REAL-contribution providers");
        if (built.has_value()) {
            Principal principal{"p-composed-2", ""};
            std::vector<Message> empty_history;
            SessionContext session_ctx{"s-composed-2", principal, empty_history};
            EffectContext effect_ctx{};
            auto contribution = agentengine::test_support::run_task_sync<result<ContextContribution>>(
                built->session().history_provider().on_context(session_ctx, effect_ctx));
            check(contribution.has_value() && contribution->messages.size() == 2,
                  "B18: both providers' messages reach the combined contribution");
            check(contribution.has_value() && contribution->messages.size() == 2 &&
                      std::get<Text>(contribution->messages[0].content.front().value).text == "first" &&
                      std::get<Text>(contribution->messages[1].content.front().value).text == "second",
                  "B18: declared order is preserved -- 'first' before 'second', not reordered or "
                  "reversed");

            TurnView turn{std::span<Message const>{empty_history.data(), empty_history.size()}};
            agentengine::test_support::run_task_sync<std::monostate>(
                built->session().history_provider().on_turn_end(turn, effect_ctx));
            check(*calls_first == 1 && *calls_second == 1,
                  "B19: on_turn_end() fans out to BOTH wrapped providers, not just the first");
        }
    }
    {
        // B20 (rewritten 2026-08-30, ADR-116): this used to prove the OPPOSITE of what it proves now.
        // Originally (round 5 red-team finding A) it demonstrated that `target.history_provider() =
        // std::move(source.history_provider())` -- a real cross-SESSION move through the public
        // accessor -- successfully transferred live provider state, and merely checked the move
        // mechanics (moved-from correctly `not_engaged`, recoverable via re-`engage()`) were sound.
        // ADR-116 closed that transfer itself as a disclosed, live I1/I4-adjacent aliasing hazard
        // (composed_context_provider.hpp's own top comment, ADR-102 §48/§51) -- `operator=` now
        // refuses (a silent, fully-disclosed no-op) whenever both sides are tagged with two DIFFERENT
        // sessions' own addresses. This test now proves THAT: the exact same statement that used to
        // succeed now leaves BOTH sessions' own content completely untouched.
        using SingleBuilder = ComposedQuickstartSessionBuilder<quickstart::Provider::openai,
                                                                  InMemorySecretStore, RequiredArgProvider>;
        auto counter1 = std::make_shared<std::size_t>(0);
        auto counter2 = std::make_shared<std::size_t>(0);
        auto built1 = SingleBuilder("gpt-4o-mini")
                          .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                          .providers(std::make_tuple(RequiredArgProvider{"session-1", counter1}))
                          .build();
        auto built2 = SingleBuilder("gpt-4o-mini")
                          .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                          .providers(std::make_tuple(RequiredArgProvider{"session-2", counter2}))
                          .build();
        check(built1.has_value() && built2.has_value(), "B20 setup: both sessions build() successfully");
        if (built1.has_value() && built2.has_value()) {
            // The exact statement ADR-116 closes -- a real cross-session move through the same public
            // accessor this file's own B20 has exercised since round 5.
            built2->session().history_provider() = std::move(built1->session().history_provider());

            Principal principal{"p-move", ""};
            std::vector<Message> empty_history;
            SessionContext session_ctx{"s-move", principal, empty_history};
            EffectContext effect_ctx{};

            auto still_session1 = agentengine::test_support::run_task_sync<result<ContextContribution>>(
                built1->session().history_provider().on_context(session_ctx, effect_ctx));
            check(still_session1.has_value() && still_session1->messages.size() == 1 &&
                      std::get<Text>(still_session1->messages[0].content.front().value).text ==
                          "session-1",
                  "B20: the refused move leaves session1's OWN provider completely untouched -- "
                  "still engaged, still its own original content, not reset to not_engaged");

            auto still_session2 = agentengine::test_support::run_task_sync<result<ContextContribution>>(
                built2->session().history_provider().on_context(session_ctx, effect_ctx));
            check(still_session2.has_value() && still_session2->messages.size() == 1 &&
                      std::get<Text>(still_session2->messages[0].content.front().value).text ==
                          "session-2",
                  "B20: the refused move leaves session2's OWN provider completely untouched -- "
                  "still its own original content, NOT silently overwritten with session1's");
        }
    }
    {
        // B23 (independent red-team, same day as ADR-116): the ORIGINAL ADR-116 shape left `owner_`
        // `nullptr` on a freshly move-CONSTRUCTED instance, reasoning it "is not yet embedded in any
        // session's member slot". That let session1's LIVE content pass through an untagged
        // intermediate local and defeat `operator=`'s whole check in exactly two ordinary-looking
        // lines: `auto smuggler = std::move(session1.history_provider());` (move-CONSTRUCTION, leaves
        // `smuggler.owner_ == nullptr` under the original shape) then
        // `session2.history_provider() = std::move(smuggler);` (passes cleanly, since one side being
        // `nullptr` was enough to skip the refusal regardless of the other side's tag). Empirically
        // confirmed this FAILED against the original ADR-116 code, before the move constructor was
        // fixed to propagate `other.owner_` instead of defaulting to `nullptr` (composed_context_
        // provider.hpp's own comment on that constructor).
        using SingleBuilder = ComposedQuickstartSessionBuilder<quickstart::Provider::openai,
                                                                  InMemorySecretStore, RequiredArgProvider>;
        auto counter1 = std::make_shared<std::size_t>(0);
        auto counter2 = std::make_shared<std::size_t>(0);
        auto built1 = SingleBuilder("gpt-4o-mini")
                          .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                          .providers(std::make_tuple(RequiredArgProvider{"b23-1", counter1}))
                          .build();
        auto built2 = SingleBuilder("gpt-4o-mini")
                          .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                          .providers(std::make_tuple(RequiredArgProvider{"b23-2", counter2}))
                          .build();
        check(built1.has_value() && built2.has_value(), "B23 setup: both sessions build() successfully");
        if (built1.has_value() && built2.has_value()) {
            auto smuggler = std::move(built1->session().history_provider());  // move-CONSTRUCTION
            built2->session().history_provider() = std::move(smuggler);      // second hop

            Principal principal{"p-b23", ""};
            std::vector<Message> empty_history;
            SessionContext session_ctx{"s-b23", principal, empty_history};
            EffectContext effect_ctx{};
            auto session2_now = agentengine::test_support::run_task_sync<result<ContextContribution>>(
                built2->session().history_provider().on_context(session_ctx, effect_ctx));
            bool leaked = session2_now.has_value() && session2_now->messages.size() == 1 &&
                          std::get<Text>(session2_now->messages[0].content.front().value).text ==
                              "b23-1";
            check(!leaked,
                  "B23: a two-hop move through an untagged intermediate LOCAL VARIABLE must NOT leak "
                  "session1's content into session2 -- closing a real bypass the original ADR-116 shape "
                  "had, found by a same-day independent red-team pass");
        }
    }
    {
        // B24 (independent red-team, same day as ADR-116, same finding as B23's own comment): B23
        // alone would still pass if ONLY the move CONSTRUCTOR propagated `owner_` -- this test proves
        // the deeper chain also needs `operator=` itself to propagate `other`'s tag onto an untagged
        // `this`, or a THIRD hop through a standalone `operator=`-assigned "relay" variable (as opposed
        // to a move-constructed one) reopens the exact same bypass: `relay = std::move(smuggler);`
        // (operator=, not construction) into a bare, never-session-touched `relay` must leave `relay`
        // carrying session1's own provenance tag, not `nullptr`, so a later `session2.history_provider()
        // = std::move(relay);` is still correctly refused.
        using SingleBuilder = ComposedQuickstartSessionBuilder<quickstart::Provider::openai,
                                                                  InMemorySecretStore, RequiredArgProvider>;
        using ProviderT = SingleBuilder::HistoryProviderT;
        auto counter1 = std::make_shared<std::size_t>(0);
        auto counter2 = std::make_shared<std::size_t>(0);
        auto built1 = SingleBuilder("gpt-4o-mini")
                          .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                          .providers(std::make_tuple(RequiredArgProvider{"b24-1", counter1}))
                          .build();
        auto built2 = SingleBuilder("gpt-4o-mini")
                          .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                          .providers(std::make_tuple(RequiredArgProvider{"b24-2", counter2}))
                          .build();
        check(built1.has_value() && built2.has_value(), "B24 setup: both sessions build() successfully");
        if (built1.has_value() && built2.has_value()) {
            auto smuggler = std::move(built1->session().history_provider());  // hop 1: move-CONSTRUCTION
            ProviderT relay;                                                  // bare, never session-touched
            relay = std::move(smuggler);                                      // hop 2: operator=, not construction
            built2->session().history_provider() = std::move(relay);         // hop 3: into a DIFFERENT session

            Principal principal{"p-b24", ""};
            std::vector<Message> empty_history;
            SessionContext session_ctx{"s-b24", principal, empty_history};
            EffectContext effect_ctx{};
            auto session2_now = agentengine::test_support::run_task_sync<result<ContextContribution>>(
                built2->session().history_provider().on_context(session_ctx, effect_ctx));
            bool leaked = session2_now.has_value() && session2_now->messages.size() == 1 &&
                          std::get<Text>(session2_now->messages[0].content.front().value).text ==
                              "b24-1";
            check(!leaked,
                  "B24: a three-hop move through a standalone RELAY variable assigned to (not "
                  "move-constructed) must NOT leak session1's content into session2 -- proves "
                  "operator='s own propagation-to-untagged-destination fix is independently necessary, "
                  "not just the move constructor's");
        }
    }
    {
        // B25 (independent red-team, same day as ADR-116) -- ABA / address-reuse regression. Before
        // this same-day follow-on, `owner_` was `AgentSession`'s raw `this` (a `void const*`), with no
        // generation/epoch counter. Empirically confirmed real hole, not theoretical: heap-allocate
        // session1 via `ComposedQuickstartSessionBuilder` (real `make_unique`), move-construct its
        // `history_provider()` out into a `smuggler` (tagging it with session1's address), let session1
        // (and its heap-owned `AgentSession`) be destroyed, then heap-allocate a completely unrelated
        // session3 -- the CRT allocator reliably handed back the EXACT SAME freed block in this test
        // (confirmed below, not assumed), making session3's own address collide with `smuggler`'s stale
        // tag. Against the original raw-address `owner_`, `operator=` wrongly treated session3 as "the
        // same session" and allowed the merge, leaking session1's content into session3. Closed by
        // tagging with `AgentSession::session_identity_` (a monotonic, process-wide, never-reused
        // counter) instead of the object's address -- see that member's own comment in
        // agent_session.hpp.
        using SingleBuilder = ComposedQuickstartSessionBuilder<quickstart::Provider::openai,
                                                                  InMemorySecretStore, RequiredArgProvider>;
        void const* addr1 = nullptr;
        using ProviderT = SingleBuilder::HistoryProviderT;
        ProviderT smuggler;
        {
            auto counter1 = std::make_shared<std::size_t>(0);
            auto built1 = SingleBuilder("gpt-4o-mini")
                              .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                              .providers(std::make_tuple(RequiredArgProvider{"b25-1", counter1}))
                              .build();
            check(built1.has_value(), "B25 setup: session1 build() succeeds");
            if (built1.has_value()) {
                addr1 = static_cast<void const*>(&built1->session());
                smuggler = std::move(built1->session().history_provider());
            }
            // built1 (and its heap-owned AgentSession) is destroyed here, at scope exit.
        }
        auto counter3 = std::make_shared<std::size_t>(0);
        auto built3 = SingleBuilder("gpt-4o-mini")
                          .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                          .providers(std::make_tuple(RequiredArgProvider{"b25-3", counter3}))
                          .build();
        check(built3.has_value(), "B25 setup: session3 build() succeeds");
        if (built3.has_value()) {
            void const* addr3 = static_cast<void const*>(&built3->session());
            check(addr1 == addr3,
                  "B25 precondition: session3 is heap-allocated at the EXACT SAME address session1 "
                  "used to occupy -- if this ever stops holding (a different allocator behavior), this "
                  "test still passes but is no longer exercising the real ABA scenario it was written "
                  "for");
            built3->session().history_provider() = std::move(smuggler);
            Principal principal{"p-b25", ""};
            std::vector<Message> empty_history;
            SessionContext session_ctx{"s-b25", principal, empty_history};
            EffectContext effect_ctx{};
            auto session3_now = agentengine::test_support::run_task_sync<result<ContextContribution>>(
                built3->session().history_provider().on_context(session_ctx, effect_ctx));
            bool leaked = session3_now.has_value() && session3_now->messages.size() == 1 &&
                          std::get<Text>(session3_now->messages[0].content.front().value).text == "b25-1";
            check(!leaked,
                  "B25: session3 must NOT receive session1's content merely because they happened to "
                  "share the same (reused) heap address -- owner_ must be a never-reused identity, not "
                  "a raw address");
        }
    }
    {
        // B21: round 6 red-team's own test-gap closure -- B20 only covered ONE generation of
        // move-ASSIGNMENT between two distinct instances. Four scenarios round 6 identified as
        // unexamined, all closed here as permanent regression coverage (round 6 itself verified all
        // four hold via temporary probes before reporting this as a test-gap, not a live bug).
        Principal principal{"p-move-2", ""};
        std::vector<Message> empty_history;
        SessionContext session_ctx{"s-move-2", principal, empty_history};
        EffectContext effect_ctx{};

        // B21a: self-move-assignment (`x = std::move(x)`) must be a safe no-op, not a state-destroying
        // self-clear -- the `if (this != &other)` guard in operator=(ComposedContextProvider&&)
        // exists specifically for this case.
        {
            using SingleBuilder = ComposedQuickstartSessionBuilder<quickstart::Provider::openai,
                                                                      InMemorySecretStore, RequiredArgProvider>;
            auto counter = std::make_shared<std::size_t>(0);
            auto built   = SingleBuilder("gpt-4o-mini")
                             .api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                             .providers(std::make_tuple(RequiredArgProvider{"self-move", counter}))
                             .build();
            check(built.has_value(), "B21a setup: build() succeeds");
            if (built.has_value()) {
                auto& hp = built->session().history_provider();
                hp       = std::move(hp);
                auto contribution = agentengine::test_support::run_task_sync<
                    result<ContextContribution>>(hp.on_context(session_ctx, effect_ctx));
                check(contribution.has_value() && contribution->messages.size() == 1 &&
                          std::get<Text>(contribution->messages[0].content.front().value).text ==
                              "self-move",
                      "B21a: self-move-assignment leaves the instance's own content intact, not "
                      "self-cleared by the moved-from-reset logic");
            }
        }

        // B21b: move-CONSTRUCTION (not assignment) must reset the SOURCE's engaged_ the same way
        // move-assignment does -- both are separately hand-written, not just one delegating to the
        // other, so this is a genuinely distinct code path from B20's own coverage.
        {
            using ProviderT = ComposedQuickstartSessionBuilder<quickstart::Provider::openai,
                                                                  InMemorySecretStore,
                                                                  RequiredArgProvider>::HistoryProviderT;
            auto counter = std::make_shared<std::size_t>(0);
            ProviderT source;
            auto engaged = source.engage(std::make_tuple(RequiredArgProvider{"ctor-move", counter}));
            check(engaged.has_value(), "B21b setup: engage() succeeds");

            ProviderT moved_to(std::move(source));  // move CONSTRUCTION, not operator=

            auto from_source = agentengine::test_support::run_task_sync<result<ContextContribution>>(
                source.on_context(session_ctx, effect_ctx));
            check(!from_source.has_value() &&
                      from_source.error().code == "composed_context.not_engaged",
                  "B21b: move-CONSTRUCTION resets the source's engaged_ exactly like move-assignment "
                  "does, not left stale by a separate, unfixed code path");

            auto from_moved_to = agentengine::test_support::run_task_sync<result<ContextContribution>>(
                moved_to.on_context(session_ctx, effect_ctx));
            check(from_moved_to.has_value() && from_moved_to->messages.size() == 1,
                  "B21b: the move-constructed instance carries the source's real content");
        }

        // B21c: a THIRD generation (move -> re-engage -> move again) must not accumulate stale state
        // or corrupt contributors_ across repeated reserve()/clear() cycles.
        {
            using ProviderT = ComposedQuickstartSessionBuilder<quickstart::Provider::openai,
                                                                  InMemorySecretStore,
                                                                  RequiredArgProvider>::HistoryProviderT;
            auto counter_a = std::make_shared<std::size_t>(0);
            auto counter_b = std::make_shared<std::size_t>(0);
            ProviderT a;
            (void)a.engage(std::make_tuple(RequiredArgProvider{"gen-1", counter_a}));
            ProviderT b(std::move(a));  // generation 1 -> 2
            (void)a.engage(std::make_tuple(RequiredArgProvider{"gen-1-reengaged", counter_a}));
            ProviderT c(std::move(b));  // generation 2 -> 3 (b is now the moved-from one)

            auto from_c = agentengine::test_support::run_task_sync<result<ContextContribution>>(
                c.on_context(session_ctx, effect_ctx));
            check(from_c.has_value() && from_c->messages.size() == 1 &&
                      std::get<Text>(from_c->messages[0].content.front().value).text == "gen-1",
                  "B21c: a third-generation move carries the right content through two hops");
            auto from_b = agentengine::test_support::run_task_sync<result<ContextContribution>>(
                b.on_context(session_ctx, effect_ctx));
            check(!from_b.has_value() &&
                      from_b.error().code == "composed_context.not_engaged",
                  "B21c: the intermediate (twice-moved-from) instance is genuinely not_engaged, not "
                  "left in some stale intermediate state");
            (void)counter_b;
        }

        // B21d: a second .build() call on the SAME ComposedQuickstartSessionBuilder instance fails
        // closed -- the §2a base builder has this at B6, but §2b never had its own equivalent, even
        // though finding 11 changed the exact move machinery build()/engage() depend on.
        {
            using SingleBuilder = ComposedQuickstartSessionBuilder<quickstart::Provider::openai,
                                                                      InMemorySecretStore, RequiredArgProvider>;
            auto counter = std::make_shared<std::size_t>(0);
            SingleBuilder builder("gpt-4o-mini");
            builder.api_key_from_env("openai-api-key", "AE_TEST_QUICKSTART_KEY")
                .providers(std::make_tuple(RequiredArgProvider{"double-build", counter}));
            auto first = builder.build();
            check(first.has_value(), "B21d: first build() call succeeds");
            auto second = builder.build();
            check(!second.has_value(), "B21d: second build() call on the SAME builder fails closed");
            if (!second.has_value()) {
                check(second.error().code == "quickstart_builder.no_store",
                      "B21d: the failure is specifically 'no_store' (store_ already moved out by the "
                      "first call), matching the base builder's own B6");
            }
            if (first.has_value()) {
                auto contribution = agentengine::test_support::run_task_sync<
                    result<ContextContribution>>(
                    first->session().history_provider().on_context(session_ctx, effect_ctx));
                check(contribution.has_value() && contribution->messages.size() == 1,
                      "B21d: the first, successful Bundle remains intact and correctly engaged after "
                      "the second call fails");
            }
        }
    }

    // B22: round 8 red-team, finding 15 -- engage() must be exception-safe. If a later Ms's move
    // constructor (or make_context_provider_descriptor()'s own internal make_shared) throws partway
    // through build_contributors(), contributors_ used to be left PARTIALLY populated while engaged_
    // stayed false -- engaged_ == false does not block a retry (and a retry is exactly what a caller
    // seeing an exception has every reason to attempt), but the old shape push_back'd directly into
    // contributors_ without clearing it first, so a successful retry silently carried the FAILED
    // attempt's stale entry forward too, duplicating it on the wire.
    {
        using ProviderT = ComposedQuickstartSessionBuilder<quickstart::Provider::openai,
                                                              InMemorySecretStore, CountingProvider,
                                                              ThrowingProvider>::HistoryProviderT;
        Principal principal{"p-b22", ""};
        std::vector<Message> empty_history;
        SessionContext session_ctx{"s-b22", principal, empty_history};
        EffectContext effect_ctx{};

        CountingProvider::on_context_calls = 0;
        ProviderT lcp;

        // First attempt: CountingProvider (index 0) is pushed successfully; ThrowingProvider (index 1)
        // throws while build_contributors() extracts it -- move #1 is engage()'s own by-value
        // `providers` parameter binding (must succeed, or the throw would prove nothing about engage()
        // itself), move #2 is the extraction under test.
        ThrowingProvider::move_count = 0;
        ThrowingProvider::throw_on_move_number = 2;
        bool threw = false;
        try {
            (void)lcp.engage(std::make_tuple(CountingProvider{}, ThrowingProvider{}));
        } catch (std::exception const&) {
            threw = true;
        }
        check(threw, "B22 setup: the first engage() attempt throws partway through");

        auto after_throw = agentengine::test_support::run_task_sync<result<ContextContribution>>(
            lcp.on_context(session_ctx, effect_ctx));
        check(!after_throw.has_value() &&
                  after_throw.error().code == "composed_context.not_engaged",
              "B22: after the failed attempt, on_context() correctly fails closed (not_engaged) -- "
              "engaged_ was never set to true");
        check(CountingProvider::on_context_calls == 0,
              "B22: the failed attempt's partially-built contributor was never invoked");

        // Retry with fresh, non-throwing providers -- must succeed AND must not carry the failed
        // attempt's stale contributor forward.
        ThrowingProvider::throw_on_move_number = -1;
        auto retried = lcp.engage(std::make_tuple(CountingProvider{}, ThrowingProvider{}));
        check(retried.has_value(), "B22: the retry engage() call succeeds");

        auto after_retry = agentengine::test_support::run_task_sync<result<ContextContribution>>(
            lcp.on_context(session_ctx, effect_ctx));
        check(after_retry.has_value(), "B22: on_context() succeeds after the retry");
        check(CountingProvider::on_context_calls == 1,
              "B22 (THE FIX UNDER TEST): a single on_context() call after the retry invokes the "
              "counting contributor EXACTLY ONCE -- 2 would mean the stale contributor from the FAILED "
              "first attempt survived into the successful retry and is being invoked a second, "
              "duplicate time on the wire, exactly the hazard engage()'s own already_engaged guard "
              "exists to prevent");
    }

    std::fprintf(stderr, g_failures == 0 ? "test_session_builder: ALL PASS\n"
                                          : "test_session_builder: %d FAILURE(S)\n",
                 g_failures);
    return g_failures == 0 ? 0 : 1;
}
