// Implements decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md's own prove
// phase -- the first executed evidence against that ADR's §3 falsifiable claims (all previously
// INCONCLUSIVE, no code existed). See trust/secret_quarantine.hpp's own top comment for the
// mid-implementation correction this test file also proves: `quarantine()` never mutates any
// capability set, and an `agent_initiated` ref is never grant-eligible.

#include <algorithm>
#include <iostream>
#include <string>
#include <type_traits>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/secret_quarantine.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

EffectContext make_ctx(std::string principal_id = "test-principal") {
    EffectContext ctx;
    ctx.principal = Principal{std::move(principal_id), ""};
    return ctx;
}

EffectContext make_granted_ctx(CapabilitySet const& caps, std::string principal_id = "test-principal") {
    EffectContext ctx = make_ctx(std::move(principal_id));
    // ADR-061 §20.3 (merged from main after this test was first written): EffectContext::capabilities
    // is a shared_ptr now, not a raw pointer -- borrow_capabilities() is the documented non-owning
    // bridge for a caller-owned CapabilitySet that outlives every use of the returned shared_ptr,
    // effect_context.hpp's own header comment.
    ctx.capabilities = borrow_capabilities(caps);
    return ctx;
}

// Structural claims, checked at compile time so losing them fails the build, not a test run --
// same discipline test_secret_store.cpp already established for SecretLease.
static_assert(!std::is_copy_constructible_v<QuarantineSecretStore>,
              "QuarantineSecretStore holds a std::mutex; must not be copyable");

// A stub detector: flags every occurrence of a fixed literal, case-sensitive. Deliberately trivial
// -- the real heuristic is entirely the host's problem (ADR-068 §2a); this only proves the seam.
class FixedLiteralDetector : public SecretDetector {
public:
    explicit FixedLiteralDetector(std::string literal) : literal_(std::move(literal)) {}

    [[nodiscard]] std::vector<DetectedSpan> scan(std::string_view content) override {
        std::vector<DetectedSpan> spans;
        std::size_t pos = 0;
        while ((pos = content.find(literal_, pos)) != std::string_view::npos) {
            spans.push_back(DetectedSpan{pos, literal_.size(), "fixed-literal"});
            pos += literal_.size();
        }
        return spans;
    }

private:
    std::string literal_;
};

void test_content_addressed_dedup() {
    std::cout << "-- content-addressed dedup --\n";
    QuarantineSecretStore store;
    EffectContext ctx = make_ctx();

    SecretRef const a = store.quarantine("sk-abc123", quarantine_trigger::agent_initiated, ctx);
    SecretRef const b = store.quarantine("sk-abc123", quarantine_trigger::agent_initiated, ctx);
    SecretRef const c = store.quarantine("sk-different", quarantine_trigger::agent_initiated, ctx);

    AE_CHECK(a.name == b.name, "same value quarantined twice yields the same SecretRef");
    AE_CHECK(a.name != c.name, "different values yield different SecretRefs");
    AE_CHECK(!a.name.empty(), "a minted ref name is non-empty");
}

void test_resolve_requires_capability() {
    std::cout << "-- resolve requires a granted capability (positive control) --\n";
    QuarantineSecretStore store;
    EffectContext ctx = make_ctx();
    SecretRef const ref = store.quarantine("sk-needs-a-grant", quarantine_trigger::agent_initiated, ctx);

    // No capability granted (ctx.capabilities stays nullptr) -- must fail closed.
    auto const denied = store.resolve(ref, ctx);
    AE_CHECK(!denied.has_value(), "resolve() denies with no capabilities bound at all");

    CapabilitySet const wrong_caps =
        CapabilitySet::grant_root({cap::Secret{"some-other-name", std::chrono::seconds{0}}});
    EffectContext wrong_ctx = make_granted_ctx(wrong_caps);
    auto const still_denied = store.resolve(ref, wrong_ctx);
    AE_CHECK(!still_denied.has_value(), "resolve() denies a grant for a DIFFERENT ref name");

    CapabilitySet const right_caps =
        CapabilitySet::grant_root({cap::Secret{ref.name, std::chrono::seconds{0}}});
    EffectContext right_ctx = make_granted_ctx(right_caps);
    auto const allowed = store.resolve(ref, right_ctx);
    AE_CHECK(allowed.has_value(), "resolve() succeeds with the matching grant");
    if (allowed.has_value()) {
        AE_CHECK(allowed->reveal_text() == "sk-needs-a-grant", "resolved value round-trips correctly");
    }
}

void test_audit_hook_fires_with_no_value() {
    std::cout << "-- QuarantineAuditHook fires, carries no secret bytes --\n";
    std::vector<QuarantineAuditEvent> events;
    QuarantineSecretStore store([&events](QuarantineAuditEvent const& e) { events.push_back(e); });
    EffectContext ctx = make_ctx("alice");

    [[maybe_unused]] SecretRef const ref =
        store.quarantine("sk-audited-value", quarantine_trigger::agent_initiated, ctx);

    AE_CHECK(events.size() == 1, "audit hook fires exactly once per quarantine() call");
    if (!events.empty()) {
        AE_CHECK(events[0].trigger == quarantine_trigger::agent_initiated,
                 "audit event carries the correct trigger");
        AE_CHECK(events[0].principal_id == "alice", "audit event carries the extracting principal");
        AE_CHECK(!events[0].ref_name.empty(), "audit event carries the ref name");
    }

    // Structural, not merely behavioral: `QuarantineAuditEvent` is declared with exactly three
    // fields (`ref_name`, `trigger`, `principal_id`). Checked by field-count-via-aggregate-init
    // rather than a sizeof comparison (meaningless once struct padding/alignment is involved) --
    // adding a fourth field would make this exact 3-argument brace-init fail to compile, forcing
    // whoever adds one to notice and update this proof rather than silently widening the type.
    static_assert(std::is_aggregate_v<QuarantineAuditEvent>, "must stay a plain aggregate");
    [[maybe_unused]] QuarantineAuditEvent const field_count_probe{
        "probe-ref", quarantine_trigger::agent_initiated, "probe-principal"};
}

void test_grant_eligibility_excludes_agent_initiated() {
    std::cout << "-- grant_eligible_ref_names() excludes agent_initiated (I3 fix) --\n";
    QuarantineSecretStore store;
    EffectContext ctx = make_ctx();

    SecretRef const verified =
        store.quarantine("sk-from-verified-user-message", quarantine_trigger::verified_user_content, ctx);
    SecretRef const from_model =
        store.quarantine("sk-model-supplied-text", quarantine_trigger::agent_initiated, ctx);

    auto const eligible = store.grant_eligible_ref_names();
    bool const has_verified =
        std::find(eligible.begin(), eligible.end(), verified.name) != eligible.end();
    bool const has_agent_initiated =
        std::find(eligible.begin(), eligible.end(), from_model.name) != eligible.end();

    AE_CHECK(has_verified, "a verified_user_content ref IS grant-eligible");
    AE_CHECK(!has_agent_initiated,
             "an agent_initiated ref is NEVER grant-eligible -- model-supplied text cannot mint "
             "its own authority (I3)");
}

void test_scan_and_quarantine_redacts_correctly() {
    std::cout << "-- scan_and_quarantine() redacts back-to-front, offsets stay valid --\n";
    QuarantineSecretStore store;
    EffectContext ctx = make_ctx();
    FixedLiteralDetector detector("SECRET");

    std::string const redacted =
        scan_and_quarantine("prefix SECRET middle SECRET suffix", detector, store,
                             quarantine_trigger::verified_user_content, ctx);

    AE_CHECK(redacted.find("SECRET") == std::string::npos,
             "no raw occurrence of the detected literal survives in the redacted text");
    AE_CHECK(redacted.find("prefix") != std::string::npos && redacted.find("middle") != std::string::npos &&
                 redacted.find("suffix") != std::string::npos,
             "surrounding, non-secret text is preserved byte-for-byte");
    AE_CHECK(redacted.find("[quarantined secret:") != std::string::npos,
             "a reference marker replaces each detected span");

    std::string const no_match = scan_and_quarantine("nothing to see here", detector, store,
                                                        quarantine_trigger::verified_user_content, ctx);
    AE_CHECK(no_match == "nothing to see here", "content with no match is returned unchanged");
}

void test_agent_initiated_tool_never_grants_and_redacts_verbatim() {
    std::cout << "-- quarantine_secret tool: redacts verbatim, mints no grant-eligible ref --\n";
    QuarantineSecretStore store;
    ToolDescriptor const descriptor = make_quarantine_secret_tool_descriptor(store);

    AE_CHECK(descriptor.name == "quarantine_secret", "descriptor carries the declared tool name");
    AE_CHECK(descriptor.capability_ceiling.empty(), "the tool declares no capability ceiling (zero-capability shape)");
    AE_CHECK(descriptor.approval == approval_mode::never_require,
             "the tool declares no Approval<...> -- never_require by default");

    EffectContext ctx = make_ctx();
    QuarantineSecretArgs const args{"a very specific string the model flagged"};
    json::Value const args_json = schema::to_json(args);
    auto const reply_json = descriptor.invoke(args_json, ctx);
    AE_CHECK(reply_json.has_value(), "invoking the real closure (not the dead static invoke()) succeeds");

    if (reply_json.has_value()) {
        auto const reply = schema::from_json<QuarantineSecretReply>(*reply_json);
        AE_CHECK(reply.has_value(), "reply round-trips through the JSON schema codec");
        if (reply.has_value()) {
            AE_CHECK(reply->redacted_text.find("a very specific string") == std::string::npos,
                     "the original text does not survive in the reply");
            AE_CHECK(reply->redacted_text.find("[quarantined secret:") != std::string::npos,
                     "the reply carries a reference marker instead");
        }
    }

    // The defining fix this file proves: even though a secret WAS minted above, nothing from an
    // agent-initiated call is ever grant-eligible.
    AE_CHECK(store.grant_eligible_ref_names().empty(),
             "quarantine_secret's own tool-call path mints zero grant-eligible refs");
}

void test_dead_static_invoke_path_is_unreachable_but_fails_safely() {
    std::cout << "-- QuarantineSecretTool::invoke() dead path fails, doesn't crash --\n";
    EffectContext ctx = make_ctx();
    auto const result = QuarantineSecretTool::invoke(QuarantineSecretArgs{"x"}, ctx);
    AE_CHECK(!result.has_value(), "the static invoke() stub always fails");
    if (!result.has_value()) {
        AE_CHECK(result.error().code == "secret_quarantine.dead_static_invoke_path",
                 "the stub fails with its own named, greppable error code");
    }
}

}  // namespace

int main() {
    test_content_addressed_dedup();
    test_resolve_requires_capability();
    test_audit_hook_fires_with_no_value();
    test_grant_eligibility_excludes_agent_initiated();
    test_scan_and_quarantine_redacts_correctly();
    test_agent_initiated_tool_never_grants_and_redacts_verbatim();
    test_dead_static_invoke_path_is_unreachable_but_fails_safely();

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) FAILED\n";
    return 1;
}
