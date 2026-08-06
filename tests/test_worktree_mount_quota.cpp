// Proof for 025-Worktree-and-Virtual-Filesystem.md §5's write-quota enforcement ("Quotas are
// enforced at write, and exhaustion is an ordinary `No space left on device`-class error inside the
// guest, not a protocol lecture" -- 026 §3's own mapping table, matched here verbatim), Milestone 3
// Phase C2's mount layer -- Phase C1's `mount_write` explicitly deferred this numeric enforcement as
// a named, tracked gap; this is that gap closed, proven against the same mount layer directly
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md).
//
// Every rejection is paired with a positive control (022 §5): the identical write succeeding once
// content shrinks below the cap, an uncapped `cap::FsWrite` accepting the same write a capped one
// rejects, and a second mount's independent quota staying unaffected by a sibling mount's usage.
// C3-C6 additionally proves the Ref is left completely UNCHANGED by a rejected write -- the guest
// never observes a state where the tree moved and the quota was found exceeded afterward.

#include <iostream>
#include <string>

#include "agentengine/core/worktree.hpp"

using namespace agentengine;
using quark::InMemoryStore;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " at " << __FILE__ << ":" << __LINE__ << "\n";     \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::cout << "  ok: " << (label) << "\n";                                              \
        }                                                                                           \
    } while (0)

std::vector<std::byte> bytes_of(std::string const& content) {
    std::vector<std::byte> bytes;
    bytes.reserve(content.size());
    for (char c : content) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return bytes;
}

std::string string_of(std::vector<std::byte> const& bytes) {
    std::string s;
    s.reserve(bytes.size());
    for (auto b : bytes) s.push_back(static_cast<char>(b));
    return s;
}

Digest empty_tree(InMemoryWorktreeObjectStore& store) { return *store.put_tree(Tree{}); }

Digest current_tree_digest(InMemoryStore& ref_store, std::string const& ref_name) {
    auto ref = read_ref(ref_store, ref_name);
    return (ref.has_value() && ref->has_value()) ? (*ref)->tree_digest : Digest{"<missing>"};
}

} // namespace

int main() {
    // C3-C1 (positive control): a write comfortably under both caps succeeds.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        AE_CHECK(commit_ref(ref_store, "session:quota-1", empty_tree(obj_store)).has_value(), "C3-C1: setup empty root");

        Mount mount{"/work", "session:quota-1", ""};
        cap::FsWrite capped{"/work", "", std::uint64_t{1000}, std::uint32_t{10}};

        auto ok = mount_write(obj_store, ref_store, mount, capped, "small.txt", bytes_of("hello"));
        AE_CHECK(ok.has_value(), "C3-C1: a write well under both caps succeeds");
    }

    // C3-C2: byte quota exceeded is rejected with the 026 §3-matching error, and the Ref is left
    // completely unchanged (not committed-then-flagged).
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        AE_CHECK(commit_ref(ref_store, "session:quota-2", empty_tree(obj_store)).has_value(), "C3-C2: setup empty root");

        Mount mount{"/work", "session:quota-2", ""};
        cap::FsWrite tiny{"/work", "", std::uint64_t{5}, std::nullopt};

        auto before = current_tree_digest(ref_store, "session:quota-2");
        auto rejected = mount_write(obj_store, ref_store, mount, tiny, "toobig.txt", bytes_of("this is way over 5"));
        AE_CHECK(!rejected.has_value() && rejected.error().code == "worktree.mount_write_quota_exceeded",
                 "C3-C2: a write exceeding quota_bytes is rejected");
        AE_CHECK(!rejected.has_value() && rejected.error().klass == failure_class::resource,
                 "C3-C2: quota exhaustion is failure_class::resource, not policy");
        AE_CHECK(!rejected.has_value() && rejected.error().message == "No space left on device",
                 "C3-C2: the message matches 026 §3's mapping table verbatim, not a policy identifier");
        auto after = current_tree_digest(ref_store, "session:quota-2");
        AE_CHECK(before == after, "C3-C2: the Ref is left completely unchanged by a rejected write");
    }

    // C3-C3: file-count cap exceeded is rejected the same way, Ref unchanged. Two files already fit
    // (cap=2); a third distinct file pushes the count to 3 and is rejected.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        AE_CHECK(commit_ref(ref_store, "session:quota-3", empty_tree(obj_store)).has_value(), "C3-C3: setup empty root");

        Mount mount{"/work", "session:quota-3", ""};
        cap::FsWrite two_files{"/work", "", std::nullopt, std::uint32_t{2}};

        AE_CHECK(mount_write(obj_store, ref_store, mount, two_files, "a.txt", bytes_of("a")).has_value(),
                 "C3-C3: first file, under the count cap, succeeds");
        AE_CHECK(mount_write(obj_store, ref_store, mount, two_files, "b.txt", bytes_of("b")).has_value(),
                 "C3-C3: second file, exactly AT the count cap, succeeds (inclusive boundary)");

        auto before = current_tree_digest(ref_store, "session:quota-3");
        auto third = mount_write(obj_store, ref_store, mount, two_files, "c.txt", bytes_of("c"));
        AE_CHECK(!third.has_value() && third.error().code == "worktree.mount_write_file_count_exceeded",
                 "C3-C3: a third distinct file exceeding file_count_cap is rejected");
        auto after = current_tree_digest(ref_store, "session:quota-3");
        AE_CHECK(before == after, "C3-C3: the Ref is left unchanged by the rejected third write");
    }

    // C3-C4: quota is cumulative across separate writes, not per-call -- two individually-small
    // writes whose COMBINED size exceeds the cap trip it on the second write, and the first write's
    // content survives untouched (only the offending write is rejected, not a rollback of history).
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        AE_CHECK(commit_ref(ref_store, "session:quota-4", empty_tree(obj_store)).has_value(), "C3-C4: setup empty root");

        Mount mount{"/work", "session:quota-4", ""};
        cap::FsWrite cumulative{"/work", "", std::uint64_t{15}, std::nullopt};

        auto first = mount_write(obj_store, ref_store, mount, cumulative, "a.txt", bytes_of("0123456789"));  // 10 bytes
        AE_CHECK(first.has_value(), "C3-C4: first 10-byte write, under the 15-byte cap, succeeds");

        auto second = mount_write(obj_store, ref_store, mount, cumulative, "b.txt", bytes_of("0123456789"));  // +10 = 20
        AE_CHECK(!second.has_value() && second.error().code == "worktree.mount_write_quota_exceeded",
                 "C3-C4: a second write whose COMBINED usage exceeds the cap is rejected");

        auto still_there =
            mount_read(obj_store, ref_store, mount, cap::FsRead{"/work", "", std::nullopt}, "a.txt");
        AE_CHECK(still_there.has_value() && string_of(*still_there) == "0123456789",
                 "C3-C4: the first write's content survives the second write's rejection");
    }

    // C3-C5 (positive control): the SAME oversized write that C3-C2 rejects succeeds once the
    // capability carries no quota_bytes at all -- uncapped means uncapped, never "0".
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        AE_CHECK(commit_ref(ref_store, "session:quota-5", empty_tree(obj_store)).has_value(), "C3-C5: setup empty root");

        Mount mount{"/work", "session:quota-5", ""};
        cap::FsWrite uncapped{"/work", "", std::nullopt, std::nullopt};

        auto ok = mount_write(obj_store, ref_store, mount, uncapped, "toobig.txt",
                               bytes_of("this is way over any small cap"));
        AE_CHECK(ok.has_value(), "C3-C5: the same oversized write succeeds under an uncapped FsWrite");
    }

    // C3-C6: quota is scoped to THIS mount's subtree_path, never the whole ref -- a sibling mount
    // rooted at a different subtree_path on the SAME ref has its own independent budget, unaffected
    // by the other mount's usage.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        AE_CHECK(commit_ref(ref_store, "session:quota-6", empty_tree(obj_store)).has_value(), "C3-C6: setup empty root");

        Mount work_mount{"/work", "session:quota-6", "work"};
        Mount out_mount{"/out", "session:quota-6", "out"};
        cap::FsWrite work_cap{"/work", "", std::uint64_t{1000}, std::nullopt};
        cap::FsWrite out_cap{"/out", "", std::uint64_t{5}, std::nullopt};

        auto filled_work =
            mount_write(obj_store, ref_store, work_mount, work_cap, "big.txt", bytes_of(std::string(200, 'x')));
        AE_CHECK(filled_work.has_value(), "C3-C6: /work accumulates 200 bytes, well under its own 1000-byte cap");

        auto out_write = mount_write(obj_store, ref_store, out_mount, out_cap, "small.txt", bytes_of("hi"));
        AE_CHECK(out_write.has_value(),
                 "C3-C6: /out's independent 5-byte cap is checked against /out's OWN usage, not "
                 "inflated by /work's 200 bytes on the same ref");
    }

    // C3-C7: overwriting a file with SMALLER content reduces recomputed usage -- proves the check
    // recomputes from the final tree state rather than accumulating a stale delta. A write that
    // would have been rejected against the old (larger) size succeeds once the old content shrinks.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        AE_CHECK(commit_ref(ref_store, "session:quota-7", empty_tree(obj_store)).has_value(), "C3-C7: setup empty root");

        Mount mount{"/work", "session:quota-7", ""};
        cap::FsWrite cap12{"/work", "", std::uint64_t{12}, std::nullopt};

        auto big = mount_write(obj_store, ref_store, mount, cap12, "a.txt", bytes_of("0123456789"));  // 10 bytes
        AE_CHECK(big.has_value(), "C3-C7: initial 10-byte write fits the 12-byte cap");

        auto blocked = mount_write(obj_store, ref_store, mount, cap12, "b.txt", bytes_of("xyz"));  // would be 13
        AE_CHECK(!blocked.has_value() && blocked.error().code == "worktree.mount_write_quota_exceeded",
                 "C3-C7: adding a second file that would push usage to 13 bytes is rejected");

        auto shrink = mount_write(obj_store, ref_store, mount, cap12, "a.txt", bytes_of("01"));  // now 2 bytes total
        AE_CHECK(shrink.has_value(), "C3-C7: shrinking a.txt to 2 bytes succeeds (usage recomputed downward)");

        auto now_fits = mount_write(obj_store, ref_store, mount, cap12, "b.txt", bytes_of("xyz"));  // 2 + 3 = 5
        AE_CHECK(now_fits.has_value(),
                 "C3-C7: the SAME write C3-C7 rejected above now succeeds, because usage is "
                 "recomputed from final tree state (2+3=5), not a stale running total (10+3=13)");
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " FAILURE(S)\n";
    return 1;
}
