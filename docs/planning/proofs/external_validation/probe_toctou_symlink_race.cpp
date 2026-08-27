// EXTERNAL-VALIDATION PROBE: replays ADR-014's own real, deterministic TOCTOU exploit technique
// (this same codebase, `decisions/ADR-014-worktree-mount-path-canonicalization.md`, §5 C2-7)
// against `RealIoFileSystem`'s real `reject_symlink_escape()`/`write()` code, instead of leaving
// that file's own "HONEST RESIDUAL... check-then-use gap (TOCTOU)" comment as a purely theoretical
// disclosure.
//
// ADR-014's own real, executed finding, restated precisely (not paraphrased from memory -- read
// directly from the ADR before writing this probe): Design A (canonicalize a path as a STRING via
// a pure string transform, string-prefix-check it against the root, then LATER re-open that SAME
// STRING via a fresh syscall) is a real, deterministically-reproducible TOCTOU vulnerability --
// proven by validating a path that resolves inside a mount root, swapping the checked directory
// for a real junction pointing outside the root, then reopening the ORIGINAL checked string and
// observing it silently reads the SWAPPED-IN outside content. Design B (open ONE real handle,
// verify what the RESULTING HANDLE resolves to, never re-derive from a string) is immune: a handle
// obtained before the swap keeps referencing the original object; a fresh request after the swap
// correctly re-resolves and rejects. ADR-014 applies the swap BY HAND (delete + `mklink /J`),
// deterministically, not via real concurrent threads -- the same discrete-event-simulation
// technique this probe reuses, matching CLAUDE.md's machine-safety constraint against spawning
// extra threads to win a real race.
//
// THE QUESTION THIS PROBE ANSWERS: `RealIoFileSystem::reject_symlink_escape()` has EXACTLY Design
// A's shape -- `std::filesystem::weakly_canonical()` a path into a STRING, string-prefix-check it,
// return. `write()`'s own tail end then SEPARATELY re-derives `host_root_ / relative_path` and
// opens THAT via `std::ofstream` -- a second, independent operation against the filesystem's
// CURRENT state, not the handle/object the check itself resolved. This design has never had a
// handle-based (Design B-shaped) verification primitive at all -- everything in
// `real_io_filesystem.hpp` is `std::filesystem`-path/string-based. This probe tests whether the
// SAME real exploit technique ADR-014 already proved against a DIFFERENT, sibling mediation
// primitive in this exact codebase also defeats THIS design's own path-safety check.

#include "../worktree_io/real_io_filesystem.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace {
std::string read_file(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

int main() {
    using namespace probe;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::filesystem::path const scratch =
        std::filesystem::temp_directory_path() / "ae_toctou_symlink_race_probe";
    std::filesystem::path const host_root = scratch / "mount_root";
    std::filesystem::path const outside_dir = scratch / "outside_secret_area";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(host_root);
    std::filesystem::create_directories(outside_dir);

    // The pre-populated "outside" content, exactly matching ADR-014's own C2-7 setup ("TOCTOU_
    // OUTSIDE_SECRET" vs "TOCTOU_INSIDE").
    {
        std::ofstream out(outside_dir / "victim.txt", std::ios::binary | std::ios::trunc);
        out << "TOCTOU_OUTSIDE_SECRET";
    }

    // The legitimate, currently-INSIDE target: host_root/toctou_dir/victim.txt.
    std::filesystem::create_directories(host_root / "toctou_dir");
    {
        std::ofstream out(host_root / "toctou_dir" / "victim.txt", std::ios::binary | std::ios::trunc);
        out << "TOCTOU_INSIDE";
    }
    std::string const relative_path = "toctou_dir/victim.txt";

    // === Positive control (ADR-014's own C2-7a/C2-3b shape): the check passes for the currently- ===
    // === real, legitimate inside path, with no swap. =================================================
    auto safe = reject_unsafe_relative_path(relative_path);
    CHECK(safe.has_value());
    auto check1 = reject_symlink_escape(host_root, relative_path);
    CHECK(check1.has_value());
    std::printf("[1] POSITIVE CONTROL (ADR-014 C2-7a's shape): reject_symlink_escape() correctly "
                "PASSES for the currently-real, legitimate inside path -- PASS\n");

    // === THE SWAP (ADR-014's own C2-7b technique, applied deterministically by hand, not via a ======
    // === real race): delete the checked directory, replace it with a REAL junction pointing to a ====
    // === different, pre-populated OUTSIDE directory. =================================================
    std::filesystem::remove_all(host_root / "toctou_dir", ec);
    CHECK(!ec);
    std::string const mklink_cmd = "cmd /c mklink /J \"" + (host_root / "toctou_dir").string() +
                                     "\" \"" + outside_dir.string() + "\" >nul 2>&1";
    int const mklink_result = std::system(mklink_cmd.c_str());
    CHECK(mklink_result == 0);
    CHECK(std::filesystem::exists(host_root / "toctou_dir" / "victim.txt"));
    std::printf("[2] SETUP (ADR-014 C2-7b's technique): host_root/toctou_dir successfully swapped "
                "for a REAL junction pointing to the outside directory\n");

    // === THE EXPLOIT ATTEMPT: reopen the SAME relative_path string the check (step 1) already ======
    // === validated, exactly the way write()'s own tail end does -- re-derive the path, open it. ====
    // === reject_symlink_escape() is DELIBERATELY NOT called again here: this is the exact Design-A ==
    // === shape ADR-014 proved vulnerable -- "check a string, later reopen that same string" -- not ==
    // === a claim that write() itself skips its own check on every call (it doesn't; see part 2). ====
    std::filesystem::path const full = host_root / relative_path;
    std::string const content_after_swap = read_file(full);

    std::printf("[3] EXPLOIT ATTEMPT: reopened the SAME path string step 1 already validated as "
                "'inside' -- real content read: \"%s\"\n", content_after_swap.c_str());

    bool const design_a_shape_exploitable = (content_after_swap == "TOCTOU_OUTSIDE_SECRET");
    if (design_a_shape_exploitable) {
        std::fprintf(stderr,
            "\n*** REAL, DETERMINISTIC TOCTOU CONFIRMED: reading the SAME relative_path string "
            "reject_symlink_escape() already validated as safely INSIDE host_root, after the "
            "swap, returns content from OUTSIDE host_root entirely. This is the IDENTICAL "
            "structural vulnerability ADR-014 already proved and fixed in a SIBLING mediation "
            "primitive in this exact codebase (open_within_mount_root's Design A) -- this "
            "design's own real_io_filesystem.hpp has never had the handle-based, verify-from-the-"
            "resolved-object (Design B) mechanism ADR-014 found necessary to actually close this. "
            "reject_symlink_escape()'s own comment already discloses this as a theoretical "
            "residual; this is the empirical, deterministic proof that the theoretical residual "
            "is a REAL, reproducible vulnerability, not merely a hedge.\n");
    } else {
        std::printf("\n[4] NO ESCAPE: the reopened path still read the ORIGINAL inside content -- "
                    "this design's check-then-use pattern did not exhibit the ADR-014 Design-A "
                    "vulnerability in this specific interleaving.\n");
    }

    // === Part 2: does the REAL, literal write() -- which re-checks EVERY call, immediately before ===
    // === use, with no caller-visible gap to inject a swap into -- correctly reject a write against ==
    // === the NOW-swapped (post-junction) directory when called FRESH, without ANY prior check to ====
    // === exploit? This is the practical, currently-achievable mitigation this design DOES have: =====
    // === not Design B's structural immunity, but "always re-check immediately before use, never ======
    // === reuse a stale validation," which write()'s own real code already, correctly, does. ==========
    RealIoFileSystem fs(host_root);
    auto fresh_write = fs.write(relative_path, {std::byte{'X'}});
    CHECK(!fresh_write.has_value());
    CHECK(fresh_write.error().code == "worktree.mount_path_escapes_root");
    std::printf("[5] a FRESH call to the REAL write() against the now-swapped (crossing-junction) "
                "target is correctly REJECTED (%s) -- write()'s own real code re-checks every "
                "single call, immediately before use, with no caller-visible window between check "
                "and use for a swap to be injected into FROM THIS PROBE's own vantage point -- the "
                "achievable mitigation this design already has, distinct from (and weaker than) "
                "Design B's structural, handle-based immunity -- PASS\n",
                fresh_write.error().code.c_str());

    std::filesystem::remove_all(scratch, ec);
    if (design_a_shape_exploitable) {
        std::printf("\nRESULT: TOCTOU CONFIRMED against the check-then-reopen shape (steps 1-3); "
                    "the achievable mitigation (always re-check immediately before use, step 5) "
                    "is proven present and working. Exiting non-zero to make the finding visible "
                    "to any automated re-run, not as a probe defect.\n");
        return 1;
    }
    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
