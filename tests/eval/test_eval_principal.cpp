// Implements decisions/ADR-195-evaluation-harness.md §3.9 / E25's round-4 positive control: the
// reserved `eval:` tenant prefix is only a real barrier if the delimiter it is anchored to is also
// reserved. Round 4 found `memory_mount_id`/`memory_ref_name` join tenant_id and id with a bare,
// unescaped ':', so (tenant "eval:acme", id "run1") and (tenant "eval", id "acme:run1") collide on
// the identical string "memory:eval:acme:run1". This test executes that exact collision against the
// real `memory_mount_id` to prove it exists, then proves `mint_eval_trial_principal` refuses to
// construct either half of it.

#include <iostream>
#include <string>

#include "agentengine/core/memory.hpp"
#include "agentengine/eval/eval_principal.hpp"

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

}  // namespace

int main() {
    namespace ev = ae::eval;

    // ---- the collision itself, against real production code, not a model of it --------------------
    ae::Principal const p1{"run1", "eval:acme"};
    ae::Principal const p2{"acme:run1", "eval"};
    AE_CHECK(ae::memory_mount_id(p1) == ae::memory_mount_id(p2),
             "round-4 positive control: unescaped ':' join lets two different (tenant,id) pairs "
             "collide on the same memory_mount_id");
    AE_CHECK(ae::memory_ref_name(p1) == ae::memory_ref_name(p2),
             "the same collision reaches memory_ref_name too");

    // ---- the fix: mint_eval_trial_principal refuses either half of the collision --------------------
    auto minted_ok = ev::mint_eval_trial_principal("acme", "run1");
    AE_CHECK(minted_ok.has_value(), "an ordinary tenant suffix and id mint successfully");
    if (minted_ok) {
        AE_CHECK(minted_ok->tenant_id == "eval:acme", "the minted principal's tenant carries the reserved prefix");
        AE_CHECK(minted_ok->id == "run1", "the minted principal's id is unchanged");
    }

    auto colon_in_tenant = ev::mint_eval_trial_principal("eval:acme", "run1");
    AE_CHECK(!colon_in_tenant.has_value(),
             "mint_eval_trial_principal refuses a ':' in the tenant suffix (would-be collision half 1)");

    auto colon_in_id = ev::mint_eval_trial_principal("eval", "acme:run1");
    AE_CHECK(!colon_in_id.has_value(),
             "mint_eval_trial_principal refuses a ':' in the id (would-be collision half 2)");

    // ---- planted mutant: if the guard were "reject only literally 'eval:acme'" instead of "reject
    // any colon", these would slip through -- prove they don't for a different colon placement too.
    auto colon_elsewhere_tenant = ev::mint_eval_trial_principal("a:b", "c");
    AE_CHECK(!colon_elsewhere_tenant.has_value(), "any colon in the tenant suffix is refused, not just 'eval:'");
    auto colon_elsewhere_id = ev::mint_eval_trial_principal("a", "b:c");
    AE_CHECK(!colon_elsewhere_id.has_value(), "any colon in the id is refused, not just the executed collision");

    // ---- empty identity components are refused, not silently minted --------------------------------
    AE_CHECK(!ev::mint_eval_trial_principal("", "run1").has_value(), "an empty tenant suffix is refused");
    AE_CHECK(!ev::mint_eval_trial_principal("acme", "").has_value(), "an empty id is refused");

    // ---- the production-side guard function names the same reserved space --------------------------
    AE_CHECK(ev::is_eval_reserved_tenant("eval"), "is_eval_reserved_tenant flags the bare tenant 'eval'");
    AE_CHECK(ev::is_eval_reserved_tenant("eval:acme"), "is_eval_reserved_tenant flags an eval:-prefixed tenant");
    AE_CHECK(ev::is_eval_reserved_tenant("prod:has:colons"),
             "is_eval_reserved_tenant flags ANY colon, since the join is unescaped for every tenant, not "
             "only eval-prefixed ones");
    AE_CHECK(!ev::is_eval_reserved_tenant("acme"), "an ordinary, colon-free tenant is not flagged");

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_eval_principal: all checks passed\n";
    return 0;
}
