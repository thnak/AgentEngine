// Proof for ADR-238 (Ledger over an async WorktreeObjectStore): `mutex_` is never held across a store suspension, and
// the head-moving operations are serialised by `op_mutex_` instead. Implements ADR-238 §6 T1-T9.
//
// The store is gated: a call can be told to PARK until the test releases it, so every interleaving below is forced,
// not hoped for. Everything runs on this one thread, driven by hand (`resume()` once to start, `Gate::release()` to
// let a parked store call continue), which is what makes "this operation is parked right now" an observable fact.
//
//   T1 -- N concurrent commit()s on one branch, each parked in put_tree: all complete, turns are exactly 1..N and the
//         parents chain. A std::mutex held across the suspension would deadlock this thread on the second commit.
//   T2 -- a merge started while a commit to its PARENT is parked waits for it, then merges against the post-commit
//         parent: the commit's file survives. Without op_mutex_ the merge reads the stale parent and drops it.
//   T3 -- abandon() of another branch is serialised behind an in-flight merge.
//   T4 -- an unauthorized get_blob_safe()/get_tree_safe() never reaches the store; an authorized one does, once.
//   T5 -- a put_tree that throws becomes ledger.store_faulted, refunds the quota, releases op_mutex_, and the next
//         commit succeeds.
//   T6 -- abandon_orphaned_branch() completes (it must not take op_mutex_ twice), and so does reap_pending_abandons().
//   T7 -- a merge task destroyed while parked in a store read leaves its child alive and reclaimable.
//   T8 -- an operation requested from inside a store call made under op_mutex_ is refused with ledger.op_lock_refused
//         and throws nothing.
//   T9 -- RealIoFileSystem::materialize() holds no std::mutex while parked in a Ledger read: a write() on the same
//         thread completes meanwhile.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/ledger.hpp"
#include "agentengine/core/ledger_impl.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/rt/scope.hpp"
#include "agentengine/sandbox/real_io_filesystem.hpp"

#include <coroutine>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace agentengine;
namespace fs = std::filesystem;

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

// A point a store call can park at until the test lets it go.
struct Gate {
    bool hold = false;
    std::vector<std::coroutine_handle<>> parked;

    struct Awaiter {
        Gate* gate;
        [[nodiscard]] bool await_ready() const noexcept { return !gate->hold; }
        void await_suspend(std::coroutine_handle<> h) { gate->parked.push_back(h); }
        void await_resume() const noexcept {}
    };
    [[nodiscard]] Awaiter wait() { return Awaiter{this}; }

    // Lets every parked call continue, and every later call through.
    void release() {
        hold = false;
        auto resumed = std::move(parked);
        parked.clear();
        for (auto h : resumed) h.resume();
    }
    // Forgets parked handles whose frames the test destroyed.
    void forget() { parked.clear(); }
};

struct SharedStore {
    InMemoryWorktreeObjectStore inner;
    Gate put_tree_gate;
    Gate get_tree_gate;
    Gate get_blob_gate;
    bool throw_on_put_tree = false;
    int get_blob_calls = 0;
    int get_tree_calls = 0;
    std::function<task<void>()> on_put_tree;   // runs inside put_tree, i.e. under whatever lock the caller holds
};

// The store the Ledger holds: a thin handle on SharedStore so the test can reach the gates after the Ledger took it.
class GatedStore {
public:
    explicit GatedStore(std::shared_ptr<SharedStore> s) : s_(std::move(s)) {}

    [[nodiscard]] task<result<Digest>> put_blob(std::span<std::byte const> bytes) {
        co_return co_await s_->inner.put_blob(bytes);
    }
    [[nodiscard]] task<result<std::vector<std::byte>>> get_blob(Digest digest) {
        ++s_->get_blob_calls;
        co_await s_->get_blob_gate.wait();
        co_return co_await s_->inner.get_blob(digest);
    }
    [[nodiscard]] task<result<Digest>> put_tree(Tree tree) {
        if (s_->on_put_tree) co_await s_->on_put_tree();
        co_await s_->put_tree_gate.wait();
        if (s_->throw_on_put_tree) {
            s_->throw_on_put_tree = false;
            throw std::runtime_error("planted put_tree fault");
        }
        co_return co_await s_->inner.put_tree(std::move(tree));
    }
    [[nodiscard]] task<result<Tree>> get_tree(Digest digest) {
        ++s_->get_tree_calls;
        co_await s_->get_tree_gate.wait();
        co_return co_await s_->inner.get_tree(digest);
    }
    [[nodiscard]] std::size_t blob_count() const { return s_->inner.blob_count(); }

private:
    std::shared_ptr<SharedStore> s_;
};

static_assert(WorktreeObjectStore<GatedStore>);

}  // namespace

template class agentengine::Ledger<GatedStore>;

namespace {

using L = Ledger<GatedStore>;

[[nodiscard]] std::vector<std::byte> to_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

// Starts a task by hand: runs it to its first suspension (or to completion).
template <class T>
void start(task<T>& t) {
    t.resume();
}

struct Fixture {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle owner;
    std::shared_ptr<SharedStore> shared = std::make_shared<SharedStore>();
    L ledger;
    agentengine::rt::AsyncQuota<StorageBytes> storage;
    agentengine::rt::AsyncQuota<BranchCost> branches;
    agentengine::rt::AsyncQuota<MergeCost> merges;

    explicit Fixture(std::string const& name, std::optional<fs::path> durable = std::nullopt)
        : owner(authority.mint_root(name)),
          ledger(GatedStore(shared), std::move(durable)),
          storage(*agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000)),
          branches(*agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100)),
          merges(*agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100)) {}

    [[nodiscard]] BranchHandle<GatedStore> root() {
        auto r = agentengine::rt::block_on(ledger.create_root_branch(owner));
        return std::move(*r);
    }
    [[nodiscard]] BranchHandle<GatedStore> child_of(BranchHandle<GatedStore> const& parent) {
        auto r = agentengine::rt::block_on(ledger.branch_from(parent, owner, branches));
        return std::move(*r);
    }
    // A tree of `files`, each a fresh blob written by `owner`.
    [[nodiscard]] Tree tree_of(std::vector<std::pair<std::string, std::string>> const& files) {
        Tree t;
        for (auto const& [name, text] : files) {
            auto d = agentengine::rt::block_on(ledger.put_blob_safe(to_bytes(text), owner));
            t.entries.push_back(TreeEntry{name, *d, false});
        }
        return t;
    }
    [[nodiscard]] std::vector<std::string> names_at_head(std::string const& branch) {
        auto digest = ledger.head_tree_digest(branch, owner);
        std::vector<std::string> names;
        if (!digest) return names;
        auto t = agentengine::rt::block_on(ledger.get_tree_safe(*digest, owner));
        if (!t) return names;
        for (auto const& e : t->entries) names.push_back(e.name);
        return names;
    }
};

// ---- T1 --------------------------------------------------------------------------------------------------------
void t1_no_mutex_across_suspension() {
    Fixture f("t1-owner");
    auto root = f.root();
    constexpr int kN = 4;
    std::vector<Tree> trees;
    for (int i = 0; i < kN; ++i) trees.push_back(f.tree_of({{"f" + std::to_string(i), "v" + std::to_string(i)}}));

    f.shared->put_tree_gate.hold = true;
    std::vector<task<result<Checkpoint>>> commits;
    for (int i = 0; i < kN; ++i) commits.push_back(f.ledger.commit(root, trees[i], f.owner, f.storage));
    for (auto& c : commits) start(c);   // the first parks in put_tree; the rest park on op_mutex_
    check(std::none_of(commits.begin(), commits.end(), [](auto& c) { return c.done(); }),
          "T1: every commit is parked while the first one is held in put_tree");

    f.shared->put_tree_gate.release();
    bool all_done = std::all_of(commits.begin(), commits.end(), [](auto& c) { return c.done(); });
    check(all_done, "T1: all commits complete once the store call is released");
    if (!all_done) return;
    std::vector<Checkpoint> cps;
    for (auto& c : commits) {
        auto r = c.take_value();
        check(r.has_value(), "T1: a concurrent commit succeeds");
        if (r) cps.push_back(*r);
    }
    std::vector<bool> seen(kN + 1, false);
    for (auto const& cp : cps) {
        if (cp.turn_index >= 1 && cp.turn_index <= kN) seen[cp.turn_index] = true;
    }
    bool turns_ok = cps.size() == kN;
    for (int t = 1; t <= kN; ++t) turns_ok = turns_ok && seen[t];
    check(turns_ok, "T1: the turn indices are exactly 1..N, none duplicated or skipped");
    bool chained = true;
    for (auto const& cp : cps) {
        if (cp.turn_index == 1) continue;
        bool found = false;
        for (auto const& other : cps) found = found || (other.turn_index + 1 == cp.turn_index && other.self_digest == cp.parent);
        chained = chained && found;
    }
    check(chained, "T1: each checkpoint's parent is the previous turn's self digest (one linear chain)");
}

// ---- T2 --------------------------------------------------------------------------------------------------------
void t2_merge_waits_for_commit() {
    Fixture f("t2-owner");
    auto parent = f.root();
    auto child = f.child_of(parent);
    std::string const parent_name = parent.name();
    {
        auto r = agentengine::rt::block_on(
            f.ledger.commit(child, f.tree_of({{"x.txt", "from child"}}), f.owner, f.storage));
        check(r.has_value(), "T2: setup: the child commits x.txt");
    }
    Tree parent_tree = f.tree_of({{"y.txt", "from parent"}});

    f.shared->put_tree_gate.hold = true;
    auto commit = f.ledger.commit(parent, parent_tree, f.owner, f.storage);
    start(commit);
    auto merge = f.ledger.merge(std::move(child), parent, f.owner, f.merges);
    start(merge);
    check(!commit.done() && !merge.done(), "T2: the commit is parked in put_tree and the merge waits behind it");

    f.shared->put_tree_gate.release();
    check(commit.done() && merge.done(), "T2: both finish once the commit is released");
    if (!(commit.done() && merge.done())) return;
    auto commit_r = commit.take_value();
    auto merge_r = merge.take_value();
    check(commit_r.has_value() && merge_r.has_value(), "T2: the commit and the merge both succeed");
    auto names = f.names_at_head(parent_name);
    auto has = [&](char const* n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    check(has("x.txt") && has("y.txt"), "T2: the merged head holds the child's x.txt AND the commit's y.txt");
}

// ---- T3 --------------------------------------------------------------------------------------------------------
void t3_abandon_serialised_behind_merge() {
    Fixture f("t3-owner");
    auto parent = f.root();
    auto merging = f.child_of(parent);
    auto doomed = f.child_of(parent);
    std::string const doomed_name = doomed.name();
    {
        auto r = agentengine::rt::block_on(
            f.ledger.commit(merging, f.tree_of({{"m.txt", "m"}}), f.owner, f.storage));
        check(r.has_value(), "T3: setup: the merging child commits");
    }
    f.shared->get_tree_gate.hold = true;
    auto merge = f.ledger.merge(std::move(merging), parent, f.owner, f.merges);
    start(merge);   // parks in get_tree, holding op_mutex_
    auto abandon = f.ledger.abandon(std::move(doomed));
    start(abandon);
    check(!merge.done() && !abandon.done(), "T3: the merge is parked and the abandon waits behind it");
    check(f.ledger.head_tree_digest(doomed_name, f.owner).has_value(),
          "T3: the doomed branch still exists while the merge is in flight");

    f.shared->get_tree_gate.release();
    check(merge.done() && abandon.done(), "T3: both finish once the merge is released");
    if (!(merge.done() && abandon.done())) return;
    check(merge.take_value().has_value(), "T3: the merge succeeds");
    check(abandon.take_value().has_value(), "T3: the abandon succeeds afterwards");
    check(!f.ledger.head_tree_digest(doomed_name, f.owner).has_value(), "T3: the doomed branch is gone");
}

// ---- T4 --------------------------------------------------------------------------------------------------------
void t4_authorization_gates_reads() {
    Fixture f("t4-owner");
    IdentityHandle stranger = f.authority.mint_root("t4-stranger");
    auto blob = agentengine::rt::block_on(f.ledger.put_blob_safe(to_bytes("secret"), f.owner));
    auto root = f.root();
    auto cp = agentengine::rt::block_on(
        f.ledger.commit(root, f.tree_of({{"s.txt", "secret"}}), f.owner, f.storage));

    f.shared->get_blob_calls = 0;
    f.shared->get_tree_calls = 0;
    auto denied_blob = agentengine::rt::block_on(f.ledger.get_blob_safe(*blob, stranger));
    auto denied_tree = agentengine::rt::block_on(f.ledger.get_tree_safe(cp->tree, stranger));
    check(!denied_blob.has_value() && denied_blob.error().code == "ledger.blob_access_denied",
          "T4: an unauthorized get_blob_safe() is refused with ledger.blob_access_denied");
    check(!denied_tree.has_value() && denied_tree.error().code == "ledger.tree_access_denied",
          "T4: an unauthorized get_tree_safe() is refused with ledger.tree_access_denied");
    check(f.shared->get_blob_calls == 0 && f.shared->get_tree_calls == 0,
          "T4: neither refused read reached the store");
    auto ok_blob = agentengine::rt::block_on(f.ledger.get_blob_safe(*blob, f.owner));
    auto ok_tree = agentengine::rt::block_on(f.ledger.get_tree_safe(cp->tree, f.owner));
    check(ok_blob.has_value() && ok_tree.has_value(), "T4: the authorized owner reads both");
    check(f.shared->get_blob_calls == 1 && f.shared->get_tree_calls == 1,
          "T4: each authorized read reached the store exactly once");
}

// ---- T5 --------------------------------------------------------------------------------------------------------
void t5_store_fault() {
    Fixture f("t5-owner");
    auto root = f.root();
    Tree t1 = f.tree_of({{"a.txt", "a"}});
    Tree t2 = f.tree_of({{"b.txt", "b"}});
    auto const before = f.storage.remaining();
    f.shared->throw_on_put_tree = true;
    auto faulted = agentengine::rt::block_on(f.ledger.commit(root, t1, f.owner, f.storage));
    check(!faulted.has_value() && faulted.error().code == "ledger.store_faulted",
          "T5: a put_tree that throws becomes ledger.store_faulted");
    check(f.storage.remaining() == before, "T5: the storage quota consumed for the failed commit is refunded");
    auto next = agentengine::rt::block_on(f.ledger.commit(root, t2, f.owner, f.storage));
    check(next.has_value() && next->turn_index == 1,
          "T5: op_mutex_ was released, so the next commit succeeds (and the failed one left no turn)");
}

// ---- T6 --------------------------------------------------------------------------------------------------------
void t6_no_self_deadlock() {
    fs::path const dir = fs::temp_directory_path() / "ae_ledger_async_t6";
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::string orphan_name;
    IdentityHandle owner = IdentityAuthority::bootstrap().mint_root("t6-owner");
    {
        auto shared = std::make_shared<SharedStore>();
        L first(GatedStore(shared), dir);
        auto root = agentengine::rt::block_on(first.create_root_branch(owner));
        orphan_name = root->name();
    }   // the handle's destructor queues an abandon in a ledger that is going away; the snapshot keeps the branch
    auto shared = std::make_shared<SharedStore>();
    L second(GatedStore(shared), dir);
    auto orphans = second.orphaned_branches();
    check(std::find(orphans.begin(), orphans.end(), orphan_name) != orphans.end(),
          "T6: setup: the restored ledger lists the branch as an orphan");
    auto abandoned = agentengine::rt::block_on(second.abandon_orphaned_branch(orphan_name, owner));
    check(abandoned.has_value(), "T6: abandon_orphaned_branch() completes (does not wait on its own op lock)");
    check(!second.head_tree_digest(orphan_name, owner).has_value(), "T6: the orphan is gone");

    {
        auto root2 = agentengine::rt::block_on(second.create_root_branch(owner, "again"));
        // `root2` is dropped here: its destructor queues an abandon for the reaper.
    }
    auto reaped = agentengine::rt::block_on(second.reap_pending_abandons());
    check(reaped == 1, "T6: reap_pending_abandons() completes and erases the dropped branch");
    fs::remove_all(dir, ec);
}

// ---- T7 --------------------------------------------------------------------------------------------------------
void t7_cancel_in_store_read() {
    Fixture f("t7-owner");
    auto parent = f.root();
    auto child = f.child_of(parent);
    std::string const child_name = child.name();
    {
        auto r = agentengine::rt::block_on(
            f.ledger.commit(child, f.tree_of({{"c.txt", "c"}}), f.owner, f.storage));
        check(r.has_value(), "T7: setup: the child commits");
    }
    f.shared->get_tree_gate.hold = true;
    {
        auto merge = f.ledger.merge(std::move(child), parent, f.owner, f.merges);
        start(merge);
        check(!merge.done(), "T7: the merge is parked in a store read");
        // `merge` is destroyed here with its frame still parked: the by-value child handle goes with it.
    }
    f.shared->get_tree_gate.forget();
    f.shared->get_tree_gate.hold = false;
    auto orphans = f.ledger.orphaned_branches();
    check(std::find(orphans.begin(), orphans.end(), child_name) != orphans.end(),
          "T7: the cancelled merge's child is registered as a reclaimable orphan");
    auto reaped = agentengine::rt::block_on(f.ledger.reap_pending_abandons());
    check(reaped == 0, "T7: reap_pending_abandons() erases nothing for it");
    check(f.ledger.head_tree_digest(child_name, f.owner).has_value(), "T7: the child branch still exists");
    auto reclaimed = f.ledger.reclaim_orphaned_branch(child_name, f.owner);
    check(reclaimed.has_value(), "T7: the caller can reclaim it");
}

// ---- T8 --------------------------------------------------------------------------------------------------------
// The refusal exists for a task FORKED by a scope (rt::when_all) from the lock's holder: it is an ancestor awaiting
// the child, so waiting for the lock would wait on itself forever. Reproduced the only way it can arise: from inside
// a store call made under op_mutex_, on a real runtime lane.
agentengine::rt::task<std::string> nested_reset(L* ledger, BranchHandle<GatedStore>* handle, IdentityHandle owner,
                                                  bool* threw) {
    try {
        auto r = co_await ledger->reset_to(*handle, 0, owner);
        co_return r.has_value() ? std::string("ok") : r.error().code;
    } catch (...) {
        *threw = true;
        co_return std::string("threw");
    }
}

agentengine::rt::task<void> forked_nested_reset(L* ledger, BranchHandle<GatedStore>* handle, IdentityHandle owner,
                                                 bool* threw, std::string* code) {
    auto results = co_await agentengine::rt::when_all(nested_reset(ledger, handle, owner, threw));
    *code = std::get<0>(results);
}

void t8_refusal() {
    Fixture f("t8-owner");
    auto root = f.root();
    Tree t = f.tree_of({{"r.txt", "r"}});
    bool threw = false;
    std::string nested_code;
    BranchHandle<GatedStore>* root_ptr = &root;
    L* ledger = &f.ledger;
    IdentityHandle owner = f.owner;
    bool* threw_ptr = &threw;
    std::string* code_ptr = &nested_code;
    f.shared->on_put_tree = [ledger, root_ptr, owner, threw_ptr, code_ptr]() -> agentengine::rt::task<void> {
        co_await forked_nested_reset(ledger, root_ptr, owner, threw_ptr, code_ptr);
    };
    agentengine::rt::RuntimeConfig cfg;
    cfg.lanes = 1;
    agentengine::rt::Runtime rt(cfg);
    auto outer = rt.run(f.ledger.commit(root, t, f.owner, f.storage));
    f.shared->on_put_tree = nullptr;
    check(!threw, "T8: the refused nested operation threw nothing");
    check(nested_code == "ledger.op_lock_refused", "T8: the nested operation is refused with ledger.op_lock_refused");
    check(outer.has_value(), "T8: the outer commit, which held the lock, still succeeds");
}

// ---- T9 --------------------------------------------------------------------------------------------------------
void t9_materialize_holds_no_std_mutex() {
    fs::path const dir = fs::temp_directory_path() / "ae_ledger_async_t9";
    std::error_code ec;
    fs::remove_all(dir, ec);
    Fixture f("t9-owner");
    auto root = f.root();
    auto cp = agentengine::rt::block_on(
        f.ledger.commit(root, f.tree_of({{"kept.txt", "kept"}}), f.owner, f.storage));
    RealIoFileSystem filesystem(dir);

    f.shared->get_blob_gate.hold = true;
    auto materialize = filesystem.materialize(f.ledger, cp->tree, f.owner);
    start(materialize);
    check(!materialize.done(), "T9: materialize() is parked in a Ledger blob read");
    auto written = filesystem.write("during.txt", to_bytes("written while parked"));
    check(written.has_value(), "T9: a write() on this thread completes while materialize() is parked");

    f.shared->get_blob_gate.release();
    check(materialize.done() && materialize.take_value().has_value(), "T9: materialize() then completes");
    fs::remove_all(dir, ec);
}

}  // namespace

int main() {
    t1_no_mutex_across_suspension();
    t2_merge_waits_for_commit();
    t3_abandon_serialised_behind_merge();
    t4_authorization_gates_reads();
    t5_store_fault();
    t6_no_self_deadlock();
    t7_cancel_in_store_read();
    t8_refusal();
    t9_materialize_holds_no_std_mutex();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
