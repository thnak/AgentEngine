// Milestone 5 Phase F3 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, Phase F's
// "Design decisions (2026-08-07...)" block): proves core/failover_chat_client.hpp's
// `FailoverChatClient<Primary, Fallback...>` against hand-rolled `ChatClient` conformers with
// scriptable canned behavior -- no real network call, mirroring test_recorded_chat_client.cpp's own
// "no live backend needed to exercise the seam" shape.
//
// Uses a manual check()/g_failures counter (test_chat_client_stream.cpp's own pattern), NOT plain
// assert(): this repo's local build config is RelWithDebInfo (build/CMakeCache.txt), which defines
// NDEBUG -- under NDEBUG, assert() compiles to nothing, so an assert()-based test would silently pass
// regardless of whether FailoverChatClient is actually correct. Confirmed by direct compilation
// during this task (a stray unused-variable warning on an assert()-only local surfaced exactly this).
//
// `ScriptedChatClient<Tag>` is a class TEMPLATE, not one reused type, because
// FailoverChatClient<Primary, Fallback...> requires Primary and every Fallback to be distinct C++
// types (the file-top comment in failover_chat_client.hpp explains why: the failover order is
// expressed by the pack's declaration order alone, which is meaningless if the same backend type
// appears twice) -- an empty tag type per chain position is enough to make each instantiation a
// distinct type while sharing one implementation.
//
// Each conformer optionally takes a `std::shared_ptr<int>` call counter: because
// FailoverChatClient owns its Primary/Fallback members BY VALUE (moved in at construction), a test
// cannot inspect the moved-from local variable afterward to prove "was this backend actually
// called" -- the shared_ptr is an external side channel that survives the move, incremented from
// inside chat() every time it runs.

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/failover_chat_client.hpp"
#include "agentengine/trust/principal.hpp"
#include "support/run_task_sync.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// Distinct tag types -- see file-top comment for why ScriptedChatClient must be instantiated once
// per chain position rather than reused as one shared type.
struct PrimaryTag {};
struct Fallback1Tag {};
struct Fallback2Tag {};

template <class Tag>
class ScriptedChatClient {
public:
    ScriptedChatClient(std::string name, bool succeed, std::string text_or_code,
                        ae::ChatClientCapabilities caps = {},
                        std::shared_ptr<int> call_count = nullptr)
        : name_(std::move(name)),
          succeed_(succeed),
          text_or_code_(std::move(text_or_code)),
          caps_(caps),
          call_count_(std::move(call_count)) {}

    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return caps_; }

    // A raw conformer never sets ChatResponse::fallback_tier (chat_client.hpp: "0 = the primary
    // backend answered" is the honest default for anything that ISN'T FailoverChatClient itself) --
    // left at its default 0 here deliberately, so test_capabilities_and_tier_authority below proves
    // the wrapper OVERWRITES it rather than merely happening to agree with it.
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        if (call_count_) ++(*call_count_);
        if (succeed_) {
            ae::ChatResponse resp{};
            resp.message.role = ae::role::assistant;
            ae::ContentItem item{};
            item.origin = ae::content_origin::assistant;
            item.value = ae::Text{text_or_code_};
            resp.message.content.push_back(std::move(item));
            resp.model = name_;
            co_return resp;
        }
        co_return std::unexpected(ae::error{ae::failure_class::transient, name_ + " failed", text_or_code_});
    }

    // Streaming is out of scope for this test file -- failover_chat_client.hpp's own file-top comment
    // names chat_stream() as Primary-only, unconditionally, so there is nothing chain-order-dependent
    // to prove here; an empty, invalid stream is enough to satisfy the ChatClient concept.
    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        return {};
    }

private:
    std::string name_;
    bool succeed_;
    std::string text_or_code_;
    ae::ChatClientCapabilities caps_;
    std::shared_ptr<int> call_count_;
};

static_assert(ae::ChatClient<ScriptedChatClient<PrimaryTag>>,
              "ScriptedChatClient must itself satisfy ChatClient (004 §1) before it can stand in as "
              "a FailoverChatClient chain member");

using PrimaryClient = ScriptedChatClient<PrimaryTag>;
using Fallback1Client = ScriptedChatClient<Fallback1Tag>;
using Fallback2Client = ScriptedChatClient<Fallback2Tag>;

static_assert(ae::ChatClient<ae::FailoverChatClient<PrimaryClient, Fallback1Client>>,
              "FailoverChatClient must itself satisfy ChatClient (004 §1) -- the wrapper is a "
              "conformer too, never a base class or type-erased decorator");

ae::EffectContext make_ctx() {
    ae::EffectContext ctx{};
    ctx.principal = ae::Principal{"p-1", "tenant-1"};
    ctx.trace_id = "trace-1";
    ctx.span_id = "span-1";
    return ctx;
}

// ---- (1) primary succeeds -> fallback_tier == 0, fallback is never called -----------------------
void test_primary_succeeds_fallback_never_called() {
    auto primary_calls = std::make_shared<int>(0);
    auto fallback_calls = std::make_shared<int>(0);

    PrimaryClient primary("primary", /*succeed=*/true, "primary reply", {}, primary_calls);
    Fallback1Client fallback("fallback-1", /*succeed=*/true, "fallback reply", {}, fallback_calls);

    ae::FailoverChatClient<PrimaryClient, Fallback1Client> client(std::move(primary), std::move(fallback));
    ae::EffectContext ctx = make_ctx();
    ae::ChatRequest request{};

    auto result = ae::test_support::run_task_sync<ae::result<ae::ChatResponse>>(client.chat(request, ctx));

    check(result.has_value(), "T1: primary success -> overall success");
    if (result.has_value()) {
        check(result->fallback_tier == 0, "T1: fallback_tier == 0 (primary answered)");
    }
    check(*primary_calls == 1, "T1: primary was called exactly once");
    check(*fallback_calls == 0, "T1: fallback is never called once primary already succeeded");
}

// ---- (2) primary fails, first fallback succeeds -> fallback_tier == 1, primary WAS called --------
void test_primary_fails_first_fallback_succeeds() {
    auto primary_calls = std::make_shared<int>(0);
    auto fallback_calls = std::make_shared<int>(0);

    PrimaryClient primary("primary", /*succeed=*/false, "E_PRIMARY_DOWN", {}, primary_calls);
    Fallback1Client fallback("fallback-1", /*succeed=*/true, "fallback reply", {}, fallback_calls);

    ae::FailoverChatClient<PrimaryClient, Fallback1Client> client(std::move(primary), std::move(fallback));
    ae::EffectContext ctx = make_ctx();
    ae::ChatRequest request{};

    auto result = ae::test_support::run_task_sync<ae::result<ae::ChatResponse>>(client.chat(request, ctx));

    check(result.has_value(), "T2: first fallback success -> overall success");
    check(*primary_calls == 1, "T2: primary WAS called (proves it wasn't skipped)");
    check(*fallback_calls == 1, "T2: fallback attempted exactly once (never retried -- that's F1's job)");
    if (result.has_value()) {
        check(result->fallback_tier == 1, "T2: fallback_tier == 1 (first fallback answered)");
        auto const* text = std::get_if<ae::Text>(&result->message.content.front().value);
        check(text != nullptr, "T2: response carries a Text content item");
        if (text) check(text->text == "fallback reply", "T2: response text came from the fallback, not the primary");
    }
}

// ---- (3) primary and first fallback both fail, second fallback succeeds -> fallback_tier == 2 ----
void test_two_failures_then_second_fallback_succeeds() {
    auto primary_calls = std::make_shared<int>(0);
    auto fb1_calls = std::make_shared<int>(0);
    auto fb2_calls = std::make_shared<int>(0);

    PrimaryClient primary("primary", /*succeed=*/false, "E_PRIMARY_DOWN", {}, primary_calls);
    Fallback1Client fb1("fallback-1", /*succeed=*/false, "E_FB1_DOWN", {}, fb1_calls);
    Fallback2Client fb2("fallback-2", /*succeed=*/true, "fb2 reply", {}, fb2_calls);

    ae::FailoverChatClient<PrimaryClient, Fallback1Client, Fallback2Client> client(
        std::move(primary), std::move(fb1), std::move(fb2));
    ae::EffectContext ctx = make_ctx();
    ae::ChatRequest request{};

    auto result = ae::test_support::run_task_sync<ae::result<ae::ChatResponse>>(client.chat(request, ctx));

    check(result.has_value(), "T3: second fallback success -> overall success");
    if (result.has_value()) {
        check(result->fallback_tier == 2, "T3: fallback_tier == 2 (second fallback answered)");
    }
    check(*primary_calls == 1, "T3: primary was attempted");
    check(*fb1_calls == 1, "T3: first fallback was attempted");
    check(*fb2_calls == 1, "T3: second fallback was attempted exactly once");
}

// ---- (4) every backend fails -> returns the LAST backend's error, not the first ------------------
void test_all_fail_returns_last_error() {
    PrimaryClient primary("primary", /*succeed=*/false, "E_PRIMARY_DOWN");
    Fallback1Client fb1("fallback-1", /*succeed=*/false, "E_FB1_DOWN");
    Fallback2Client fb2("fallback-2", /*succeed=*/false, "E_FB2_DOWN");

    ae::FailoverChatClient<PrimaryClient, Fallback1Client, Fallback2Client> client(
        std::move(primary), std::move(fb1), std::move(fb2));
    ae::EffectContext ctx = make_ctx();
    ae::ChatRequest request{};

    auto result = ae::test_support::run_task_sync<ae::result<ae::ChatResponse>>(client.chat(request, ctx));

    check(!result.has_value(), "T4: all backends failing -> overall failure");
    if (!result.has_value()) {
        // The LAST attempted backend's error (fallback-2's), never the primary's or fallback-1's --
        // "the most final failure," per this file's own task brief and failover_chat_client.hpp's
        // chat() doc comment.
        check(result.error().code == "E_FB2_DOWN", "T4: error code is the LAST backend's, not the first");
        check(result.error().message == "fallback-2 failed", "T4: error message is the LAST backend's");
        check(result.error().klass == ae::failure_class::transient, "T4: error class is preserved");
    }
}

// ---- (5) capabilities() reflects the primary's, never a fallback's -------------------------------
void test_capabilities_and_tier_authority() {
    ae::ChatClientCapabilities primary_caps{};
    primary_caps.tool_calling = true;
    primary_caps.streaming = false;
    primary_caps.context_window = 128000;

    ae::ChatClientCapabilities fallback_caps{};
    fallback_caps.tool_calling = false;  // deliberately the opposite of primary's, so a wrapper that
    fallback_caps.streaming = true;      // wrongly merged/used the fallback's capabilities instead of
    fallback_caps.context_window = 8000; // the primary's would fail every assertion below, not just one.

    PrimaryClient primary("primary", /*succeed=*/true, "ok", primary_caps);
    Fallback1Client fallback("fallback-1", /*succeed=*/true, "ok", fallback_caps);

    ae::FailoverChatClient<PrimaryClient, Fallback1Client> client(std::move(primary), std::move(fallback));

    ae::ChatClientCapabilities reported = client.capabilities();
    check(reported.tool_calling == true, "T5: capabilities().tool_calling matches the PRIMARY's (true), not the fallback's");
    check(reported.streaming == false, "T5: capabilities().streaming matches the PRIMARY's (false), not the fallback's");
    check(reported.context_window == 128000, "T5: capabilities().context_window matches the PRIMARY's (128000), not the fallback's (8000)");
}

}  // namespace

int main() {
    test_primary_succeeds_fallback_never_called();
    test_primary_fails_first_fallback_succeeds();
    test_two_failures_then_second_fallback_succeeds();
    test_all_fail_returns_last_error();
    test_capabilities_and_tier_authority();

    if (g_failures == 0) {
        std::fprintf(stderr, "test_failover_chat_client: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_failover_chat_client: %d FAILURE(S)\n", g_failures);
    return 1;
}
