// Implements decisions/ADR-195-evaluation-harness.md §3.9's `EvalStore`: a fresh in-memory
// object/ref store pair, an eval-tenant Principal, and read/write capabilities scoped to that
// mount alone. Proves the colon-collision guard round-trips through this call site (not just
// eval_principal.hpp's own test), and that isolation is by the FRESH STORE, not by name -- §3.2's
// own claim ("the ref_name may be reused across trials because the store is fresh").

#include <iostream>
#include <string>

#include "agentengine/core/memory.hpp"
#include "agentengine/eval/eval_store.hpp"

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

    // ---- happy path: minted principal carries the reserved prefix -----------------------------
    auto store1 = ev::EvalStore::make("trial", "a");
    AE_CHECK(store1.has_value(), "EvalStore::make succeeds for an ordinary tenant suffix/id");
    if (store1) {
        AE_CHECK(store1->principal().tenant_id == "eval:trial",
                 "the minted principal's tenant carries the reserved eval: prefix");
        AE_CHECK(store1->principal().id == "a", "the minted principal's id is unchanged");
        AE_CHECK(store1->mount().mount_id == ae::memory_mount_id(store1->principal()),
                 "the store's mount matches memory_mount() for its own principal");
    }

    // ---- colon-collision guard round-trips through EvalStore::make, not just mint_eval_trial_principal
    auto colon_in_suffix = ev::EvalStore::make("eval:trial", "a");
    AE_CHECK(!colon_in_suffix.has_value(), "a colon in tenant_suffix is refused at this call site");
    auto colon_in_id = ev::EvalStore::make("trial", "a:b");
    AE_CHECK(!colon_in_id.has_value(), "a colon in id is refused at this call site");

    // ---- isolation is by the FRESH STORE, not by name (§3.2) -----------------------------------
    // Write an item into the first store, under the SAME identity ("trial","a") a second EvalStore
    // is then minted with, and prove the second store holds nothing: a naive implementation that
    // shared some static/process-wide backing store keyed by ref_name would leak the first item.
    if (store1) {
        ae::MemoryItem seeded{};
        seeded.kind = ae::memory_kind::procedural;
        seeded.content = "isolation probe: this must never be visible from a second EvalStore";
        seeded.salience = 0.5f;
        seeded.origin = ae::MemoryOrigin{ae::memory_source::model_inferred, "run-0", "turn-0",
                                          store1->principal()};
        AE_CHECK(ae::write_memory_item(store1->object_store(), store1->ref_store(), store1->mount(),
                                        store1->write_cap(), seeded)
                     .has_value(),
                 "setup: seeding an item into the first store succeeds");

        auto store2 = ev::EvalStore::make("trial", "a");
        AE_CHECK(store2.has_value(), "a second EvalStore with the SAME identity mints successfully");
        if (store2) {
            auto listed = ae::list_memory_items(store2->object_store(), store2->ref_store(),
                                                 store2->mount(), store2->read_cap());
            AE_CHECK(listed.has_value() && listed->empty(),
                     "the second, same-identity EvalStore's fresh store holds ZERO items from the "
                     "first -- isolation is by the fresh store object, not by ref_name/identity");
        }
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_eval_store: all checks passed\n";
    return 0;
}
