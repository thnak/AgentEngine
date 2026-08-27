// PROVE-PHASE PROBE: real concurrent multi-session I/O -- multiple RealIoFileSystem instances (each
// with its OWN real host directory, simulating independent sessions), all draining into the SAME
// shared Ledger concurrently. This is exactly the scenario that would have hit the real segfault
// this file's own sibling probes found and fixed (Ledger::commit()/create_root_branch() calling the
// unsynchronized real object store outside any lock, and external callers like drain_into_tree()
// bypassing Ledger's lock entirely via a raw store reference) -- run here specifically to confirm the
// fix (Ledger::put_blob_safe()/get_tree_safe(), no more raw object_store() accessor) holds under real
// contention, not just the single-session case §27 already proved.

#include "real_io_filesystem.hpp"
#include "../common/block_on.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

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
}  // namespace

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal root_owner = authority.mint_root("concurrent-io-root");
    auto quota = AsyncQuota<StorageBytes>::mint_root(authority, root_owner, 1'000'000'000);
    CHECK(quota.has_value());

    Ledger ledger;

    constexpr int kSessions = 10;
    constexpr int kWritesPerSession = 50;
    std::vector<std::thread> threads;
    std::vector<bool> session_ok(kSessions, false);
    std::vector<std::filesystem::path> host_roots(kSessions);

    for (int s = 0; s < kSessions; ++s) {
        host_roots[s] = std::filesystem::temp_directory_path() /
                        ("ae_concurrent_io_probe_session_" + std::to_string(s));
        std::error_code ec;
        std::filesystem::remove_all(host_roots[s], ec);
    }

    for (int s = 0; s < kSessions; ++s) {
        threads.emplace_back([&, s]() {
            Principal owner = authority.derive_child(root_owner, "io-session-" + std::to_string(s));
            // A REAL FINDING a code-review pass caught elsewhere (AsyncQuota::try_consume() used to
            // silently skip its own spender-identity check) means this probe must now do what the
            // design always specified: a derived-child principal spends from a SHARE its parent
            // explicitly allocated (§21/§34's own store-wide-ceiling pattern), never straight from a
            // quota it was never granted a share of.
            auto session_quota = block_on(quota->allocate_child_share(owner, 10'000'000));
            if (!session_quota.has_value()) return;
            auto root_branch = block_on(ledger.create_root_branch(owner));
            if (!root_branch.has_value()) return;
            BranchHandle branch = std::move(*root_branch);

            RealIoFileSystem fs(host_roots[s]);
            bool ok = true;
            for (int i = 0; i < kWritesPerSession; ++i) {
                std::string content = "session-" + std::to_string(s) + "-write-" + std::to_string(i);
                auto w = fs.write("f" + std::to_string(i) + ".txt", to_bytes(content));
                if (!w.has_value()) { ok = false; break; }
                auto tree = block_on(fs.drain_into_tree(ledger, owner));
                if (!tree.has_value()) { ok = false; break; }
                auto cp = block_on(ledger.commit(branch, *tree, owner, *session_quota));
                if (!cp.has_value()) { ok = false; break; }
            }
            session_ok[s] = ok;
            (void)block_on(ledger.abandon(std::move(branch)));
        });
    }
    for (auto& th : threads) th.join();

    bool all_ok = true;
    for (bool ok : session_ok) if (!ok) all_ok = false;
    std::printf("[1] %d concurrent sessions x %d real writes+drains+commits each, all sharing ONE "
                "Ledger's object store: all_ok=%d (no segfault, no lost/corrupted commit -- the "
                "exact scenario that segfaulted before Ledger's put_blob_safe()/get_tree_safe() "
                "fix)\n", kSessions, kWritesPerSession, (int)all_ok);
    CHECK(all_ok);

    std::size_t const final_blob_count = ledger.blob_count_safe();
    std::printf("[2] final blob_count() = %zu (expect %d -- one unique blob per distinct write "
                "across all sessions, since every write's content is unique)\n",
                final_blob_count, kSessions * kWritesPerSession);
    CHECK(final_blob_count == static_cast<std::size_t>(kSessions * kWritesPerSession));

    for (auto const& root : host_roots) { std::error_code ec; std::filesystem::remove_all(root, ec); }
    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
