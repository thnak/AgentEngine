// ADR-209 build step 2 (GitHub issue #146): `Ledger::head_checkpoint()` and `SandboxRuntime::run_live()`, driven
// against an in-test `PersistentShellSurface` -- NO daemon. The live-Docker half is
// test_live_shell_sandbox_provider_live.cpp.
//
//   H1  head_checkpoint(): a fresh root reports turn 0 with an empty self_digest; after a commit it is that
//       commit's checkpoint; after reset_to() back to an EQUAL tree it is a NEW checkpoint (new self_digest,
//       same tree) -- the property the live shell's sync check depends on (ADR-209 pass 3 #2).
//   H2  head_checkpoint() is ACL-gated like head_tree_digest(): an identity with no access is refused.
//   R1  the first run_live() opens (hook called once, `reopened`), runs, commits the write.
//   R2  a second run_live() at the same head does NOT re-open, and shell state (cwd) carried over.
//   R3  a deletion is committed (the drain empties staging first -- issue #143).
//   R4  reset_to_turn() to a checkpoint whose tree equals the head's still forces a re-open (sync on the
//       checkpoint, not the tree).
//   R5  a commit that fails after the command ran (StorageBytes exhausted) is a VALUE: exit code and output
//       intact, `commit_error` set, no checkpoint; the next command re-opens (C17).
//   R6  an open-hook refusal (LiveShellOpen spent) is an error and RunCost is refunded: nothing ran.
//   R7  container_lost: nothing to drain, `commit_error`, the next command re-opens.
//   R8  shell_lost: the workspace is still drained and committed (partial writes recorded), then re-open.
//   R9  a command the surface refuses up front is an error, RunCost refunded, and the shell is NOT re-opened.
//   R10 an identity that is not the quota owner is refused before anything runs (C6): no open, no command.
//
// Positive controls (planted by hand, recorded in ADR-209 §15): syncing on `head->tree` instead of
// `head->self_digest` fails R4; returning the commit error as an error result fails R5; skipping the
// RunCost refund on a hook refusal fails R6.

#include "agentengine/sandbox/execution_surface.hpp"
#include "agentengine/sandbox/persistent_shell.hpp"
#include "agentengine/sandbox/sandbox_runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// A "container" that is a private directory plus a fake shell (cwd string, env map). Commands:
// "write <p> <c>" | "rm <p>" | "cd <d>" | "export K=V" | "noop" | "lose" (shell dies after writing lost.txt) |
// "container_lost" | "refuse" (rejected up front).
class FakeLiveSurface {
public:
    explicit FakeLiveSurface(fs::path box) : box_(std::move(box)) {}

    [[nodiscard]] result<void> open(fs::path const& host_dir, ShellSnapshot const* snap) {
        ++opens;
        std::error_code ec;
        fs::remove_all(box_, ec);
        fs::create_directories(box_, ec);
        fs::copy(host_dir, box_, fs::copy_options::recursive, ec);
        if (ec) return std::unexpected(error{failure_class::fatal, "seed failed: " + ec.message(), "test.seed"});
        cwd_ = "/workspace";
        env_.clear();
        if (snap != nullptr) {
            replayed.push_back(*snap);
            if (!snap->cwd.empty()) cwd_ = snap->cwd;
            for (auto const& [k, v] : snap->env_set) env_[k] = v;
        }
        live_ = true;
        has_container_ = true;
        return {};
    }

    [[nodiscard]] result<LiveExecOutcome> exec(std::string const& command, std::chrono::milliseconds) {
        if (!live_) return std::unexpected(error{failure_class::contract, "no live shell", "test.not_live"});
        if (command == "refuse") return std::unexpected(error{failure_class::policy, "refused", "test.refused"});
        ++execs;
        LiveExecOutcome out;
        out.exit_code = 0;
        std::error_code ec;
        if (command.rfind("write ", 0) == 0) {
            auto const rest = command.substr(6);
            auto const sp = rest.find(' ');
            std::ofstream(box_ / rest.substr(0, sp), std::ios::binary) << rest.substr(sp + 1);
        } else if (command.rfind("rm ", 0) == 0) {
            fs::remove(box_ / command.substr(3), ec);
        } else if (command.rfind("cd ", 0) == 0) {
            cwd_ = command.substr(3);
        } else if (command.rfind("export ", 0) == 0) {
            auto const kv = command.substr(7);
            env_[kv.substr(0, kv.find('='))] = kv.substr(kv.find('=') + 1);
        } else if (command == "lose") {
            std::ofstream(box_ / "lost.txt", std::ios::binary) << "partial";
            live_ = false;
            out.shell_lost = true;
            out.exit_code = -1;
            out.output = "partial output";
            return out;
        } else if (command == "container_lost") {
            live_ = false;
            has_container_ = false;
            out.container_lost = true;
            out.timed_out = true;
            out.exit_code = -1;
            return out;
        }
        out.output = "ran: " + command + " in " + cwd_;
        out.output_bytes = out.output.size();
        ShellSnapshot snap;
        snap.cwd = cwd_;
        for (auto const& [k, v] : env_) snap.env_set.emplace_back(k, v);
        out.snapshot = snap;
        return out;
    }

    [[nodiscard]] result<void> drain_to(fs::path const& host_dir) {
        if (!has_container_) return std::unexpected(error{failure_class::fatal, "no container", "test.no_container"});
        auto cleared = clear_directory_contents(host_dir, "test.drain_clear");
        if (!cleared.has_value()) return std::unexpected(cleared.error());
        std::error_code ec;
        fs::create_directories(host_dir, ec);
        fs::copy(box_, host_dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) return std::unexpected(error{failure_class::fatal, "drain failed: " + ec.message(), "test.drain"});
        return {};
    }

    [[nodiscard]] result<void> close() {
        live_ = false;
        has_container_ = false;
        return {};
    }
    [[nodiscard]] bool is_live() const { return live_; }

    int opens = 0;
    int execs = 0;
    std::vector<ShellSnapshot> replayed;

private:
    fs::path box_;
    bool live_ = false;
    bool has_container_ = false;
    std::string cwd_;
    std::map<std::string, std::string> env_;
};
static_assert(PersistentShellSurface<FakeLiveSurface>);

[[nodiscard]] std::vector<std::string> committed_names(Ledger<>& ledger, Checkpoint const& cp, IdentityHandle who) {
    std::vector<std::string> out;
    auto tree = ledger.get_tree_safe(cp.tree, who);
    if (!tree.has_value()) return {"<tree unreadable>"};
    for (auto const& e : tree->entries) out.push_back(e.name);
    std::sort(out.begin(), out.end());
    return out;
}

[[nodiscard]] bool has(std::vector<std::string> const& v, std::string const& n) {
    return std::find(v.begin(), v.end(), n) != v.end();
}

}  // namespace

int main() {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle owner = authority.mint_root("runtime-live-owner");
    IdentityHandle stranger = authority.mint_root("runtime-live-stranger");
    Ledger<> ledger;
    auto storage_quota = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    auto tiny_storage = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 1);
    auto run_quota = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    auto reset_quota = agentengine::rt::AsyncQuota<ResetCost>::mint_root(authority, owner, 10);
    auto root = drive(ledger.create_root_branch(owner));
    if (!storage_quota || !tiny_storage || !run_quota || !reset_quota || !root) {
        std::printf("[FAIL] setup\n");
        return EXIT_FAILURE;
    }
    std::string const branch_name = root->name();

    // ---- H1/H2: head_checkpoint().
    {
        auto h0 = ledger.head_checkpoint(branch_name, owner);
        check(h0.has_value() && h0->turn_index == 0 && h0->self_digest.empty(),
              "H1: a fresh root's head is turn 0 with an empty self_digest");
        auto h_denied = ledger.head_checkpoint(branch_name, stranger);
        check(!h_denied.has_value() && h_denied.error().code == "ledger.tree_access_denied",
              "H2: an identity with no access to the head tree is refused");
    }

    fs::path const staging = fs::temp_directory_path() / "ae_test_runtime_live_staging";
    fs::path const box = fs::temp_directory_path() / "ae_test_runtime_live_box";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::remove_all(box, ec);

    SandboxRuntime runtime(ledger, std::move(*root), staging);
    FakeLiveSurface surface(box);
    LiveShellSync sync;
    int hook_calls = 0;
    bool hook_refuses = false;
    std::map<std::string, ShellSnapshot> snapshots;  // keyed by checkpoint self_digest, as the provider keeps them
    auto hook = [&](Checkpoint const& head) -> agentengine::rt::task<result<std::optional<ShellSnapshot>>> {
        ++hook_calls;
        if (hook_refuses) {
            co_return std::unexpected(error{failure_class::resource, "LiveShellOpen spent", "test.open_refused"});
        }
        auto it = snapshots.find(head.self_digest);
        if (it == snapshots.end()) co_return std::optional<ShellSnapshot>{};
        co_return std::optional<ShellSnapshot>{it->second};
    };
    auto live_as = [&](std::string const& cmd, IdentityHandle who,
                       agentengine::rt::AsyncQuota<StorageBytes>* storage) {
        auto r = drive(runtime.run_live(surface, cmd, who, *run_quota, storage ? *storage : *storage_quota, sync,
                                        hook, std::chrono::milliseconds(1000)));
        if (r.has_value() && r->checkpoint.has_value() && r->exec.snapshot.has_value()) {
            snapshots[r->checkpoint->self_digest] = *r->exec.snapshot;
        }
        return r;
    };
    auto live = [&](std::string const& cmd) { return live_as(cmd, owner, nullptr); };

    // ---- R1: first use opens.
    auto r1 = live("write a.txt A");
    check(r1.has_value() && r1->reopened && hook_calls == 1 && surface.opens == 1,
          "R1: the first run_live() opens exactly once");
    check(r1.has_value() && r1->checkpoint.has_value() && !r1->commit_error.has_value() &&
              has(committed_names(ledger, *r1->checkpoint, owner), "a.txt"),
          "R1: ... and commits the write before returning");
    {
        auto h1 = ledger.head_checkpoint(branch_name, owner);
        check(h1.has_value() && r1.has_value() && r1->checkpoint.has_value() &&
                  h1->self_digest == r1->checkpoint->self_digest && h1->turn_index == r1->checkpoint->turn_index,
              "H1: after a commit the head checkpoint is that commit's checkpoint");
    }

    // ---- R2: same head, no re-open; state carried.
    (void)live("cd /workspace/sub");
    auto r2 = live("write b.txt B");
    check(r2.has_value() && !r2->reopened && hook_calls == 1 && surface.opens == 1,
          "R2: commands at the synced head do not re-open");
    check(r2.has_value() && r2->exec.output.find("in /workspace/sub") != std::string::npos,
          "R2: ... and the shell's cwd persisted between commands");
    std::uint64_t const turn_after_b = r2.has_value() && r2->checkpoint ? r2->checkpoint->turn_index : 0;

    // ---- R3: deletion.
    auto r3 = live("rm a.txt");
    check(r3.has_value() && r3->checkpoint.has_value() &&
              !has(committed_names(ledger, *r3->checkpoint, owner), "a.txt") &&
              has(committed_names(ledger, *r3->checkpoint, owner), "b.txt"),
          "R3: a deletion is committed (a.txt absent, b.txt kept)");

    // ---- R4: reset to an EQUAL tree still re-opens.
    {
        auto noop = live("noop");  // a checkpoint whose tree equals r3's
        check(noop.has_value() && noop->checkpoint.has_value() && r3.has_value() && r3->checkpoint.has_value() &&
                  noop->checkpoint->tree == r3->checkpoint->tree,
              "R4 setup: a cwd-only/no-op command commits the same tree");
        int const opens_before = surface.opens;
        auto reset = drive(runtime.reset_to_turn(r3->checkpoint->turn_index, owner, *reset_quota));
        check(reset.has_value() && reset->tree == r3->checkpoint->tree &&
                  reset->self_digest != noop->checkpoint->self_digest,
              "H1: reset_to() back to an equal tree appends a NEW checkpoint (new self_digest, same tree)");
        auto after = live("noop");
        check(after.has_value() && after->reopened && surface.opens == opens_before + 1,
              "R4: the next command re-opens although the tree did not change (sync on the checkpoint)");
    }

    // ---- R5: commit failure after the command ran.
    {
        auto r5 = live_as("write big.txt payload", owner, &*tiny_storage);
        check(r5.has_value(), "R5: a commit failing after the command ran is a VALUE, not an error result");
        if (r5.has_value()) {
            check(!r5->checkpoint.has_value() && r5->commit_error.has_value() &&
                      r5->commit_error->code == "async_quota.exhausted",
                  "R5: ... with commit_error set and no checkpoint");
            check(r5->exec.exit_code == 0 && r5->exec.output.find("write big.txt") != std::string::npos,
                  "R5: ... and the command's exit code and output intact");
        }
        check(sync.desynced, "R5: the shell is marked desynced");
        int const opens_before = surface.opens;
        auto next = live("noop");
        check(next.has_value() && next->reopened && surface.opens == opens_before + 1,
              "R5: the next command re-opens from the head (the unrecorded change is discarded)");
        check(next.has_value() && next->checkpoint.has_value() &&
                  !has(committed_names(ledger, *next->checkpoint, owner), "big.txt"),
              "R5: ... and the unrecorded file is not resurrected into the next commit");
    }

    // ---- R6: open-hook refusal refunds RunCost.
    {
        sync.desynced = true;  // force an open
        hook_refuses = true;
        std::uint64_t const before = run_quota->remaining();
        int const execs_before = surface.execs;
        auto r6 = live("write never.txt x");
        check(!r6.has_value() && r6.error().code == "test.open_refused", "R6: an open refusal is an error");
        check(run_quota->remaining() == before, "R6: ... RunCost refunded");
        check(surface.execs == execs_before, "R6: ... and the command never ran");
        hook_refuses = false;
    }

    // ---- R7: container lost.
    {
        auto r7 = live("container_lost");
        check(r7.has_value() && r7->exec.container_lost && r7->commit_error.has_value() &&
                  r7->commit_error->code == "sandbox_runtime.live_container_lost" && !r7->checkpoint.has_value(),
              "R7: container_lost is reported with commit_error and no checkpoint");
        int const opens_before = surface.opens;
        auto next = live("noop");
        check(next.has_value() && next->reopened && surface.opens == opens_before + 1,
              "R7: the next command opens a fresh environment");
    }

    // ---- R8: shell lost, container alive -> partial writes committed, then re-open with the last snapshot.
    {
        (void)live("cd /workspace/keep");
        (void)live("export KEPT=1");
        auto r8 = live("lose");
        check(r8.has_value() && r8->exec.shell_lost && r8->checkpoint.has_value() &&
                  has(committed_names(ledger, *r8->checkpoint, owner), "lost.txt"),
              "R8: a lost shell's partial writes are drained and committed");
        // A lost shell reports no snapshot; the provider maps the new checkpoint to the prior head's snapshot.
        if (r8.has_value() && r8->checkpoint.has_value()) {
            auto prior = snapshots.find(r8->prior_head.self_digest);
            if (prior != snapshots.end()) snapshots[r8->checkpoint->self_digest] = prior->second;
        }
        std::size_t const replays_before = surface.replayed.size();
        auto next = live("noop");
        check(next.has_value() && next->reopened && surface.replayed.size() == replays_before + 1 &&
                  surface.replayed.back().cwd == "/workspace/keep",
              "R8: the next command re-opens and replays the last completed command's cwd");
    }

    // ---- R9: refused up front.
    {
        std::uint64_t const before = run_quota->remaining();
        int const opens_before = surface.opens;
        auto r9 = live("refuse");
        check(!r9.has_value() && r9.error().code == "test.refused" && run_quota->remaining() == before,
              "R9: a command refused up front is an error and RunCost is refunded");
        auto next = live("noop");
        check(next.has_value() && !next->reopened && surface.opens == opens_before,
              "R9: ... and the shell is not re-opened for it");
    }

    // ---- R10: a non-owner is refused before anything runs.
    {
        sync.desynced = true;
        int const hook_before = hook_calls;
        int const execs_before = surface.execs;
        auto r10 = live_as("write intruder.txt x", stranger, nullptr);
        check(!r10.has_value() && r10.error().code == "async_quota.unauthorized_spender",
              "R10: a non-owner identity is refused with a named error");
        check(hook_calls == hook_before && surface.execs == execs_before,
              "R10: ... before any open or command");
    }
    (void)turn_after_b;

    fs::remove_all(staging, ec);
    fs::remove_all(box, ec);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
