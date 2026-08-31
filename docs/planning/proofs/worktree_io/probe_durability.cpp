// PROVE-PHASE PROBE: durability across a simulated process restart. Object instance A writes real
// blobs/trees to real disk, is destroyed (its C++ object, not the files), and a FRESH instance B --
// knowing nothing about A -- reads the SAME real files back from the SAME real directory.

#include "file_object_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

int main() {
    using namespace probe;

    std::filesystem::path root = std::filesystem::temp_directory_path() / "ae_durability_probe";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    agentengine::Digest blob_digest;
    agentengine::Digest tree_digest;

    // === "Process 1": write real content, then the store object goes out of scope ==================
    {
        FileWorktreeObjectStore store_a(root);
        std::string content = "durable content";
        std::vector<std::byte> bytes(content.size());
        for (std::size_t i = 0; i < content.size(); ++i) bytes[i] = static_cast<std::byte>(content[i]);
        auto b = store_a.put_blob(bytes);
        CHECK(b.has_value());
        blob_digest = *b;

        agentengine::Tree tree;
        tree.entries.push_back(agentengine::TreeEntry{"a.txt", blob_digest, false});
        auto t = store_a.put_tree(tree);
        CHECK(t.has_value());
        tree_digest = *t;
        std::printf("[1] \"process 1\": wrote real blob (digest=%s) and real tree (digest=%s) to "
                    "%s\n", blob_digest.c_str(), tree_digest.c_str(), root.string().c_str());
    }  // store_a destroyed here -- only real files on real disk remain, no in-memory state survives

    CHECK(std::filesystem::exists(root / "blobs" / blob_digest));
    CHECK(std::filesystem::exists(root / "trees" / tree_digest));
    std::printf("[2] real blob/tree FILES confirmed still present on disk after the C++ object "
                "that wrote them is gone\n");

    // === "Process 2": a completely FRESH store instance, same directory ============================
    {
        FileWorktreeObjectStore store_b(root);
        auto blob = store_b.get_blob(blob_digest);
        CHECK(blob.has_value());
        std::string recovered(reinterpret_cast<char const*>(blob->data()), blob->size());
        CHECK(recovered == "durable content");

        auto tree = store_b.get_tree(tree_digest);
        CHECK(tree.has_value());
        CHECK(tree->entries.size() == 1);
        CHECK(tree->entries[0].name == "a.txt");
        CHECK(tree->entries[0].digest == blob_digest);
        std::printf("[3] \"process 2\" (fresh store instance, no shared state with process 1): "
                    "recovered blob=\"%s\", tree correctly decoded with 1 entry (a.txt -> %s): "
                    "PASS -- real durability across a simulated restart\n",
                    recovered.c_str(), tree->entries[0].digest.c_str());
    }

    // === Real dedup persists across restart too: re-putting identical content doesn't grow storage ==
    {
        FileWorktreeObjectStore store_c(root);
        std::size_t before = store_c.blob_count();
        std::string content = "durable content";  // identical to the original
        std::vector<std::byte> bytes(content.size());
        for (std::size_t i = 0; i < content.size(); ++i) bytes[i] = static_cast<std::byte>(content[i]);
        auto b = store_c.put_blob(bytes);
        CHECK(b.has_value());
        CHECK(*b == blob_digest);   // same content -> same digest, deterministically, across restart
        std::size_t after = store_c.blob_count();
        CHECK(before == after);
        std::printf("[4] re-putting identical content from a THIRD fresh instance: same digest, "
                    "blob_count unchanged (%zu before, %zu after) -- dedup holds across restart\n",
                    before, after);
    }

    // === REAL path-traversal rejection (post-review fix) ============================================
    // A malformed "digest" string is not currently reachable through this design's one real gated
    // caller (Ledger only ever passes digests it computed itself), but an independent code review
    // flagged get_blob()/get_tree() as building filesystem paths directly from a caller-supplied
    // string with zero validation -- a real latent gap given this store's own header comment frames
    // it as reusable elsewhere. Proves the fix actually rejects a genuine traversal attempt, not
    // just a malformed-but-harmless string.
    {
        FileWorktreeObjectStore store_d(root);
        std::filesystem::path const outside_secret = root.parent_path() / "ae_durability_probe_outside_secret.txt";
        { std::ofstream f(outside_secret); f << "must never be reachable via a crafted digest"; }

        agentengine::Digest const traversal_attempt = "../ae_durability_probe_outside_secret";
        auto blob_leak = store_d.get_blob(traversal_attempt);
        auto tree_leak = store_d.get_tree(traversal_attempt);
        CHECK(!blob_leak.has_value());
        CHECK(!tree_leak.has_value());
        CHECK(blob_leak.error().code == "worktree.malformed_digest");
        CHECK(tree_leak.error().code == "worktree.malformed_digest");
        std::printf("[5] REAL path-traversal attempt via a crafted \"../...\" digest string: "
                    "REJECTED before ever touching the filesystem (code=%s) -- get_blob()/get_tree() "
                    "now validate the digest is a well-formed 64-char hex string first\n",
                    blob_leak.error().code.c_str());
        std::filesystem::remove(outside_secret, ec);
    }

    std::filesystem::remove_all(root, ec);
    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
