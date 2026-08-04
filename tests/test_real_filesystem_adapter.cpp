// Correctness gate for src/backends/native_jail/real_filesystem_adapter.{hpp,cpp} — ADR-001 §2.5.5.
// Proves RealFileSystemAdapter's path-escape list against a real temp-directory root: `..`
// traversal, absolute drive-letter redirection, UNC paths, reserved device names, Alternate Data
// Stream syntax, and (best-effort, environment-permitting) a real junction-based escape, plus a
// positive control (an ordinary in-bounds read/write round-trip actually works) so a denial can't
// be a test that always fails closed for the wrong reason.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <crtdbg.h>
#else
#include <unistd.h>
#endif

#include "backends/native_jail/real_filesystem_adapter.hpp"

namespace fs = std::filesystem;
using agentengine::native_jail::RealFileSystemAdapter;

namespace {

// A failed assert() under the MSVC CRT pops an interactive "Debug Assertion Failed" dialog by
// default, which blocks forever in a non-interactive CTest run — exactly the kind of hang
// CLAUDE.md's Machine Safety section rules out. Route assertion reports to stderr instead so a
// real failure prints and the process exits, rather than hanging the dev box waiting for a click
// that will never come.
void disable_crt_assert_dialog() {
#if defined(_WIN32)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

// `_getpid` (process.h) is the MSVC CRT spelling; POSIX has no leading underscore and lives in
// unistd.h. Only used to make each test run's temp-root name unique, not a portability seam.
[[nodiscard]] int current_pid() noexcept {
#if defined(_WIN32)
    return ::_getpid();
#else
    return ::getpid();
#endif
}

fs::path make_temp_root() {
    fs::path root = fs::temp_directory_path() /
                    ("ae_fs_adapter_test_" + std::to_string(current_pid()));
    fs::create_directories(root);
    return root;
}

void expect_error_code(agentengine::result<std::filesystem::path> const& r, char const* expected_code,
                        char const* label) {
    if (r.has_value()) {
        std::cerr << "FAIL (" << label << "): expected error code '" << expected_code
                  << "' but got a value: " << r->string() << "\n";
        std::abort();
    }
    if (r.error().code != expected_code) {
        std::cerr << "FAIL (" << label << "): expected error code '" << expected_code
                  << "' but got '" << r.error().code << "' (" << r.error().message << ")\n";
        std::abort();
    }
    std::cout << "  ok: " << label << " -> " << r.error().code << "\n";
}

} // namespace

int main() {
    disable_crt_assert_dialog();
    fs::path root = make_temp_root();
    auto adapter_r = RealFileSystemAdapter::create(root);
    assert(adapter_r.has_value() && "adapter must construct against a real, existing directory");
    RealFileSystemAdapter adapter = std::move(*adapter_r);

    // ---- Positive control: an ordinary in-bounds path round-trips correctly -------------------
    {
        std::string payload = "hello from the sandbox";
        std::vector<std::byte> bytes(payload.size());
        for (std::size_t i = 0; i < payload.size(); ++i) bytes[i] = std::byte(payload[i]);
        auto written = adapter.write_file("/greeting.txt", bytes, false);
        assert(written.has_value() && "in-bounds write must succeed");
        auto read_back = adapter.read_file("/greeting.txt");
        assert(read_back.has_value() && "in-bounds read must succeed");
        std::string round_tripped(reinterpret_cast<char const*>(read_back->data()), read_back->size());
        assert(round_tripped == payload && "round-tripped content must match exactly");
        std::cout << "  ok: positive control (in-bounds read/write round-trip)\n";
    }

    // ---- `..` traversal -----------------------------------------------------------------------
    expect_error_code(adapter.validate_and_resolve("/../outside.txt"), "shell.fs.escape_dotdot",
                       "dotdot traversal");
    expect_error_code(adapter.validate_and_resolve("sub/../../outside.txt"), "shell.fs.escape_dotdot",
                       "dotdot traversal (nested)");

    // ---- absolute drive-letter redirection -----------------------------------------------------
    expect_error_code(adapter.validate_and_resolve("C:\\Windows\\System32"),
                       "shell.fs.escape_absolute", "drive-letter absolute path");

    // ---- UNC / extended-length prefixes ---------------------------------------------------------
    expect_error_code(adapter.validate_and_resolve("\\\\server\\share\\file.txt"),
                       "shell.fs.escape_unc", "UNC path");
    expect_error_code(adapter.validate_and_resolve("\\\\?\\C:\\Windows"), "shell.fs.escape_unc",
                       "extended-length prefix");

    // ---- reserved device names, with and without an extension ----------------------------------
    expect_error_code(adapter.validate_and_resolve("/CON"), "shell.fs.escape_device_name",
                       "reserved device name (bare)");
    expect_error_code(adapter.validate_and_resolve("/nul.txt"), "shell.fs.escape_device_name",
                       "reserved device name (with extension)");
    expect_error_code(adapter.validate_and_resolve("/sub/COM1"), "shell.fs.escape_device_name",
                       "reserved device name (nested)");

    // ---- Alternate Data Stream syntax -----------------------------------------------------------
    expect_error_code(adapter.validate_and_resolve("/greeting.txt:hidden"), "shell.fs.escape_ads",
                       "ADS marker");

    // ---- case-fold consistency: the same logical file, different case, is the SAME file --------
    {
        auto lower = adapter.validate_and_resolve("/Greeting.TXT");
        assert(lower.has_value() && "differently-cased in-bounds path must still resolve");
        auto exists_diff_case = adapter.exists("/GREETING.txt");
        assert(exists_diff_case.has_value() && *exists_diff_case &&
               "case-insensitive filesystem: differently-cased path must see the same file");
        std::cout << "  ok: case-fold-consistent comparison\n";
    }

    // ---- best-effort: a real junction escaping the root is rejected ----------------------------
    {
        fs::path outside = fs::temp_directory_path() /
                            ("ae_fs_adapter_outside_" + std::to_string(current_pid()));
        fs::create_directories(outside);
        fs::path link = root / "escape_link";
        std::string cmd = "cmd /c mklink /J \"" + link.string() + "\" \"" + outside.string() +
                           "\" > NUL 2>&1";
        int rc = std::system(cmd.c_str());
        if (rc == 0 && fs::exists(link)) {
            auto resolved = adapter.validate_and_resolve("/escape_link/x.txt");
            if (!resolved.has_value() && resolved.error().code == "shell.fs.escape_symlink") {
                std::cout << "  ok: junction escape rejected (shell.fs.escape_symlink)\n";
            } else {
                std::cerr << "FAIL: a junction pointing outside the root was NOT rejected — got "
                          << (resolved.has_value() ? resolved->string() : resolved.error().code)
                          << "\n";
                return 1;
            }
        } else {
            std::cout << "  SKIPPED (INCONCLUSIVE): junction creation via 'mklink /J' did not "
                         "succeed in this environment (rc="
                      << rc
                      << ") — the junction/symlink-escape half of ADR-001 §2.5.5 could not be "
                         "exercised end-to-end in this run; string-level escape checks above are "
                         "still verified.\n";
        }
        std::error_code ec;
        fs::remove(link, ec);
        fs::remove_all(outside, ec);
    }

    fs::remove_all(root);
    std::cout << "test_real_filesystem_adapter: PASS\n";
    return 0;
}
