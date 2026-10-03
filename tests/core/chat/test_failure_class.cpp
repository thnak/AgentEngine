// Implements ADR-237 §9 D6 (decisions/ADR-237-async-extension-points-and-io-reactor.md) against 001 §6:
// `failure_class::canceled`, its one spelling, and the ONE retry decision (`is_retryable(failure_class,
// retry_budget)` in core/error.hpp) that every retrying site now calls.
//
//   F1  the whole decision table, class x budget: transient always; resource only for a fresh attempt;
//       canceled, policy, contract, fatal never. A mutant that lets `canceled` retry fails F1 and F2.
//   F2  the gateway's shared-budget predicate (model_call_gateway_detail::is_retryable) is that table's
//       `shared` column -- canceled and resource (a deadline) are not retried inside the same budget.
//   F3  every class has one spelling and round-trips through it; "canceled" is spelled with one l (A2A's
//       TASK_STATE_CANCELED, 027); "cancelled" -- the stream_terminal spelling -- is not a class name.
//   F4  the gateway does not fail a canceled call over to its next tier (fails_over); every other class does.

#include <cstdio>
#include <string>

#include "agentengine/core/error.hpp"
#include "agentengine/core/model_call_gateway.hpp"

namespace {

int g_failures = 0;

void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "  ok: %s\n", what.c_str());
    }
}

using agentengine::failure_class;
using agentengine::retry_budget;

constexpr failure_class kAll[] = {failure_class::transient, failure_class::policy,   failure_class::contract,
                                  failure_class::resource,  failure_class::fatal,    failure_class::canceled};

// The table is a compile-time fact too: a constexpr decision cannot drift from what the sites see.
static_assert(!agentengine::is_retryable(failure_class::canceled, retry_budget::shared));
static_assert(!agentengine::is_retryable(failure_class::canceled, retry_budget::fresh));
static_assert(agentengine::failure_class_to_string(failure_class::canceled) == "canceled");

}  // namespace

int main() {
    // ---- F1 ----------------------------------------------------------------------------------------------
    for (failure_class k : kAll) {
        std::string const name(agentengine::failure_class_to_string(k));
        bool const want_shared = k == failure_class::transient;
        bool const want_fresh  = k == failure_class::transient || k == failure_class::resource;
        check(agentengine::is_retryable(k, retry_budget::shared) == want_shared,
              "F1: " + name + " under a shared budget -> " + (want_shared ? "retry" : "no retry"));
        check(agentengine::is_retryable(k, retry_budget::fresh) == want_fresh,
              "F1: " + name + " by a fresh attempt -> " + (want_fresh ? "retry" : "no retry"));
    }

    // ---- F2 ----------------------------------------------------------------------------------------------
    using agentengine::model_call_gateway_detail::is_retryable;
    check(!is_retryable(failure_class::canceled), "F2: the gateway never retries canceled");
    check(!is_retryable(failure_class::resource), "F2: the gateway never retries resource (a deadline) in its budget");
    check(is_retryable(failure_class::transient), "F2 control: the gateway retries transient");

    // ---- F3 ----------------------------------------------------------------------------------------------
    for (failure_class k : kAll) {
        std::string_view const s = agentengine::failure_class_to_string(k);
        auto const back          = agentengine::failure_class_from_string(s);
        check(back.has_value() && *back == k, "F3: " + std::string(s) + " round-trips through its spelling");
    }
    check(agentengine::failure_class_to_string(failure_class::canceled) == "canceled", "F3: spelled canceled");
    check(!agentengine::failure_class_from_string("cancelled").has_value(),
          "F3: \"cancelled\" is not a class name (it is a stream_terminal spelling)");
    check(!agentengine::failure_class_from_string("").has_value(), "F3: the empty string is not a class");

    // ---- F4 ----------------------------------------------------------------------------------------------
    for (failure_class k : kAll) {
        bool const want = k != failure_class::canceled;
        check(agentengine::model_call_gateway_detail::fails_over(agentengine::error{k, "m", "c"}) == want,
              "F4: " + std::string(agentengine::failure_class_to_string(k)) + (want ? " fails over" : " does NOT fail over"));
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "test_failure_class: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_failure_class: ALL PASS\n");
    return 0;
}
