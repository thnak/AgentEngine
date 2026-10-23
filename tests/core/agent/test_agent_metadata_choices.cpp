// Proof for decisions/ADR-244-required-choices-step-1b.md §2.1/§2.2 (slice 4b, G-C1/G-C2/G-C3): the five choices
// 002 §3 used to default -- turn cap, token budget, approval, concurrency, strictness -- are stated or the agent
// does not validate, on every surface that produces an `AgentMetadata` (I6).
//
//   M1 (positive control) -- an `AgentMetadata` stating all five validates. Every check below is a rejection, so
//         acceptance is established first.
//   M2 -- each of the five, left unset, fails `choice.missing.<field>` and the message lists the options.
//   M3 -- `at_most(0)` is refused (`choice.zero_bound`) for the turn cap and the budget; `unbounded()` is a stated
//         value and validates, and is recorded as unbounded (not as "nothing chosen").
//   M4 -- the CRTP surface: `MaxTurns<at_most(7)>` compiles to 7, `MaxTurns<unbounded()>` and
//         `TokenBudget<unbounded()>` compile to an explicit unbounded, and a hand-spelled `BoundValue{0, false}`
//         (the one way past `at_most`'s consteval) is refused at registration.
//   M5 -- the declarative surface: a YAML document without `limits`, `approval`, `concurrency` or `sandbox` compiles
//         (the compiler states nothing for the author) and is refused at validation, naming the first missing
//         choice; the same document with all five stated validates (I6: the same rule on both surfaces).
//   M6 -- the bridge: `bind_agent_session` refuses a metadata with an unset choice before building a session.
//   M7 -- an empty sandbox registry is `choice.missing.sandbox_backend` on both entry points.
//   (A spawn target with no stated turn cap is refused before anything is spent: test_rt_spawn_authority P4.)
//
// Planted mutants (each must fail this test, then be reverted): in `check_required_choices` drop the
// `sandbox_profile` line (M2); make `validate_agent_metadata` skip `check_required_choices` (M2, M5, M6); make
// `at_most(0)` return `{0, false}` instead of throwing and drop the `check_bound` calls (M3, M4).
//
// Needs no daemon, no network and no credentials.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/agent_yaml_compiler.hpp"
#include "agentengine/core/yaml_value.hpp"
#include "agentengine/testing/scripted_chat_client.hpp"
#include "../../support/agent_options.hpp"

namespace ae   = agentengine;
namespace yaml = agentengine::yaml;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

using B = ae::Bound<std::uint64_t>;

// A metadata that validates, so a test unsets exactly one field.
ae::AgentMetadata stated() {
    ae::AgentMetadata meta = ae_test::stated_metadata();
    meta.agent_name        = "choices";
    meta.chat_client_id    = "test:scripted";
    return meta;
}

std::string code_of(ae::result<void> const& r) { return r ? std::string{} : r.error().code; }

struct UnboundedAgent
    : ae::Agent<UnboundedAgent, ae::ChatClientId<"test:scripted">, ae::MaxTurns<ae::unbounded()>,
                ae::TokenBudget<ae::unbounded()>, ae::Approval<ae::approval_mode::never_require>,
                ae::Concurrency<ae::concurrency_mode::parallel>, ae::SandboxProfile<ae::Strict>> {
    static constexpr std::string_view name         = "unbounded";
    static constexpr std::string_view instructions = "States every choice, two of them unbounded.";
};

struct BoundedAgent
    : ae::Agent<BoundedAgent, ae::ChatClientId<"test:scripted">, ae::MaxTurns<ae::at_most(7)>,
                ae::TokenBudget<ae::at_most(900)>, ae::Approval<ae::approval_mode::always_require>,
                ae::Concurrency<ae::concurrency_mode::sequential>, ae::SandboxProfile<ae::Strict>> {
    static constexpr std::string_view name         = "bounded";
    static constexpr std::string_view instructions = "States every choice with a number.";
};

// The one way past `at_most`'s consteval: spell the literal by hand. Registration must still refuse it.
struct HandSpelledZeroAgent
    : ae::Agent<HandSpelledZeroAgent, ae::ChatClientId<"test:scripted">,
                ae::MaxTurns<ae::BoundValue<std::uint64_t>{0, false}>, ae::TokenBudget<ae::unbounded()>,
                ae::Approval<ae::approval_mode::policy_driven>, ae::Concurrency<ae::concurrency_mode::sequential>,
                ae::SandboxProfile<ae::Strict>> {
    static constexpr std::string_view name         = "hand-spelled-zero";
    static constexpr std::string_view instructions = "A zero turn cap, spelled around the consteval.";
};

std::string doc(std::string_view spec_body) {
    return "apiVersion: agentengine/v1\nkind: Agent\nmetadata:\n  id: yaml-agent\nspec:\n  instructions: hi\n"
           "  provider:\n    model: test:scripted\n" +
           std::string(spec_body);
}

constexpr std::string_view kAllFive =
    "  limits:\n    max_turns: 4\n    token_budget: 100\n  approval: policy_driven\n"
    "  concurrency: sequential\n  sandbox:\n    profile: strict\n";

}  // namespace

int main() {
    // ---- M1: the positive control.
    {
        auto ok = ae::validate_agent_metadata(stated(), nullptr, ae_test::sandboxes());
        check(ok.has_value(), "M1 (positive control): a metadata stating all five choices validates");
    }

    // ---- M2: each choice, left unset, is a named error listing its options.
    {
        struct Case {
            char const*                         field;
            void (*unset)(ae::AgentMetadata&);
            char const*                         an_option;
        };
        Case const cases[] = {
            {"max_turns", [](ae::AgentMetadata& m) { m.max_turns.reset(); }, "unbounded()"},
            {"token_budget", [](ae::AgentMetadata& m) { m.token_budget.reset(); }, "unbounded()"},
            {"approval", [](ae::AgentMetadata& m) { m.approval.reset(); }, "policy_driven"},
            {"concurrency", [](ae::AgentMetadata& m) { m.concurrency.reset(); }, "parallel"},
            {"sandbox_profile", [](ae::AgentMetadata& m) { m.sandbox_profile.reset(); }, "strict"},
        };
        for (Case const& c : cases) {
            ae::AgentMetadata meta = stated();
            c.unset(meta);
            auto r = ae::validate_agent_metadata(meta, nullptr, ae_test::sandboxes());
            std::string const want = std::string("choice.missing.") + c.field;
            check(!r.has_value() && r.error().code == want,
                  std::string("M2: an unset ") + c.field + " fails " + want);
            check(!r.has_value() && r.error().message.find(c.an_option) != std::string::npos,
                  std::string("M2: ... and the message lists the options (names ") + c.an_option + ")");
        }
    }

    // ---- M3: zero is refused; unbounded() is a value.
    {
        ae::AgentMetadata zero_turns = stated();
        zero_turns.max_turns         = B::at_most(0);
        check(code_of(ae::validate_agent_metadata(zero_turns, nullptr, ae_test::sandboxes())) == "choice.zero_bound",
              "M3: max_turns of at_most(0) is refused with choice.zero_bound");
        ae::AgentMetadata zero_budget = stated();
        zero_budget.token_budget      = B::at_most(0);
        check(code_of(ae::validate_agent_metadata(zero_budget, nullptr, ae_test::sandboxes())) == "choice.zero_bound",
              "M3: token_budget of at_most(0) is refused with choice.zero_bound");
        ae::AgentMetadata unbounded = stated();
        unbounded.max_turns         = B::unbounded();
        unbounded.token_budget      = B::unbounded();
        check(ae::validate_agent_metadata(unbounded, nullptr, ae_test::sandboxes()).has_value(),
              "M3 control: unbounded() for both validates -- \"no limit\" is a stated value");
        check(unbounded.max_turns.has_value() && unbounded.max_turns->is_unbounded(),
              "M3: ... and it is recorded as unbounded, not as nothing chosen");
    }

    // ---- M4: the CRTP surface.
    {
        auto bounded = ae::register_agent<BoundedAgent>(nullptr, ae_test::sandboxes());
        check(bounded.has_value() && bounded->max_turns == B::at_most(7) && bounded->token_budget == B::at_most(900),
              "M4: MaxTurns<at_most(7)> and TokenBudget<at_most(900)> compile to 7 and 900");
        check(bounded.has_value() && bounded->approval == ae::approval_mode::always_require &&
                  bounded->concurrency == ae::concurrency_mode::sequential && bounded->sandbox_profile.has_value() &&
                  bounded->sandbox_profile->is_strict,
              "M4: approval, concurrency and strictness are carried as stated");
        auto unbounded = ae::register_agent<UnboundedAgent>(nullptr, ae_test::sandboxes());
        check(unbounded.has_value() && unbounded->max_turns == B::unbounded() &&
                  unbounded->token_budget == B::unbounded() && unbounded->concurrency == ae::concurrency_mode::parallel,
              "M4: MaxTurns<unbounded()> and TokenBudget<unbounded()> compile to an explicit unbounded");
        auto zero = ae::register_agent<HandSpelledZeroAgent>(nullptr, ae_test::sandboxes());
        check(!zero.has_value() && zero.error().code == "choice.zero_bound",
              "M4: a hand-spelled BoundValue{0, false} is refused at registration (the consteval is not the only gate)");
    }

    // ---- M5: the declarative surface -- the same rule.
    {
        auto bare = ae::compile_agent_document(*yaml::parse(doc("")));
        check(bare.has_value(), "M5: a document stating none of the five still compiles (the compiler decides nothing)");
        if (bare.has_value()) {
            bare->chat_client_id = "test:scripted";
            check(!bare->max_turns && !bare->token_budget && !bare->approval && !bare->concurrency &&
                      !bare->sandbox_profile,
                  "M5: ... leaving all five unset, not defaulted");
            check(code_of(ae::validate_agent_metadata(*bare, nullptr, ae_test::sandboxes())) == "choice.missing.max_turns",
                  "M5: ... and validation names the first missing choice");
        }
        auto full = ae::compile_agent_document(*yaml::parse(doc(kAllFive)));
        check(full.has_value() && ae::validate_agent_metadata(*full, nullptr, ae_test::sandboxes()).has_value(),
              "M5 control: the same document stating all five validates");
        auto partial = ae::compile_agent_document(
            *yaml::parse(doc("  limits:\n    max_turns: 4\n    token_budget: 100\n  approval: policy_driven\n")));
        check(partial.has_value() && code_of(ae::validate_agent_metadata(*partial, nullptr, ae_test::sandboxes())) == "choice.missing.concurrency",
              "M5: a document missing only concurrency fails choice.missing.concurrency");
        // ADR-244 slice 4c: a declared fallback names backends the host registered.
        ae::AgentMetadata registered = stated();
        registered.sandbox_fallback  = {"test-stub"};
        check(ae::validate_agent_metadata(registered, nullptr, ae_test::sandboxes()).has_value(),
              "M5b control: a fallback naming a registered backend validates");
        ae::AgentMetadata unregistered = stated();
        unregistered.sandbox_fallback  = {"test-stub", "no-such-backend"};
        check(code_of(ae::validate_agent_metadata(unregistered, nullptr, ae_test::sandboxes())) ==
                  "sandbox_backend_registry.name_not_found",
              "M5b: a fallback naming an unregistered backend is refused with the registry's own error");
    }

    // ---- M6: the bridge refuses before building a session.
    {
        ae::AgentMetadata meta = stated();
        meta.approval.reset();
        ae::AgentSessionOptions opts = ae_test::declared_options(stated());
        auto bound = ae::bind_agent_session(meta, ae::testing::ScriptedChatClient{}, std::move(opts), nullptr, ae_test::sandboxes());
        check(!bound.has_value() && bound.error().code == "choice.missing.approval",
              "M6: bind_agent_session refuses a metadata with an unset approval, before any session exists");
    }

    // ---- M7: the sandbox registry is required, and empty is refused.
    {
        ae::SandboxBackendRegistry empty;
        auto native = ae::register_agent<BoundedAgent>(nullptr, empty);
        check(!native.has_value() && native.error().code == "choice.missing.sandbox_backend",
              "M7: register_agent against an empty sandbox registry fails choice.missing.sandbox_backend");
        auto declarative = ae::validate_agent_metadata(stated(), nullptr, empty);
        check(!declarative.has_value() && declarative.error().code == "choice.missing.sandbox_backend",
              "M7: validate_agent_metadata (every declarative bind) refuses it the same way");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
