// GitHub issue #143: a file a sandboxed command deletes must be absent from the checkpoint `run()` commits.
//
// `SandboxRuntime::run()` materializes the head into staging (step 2), seeds the surface from it (step 3),
// runs the command (step 4), drains the surface back into staging (step 5), scans staging (step 6) and
// commits (step 7). A real drain only ADDS -- `docker cp` copies over what is there and removes nothing --
// so before the fix, a file the command removed was still in staging from step 2, and the scan committed it
// again. `rm f` reported success and `f` stayed in history.
//
// The fix is in `drain_to()`'s contract (execution_surface.hpp): afterwards `host_dir` holds EXACTLY the
// surface's view. A copy-out surface meets it by calling `clear_directory_contents()` before copying; a
// bind-mounting surface already meets it. The first fix wiped staging in `run()` instead, which destroyed
// a bind-mounting surface's whole output -- test_composed_containerd_providers_live caught it on Linux CI.
// Both shapes are exercised here with no daemon:
//
//   `CopyOutSurface`  -- a private "container" directory, copied back with `docker cp`'s additive
//                        semantics after `clear_directory_contents()`, exactly as DockerExecutionSurface.
//   `InPlaceSurface`  -- the command works directly in `host_dir` and `drain_to()` is a no-op, exactly as
//                        ContainerdExecutionSurface's bind mount.
//
// The live halves are test_sandbox_runtime.cpp's [11] (Docker) and test_composed_containerd_providers_live.
//
//   D1 -- the seed turn commits {keep.txt, gone.txt, dir/inner.txt}. Everything after it is measured
//         against this, so it comes first.
//   D2 -- a turn that deletes gone.txt commits a tree WITHOUT gone.txt, and keep.txt is still there
//         (the clear did not throw away what the surface returned).
//   D3 -- deleting a whole directory removes every entry under it.
//   D4 -- a turn that deletes nothing still commits the same set (no over-deletion).
//   D5 -- with `InPlaceSurface`, a turn that writes one file and deletes another commits exactly that:
//         the written file present, the deleted one absent. Nothing between run and scan may wipe staging.
//
// Positive controls (planted mutants, run by hand, recorded in the fixing commit): with
// `clear_directory_contents()` reduced to `return {};`, D2 and D3 fail; with a wipe of staging put back
// into `run()` before the drain, D5 fails.
//
// Needs no daemon, no network and no privileges.

#include "agentengine/sandbox/execution_surface.hpp"
#include "agentengine/sandbox/sandbox_runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// "write <path> <content>" | "rm <path>" | "rmdir <path>" (recursive) | "noop", applied under `root`.
[[nodiscard]] result<SurfaceRunOutcome> apply(fs::path const& root, std::string const& command) {
    std::error_code ec;
    if (command.rfind("write ", 0) == 0) {
        auto const rest = command.substr(6);
        auto const sp = rest.find(' ');
        fs::path const p = root / rest.substr(0, sp);
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary) << rest.substr(sp + 1);
    } else if (command.rfind("rm ", 0) == 0) {
        fs::remove(root / command.substr(3), ec);
    } else if (command.rfind("rmdir ", 0) == 0) {
        fs::remove_all(root / command.substr(6), ec);
    } else if (command != "noop") {
        return std::unexpected(error{failure_class::contract, "unknown command: " + command, "test.cmd"});
    }
    return SurfaceRunOutcome{ec ? 1 : 0, {}};
}

// A surface whose "container" is a private directory. `reset()` replaces it with a copy of host_dir, and
// `drain_to()` empties host_dir then copies the box OVER it without removing anything --
// DockerExecutionSurface's `clear_directory_contents()` + `docker cp container:/workspace/. host_dir`.
class CopyOutSurface {
public:
    explicit CopyOutSurface(fs::path box) : box_(std::move(box)) {}

    [[nodiscard]] result<void> reset(fs::path const& host_dir) {
        std::error_code ec;
        fs::remove_all(box_, ec);
        fs::create_directories(box_, ec);
        fs::copy(host_dir, box_, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) return std::unexpected(error{failure_class::fatal, "seed copy failed: " + ec.message(), "test.seed"});
        return {};
    }

    [[nodiscard]] result<SurfaceRunOutcome> run(std::string const& command) { return apply(box_, command); }

    [[nodiscard]] result<void> drain_to(fs::path const& host_dir) {
        auto cleared = clear_directory_contents(host_dir, "test.drain_clear");
        if (!cleared.has_value()) return std::unexpected(cleared.error());
        std::error_code ec;
        fs::create_directories(host_dir, ec);
        fs::copy(box_, host_dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) return std::unexpected(error{failure_class::fatal, "drain copy failed: " + ec.message(), "test.drain"});
        return {};
    }

private:
    fs::path box_;
};
static_assert(ExecutionSurface<CopyOutSurface>);

// The command works directly in the directory `reset()` was given, and `drain_to()` has nothing to do --
// ContainerdExecutionSurface's bind mount at /workspace.
class InPlaceSurface {
public:
    [[nodiscard]] result<void> reset(fs::path const& host_dir) {
        dir_ = host_dir;
        return {};
    }
    [[nodiscard]] result<SurfaceRunOutcome> run(std::string const& command) { return apply(dir_, command); }
    [[nodiscard]] result<void> drain_to(fs::path const&) { return {}; }

private:
    fs::path dir_;
};
static_assert(ExecutionSurface<InPlaceSurface>);

[[nodiscard]] std::vector<std::string> committed_names(Ledger<>& ledger, Checkpoint const& cp, IdentityHandle who) {
    std::vector<std::string> out;
    auto tree = ledger.get_tree_safe(cp.tree, who);
    if (!tree.has_value()) return {"<tree unreadable>"};
    for (auto const& e : tree->entries) out.push_back(e.name);
    std::sort(out.begin(), out.end());
    return out;
}

[[nodiscard]] std::string show(std::vector<std::string> const& v) {
    std::string s = "{";
    for (auto const& n : v) s += (s.size() > 1 ? ", " : "") + n;
    return s + "}";
}

}  // namespace

int main() {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle owner = authority.mint_root("sandbox-runtime-deletions-owner");
    Ledger<> ledger;
    auto storage_quota = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    auto run_quota = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    auto root = drive(ledger.create_root_branch(owner));
    if (!storage_quota || !run_quota || !root) {
        std::printf("[FAIL] setup (quota or root branch)\n");
        return EXIT_FAILURE;
    }

    fs::path const staging = fs::temp_directory_path() / "ae_test_runtime_deletions_staging";
    fs::path const box = fs::temp_directory_path() / "ae_test_runtime_deletions_box";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::remove_all(box, ec);

    SandboxRuntime runtime(ledger, std::move(*root), staging);
    CopyOutSurface surface(box);
    auto turn = [&](std::string const& cmd) {
        return drive(runtime.run(surface, cmd, owner, *run_quota, *storage_quota));
    };

    // ---- D1: the seed.
    (void)turn("write keep.txt k");
    (void)turn("write gone.txt g");
    auto seed = turn("write dir/inner.txt i");
    check(seed.has_value(), "D1: the seed turns succeed");
    if (!seed.has_value()) return EXIT_FAILURE;
    std::vector<std::string> const all{"dir/inner.txt", "gone.txt", "keep.txt"};
    check(committed_names(ledger, seed->checkpoint, owner) == all,
          "D1: the seed checkpoint holds " + show(all) + ", got " + show(committed_names(ledger, seed->checkpoint, owner)));

    // ---- D2: delete one file.
    {
        auto r = turn("rm gone.txt");
        check(r.has_value() && r->exec.exit_code == 0, "D2: the deleting turn succeeds");
        if (r.has_value()) {
            auto const names = committed_names(ledger, r->checkpoint, owner);
            check(std::find(names.begin(), names.end(), "gone.txt") == names.end(),
                  "D2: gone.txt is ABSENT from the committed tree (was resurrected before #143's fix); got " + show(names));
            check(std::find(names.begin(), names.end(), "keep.txt") != names.end(),
                  "D2: ... and keep.txt is still there -- the drain kept what the surface returned");
        }
    }

    // ---- D3: delete a directory.
    {
        auto r = turn("rmdir dir");
        check(r.has_value(), "D3: the directory-deleting turn succeeds");
        if (r.has_value()) {
            auto const names = committed_names(ledger, r->checkpoint, owner);
            check(names == std::vector<std::string>{"keep.txt"},
                  "D3: every entry under the deleted directory is gone; got " + show(names));
        }
    }

    // ---- D4: no over-deletion.
    {
        auto r = turn("noop");
        check(r.has_value() && committed_names(ledger, r->checkpoint, owner) == std::vector<std::string>{"keep.txt"},
              "D4: a turn that deletes nothing commits the same set");
    }

    // ---- D5: a bind-mount-shaped surface keeps its output, deletions included.
    {
        auto root5 = drive(ledger.create_root_branch(owner, "in-place"));
        check(root5.has_value(), "D5 setup: a second root branch");
        if (root5.has_value()) {
            fs::path const staging5 = fs::temp_directory_path() / "ae_test_runtime_deletions_staging_inplace";
            fs::remove_all(staging5, ec);
            SandboxRuntime runtime5(ledger, std::move(*root5), staging5);
            InPlaceSurface in_place;
            auto in = [&](std::string const& cmd) {
                return drive(runtime5.run(in_place, cmd, owner, *run_quota, *storage_quota));
            };
            (void)in("write old.txt o");
            auto r = in("write made.txt m");
            auto d = in("rm old.txt");
            check(r.has_value() && d.has_value(), "D5: the in-place turns succeed");
            if (d.has_value()) {
                auto const names = committed_names(ledger, d->checkpoint, owner);
                check(names == std::vector<std::string>{"made.txt"},
                      "D5: the in-place surface's output is committed (made.txt) and its deletion holds "
                      "(old.txt gone); got " + show(names));
            }
            fs::remove_all(staging5, ec);
        }
    }

    fs::remove_all(staging, ec);
    fs::remove_all(box, ec);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
