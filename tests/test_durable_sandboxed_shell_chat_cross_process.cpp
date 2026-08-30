// Independent red-team round (2026-08-30) on ADR-134 (decisions/ADR-134-durable-sandboxed-shell-chat.md):
// `tools/durable_sandboxed_shell_chat.cpp` had ZERO automated coverage of its own -- ADR-134's own §3
// evidence was a human manually running the compiled binary twice and eyeballing the filesystem. This is
// that claim turned into a permanent regression guard: invokes the REAL, already-built
// `agentengine_durable_sandboxed_shell_chat.exe` TWICE, as two genuinely separate OS process invocations
// (`std::system()`, the same real-subprocess discipline `test_content_durability_cross_process.cpp`
// already established for this exact design line).
//
// REAL FINDING made while authoring this test's own sanity check (not merely asserted -- see the
// bottom of this comment for how it was found): ADR-134 §3's own manual verification methodology --
// "the real, on-disk tree count stayed at exactly 1, not 2" -- does NOT actually distinguish
// `bind_root_branch()` genuinely reclaiming the existing root branch from a hypothetically-broken
// version that ALWAYS takes the CREATE path instead. Both `create_root_branch()` and
// `reclaim_orphaned_branch()` resolve to the SAME deterministic branch name
// (`"root-" + owner.id() + "-" + disambiguator`, both a durable IdentityAuthority id and this tool's own
// fixed session_id), and `create_root_branch()`'s own `branches_.insert_or_assign(name, ...)` OVERWRITES
// rather than duplicates an existing entry of that name -- and the branch's initial tree is always the
// SAME content-addressed empty `Tree{}` digest, so even a wrongly-repeated CREATE produces exactly the
// same single tree object a correct RECLAIM would leave in place. Tree-object count is therefore a
// necessary but NOT sufficient signal for this tool's own claim.
//
// The signal that IS decisive: `Ledger<Store>::create_root_branch()` unconditionally calls
// `persist_snapshot_locked()` (rewrites `ledger_state.snapshot`) on every call, while
// `reclaim_orphaned_branch()` (`include/agentengine/core/ledger.hpp`) does NOT write to the durable
// snapshot at all -- it only mutates the in-memory `orphaned_from_restart_` set. So the real,
// decisive check is: does `ledger_state.snapshot`'s own last-write-time change across the second
// invocation? Unchanged means RECLAIM (no snapshot write happened); changed means CREATE ran again
// (the "core claim" would be false even though the tree count still reads 1). This test asserts BOTH:
// tree count stays 1 (no corruption/duplication) AND the snapshot file's mtime is UNCHANGED across the
// second run (the actual reclaim-vs-create distinction ADR-134 itself needed but did not check).
//
// Two things this test does NOT rely on the tool's own source for, because the tool exposes neither as a
// CLI override:
//   [a] `OPENAI_API_KEY` is explicitly forced ABSENT (not merely empty -- `_putenv_s(name, "")` on
//       Windows genuinely deletes the variable, POSIX `unsetenv()` does the same) for both child
//       invocations, regardless of whatever this test's own parent process/CI environment happens to
//       have set. This is deliberate, not incidental: an empty-but-PRESENT OPENAI_API_KEY was found,
//       empirically, while authoring this test, to still satisfy `api_key_from_env()` and let the child
//       proceed all the way to its own interactive `std::getline(std::cin, ...)` REPL loop -- which would
//       then either block this test indefinitely on stdin (a real CLAUDE.md Machine Safety concern for
//       anything invoked by an automated suite) or, with a real key, attempt a REAL network call to
//       OpenAI from a unit test. Forcing the key genuinely absent instead reaches the tool's own
//       documented, clean, no-network `quickstart_builder.no_store` failure (exit code 1) every time --
//       far enough to prove the durable-state claim, exactly the stopping point ADR-134 §3 itself used.
//   [b] `USERPROFILE`/`HOME` is redirected to a fresh, this-test-owned temporary directory for both child
//       invocations -- the tool hardcodes `~/.agentengine/durable_shell_chat` with no override flag, so
//       without this redirection an automated run of this test would read/write the REAL, human-owned
//       durable state at that path (CLAUDE.md: "Real durable state... is REAL, intentional state... don't
//       delete it destructively").
//
// Sanity-checked, both ways, in the red-team pass that added this file: temporarily forcing
// `MandatorySandboxProvider::bind_root_branch()`'s own `is_orphan` to always be `false` (skipping the
// reclaim path unconditionally) rebuilt clean but left the tree-count assertion UNCHANGED (confirming the
// finding above -- tree count alone would NOT have caught this regression) while the snapshot-mtime
// assertion below correctly FAILED (the file was rewritten on the second run); reverting the sabotage
// made both assertions pass again.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <cstdlib>  // _putenv_s
#else
#include <cstdlib>  // setenv/unsetenv
#endif

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "ok: %s\n", what);
    }
}

// Deletes (not merely blanks) OPENAI_API_KEY for THIS process -- std::system()'s child inherits this
// process's own current environment block, so this reaches both child invocations below.
void force_openai_api_key_absent() {
#ifdef _WIN32
    // MSVC CRT documents `_putenv_s(name, "")` as REMOVING the variable, not setting it to an empty
    // string -- verified against this exact tool's own observed behavior (an empty-but-PRESENT
    // OPENAI_API_KEY reaches api_key_from_env() as a present value, deterministically skipping the
    // quickstart_builder.no_store path this test relies on; that difference was found empirically while
    // authoring this test, not assumed).
    (void)_putenv_s("OPENAI_API_KEY", "");
#else
    (void)unsetenv("OPENAI_API_KEY");
#endif
}

void set_fake_home(std::filesystem::path const& dir) {
#ifdef _WIN32
    (void)_putenv_s("USERPROFILE", dir.string().c_str());
#else
    (void)setenv("HOME", dir.string().c_str(), 1);
#endif
}

[[nodiscard]] std::filesystem::path durable_root(std::filesystem::path const& fake_home) {
    return fake_home / ".agentengine" / "durable_shell_chat";
}

[[nodiscard]] std::size_t count_tree_objects(std::filesystem::path const& fake_home) {
    std::filesystem::path const trees_dir = durable_root(fake_home) / "objects" / "trees";
    std::error_code ec;
    if (!std::filesystem::exists(trees_dir, ec)) return 0;
    std::size_t count = 0;
    for (auto const& entry : std::filesystem::directory_iterator(trees_dir, ec)) {
        if (entry.is_regular_file()) ++count;
    }
    return count;
}

// nullopt if the snapshot file does not exist yet (first-ever run).
[[nodiscard]] std::optional<std::filesystem::file_time_type> snapshot_mtime(
        std::filesystem::path const& fake_home) {
    std::filesystem::path const snapshot = durable_root(fake_home) / "ledger" / "ledger_state.snapshot";
    std::error_code ec;
    if (!std::filesystem::exists(snapshot, ec)) return std::nullopt;
    auto t = std::filesystem::last_write_time(snapshot, ec);
    if (ec) return std::nullopt;
    return t;
}

// Same Windows cmd.exe double-quoting workaround `test_content_durability_cross_process.cpp` already
// established and red-teamed for this exact class of std::system() call (see that file's own comment for
// why the POSIX side must NOT receive the identical extra wrap). Stdin is explicitly redirected from the
// platform's null device on BOTH platforms -- belt-and-suspenders alongside [a] above: even if
// OPENAI_API_KEY were somehow still present, the child's own std::getline() sees immediate EOF rather
// than blocking this test on whatever stdin this test process itself inherited from ctest.
[[nodiscard]] int run_once(std::filesystem::path const& exe_path) {
#ifdef _WIN32
    std::string const inner = "\"" + exe_path.string() + "\" < NUL";
    std::string const command = "\"" + inner + "\"";
#else
    std::string const command = "\"" + exe_path.string() + "\" < /dev/null";
#endif
    return std::system(command.c_str());
}

}  // namespace

int main() {
    namespace fs = std::filesystem;

#ifndef AE_DURABLE_SHELL_CHAT_EXE
#error "AE_DURABLE_SHELL_CHAT_EXE must be defined by CMakeLists.txt via $<TARGET_FILE:...>"
#endif
    fs::path const exe_path = AE_DURABLE_SHELL_CHAT_EXE;
    check(fs::exists(exe_path), "the real, already-built agentengine_durable_sandboxed_shell_chat.exe "
                                 "exists at the path CMake supplied");
    if (!fs::exists(exe_path)) return EXIT_FAILURE;

    fs::path const fake_home = fs::temp_directory_path() / "ae_test_durable_shell_chat_fake_home";
    std::error_code ec;
    fs::remove_all(fake_home, ec);  // start from a genuinely clean slate, not leftover state from a
                                     // prior failed run of this same test
    fs::create_directories(fake_home, ec);

    force_openai_api_key_absent();
    set_fake_home(fake_home);

    // ---- Run 1: no durable state exists yet under fake_home -- must take the CREATE path. ------------
    int const exit1 = run_once(exe_path);
    check(exit1 == 1, "[1] first invocation exits with the documented, clean "
                       "quickstart_builder.no_store failure (exit code 1) -- not a crash, not success");

    std::size_t const trees_after_1 = count_tree_objects(fake_home);
    check(trees_after_1 == 1, "[1] exactly one real tree object exists on disk after the first "
                               "invocation (the empty root tree create_root_branch() produces)");

    auto const mtime_after_1 = snapshot_mtime(fake_home);
    check(mtime_after_1.has_value(),
          "[1] ledger_state.snapshot exists on disk after the first invocation");

    // ---- Run 2: durable state from run 1 exists -- THE CORE CLAIM: must take the RECLAIM path, not ---
    // ---- create a second, duplicate root. --------------------------------------------------------
    int const exit2 = run_once(exe_path);
    check(exit2 == 1, "[2] second invocation ALSO exits with the same documented, clean failure -- "
                       "crash-recovery reattachment does not change this tool's own no-API-key behavior");

    std::size_t const trees_after_2 = count_tree_objects(fake_home);
    check(trees_after_2 == 1, "[2] the real, on-disk tree count is still exactly 1 after a second "
                               "invocation (necessary, but on its own NOT sufficient -- see this file's "
                               "own top comment -- to prove reclaim over create)");

    auto const mtime_after_2 = snapshot_mtime(fake_home);
    check(mtime_after_2.has_value(),
          "[2] ledger_state.snapshot still exists on disk after the second invocation");
    // THE DECISIVE CHECK: create_root_branch() unconditionally rewrites ledger_state.snapshot;
    // reclaim_orphaned_branch() never touches it. An UNCHANGED mtime across the second invocation is
    // only possible if bind_root_branch() genuinely took the RECLAIM path -- proven by finding, not
    // assumed (see this file's own top comment for the sabotage-and-revert sanity check that confirmed
    // this signal, and confirmed tree-object count alone does NOT).
    check(mtime_after_1.has_value() && mtime_after_2.has_value() && *mtime_after_1 == *mtime_after_2,
          "[2] THE CORE CLAIM: ledger_state.snapshot's own last-write-time is UNCHANGED across the "
          "second, genuinely separate process invocation -- bind_root_branch() reclaimed the SAME root "
          "branch (no durable snapshot write happened) rather than creating a duplicate/overwriting "
          "root, real crash-recovery across two real process invocations of the actual shipped tool");

    fs::remove_all(fake_home, ec);

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- tools/durable_sandboxed_shell_chat.cpp's own durable "
                     "identity+content wiring genuinely survives across two real, separate OS process "
                     "invocations of the actual shipped binary (reclaim, not silent re-create, proven by "
                     "the snapshot-mtime signal), automated and reproducible, not merely manually "
                     "eyeballed.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
