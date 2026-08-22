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

    // ---- B5: the GENERIC path (.api_key() + .store()) works with the REAL production store type,
    // AgentEngineSecretStore over EnvSecretSource -- not merely InMemorySecretStore-shaped. This closes
    // the finding named in the header's own top comment (finding 3): AgentEngineSecretStore has no
    // default constructor and no .set(), so .api_key_from_env() cannot even be NAMED against it (a
    // compile-time `requires` gate, not a runtime check) -- .api_key()+.store() is the only path for
    // this Store, and this proves it actually works end to end, against the real type, not a stand-in.
    {
#if defined(_WIN32)
        _putenv_s("QUARK_SECRET_prod-api-key", "sk-prod-value-789");
#else
        setenv("QUARK_SECRET_prod-api-key", "sk-prod-value-789", 1);
#endif
        AgentEngineSecretStore prod_store(std::make_unique<EnvSecretSource>());
        quickstart::QuickstartSessionBuilder<quickstart::Provider::openai, AgentEngineSecretStore>
            builder("gpt-4o-mini");
        builder.api_key(SecretRef{"prod-api-key"});
        builder.store(std::move(prod_store));
        auto built = builder.build();
        check(built.has_value(), "the generic .api_key()+.store() path builds successfully against the "
                                  "REAL production AgentEngineSecretStore, not just InMemorySecretStore");
        if (built.has_value()) {
            check(built->session().has_chat_client(),
                  "the built session (real production Store type) has a ChatClientT emplaced");
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

    std::fprintf(stderr, g_failures == 0 ? "test_session_builder_prototype: ALL PASS\n"
                                          : "test_session_builder_prototype: %d FAILURE(S)\n",
                 g_failures);
    return g_failures == 0 ? 0 : 1;
}
