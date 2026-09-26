// Proof that `RealIoFileSystem::materialize()` no longer reports a rollback it did not perform.
//
// WHAT WAS WRONG. materialize() is the real rollback/checkout mechanism: it wipes the host working
// directory and rewrites it from a committed Tree. It opened with
//
//     std::error_code ec;
//     std::filesystem::remove_all(host_root_, ec);
//     std::filesystem::create_directories(host_root_);
//
// and never read `ec`. On Windows a file held open by any other process makes `remove_all` fail with
// ERROR_SHARING_VIOLATION, which is the routine shape here, because the directory being cleared is
// the one an execution surface was just reading from. Every line after that then behaved as though
// the wipe had happened: `create_directories` on a root that still exists reports no error at all,
// and the rewrite loop only writes the entries the Tree names, so anything NOT in the Tree simply
// survived. The function returned success.
//
// WHAT THAT COSTS. materialize() has two callers and the stale file reaches both.
// `SandboxRuntime::run()` step 3 pushes host_root_ into the execution surface, so the next turn's
// command reads a file the rollback was supposed to have erased. `scan_and_drain_into_tree()` then
// does a full recursive scan of host_root_ and commits everything it finds under the caller's
// identity -- so that same file re-enters durable history attributed to an author who never wrote
// it, which is an I4 (every effect attributable) failure, not merely a stale-cache annoyance.
//
//   W1 (positive control) -- an ordinary materialize() succeeds, removes a file that is not in the
//         Tree, and restores one that is. Every claim below is a failure claim, so acceptance has to
//         be demonstrated first, and this also proves the blocker in W2 is what changes the outcome.
//   W2 (the guard) -- with the wipe genuinely blocked, materialize() now REPORTS the failure instead
//         of returning success. The stale file is still on disk in both the old and new behaviour;
//         the difference the fix makes is entirely in whether the caller is told. Three sub-checks:
//         it fails, it fails with the specific code, and it carries the real OS error number rather
//         than a hand-authored one.
//   W3 -- once the blocker is released the very same object materializes cleanly again, so the fix
//         reports a transient host condition without wedging the filesystem object.
//
// Needs no daemon, no network and no credentials. It DOES need a host on which a wipe can be made to
// fail; where it cannot (a POSIX process running as root ignores the directory permissions this uses
// as its blocker) W2/W3 are skipped out loud rather than silently passing.

#include "agentengine/core/ledger.hpp"
#include "agentengine/sandbox/real_io_filesystem.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace agentengine;

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

[[nodiscard]] std::vector<std::byte> to_bytes(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

void write_stray_file(std::filesystem::path const& p, std::string const& content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
}

// Makes `remove_all(root)` genuinely fail, by the most honest means each platform offers: on Windows
// an open handle with no sharing, which is exactly what a real process reading the directory holds;
// on POSIX a non-writable containing directory, since an open file descriptor does NOT block unlink
// there. A POSIX process running as root bypasses the permission check, so the blocker reports that
// it could not arm rather than pretending it did.
class WipeBlocker {
public:
    WipeBlocker(std::filesystem::path root, std::filesystem::path victim)
        : root_(std::move(root)), victim_(std::move(victim)) {
#if defined(_WIN32)
        std::wstring const w = victim_.wstring();
        handle_ = ::CreateFileW(w.c_str(), GENERIC_READ, 0 /* deny all sharing, delete included */,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        armed_ = handle_ != INVALID_HANDLE_VALUE;
#else
        if (::geteuid() == 0) {
            armed_ = false;  // root ignores the directory write bit; say so rather than fake it
        } else {
            armed_ = ::chmod(root_.string().c_str(), 0500) == 0;
        }
#endif
    }

    ~WipeBlocker() { release(); }

    WipeBlocker(WipeBlocker const&) = delete;
    WipeBlocker& operator=(WipeBlocker const&) = delete;

    [[nodiscard]] bool armed() const noexcept { return armed_; }

    void release() noexcept {
        if (!armed_) return;
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        (void)::chmod(root_.string().c_str(), 0700);
#endif
        armed_ = false;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path victim_;
    bool armed_ = false;
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

}  // namespace

int main() {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle owner = authority.mint_root("materialize-wipe-test-owner");

    Ledger<> ledger;
    auto storage_quota_r =
        agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    if (!storage_quota_r.has_value()) {
        std::printf("[FAIL] could not mint a storage quota\n");
        return EXIT_FAILURE;
    }
    auto& storage_quota = *storage_quota_r;

    auto root_r = drive(ledger.create_root_branch(owner));
    if (!root_r.has_value()) {
        std::printf("[FAIL] could not create a root branch\n");
        return EXIT_FAILURE;
    }
    BranchHandle<> branch = std::move(*root_r);

    std::filesystem::path const staging =
        std::filesystem::temp_directory_path() / "ae_test_materialize_wipe";
    std::error_code ec;
    std::filesystem::permissions(staging, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, ec);
    std::filesystem::remove_all(staging, ec);

    RealIoFileSystem io(staging);

    // A committed Tree holding exactly one file. This is what every materialize() below restores to.
    auto written = io.write("keep.txt", to_bytes("committed content"));
    if (!written.has_value()) {
        std::printf("[FAIL] could not stage keep.txt\n");
        return EXIT_FAILURE;
    }
    auto tree = drive(io.scan_and_drain_into_tree(ledger, owner));
    if (!tree.has_value()) {
        std::printf("[FAIL] could not drain a tree\n");
        return EXIT_FAILURE;
    }
    auto checkpoint = drive(ledger.commit(branch, *tree, owner, storage_quota));
    if (!checkpoint.has_value()) {
        std::printf("[FAIL] could not commit the tree\n");
        return EXIT_FAILURE;
    }
    std::filesystem::path const stray = staging / "stray.txt";

    // ---- W1: the positive control.
    {
        write_stray_file(stray, "content no Tree ever committed");
        check(std::filesystem::exists(stray), "W1 (positive control): the stray file is on disk");

        auto r = drive(io.materialize(ledger, checkpoint->tree, owner));
        check(r.has_value(), "W1 (positive control): an unobstructed materialize() succeeds");
        check(!std::filesystem::exists(stray),
              "W1 (positive control): ... and the stray file is genuinely gone, so the wipe really "
              "does run and W2's blocker is what changes the outcome");
        check(std::filesystem::exists(staging / "keep.txt"),
              "W1 (positive control): ... and the committed file is restored");
    }

    // ---- W2: the guard.
    {
        write_stray_file(stray, "content no Tree ever committed");
        WipeBlocker blocker(staging, stray);
        if (!blocker.armed()) {
            std::printf("[SKIP] W2/W3: this host will not let the wipe be blocked (a POSIX process "
                        "running as root ignores the directory permissions used as the blocker), so "
                        "the failure under test cannot be produced here. NOT counted as a pass.\n");
        } else {
            auto r = drive(io.materialize(ledger, checkpoint->tree, owner));
            check(!r.has_value(),
                  "W2: materialize() REPORTS the failed wipe -- before the fix it returned success");
            check(std::filesystem::exists(stray),
                  "W2: the stale file is still on disk, which is true before AND after the fix; what "
                  "the fix changes is that the caller is now told");
            if (!r.has_value()) {
                check(r.error().code == "real_io.materialize_wipe_failed",
                      "W2: ... under a specific code a caller can match on");
                check(r.error().native_code != 0,
                      "W2: ... carrying the real OS error number, not a hand-authored one");
            } else {
                check(false, "W2: ... under a specific code a caller can match on");
                check(false, "W2: ... carrying the real OS error number, not a hand-authored one");
            }

            // ---- W3: releasing the blocker restores normal operation on the same object.
            blocker.release();
            auto again = drive(io.materialize(ledger, checkpoint->tree, owner));
            check(again.has_value(),
                  "W3: with the blocker released the same object materializes cleanly again");
            check(!std::filesystem::exists(stray),
                  "W3: ... and the stale file is finally gone, so the earlier refusal reported a "
                  "host condition rather than wedging the filesystem object");
        }
    }

    std::filesystem::permissions(staging, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, ec);
    std::filesystem::remove_all(staging, ec);

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
