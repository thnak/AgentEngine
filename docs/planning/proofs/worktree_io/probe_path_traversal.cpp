// PROVE-PHASE SAFETY PROBE: real path-traversal and symlink-escape rejection for RealIoFileSystem.
// A real, previously-missing check (§27 shipped with NONE) -- this probe proves the fix actually
// stops real escape attempts, not just that the code compiles.

#include "real_io_filesystem.hpp"

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

int main() {
    using namespace probe;

    std::filesystem::path host_root = std::filesystem::temp_directory_path() / "ae_path_traversal_probe";
    std::filesystem::path outside = std::filesystem::temp_directory_path() / "ae_path_traversal_OUTSIDE";
    std::error_code ec;
    std::filesystem::remove_all(host_root, ec);
    std::filesystem::remove_all(outside, ec);
    std::filesystem::create_directories(outside);

    RealIoFileSystem fs(host_root);

    // 1. Lexical '..' rejected outright, no filesystem access needed to detect it.
    auto r1 = fs.write("../escape.txt", {std::byte{'X'}});
    CHECK(!r1.has_value());
    CHECK(r1.error().code == "real_io.path_traversal_rejected");
    CHECK(!std::filesystem::exists(outside / "escape.txt"));
    std::printf("[1] write(\"../escape.txt\"): REJECTED (%s) -- nothing escaped\n", r1.error().code.c_str());

    // 2. A deeper '..' buried in the middle of a path is still caught (per-component check, not just
    // a prefix check on the raw string).
    auto r2 = fs.write("a/b/../../../escape2.txt", {std::byte{'X'}});
    CHECK(!r2.has_value());
    CHECK(r2.error().code == "real_io.path_traversal_rejected");
    std::printf("[2] write(\"a/b/../../../escape2.txt\"): REJECTED\n");

    // 3. An absolute path is rejected.
    auto r3 = fs.write((outside / "escape3.txt").string(), {std::byte{'X'}});
    CHECK(!r3.has_value());
    CHECK(r3.error().code == "real_io.path_absolute_rejected");
    CHECK(!std::filesystem::exists(outside / "escape3.txt"));
    std::printf("[3] write(<absolute path outside host_root>): REJECTED\n");

    // 4. A legitimate, safe nested path still works (the checks are not overly broad).
    auto r4 = fs.write("safe/nested/path.txt", {std::byte{'O'}, std::byte{'K'}});
    CHECK(r4.has_value());
    CHECK(std::filesystem::exists(host_root / "safe/nested/path.txt"));
    std::printf("[4] write(\"safe/nested/path.txt\"): PASS (legitimate nested writes still work)\n");

    // 5. REAL symlink escape attempt -- create a real symlink inside host_root_ pointing OUTSIDE it,
    // then try to write through it. No literal '..' anywhere in the relative_path given to write().
    std::error_code symlink_ec;
    std::filesystem::create_directory_symlink(outside, host_root / "link_to_outside", symlink_ec);
    if (symlink_ec) {
        std::printf("[5] SKIPPED -- could not create a real symlink in this environment (%s); "
                    "Windows requires Developer Mode or admin privileges for create_directory_symlink. "
                    "The lexical-'..' and absolute-path checks (checks 1-3) are unaffected by this "
                    "skip; the symlink-specific check (reject_symlink_escape) is honestly UNVERIFIED "
                    "in this run, not silently assumed to pass.\n", symlink_ec.message().c_str());
    } else {
        auto r5 = fs.write("link_to_outside/escape5.txt", {std::byte{'X'}});
        bool const really_escaped = std::filesystem::exists(outside / "escape5.txt");
        std::printf("[5] write(\"link_to_outside/escape5.txt\") through a REAL symlink to outside "
                    "host_root_: result has_value=%d, code=%s, file actually escaped to real disk "
                    "outside sandbox=%d\n",
                    (int)r5.has_value(), r5.has_value() ? "" : r5.error().code.c_str(),
                    (int)really_escaped);
        CHECK(!r5.has_value());
        // §38: write() now rejects this via agentengine::open_within_mount_root()'s real, handle-
        // based containment check (ADR-014 Design B), not the old (now-superseded, kept only as a
        // deliberately-vulnerable reference control) reject_symlink_escape() -- hence this design's
        // OWN error code, "worktree.mount_path_escapes_root", not "real_io.symlink_escape_rejected".
        CHECK(r5.error().code == "worktree.mount_path_escapes_root");
        CHECK(!really_escaped);
        std::printf("    CONFIRMED: real symlink escape attempt REJECTED, nothing written outside "
                    "the sandbox root\n");
    }

    std::filesystem::remove_all(host_root, ec);
    std::filesystem::remove_all(outside, ec);
    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
