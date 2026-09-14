// Proof that a quota-checked `mount_write()` (core/worktree_mount.hpp) no longer copies every file in
// the mount to learn its size, and decides exactly as it did.
//
// WHAT WAS WRONG. `detail::subtree_usage()` recomputes a mount's usage on every quota-checked write,
// and sized each file with `store.get_blob(digest)->size()` -- `get_blob()` returns the content BY
// VALUE, so one write into a 1000-file mount of 100 KB files copied ~100 MB to read 1000 integers.
// Stores may now model `WorktreeObjectStoreWithBlobSize` (worktree_types.hpp); both in-tree stores
// do, and `subtree_usage()` sizes through `blob_size()` when they do.
//
//   Z1 -- the refinement's contract, on BOTH real conformers (InMemoryWorktreeObjectStore and the
//         file-backed FileWorktreeObjectStore): blob_size(d) == get_blob(d)->size() for sizes 0, 1,
//         4096, 4097 and 1 MiB, and a missing or malformed digest fails with get_blob()'s own code.
//   Z2 (positive control) -- a store WITHOUT blob_size() still works through get_blob(), and the
//         counter sees that cost: one capped write into a 200-file mount makes 201 get_blob() calls
//         copying every byte in it. Without this, "0 calls" below could mean a counter never wired up.
//   Z3 (the guard) -- the same write on a store WITH blob_size() makes ZERO get_blob() calls.
//   Z4 -- same decisions: a sequence of capped writes that crosses the byte cap, the file-count cap
//         and back (overwrite with smaller content) produces the same accept/reject codes and the
//         same committed tree digests on both stores.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/file_worktree_object_store.hpp"
#include "agentengine/core/worktree_mount.hpp"
#include "agentengine/rt/append_log_store.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace agentengine;
using RefStore = agentengine::rt::InMemoryAppendLogStore;

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

[[nodiscard]] std::vector<std::byte> bytes_of(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

// Forwards to an in-memory store and counts what get_blob() hands out. Deliberately has NO
// blob_size(), so it is sized the old way.
class CountingStore {
public:
    [[nodiscard]] result<Digest> put_blob(std::span<std::byte const> bytes) { return inner_.put_blob(bytes); }
    [[nodiscard]] result<std::vector<std::byte>> get_blob(Digest const& digest) {
        ++get_blob_calls;
        auto b = inner_.get_blob(digest);
        if (b) get_blob_bytes += b->size();
        return b;
    }
    [[nodiscard]] result<Digest> put_tree(Tree tree) { return inner_.put_tree(std::move(tree)); }
    [[nodiscard]] result<Tree> get_tree(Digest const& digest) { return inner_.get_tree(digest); }

    std::size_t get_blob_calls = 0;
    std::uint64_t get_blob_bytes = 0;

protected:
    InMemoryWorktreeObjectStore inner_;
};

// The same store, plus the refinement.
class CountingSizedStore : public CountingStore {
public:
    [[nodiscard]] result<std::uint64_t> blob_size(Digest const& digest) {
        ++blob_size_calls;
        return inner_.blob_size(digest);
    }
    std::size_t blob_size_calls = 0;
};

static_assert(WorktreeObjectStore<CountingStore> && !WorktreeObjectStoreWithBlobSize<CountingStore>);
static_assert(WorktreeObjectStoreWithBlobSize<CountingSizedStore>);

template <class S>
void check_contract(S& store, std::string const& name, Digest const& malformed) {
    for (std::size_t const n : {std::size_t{0}, std::size_t{1}, std::size_t{4096}, std::size_t{4097},
                                std::size_t{1} << 20}) {
        std::string content(n, 'q');
        if (n > 0) content[n - 1] = static_cast<char>('a' + n % 26);  // distinct content per size
        auto d = store.put_blob(bytes_of(content));
        auto size = d ? store.blob_size(*d) : result<std::uint64_t>{std::unexpected(d.error())};
        auto blob = d ? store.get_blob(*d) : result<std::vector<std::byte>>{std::unexpected(d.error())};
        check(size.has_value() && blob.has_value() && *size == blob->size() && *size == n,
              "Z1 " + name + ": blob_size() == get_blob()->size() == " + std::to_string(n));
    }
    Digest const absent(64, 'a');
    auto absent_size = store.blob_size(absent);
    auto absent_blob = store.get_blob(absent);
    check(!absent_size.has_value() && !absent_blob.has_value() &&
              absent_size.error().code == absent_blob.error().code,
          "Z1 " + name + ": an absent digest fails both with the same code (" +
              (absent_blob.has_value() ? std::string{"?"} : absent_blob.error().code) + ")");
    auto bad_size = store.blob_size(malformed);
    auto bad_blob = store.get_blob(malformed);
    check(!bad_size.has_value() && !bad_blob.has_value() && bad_size.error().code == bad_blob.error().code,
          "Z1 " + name + ": a malformed digest fails both with the same code (" +
              (bad_blob.has_value() ? std::string{"?"} : bad_blob.error().code) + ")");
}

constexpr int kFiles = 200;
constexpr std::size_t kFileBytes = 1000;

// A mount holding kFiles files of kFileBytes each, written uncapped (no usage walk), counters reset.
template <class S>
void fill(S& store, RefStore& refs, Mount const& mount) {
    (void)commit_ref(refs, mount.ref_name, *store.put_tree(Tree{}));
    cap::FsWrite const uncapped{mount.mount_id, "", std::nullopt, std::nullopt};
    for (int i = 0; i < kFiles; ++i) {
        std::string content(kFileBytes, 'x');
        content += std::to_string(i);  // distinct blobs, so nothing dedups the count away
        (void)mount_write(store, refs, mount, uncapped, "f" + std::to_string(i) + ".txt", bytes_of(content));
    }
    store.get_blob_calls = 0;
    store.get_blob_bytes = 0;
}

[[nodiscard]] std::string outcome(result<Ref> const& r) { return r.has_value() ? r->tree_digest : r.error().code; }

}  // namespace

int main() {
    // ---- Z1: the contract on both real stores.
    {
        InMemoryWorktreeObjectStore mem;
        check_contract(mem, "InMemoryWorktreeObjectStore", "not-a-digest");
        std::filesystem::path const root = std::filesystem::temp_directory_path() / "ae_blob_size_file_store";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        {
            FileWorktreeObjectStore file(root);
            check_contract(file, "FileWorktreeObjectStore", "not-a-digest");
        }
        std::filesystem::remove_all(root, ec);
    }

    Mount const mount{"/work", "session:blob-size", ""};
    cap::FsWrite const capped{"/work", "", std::uint64_t{10'000'000}, std::uint32_t{10'000}};

    // ---- Z2: positive control, the old path.
    {
        CountingStore store;
        RefStore refs;
        fill(store, refs, mount);
        auto r = mount_write(store, refs, mount, capped, "one-more.txt", bytes_of("hello"));
        check(r.has_value(), "Z2: a capped write into the 200-file mount succeeds on a store without blob_size()");
        check(store.get_blob_calls == static_cast<std::size_t>(kFiles) + 1,
              "Z2 (positive control): ... and makes " + std::to_string(kFiles + 1) +
                  " get_blob() calls to size the mount, got " + std::to_string(store.get_blob_calls) + " copying " +
                  std::to_string(store.get_blob_bytes) + " bytes");
    }

    // ---- Z3: the guard.
    {
        CountingSizedStore store;
        RefStore refs;
        fill(store, refs, mount);
        store.blob_size_calls = 0;
        auto r = mount_write(store, refs, mount, capped, "one-more.txt", bytes_of("hello"));
        check(r.has_value(), "Z3: the same write succeeds on a store with blob_size()");
        check(store.get_blob_calls == 0 && store.get_blob_bytes == 0,
              "Z3 (the guard): ... and makes ZERO get_blob() calls, got " + std::to_string(store.get_blob_calls));
        check(store.blob_size_calls == static_cast<std::size_t>(kFiles) + 1,
              "Z3: ... sizing all " + std::to_string(kFiles + 1) + " files through blob_size(), got " +
                  std::to_string(store.blob_size_calls));
    }

    // ---- Z4: same decisions and same trees.
    {
        CountingStore old_store;
        CountingSizedStore new_store;
        RefStore old_refs;
        RefStore new_refs;
        (void)commit_ref(old_refs, mount.ref_name, *old_store.put_tree(Tree{}));
        (void)commit_ref(new_refs, mount.ref_name, *new_store.put_tree(Tree{}));
        cap::FsWrite const tight{"/work", "", std::uint64_t{25}, std::uint32_t{3}};
        struct Step {
            char const* path;
            std::string content;
        };
        std::vector<Step> const steps = {
            {"a.txt", std::string(10, 'a')},      // 10 bytes, 1 file
            {"b.txt", std::string(10, 'b')},      // 20, 2
            {"c.txt", std::string(10, 'c')},      // 30 > 25: byte cap
            {"c.txt", std::string(5, 'c')},       // 25, 3: at both caps
            {"d.txt", "d"},                       // 4 files: count cap
            {"a.txt", "a"},                       // shrink: 16, 3
            {"dir/e.txt", std::string(9, 'e')},   // nested, 4 files: count cap
            {"b.txt", ""},                        // empty file still counts: 6, 3
            {"c.txt", std::string(19, 'c')},      // 1 + 0 + 19 = 20, 3
        };
        int differing = 0;
        int rejected = 0;
        for (auto const& s : steps) {
            auto o = mount_write(old_store, old_refs, mount, tight, s.path, bytes_of(s.content));
            auto n = mount_write(new_store, new_refs, mount, tight, s.path, bytes_of(s.content));
            rejected += o.has_value() ? 0 : 1;
            if (outcome(o) != outcome(n)) {
                ++differing;
                std::printf("       step %s: old %s, new %s\n", s.path, outcome(o).c_str(), outcome(n).c_str());
            }
        }
        check(rejected >= 3, "Z4: the sequence is refused " + std::to_string(rejected) +
                                 " times (both caps), so agreement covers refusals");
        check(differing == 0, "Z4: every write gets the same outcome, and every accepted one the same tree digest, "
                              "on both stores");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
