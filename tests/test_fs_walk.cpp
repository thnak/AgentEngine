// Proof for `core/fs_walk.hpp`, the shared directory walk introduced for issue #71's defect class.
//
// The shape it replaces, which three sites in this tree reached independently:
//
//     for (auto const& e : std::filesystem::recursive_directory_iterator(dir, opts, ec)) {
//         if (ec) return std::unexpected(...);
//         ...
//     }
//
// has two separate defects, and F2/F4 below are each aimed at one of them:
//
//   * the `ec` overload covers only the CONSTRUCTOR, while a range-for calls the THROWING
//     `operator++`, so the place a walk actually fails is the place that throws -- straight past a
//     `result<T>` contract;
//   * when construction DOES fail the iterator equals end, the body never runs, and the `if (ec)`
//     inside it is dead code, so the caller gets an empty result and no error.
//
//   F1 (positive control) -- the walk visits every regular file under a real tree exactly once, and
//         skips directories. Everything else here is a failure claim, so acceptance comes first.
//   F2 -- an unreadable root is an ERROR, not an empty success. This is the silent-truncation half.
//   F3 -- a visitor's own error propagates unchanged and stops the walk, so callers keep their own
//         error codes rather than having them flattened into the walk's.
//   F4 -- when the tree is deleted from under a live walk, the call RETURNS. That is the whole
//         contract: the raw iterator throws here (verified separately on this machine), and a throw
//         out of these callers is what ADR-174's red-team reproduced as a process kill.
//   F5 -- the non-recursive entry point sees the top level only, and reports directories, so the two
//         helpers are genuinely different rather than one wrapping the other.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/fs_walk.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

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

void put_file(std::filesystem::path const& p, std::string const& content) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
}

}  // namespace

int main() {
    namespace fsw = agentengine::fs_walk;
    using agentengine::result;

    std::error_code cleanup;
    std::filesystem::path const root = std::filesystem::temp_directory_path() / "ae_test_fs_walk";
    std::filesystem::remove_all(root, cleanup);
    put_file(root / "a.txt", "a");
    put_file(root / "sub" / "b.txt", "b");
    put_file(root / "sub" / "deep" / "c.txt", "c");

    // ---- F1: the positive control.
    {
        std::set<std::string> seen;
        auto r = fsw::for_each_regular_file_recursive(
            root, std::filesystem::directory_options::none, "walk failed", "test.walk_failed",
            [&](std::filesystem::directory_entry const& e) -> result<void> {
                seen.insert(e.path().filename().string());
                return result<void>{};
            });
        check(r.has_value(), "F1 (positive control): a walk over a real tree succeeds");
        check(seen.size() == 3, "F1 (positive control): it visits all three regular files");
        check(seen.count("a.txt") == 1 && seen.count("b.txt") == 1 && seen.count("c.txt") == 1,
              "F1 (positive control): ... and those are the files, so F2's emptiness is meaningful");
        check(seen.count("sub") == 0 && seen.count("deep") == 0,
              "F1 (positive control): directories are skipped, not visited as files");
    }

    // ---- F2: the silent-truncation half of the defect.
    {
        std::size_t visits = 0;
        auto r = fsw::for_each_regular_file_recursive(
            root / "no-such-directory", std::filesystem::directory_options::none,
            "walk failed", "test.walk_failed",
            [&](std::filesystem::directory_entry const&) -> result<void> {
                ++visits;
                return result<void>{};
            });
        check(!r.has_value(),
              "F2: an unreadable root is an ERROR -- the old in-loop `if (ec)` never ran and this "
              "returned an empty success");
        check(visits == 0, "F2: ... and the visitor was never called, which is why the old check was dead");
        if (!r.has_value()) {
            check(r.error().code == "test.walk_failed", "F2: ... under the caller's own walk code");
            check(r.error().native_code != 0,
                  "F2: ... carrying the real OS error number rather than a hand-authored one");
        } else {
            check(false, "F2: ... under the caller's own walk code");
            check(false, "F2: ... carrying the real OS error number rather than a hand-authored one");
        }
    }

    // ---- F3: a visitor's own error is not flattened into the walk's.
    {
        std::size_t visits = 0;
        auto r = fsw::for_each_regular_file_recursive(
            root, std::filesystem::directory_options::none, "walk failed", "test.walk_failed",
            [&](std::filesystem::directory_entry const&) -> result<void> {
                ++visits;
                return std::unexpected(agentengine::error{agentengine::failure_class::policy,
                                                          "the visitor refused", "test.visitor_refused"});
            });
        check(!r.has_value() && r.error().code == "test.visitor_refused",
              "F3: a visitor's own error propagates unchanged, keeping its class and code");
        check(visits == 1, "F3: ... and the walk stops at the first refusal rather than continuing");
    }

    // ---- F4: the throwing-increment half. The claim is that the call RETURNS.
    {
        bool threw = false;
        bool returned_error = false;
        try {
            auto r = fsw::for_each_regular_file_recursive(
                root, std::filesystem::directory_options::none, "walk failed", "test.walk_failed",
                [&](std::filesystem::directory_entry const&) -> result<void> {
                    // Pull the rest of the tree out from under the live iterator. The raw
                    // `operator++` a range-for uses throws filesystem_error here.
                    std::error_code ec;
                    std::filesystem::remove_all(root / "sub", ec);
                    return result<void>{};
                });
            returned_error = !r.has_value();
        } catch (...) {
            threw = true;
        }
        check(!threw,
              "F4: deleting the tree under a live walk does NOT throw out of the call -- a throw "
              "here is what ADR-174's red-team reproduced as a process kill");
        // Whether the increment actually fails depends on where the walk had got to, so this is
        // reported rather than asserted: the contract under test is "never throws", not "always
        // errors".
        std::printf("       F4: the walk %s an error on this run\n",
                    returned_error ? "reported" : "did not report");
    }

    // ---- F5: the non-recursive entry point.
    {
        std::filesystem::remove_all(root, cleanup);
        put_file(root / "top.txt", "t");
        put_file(root / "nested" / "inner.txt", "i");

        std::set<std::string> seen;
        auto r = fsw::for_each_directory_entry(
            root, "list failed", "test.list_failed",
            [&](std::filesystem::directory_entry const& e) -> result<void> {
                seen.insert(e.path().filename().string());
                return result<void>{};
            });
        check(r.has_value(), "F5: the non-recursive walk succeeds");
        check(seen.count("top.txt") == 1 && seen.count("nested") == 1,
              "F5: it reports the top-level file AND the directory");
        check(seen.count("inner.txt") == 0,
              "F5: ... and does not descend, so it is genuinely a different walk");
    }

    std::filesystem::remove_all(root, cleanup);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
