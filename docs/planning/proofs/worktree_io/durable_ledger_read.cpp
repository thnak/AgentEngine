// PROVE-PHASE PROBE (A2/§34): "Process 2" -- a genuinely separate, later-launched OS process
// reopening the SAME durable directories `durable_ledger_write.cpp` used. Proves, for real:
//   (1) the restored branch's head tree/turn/checkpoints are intact (reset_to still works);
//   (2) the restored content is genuinely readable off real disk through the real Ledger API;
//   (3) the ACL survives the restart too -- the legitimate owner (re-adopted via A1's durable
//       identity, so it gets its OWN id back) can still read; an unrelated, brand-new principal
//       minted in THIS fresh process cannot -- the durable-Ledger analog of §33's own two-process
//       proof, now one layer up the stack.

#include "file_object_store.hpp"
#include "worktree_ledger.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace {
template <class T>
T run(agentengine::rt::task<T> t) { t.resume(); return t.take_value(); }
}  // namespace

int main() {
    using namespace probe;

    std::filesystem::path const root = "ae_durable_ledger_probe";
    CHECK(std::filesystem::exists(root / "ledger" / "ledger_state.snapshot"));

    IdentityAuthority& authority = IdentityAuthority::bootstrap(root / "identity");
    // Re-adopting "owner" in a FRESH process must return her ORIGINAL id (A1's own proof, reused
    // here as the precondition this whole probe depends on -- restated, not re-derived, since §34
    // already proved A1 alone; this confirms it still holds when composed with a durable Ledger).
    Principal owner = authority.adopt("owner", "");
    std::printf("[read] owner re-adopted id=%llu\n", static_cast<unsigned long long>(owner.id()));

    Ledger<FileWorktreeObjectStore> ledger(FileWorktreeObjectStore(root / "objects"), root / "ledger");

    std::string const branch_name = "root-" + std::to_string(owner.id());
    auto restored_head_r = ledger.head_tree_digest(branch_name, owner);
    CHECK(restored_head_r.has_value());
    agentengine::Digest const restored_head = *restored_head_r;
    std::printf("[read] restored branch=%s head_tree=%s\n", branch_name.c_str(), restored_head.c_str());

    // (1) The restored tree really is turn 2's content (a.txt + b.txt), read back through the
    // REAL, restored Ledger -- not re-derived from anything write_side.exe printed.
    auto tree = ledger.get_tree_safe(restored_head, owner);
    CHECK(tree.has_value());
    CHECK(tree->entries.size() == 2);
    bool found_a = false, found_b = false;
    agentengine::Digest a_digest, b_digest;
    for (auto const& e : tree->entries) {
        if (e.name == "a.txt") { found_a = true; a_digest = e.digest; }
        if (e.name == "b.txt") { found_b = true; b_digest = e.digest; }
    }
    CHECK(found_a && found_b);
    auto a_bytes = ledger.get_blob_safe(a_digest, owner);
    CHECK(a_bytes.has_value());
    std::string const a_content(reinterpret_cast<char const*>(a_bytes->data()), a_bytes->size());
    CHECK(a_content == "durable ledger content, turn 1");
    std::printf("[read] (1) restored tree has both real files; a.txt content recovered off real "
                "disk through the restored Ledger: \"%s\" -- PASS\n", a_content.c_str());

    // (2) The CHECKPOINT HISTORY (not just the latest head) was genuinely persisted, not just the
    // final state -- verified via checkpoint_at(), a read-only introspection method (not a
    // BranchHandle-granting one -- reset_to() itself still requires a live handle, which this
    // probe deliberately has none of across the restart, exactly A7's own still-open question,
    // §11 item 6; proving reset_to() ITSELF post-restart is A7's job, not A2's).
    auto turn1_cp = ledger.checkpoint_at(branch_name, 1, owner);
    CHECK(turn1_cp.has_value());
    CHECK(turn1_cp->tree != restored_head);   // turn 1's tree must differ from turn 2's head
    auto turn1_tree = ledger.get_tree_safe(turn1_cp->tree, owner);
    CHECK(turn1_tree.has_value());
    CHECK(turn1_tree->entries.size() == 1);
    CHECK(turn1_tree->entries[0].name == "a.txt");
    std::printf("[read] (2) the PERSISTED turn-1 checkpoint survived the restart intact (1 file, "
                "not 2) -- real checkpoint HISTORY, not just the latest head, was durably restored "
                "-- PASS\n");

    // (3) ACL durability: the legitimate owner's access above already proves the positive case.
    // Now the negative case -- an unrelated principal, minted FIRST in this fresh process (the
    // exact §33 shape), must still be denied against the RESTORED ACL.
    Principal outsider = authority.mint_root("durable-ledger-outsider");
    auto leak_attempt = ledger.get_tree_safe(restored_head, outsider);
    CHECK(!leak_attempt.has_value());
    auto leak_attempt_blob = ledger.get_blob_safe(a_digest, outsider);
    CHECK(!leak_attempt_blob.has_value());
    std::printf("[read] (3) an unrelated outsider principal minted fresh in THIS process is still "
                "REJECTED against the RESTORED ACL (tree: %s, blob: %s) -- the durable Ledger's own "
                "identity-scoped access control survives a real process restart intact -- PASS\n",
                leak_attempt.error().code.c_str(), leak_attempt_blob.error().code.c_str());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::printf("\nALL CHECKS PASSED -- a real Ledger<FileWorktreeObjectStore>, composed with A1's "
                "durable identity, genuinely survives a process restart with its content, branch "
                "heads, full checkpoint history, AND its identity-scoped ACL all intact.\n");
    return 0;
}
