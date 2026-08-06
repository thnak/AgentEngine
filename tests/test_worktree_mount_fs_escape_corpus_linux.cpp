// Proof for 025-Worktree-and-Virtual-Filesystem.md §5's OS-level path-escape corpus (021 §6 G3),
// Linux half -- Milestone 3 Phase C4, the parity pass over
// decisions/ADR-014-worktree-mount-path-canonicalization.md (Judged, Windows). Ordinary task per
// the breakdown doc's decision 6 (ADR-014 already settled the design question; this carries its
// finding forward rather than re-litigating it), mirroring
// tests/test_worktree_mount_fs_escape_corpus.cpp's structure and numbering scheme so the two
// platforms' corpora stay legible side by side.
//
// Real Linux filesystem I/O against a scratch directory under $TMPDIR (or /tmp), real symlinks
// (unlike Windows junctions, these need no special privilege at all), a real TOCTOU interleaving
// proven deterministically the same way the Windows corpus does (mutate the filesystem by hand
// between a "check" and a "use" step, matching this project's Phase B4 discrete-event-simulation
// precedent applied to a path-canonicalization property).
//
// Named, not silently dropped: several Windows corpus items are N/A on this platform and are not
// reproduced here -- ADS (an NTFS-only concept), `\\?\` prefixes (Windows-only), and 8.3 short-name
// aliasing (Windows-only) have no Linux analog to test. Case handling is the OPPOSITE direction from
// Windows (ext4 is case-SENSITIVE by default, NTFS is not) -- C4-8 below documents that difference
// as a real, expected platform divergence rather than assuming silent parity.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "agentengine/core/worktree_mount_fs_posix.hpp"

using namespace agentengine;

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

void must_mkdir(std::string const& path) {
    if (::mkdir(path.c_str(), 0755) != 0) {
        std::cerr << "setup mkdir(" << path << ") failed: " << std::strerror(errno) << "\n";
        std::exit(2);
    }
}

void must_write_file(std::string const& path, std::string const& content) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::cerr << "setup open(" << path << ") failed: " << std::strerror(errno) << "\n";
        std::exit(2);
    }
    ssize_t n = ::write(fd, content.data(), content.size());
    ::close(fd);
    if (n != static_cast<ssize_t>(content.size())) {
        std::cerr << "setup write(" << path << ") short/failed\n";
        std::exit(2);
    }
}

void must_symlink(std::string const& target, std::string const& linkpath) {
    if (::symlink(target.c_str(), linkpath.c_str()) != 0) {
        std::cerr << "setup symlink(" << target << " -> " << linkpath << ") failed: " << std::strerror(errno)
                   << "\n";
        std::exit(2);
    }
}

std::string read_all(int fd) {
    std::string out;
    char buf[256];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

std::string make_scratch_dir() {
    char const* tmp = std::getenv("TMPDIR");
    std::string base = tmp && *tmp ? tmp : "/tmp";
    std::string tmpl = base + "/ae_c4_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* made = ::mkdtemp(buf.data());
    if (!made) {
        std::cerr << "mkdtemp failed: " << std::strerror(errno) << "\n";
        std::exit(2);
    }
    return std::string(made);
}

} // namespace

int main() {
    std::string const scratch = make_scratch_dir();
    std::string const mount_root = scratch + "/mount";
    std::string const outside = scratch + "/outside";
    must_mkdir(mount_root);
    must_mkdir(outside);
    must_write_file(mount_root + "/normal.txt", "inside-content");
    must_write_file(outside + "/secret.txt", "outside-secret");

    // C4-1: lexical `..` is rejected pre-syscall by the SAME split_mount_path contract both
    // platforms share (core/worktree.hpp) -- nothing platform-specific to prove here beyond "this
    // still reaches the shared validator."
    {
        auto r = open_within_mount_root(mount_root, "../outside/secret.txt", O_RDONLY);
        AE_CHECK(!r.has_value() && r.error().code == "worktree.mount_path_malformed",
                 "C4-1: '..' in a guest path is rejected pre-syscall (shared split_mount_path contract)");
    }

    // C4-2: an absolute guest path is rejected the same way.
    {
        auto r = open_within_mount_root(mount_root, "/etc/passwd", O_RDONLY);
        AE_CHECK(!r.has_value() && r.error().code == "worktree.mount_path_absolute",
                 "C4-2: an absolute guest path is rejected pre-syscall");
    }

    // C4-3 (positive control, baseline): an ordinary in-mount read succeeds and returns real content.
    {
        auto r = open_within_mount_root(mount_root, "normal.txt", O_RDONLY);
        AE_CHECK(r.has_value(), "C4-3: an ordinary in-mount file opens successfully");
        if (r.has_value()) {
            AE_CHECK(read_all(r->get()) == "inside-content",
                     "C4-3: the opened descriptor reads the real in-mount content");
        }
    }

    // C4-4: a symlink CROSSING the mount boundary is rejected, paired with a positive control
    // proving an IN-mount symlink is followed, not blanket-denied -- the same "an identical in-mount
    // junction is followed" property ADR-014's Windows corpus proved, ported to real symlinks (which
    // on Linux need no special privilege at all, unlike Windows junctions/symlinks).
    {
        must_symlink("../outside", mount_root + "/escape_link");
        auto escaped = open_within_mount_root(mount_root, "escape_link/secret.txt", O_RDONLY);
        AE_CHECK(!escaped.has_value() && escaped.error().code == "worktree.mount_path_escapes_root",
                 "C4-4: a symlink crossing the mount boundary is rejected");

        must_mkdir(mount_root + "/real_subdir");
        must_write_file(mount_root + "/real_subdir/file.txt", "in-mount-via-symlink");
        must_symlink("./real_subdir", mount_root + "/inside_link");
        auto followed = open_within_mount_root(mount_root, "inside_link/file.txt", O_RDONLY);
        AE_CHECK(followed.has_value(), "C4-4 (positive control): an in-mount symlink is followed, not denied");
        if (followed.has_value()) {
            AE_CHECK(read_all(followed->get()) == "in-mount-via-symlink",
                     "C4-4 (positive control): the followed in-mount symlink reads the real target content");
        }
    }

    // C4-5: an embedded NUL in a path segment is rejected structurally, before any syscall --
    // std::string is not itself NUL-terminated, so a caller-supplied segment could carry one even
    // though there is no way to express it through an ordinary literal.
    {
        std::string malicious("bad\0name", 8);
        auto r = open_within_mount_root(mount_root, malicious, O_RDONLY);
        AE_CHECK(!r.has_value() && r.error().code == "worktree.mount_path_forbidden_character",
                 "C4-5: an embedded NUL in a path segment is rejected structurally");
    }

    // C4-6: the load-bearing TOCTOU proof. Design A (naive_check_within_root / naive_open_checked_
    // path) is proven vulnerable deterministically: a real inside directory is checked, then swapped
    // by hand for a symlink pointing outside (the exact state a real racing attacker needs, made
    // reproducible instead of timing-dependent), and the naive design's reopen reads the swapped-in
    // OUTSIDE content despite the check having validated an INSIDE path.
    {
        must_mkdir(mount_root + "/toctou_dir_a");
        must_write_file(mount_root + "/toctou_dir_a/data.txt", "inside-data-a");
        must_write_file(outside + "/data.txt", "outside-secret-data");

        auto checked = redteam::naive_check_within_root(mount_root, "toctou_dir_a/data.txt");
        AE_CHECK(checked.has_value(), "C4-6a: naive_check_within_root validates the real inside path");

        // SWAP: remove the real directory, replace it with a symlink pointing outside -- the exact
        // interleaving a real TOCTOU attacker needs, reproduced by hand rather than by timing.
        AE_CHECK(::unlink((mount_root + "/toctou_dir_a/data.txt").c_str()) == 0,
                 "C4-6b (setup): remove the real file under toctou_dir_a");
        AE_CHECK(::rmdir((mount_root + "/toctou_dir_a").c_str()) == 0,
                 "C4-6b (setup): remove the now-empty real toctou_dir_a directory");
        must_symlink("../outside", mount_root + "/toctou_dir_a");

        auto reopened = redteam::naive_open_checked_path(*checked, O_RDONLY);
        AE_CHECK(reopened.has_value() && read_all(reopened->get()) == "outside-secret-data",
                 "C4-6c: THE BUG reproduced -- the naive design's reopen reads OUTSIDE content despite "
                 "having validated an INSIDE path (string check disconnected from what is later opened)");
    }

    // C4-6 continued: the accepted design (open_within_mount_root) proven immune to the IDENTICAL
    // interleaving two ways -- a descriptor opened BEFORE the swap keeps reading its original
    // content afterward (a POSIX fd references the underlying inode, not a path -- removing the
    // directory entry that pointed at it does not affect an already-open descriptor), and a FRESH
    // request made AFTER the swap re-resolves current reality and is correctly rejected.
    {
        must_mkdir(mount_root + "/toctou_dir_b");
        must_write_file(mount_root + "/toctou_dir_b/data.txt", "inside-data-b");

        auto before_swap = open_within_mount_root(mount_root, "toctou_dir_b/data.txt", O_RDONLY);
        AE_CHECK(before_swap.has_value(), "C4-6d (setup): open_within_mount_root succeeds before the swap");

        AE_CHECK(::unlink((mount_root + "/toctou_dir_b/data.txt").c_str()) == 0,
                 "C4-6e (setup): remove the real file under toctou_dir_b");
        AE_CHECK(::rmdir((mount_root + "/toctou_dir_b").c_str()) == 0,
                 "C4-6e (setup): remove the now-empty real toctou_dir_b directory");
        must_symlink("../outside", mount_root + "/toctou_dir_b");

        if (before_swap.has_value()) {
            AE_CHECK(read_all(before_swap->get()) == "inside-data-b",
                     "C4-6f: a descriptor opened BEFORE the swap still reads its ORIGINAL content "
                     "afterward -- a Linux fd references the inode, not a re-resolvable path");
        }

        auto after_swap = open_within_mount_root(mount_root, "toctou_dir_b/data.txt", O_RDONLY);
        AE_CHECK(!after_swap.has_value() && after_swap.error().code == "worktree.mount_path_escapes_root",
                 "C4-6g: a FRESH request made AFTER the swap re-resolves current reality and is "
                 "correctly rejected -- no cached, staleable 'validated' answer");
    }

    // C4-7 (positive control paired with C4-6g): a DIFFERENT, still-legitimate in-mount path
    // continues to open fine after the swap above -- proving the rejection is really about the one
    // escaped path, not the mount broadly broken by the swap.
    {
        auto still_ok = open_within_mount_root(mount_root, "normal.txt", O_RDONLY);
        AE_CHECK(still_ok.has_value() && read_all(still_ok->get()) == "inside-content",
                 "C4-7 (positive control): an unrelated, still-legitimate in-mount path is unaffected");
    }

    // C4-8 (documented platform difference, not a security gate): Linux filesystems are
    // case-SENSITIVE by default, the opposite of Windows' NTFS -- "Normal.TXT" and "normal.txt" name
    // DIFFERENT files here. Recorded explicitly so this divergence from the Windows corpus's
    // case-insensitivity check is named, not silently assumed to generalize either direction.
    {
        auto wrong_case = open_within_mount_root(mount_root, "Normal.TXT", O_RDONLY);
        AE_CHECK(!wrong_case.has_value(),
                 "C4-8 (documented platform difference): unlike Windows, a differently-cased guest "
                 "path names a DIFFERENT (here, nonexistent) file on a case-sensitive Linux filesystem");
    }

    // C4-9: the mount root itself (an empty guest path) is rejected -- this primitive is file-level
    // only, matching the Windows sibling's identical scope boundary.
    {
        auto r = open_within_mount_root(mount_root, "", O_RDONLY);
        AE_CHECK(!r.has_value() && r.error().code == "worktree.mount_path_is_root",
                 "C4-9: an empty guest path (the mount root itself) is rejected");
    }

    std::string const cleanup_cmd = "rm -rf '" + scratch + "'";
    std::system(cleanup_cmd.c_str());  // best-effort teardown, matching the Windows corpus's own

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree mount-fs Linux escape-corpus checks passed.\n";
    return 0;
}
