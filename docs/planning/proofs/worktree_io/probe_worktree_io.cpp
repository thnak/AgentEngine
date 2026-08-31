// PROVE-PHASE PROBE: real worktree content-addressing + real host filesystem I/O, end to end.
// Real SHA-256 digests (via the actual, linked src/core/worktree_digest.cpp), real files on real
// disk, real content-addressed dedup, and a real rollback that restores real bytes.

#include "real_io_filesystem.hpp"
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
T run_to_completion(agentengine::rt::task<T> t) {
    t.resume();
    CHECK(t.done());
    if constexpr (!std::is_void_v<T>) return t.take_value();
}

std::vector<std::byte> to_bytes(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}
std::string to_string(std::vector<std::byte> const& b) {
    return std::string(reinterpret_cast<char const*>(b.data()), b.size());
}
}  // namespace

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal owner = authority.mint_root("worktree-io-owner");

    Ledger ledger;
    auto root_result = run_to_completion(ledger.create_root_branch(owner));
    CHECK(root_result.has_value());
    BranchHandle branch = std::move(*root_result);

    auto quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000);
    CHECK(quota.has_value());

    std::filesystem::path host_root =
        std::filesystem::temp_directory_path() / "ae_worktree_io_probe";
    std::error_code ec;
    std::filesystem::remove_all(host_root, ec);  // clean slate from any prior run
    RealIoFileSystem fs(host_root);
    std::printf("Real host directory: %s\n\n", host_root.string().c_str());

    // === Turn 1: write "hello world" to a.txt (real disk write) ===================================
    auto w1 = fs.write("a.txt", to_bytes("hello world"));
    CHECK(w1.has_value());
    CHECK(std::filesystem::exists(host_root / "a.txt"));   // REAL file, really on disk
    std::printf("[1] wrote real file a.txt = \"hello world\" -- confirmed on real disk\n");

    auto tree1 = run_to_completion(fs.drain_into_tree(ledger, owner));
    CHECK(tree1.has_value());
    auto cp1 = run_to_completion(ledger.commit(branch, *tree1, owner, *quota));
    CHECK(cp1.has_value());
    std::printf("[2] REAL SHA-256 tree digest (turn 1) = %s\n", cp1->tree.c_str());
    std::printf("    REAL SHA-256 self_digest (turn 1) = %s\n", cp1->self_digest.c_str());
    CHECK(cp1->tree.size() == 64);       // a real, hex-encoded SHA-256 digest -- 64 chars
    CHECK(cp1->turn_index == 1);

    // === Content-addressed DEDUP: write the SAME content to a DIFFERENT path =======================
    auto w2 = fs.write("b.txt", to_bytes("hello world"));
    CHECK(w2.has_value());
    auto tree_dedup_check = run_to_completion(fs.drain_into_tree(ledger, owner));
    CHECK(tree_dedup_check.has_value());
    std::size_t const blob_count_before = ledger.blob_count_safe();
    auto cp_dedup = run_to_completion(ledger.commit(branch, *tree_dedup_check, owner, *quota));
    CHECK(cp_dedup.has_value());
    std::size_t const blob_count_after = ledger.blob_count_safe();
    std::printf("[3] wrote b.txt with IDENTICAL content \"hello world\" -- blob_count before "
                "commit=%zu, after=%zu (must be EQUAL: real content-addressed dedup, not two "
                "separate blobs for identical bytes)\n", blob_count_before, blob_count_after);
    CHECK(blob_count_before == blob_count_after);
    CHECK(blob_count_before == 1);   // exactly ONE unique blob so far ("hello world"), even though
                                       // TWO tree entries (a.txt, b.txt) now point at it

    // === Turn 2 (semantically): modify a.txt to different content ==================================
    auto w3 = fs.write("a.txt", to_bytes("modified content"));
    CHECK(w3.has_value());
    auto tree2 = run_to_completion(fs.drain_into_tree(ledger, owner));
    CHECK(tree2.has_value());
    auto cp2 = run_to_completion(ledger.commit(branch, *tree2, owner, *quota));
    CHECK(cp2.has_value());
    std::printf("[4] modified a.txt to \"modified content\" -- REAL new tree digest = %s "
                "(turn_index=%llu, differs from turn 1's %s)\n",
                cp2->tree.c_str(), (unsigned long long)cp2->turn_index, cp1->tree.c_str());
    CHECK(cp2->tree != cp1->tree);
    CHECK(cp2->turn_index == 3);   // turn 1 (a.txt), turn 2 (dedup commit), turn 3 (modified) --
                                     // wait: recount below

    // === REAL rollback: reset_to Ledger checkpoint 1, then MATERIALIZE it to real disk =============
    auto reset_cp = run_to_completion(ledger.reset_to(branch, 1, owner));
    CHECK(reset_cp.has_value());
    std::printf("[5] REAL Ledger.reset_to(target=1): new turn_index=%llu, restored tree digest=%s "
                "(matches turn 1's tree digest exactly: %d)\n",
                (unsigned long long)reset_cp->turn_index, reset_cp->tree.c_str(),
                (int)(reset_cp->tree == cp1->tree));
    CHECK(reset_cp->tree == cp1->tree);

    auto materialized = run_to_completion(fs.materialize(ledger, reset_cp->tree, owner));
    CHECK(materialized.has_value());

    // THE REAL PROOF: read REAL files off REAL disk after rollback and confirm they match turn 1's
    // state exactly -- b.txt (which existed at commit time #2, the dedup one) must be GONE (turn 1's
    // tree never had it), and a.txt must be back to "hello world" (not "modified content").
    CHECK(!std::filesystem::exists(host_root / "b.txt"));
    auto a_after_reset = fs.read_real_file("a.txt");
    CHECK(a_after_reset.has_value());
    std::string const a_content = to_string(*a_after_reset);
    std::printf("[6] AFTER real materialize(): a.txt on REAL disk = \"%s\" (must be \"hello "
                "world\", not \"modified content\"); b.txt exists on real disk = %d (must be 0 -- "
                "it never existed at turn 1)\n", a_content.c_str(),
                (int)std::filesystem::exists(host_root / "b.txt"));
    CHECK(a_content == "hello world");

    std::printf("\nALL CHECKS PASSED -- real SHA-256 content addressing, real dedup, real host "
                "filesystem I/O, and a real rollback that restores real bytes on real disk, all "
                "verified end to end.\n");

    std::filesystem::remove_all(host_root, ec);  // clean up the real scratch directory
    return 0;
}
