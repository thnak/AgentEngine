// Prototype prove pass for docs/planning/quickstart-session-builder-design-draft.md /
// core/session_builder.hpp -- covers §2a (gateway-wrapped-by-default client stack)/§2c (capability+
// secret sugar, fail-closed at build() time, generalized to any real SecretStore conformer -- proven
// against the project's own production AgentEngineSecretStore, not just InMemorySecretStore)/§3
// (Store/CapabilitySet lifetime safety across a move) ONLY, matching the header's own scope comment.
//
// Deliberately NOT exercised here, named as a real scope limit rather than silently skipped: no
// `.ask()`/`start_run()` call, so no real (or even attempted) network exchange happens in this test --
// `OpenAIChatClient::chat()`'s body is therefore never instantiated, which is also why this test needs
// no MbedTLS/provider_http_client link (see tests/CMakeLists.txt's own comment on this target). A
// live, real-network proof of `.ask()` end to end is separately scoped future work, matching this
// project's own "live-network" label convention for that class of test.
//
// Also NOT exercised: a genuine multi-threaded proof of the red-team's #1 finding fix (`ask_mutex_`
// serializing concurrent `.ask()` calls against a real, contended `session_mutex_`). That needs a
// ChatClient whose `chat()` coroutine genuinely suspends (this test's scope has none), plus real
// threads racing `.ask()` on one `Bundle` under repeated runs -- a real, larger test, not designed
// here. The fix is currently verified by code review against `rt/thread_pool.hpp`'s/`rt/drive_leaf_
// task.hpp`'s own documented precedent for this exact bug class, not by a live concurrency test.

#include <cstdio>
#include <string>

#include "agentengine/core/session_builder.hpp"
#include "agentengine/trust/secret_quarantine.hpp"

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
    using quickstart::OpenAiSessionBuilder;

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

    std::fprintf(stderr, g_failures == 0 ? "test_session_builder: ALL PASS\n"
                                          : "test_session_builder: %d FAILURE(S)\n",
                 g_failures);
    return g_failures == 0 ? 0 : 1;
}
