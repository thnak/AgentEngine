// Implements 004-Model-Provider-Plane.md §2's 2026-08-07 amendment; decisions/ADR-020-portable-
// reasoning-effort.md. Proves the amendment's central claim: `ChatRequest::reasoning_effort` is ONE
// portable ordinal level that each backend maps down to its OWN native mechanism -- not a vendor
// field passed through.
//
// The claim is only interesting because the two native shapes genuinely differ: OpenAI takes a flat
// string enum, Anthropic takes a `thinking:{type, budget_tokens}` TOKEN BUDGET. A test that only
// exercised the OpenAI side would prove nothing about portability -- it would prove a spelling
// function works. So both backends are driven from the SAME enumerator values in this one file,
// deliberately, rather than split across the two per-backend translation suites.
//
// Pure request-building logic: no network, no server, no secret store. The live half (does a real
// endpoint accept what we build) is ADR-020 §2's recorded probe evidence and the `live-network`
// suites; this file is the deterministic half and runs in the default suite.

#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>

#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"

using namespace agentengine;

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

[[nodiscard]] ChatRequest request_with(std::optional<reasoning_effort> effort) {
    ChatRequest req;
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{"hello"};
    m.content.push_back(std::move(item));
    req.messages.push_back(std::move(m));
    req.reasoning_effort = effort;
    return req;
}

// A backend that CAN reason. `max_output_tokens` is the number Anthropic's budget is a fraction of,
// so it is set explicitly here rather than left to fall back on `kDefaultMaxTokens` -- the arithmetic
// under test should be visible in the fixture, not implied by a default.
[[nodiscard]] ChatClientCapabilities reasoning_caps(std::uint64_t max_output_tokens = 8000) {
    ChatClientCapabilities caps;
    caps.reasoning = true;
    caps.max_output_tokens = max_output_tokens;
    return caps;
}

[[nodiscard]] std::string openai_effort_field(json::Value const& body) {
    auto const* v = body.find("reasoning_effort");
    return (v && v->is_string()) ? v->as_string() : std::string{};
}

}  // namespace

int main() {
    using namespace agentengine::openai::detail;
    namespace ant = agentengine::anthropic::detail;

    // ---- G1: `nullopt` is bit-for-bit today's behaviour -- the amendment is additive -------------
    // The single most important case: every pre-existing caller in the codebase leaves this field
    // unset, so if `nullopt` emitted anything at all, this change would have altered every request
    // both backends have ever built.
    {
        auto oa = build_request_body(request_with(std::nullopt), "gpt-5", /*stream=*/false, {},
                                      std::nullopt, reasoning_caps());
        check(oa.has_value(), "G1: OpenAI body builds with no reasoning_effort set");
        if (oa) {
            check(oa->find("reasoning_effort") == nullptr,
                  "G1: OpenAI emits NO `reasoning_effort` key when the request expresses no opinion -- "
                  "`nullopt` means 'take the vendor default', not 'send a default value'");
        }
        auto an = ant::build_request_body(request_with(std::nullopt), "claude-sonnet-5", reasoning_caps(),
                                           /*stream=*/false);
        check(an.has_value(), "G1: Anthropic body builds with no reasoning_effort set");
        if (an) {
            check(an->find("thinking") == nullptr,
                  "G1: Anthropic emits NO `thinking` object when the request expresses no opinion");
        }
    }

    // ---- G2: one portable level, two structurally different native shapes -----------------------
    // This is the amendment's actual claim. Same enumerator in; a STRING out of one backend and an
    // OBJECT CARRYING A COMPUTED NUMBER out of the other.
    {
        auto oa = build_request_body(request_with(reasoning_effort::high), "gpt-5", /*stream=*/false, {},
                                      std::nullopt, reasoning_caps());
        check(oa.has_value(), "G2: OpenAI body builds for effort=high");
        if (oa) {
            check(openai_effort_field(*oa) == "high",
                  "G2: OpenAI maps the portable level to its flat string enum (`reasoning_effort`:\"high\")");
        }

        auto an = ant::build_request_body(request_with(reasoning_effort::high), "claude-sonnet-5",
                                           reasoning_caps(8000), /*stream=*/false);
        check(an.has_value(), "G2: Anthropic body builds for effort=high");
        if (an) {
            auto const* thinking = an->find("thinking");
            check(thinking && thinking->is_object(),
                  "G2: Anthropic maps the SAME portable level to an OBJECT, not a string -- the two "
                  "native shapes genuinely differ, which is what makes this an abstraction");
            if (thinking && thinking->is_object()) {
                auto const* type = thinking->find("type");
                check(type && type->is_string() && type->as_string() == "enabled",
                      "G2: Anthropic's thinking block is `type:\"enabled\"` for a nonzero level");
                auto const* budget = thinking->find("budget_tokens");
                // 75% of 8000. A LEVEL had to become a NUMBER -- no string mapping could have done this.
                check(budget && budget->is_number() &&
                          static_cast<std::uint64_t>(budget->as_number()) == 6000,
                      "G2: Anthropic's budget is computed as a fraction of THIS request's own max_tokens "
                      "(high = 75% of 8000 = 6000), not a fixed absolute that could overshoot it");
            }
        }
    }

    // ---- G3: the ordinal is honoured -- levels are strictly ordered, per backend ----------------
    // A mapping that returned the same thing for every level would pass G2 and be useless.
    {
        std::uint64_t budgets[3] = {0, 0, 0};
        reasoning_effort const levels[3] = {reasoning_effort::low, reasoning_effort::medium,
                                            reasoning_effort::high};
        char const* names[3] = {"low", "medium", "high"};
        for (int i = 0; i < 3; ++i) {
            auto oa = build_request_body(request_with(levels[i]), "gpt-5", /*stream=*/false, {},
                                          std::nullopt, reasoning_caps());
            check(oa.has_value() && openai_effort_field(*oa) == names[i],
                  "G3: OpenAI emits the matching level string for each enumerator");

            auto an = ant::build_request_body(request_with(levels[i]), "claude-sonnet-5",
                                               reasoning_caps(8000), /*stream=*/false);
            if (an) {
                auto const* thinking = an->find("thinking");
                auto const* budget = thinking ? thinking->find("budget_tokens") : nullptr;
                if (budget && budget->is_number()) {
                    budgets[i] = static_cast<std::uint64_t>(budget->as_number());
                }
            }
        }
        check(budgets[0] == 2000 && budgets[1] == 4000 && budgets[2] == 6000,
              "G3: Anthropic's budgets are 25/50/75% of max_tokens (2000/4000/6000 of 8000)");
        check(budgets[0] < budgets[1] && budgets[1] < budgets[2],
              "G3: the ordinal survives translation -- low < medium < high as real token budgets, not "
              "three spellings of one value");
    }

    // ---- G4: `off` is a REQUEST, not the absence of one -----------------------------------------
    // 004 §2's amendment turns on this distinction. If `off` and `nullopt` produced the same wire
    // bytes, the four-value enum would be a three-value enum with a redundant name.
    {
        auto oa = build_request_body(request_with(reasoning_effort::off), "gpt-5", /*stream=*/false, {},
                                      std::nullopt, reasoning_caps());
        check(oa.has_value() && openai_effort_field(*oa) == "none",
              "G4: OpenAI maps `off` to its own spelling `\"none\"` -- an explicit disable, distinct "
              "from G1's emit-nothing");

        auto an = ant::build_request_body(request_with(reasoning_effort::off), "claude-sonnet-5",
                                           reasoning_caps(), /*stream=*/false);
        check(an.has_value(), "G4: Anthropic body builds for effort=off");
        if (an) {
            auto const* thinking = an->find("thinking");
            check(thinking && thinking->is_object(),
                  "G4: Anthropic emits a thinking object for `off` (not nothing -- that would be G1)");
            if (thinking && thinking->is_object()) {
                auto const* type = thinking->find("type");
                check(type && type->is_string() && type->as_string() == "disabled",
                      "G4: Anthropic maps `off` to `type:\"disabled\"` -- its own native way to say the "
                      "same thing OpenAI says with \"none\"");
                check(thinking->find("budget_tokens") == nullptr,
                      "G4: a disabled thinking block carries NO budget_tokens -- Anthropic's own shape, "
                      "not a zero-valued budget invented to keep the object uniform");
            }
        }
    }

    // ---- G5: NEGATIVE CONTROL -- the capability gate actually fires ------------------------------
    // 004 §2's degradation rule: no declared fallback for reasoning effort exists, so a backend that
    // cannot reason must REFUSE rather than drop the field. Without this control the gate could be
    // absent entirely and every case above would still pass.
    {
        ChatClientCapabilities no_reasoning;  // reasoning == false
        no_reasoning.max_output_tokens = 8000;

        for (auto level : {reasoning_effort::low, reasoning_effort::medium, reasoning_effort::high}) {
            auto oa = build_request_body(request_with(level), "gpt-5", /*stream=*/false, {}, std::nullopt,
                                          no_reasoning);
            check(!oa.has_value() && oa.error().code == "openai.reasoning_not_supported" &&
                      oa.error().klass == failure_class::contract,
                  "G5: OpenAI REFUSES a reasoning level against a backend lacking the `reasoning` bit, "
                  "with a contract-class error -- it never silently drops the field");

            auto an = ant::build_request_body(request_with(level), "claude-sonnet-5", no_reasoning,
                                               /*stream=*/false);
            check(!an.has_value() && an.error().code == "anthropic.reasoning_not_supported" &&
                      an.error().klass == failure_class::contract,
                  "G5: Anthropic REFUSES the same request the same way -- one rule, both backends");
        }

        // The deliberate asymmetry (004 §2's amendment states it explicitly): `off` is always
        // satisfiable. A backend that cannot reason already does what `off` asks for, so gating it
        // would fail a caller who sets `off` defensively across a fleet of mixed backends.
        auto oa_off = build_request_body(request_with(reasoning_effort::off), "gpt-5", /*stream=*/false,
                                          {}, std::nullopt, no_reasoning);
        check(oa_off.has_value() && openai_effort_field(*oa_off) == "none",
              "G5: `off` is EXEMPT from the gate on OpenAI -- asking a non-reasoning backend not to "
              "reason is satisfiable by construction");
        auto an_off = ant::build_request_body(request_with(reasoning_effort::off), "claude-sonnet-5",
                                               no_reasoning, /*stream=*/false);
        check(an_off.has_value() && an_off->find("thinking") != nullptr,
              "G5: `off` is EXEMPT from the gate on Anthropic too");
    }

    // ---- G6: NEGATIVE CONTROL -- Anthropic's own vendor constraints are enforced CLIENT-SIDE -----
    // ADR-020 §2 measured OpenRouter returning HTTP 200 for `budget_tokens == max_tokens` and for
    // `budget_tokens == 512`, both of which api.anthropic.com rejects. A gateway will not catch this
    // for us, so a request that cannot satisfy both `budget >= 1024` and `budget < max_tokens` must
    // fail here rather than work against the lenient hop and break against the strict one.
    {
        // max_tokens == 1024: the 1024 floor is not strictly LESS than it, so no value satisfies both.
        auto an = ant::build_request_body(request_with(reasoning_effort::low), "claude-sonnet-5",
                                           reasoning_caps(1024), /*stream=*/false);
        check(!an.has_value() && an.error().code == "anthropic.thinking_budget_unsatisfiable" &&
                  an.error().klass == failure_class::contract,
              "G6: Anthropic fails closed when max_tokens leaves no budget satisfying BOTH vendor "
              "constraints -- rather than emitting the 200-from-a-gateway/400-from-the-vendor request");

        // max_tokens == 1025: 25% of 1025 is 256, below the floor, so it clamps UP to 1024 -- which
        // now IS strictly less than max_tokens. The boundary is real and one token wide.
        auto ok = ant::build_request_body(request_with(reasoning_effort::low), "claude-sonnet-5",
                                           reasoning_caps(1025), /*stream=*/false);
        check(ok.has_value(), "G6: one token above the boundary, the same request succeeds");
        if (ok) {
            auto const* thinking = ok->find("thinking");
            auto const* budget = thinking ? thinking->find("budget_tokens") : nullptr;
            check(budget && budget->is_number() &&
                      static_cast<std::uint64_t>(budget->as_number()) == 1024,
                  "G6: a fraction below Anthropic's 1024 floor clamps UP to the floor, never below it");
        }
    }

    // ---- G7: the gate holds on BOTH entry points, not just chat() -------------------------------
    // `chat()` and `chat_stream()` build their bodies through the same function but with separately
    // written argument lists; a capability check that held on one of the two would be no check.
    {
        ChatClientCapabilities no_reasoning;
        no_reasoning.max_output_tokens = 8000;
        auto oa = build_request_body(request_with(reasoning_effort::high), "gpt-5", /*stream=*/true, {},
                                      std::nullopt, no_reasoning);
        check(!oa.has_value() && oa.error().code == "openai.reasoning_not_supported",
              "G7: OpenAI's streaming body build enforces the identical gate");
        auto an = ant::build_request_body(request_with(reasoning_effort::high), "claude-sonnet-5",
                                           no_reasoning, /*stream=*/true);
        check(!an.has_value() && an.error().code == "anthropic.reasoning_not_supported",
              "G7: Anthropic's streaming body build enforces the identical gate");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_reasoning_effort_portability: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_reasoning_effort_portability: %d FAILURE(S)\n", g_failures);
    return 1;
}
