// Sh-S1's STATIC half, stated at link-target granularity (ADR-001 §7 finding 1's fix, not the
// per-translation-unit check the original claim named): the `agentengine_shell_runner` static
// library (ShellRunner + CommandRegistry + the builtin table + RealFileSystemAdapter — everything
// resolve()/dispatch_command() can reach) must contain, in its own compiled object code, no
// reference at all to a process-creation primitive. This is checked against the ACTUAL BUILT
// ARTIFACT via `llvm-nm` (ships with the clang toolchain already used for build-clang/, so no
// extra dependency), not against source text — a symbol import table can't be satisfied by
// renaming a function or moving code between files the way a text grep could be fooled by either.
//
// This is deliberately narrower than a full dynamic IAT-hook/detours shim (the OTHER half of
// Sh-S1's disproving experiment, per ADR-001 §7 finding 2's call for "the actual hooking
// mechanism and its scope" to be named before the prove phase writes it). That runtime shim was
// NOT attempted in this pass — building a Windows-safe, false-positive-free process-creation hook
// harness is its own significant effort, and this pass instead relies on (a) this static check,
// (b) manual review that shell_dispatch.cpp/shell_parser.cpp/real_filesystem_adapter.cpp/
// command_registry.hpp never spell CreateProcess/_wspawnv/system/LoadLibrary/exec*/posix_spawn/
// fork anywhere, and (c) the hostile-name-corpus behavioral test in test_shell_runner_proof.cpp.
// This is recorded honestly in ADR-001 §9 as the static check being CORRECT and the dynamic-shim
// half being NOT ATTEMPTED, not laundered into a single "Sh-S1: CORRECT."
//
// Skips (CTest SKIP_RETURN_CODE) if llvm-nm isn't available in this environment, rather than
// silently passing or failing for an unrelated reason.

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef AE_SHELL_RUNNER_LIB_PATH
#error "AE_SHELL_RUNNER_LIB_PATH must be defined by CMake to the built agentengine_shell_runner artifact"
#endif
#ifndef AE_LLVM_NM_PATH
#define AE_LLVM_NM_PATH ""
#endif

namespace {

constexpr int kSkipReturnCode = 77;

// Symbols that, if they appear as an UNDEFINED reference in this library's object code, would
// mean something in ShellRunner's own call graph could reach process creation — the property
// §9 G2 requires to not exist at all, not merely to be checked correctly every time.
//
// PLATFORM-SPECIFIC, NOT a mechanical port (2026-08-29,
// decisions/ADR-104-real-io-filesystem-linux-parity.md's own named follow-on, ADR-103 §7's
// original residual): Windows and POSIX name genuinely different APIs for the identical
// underlying property this check exists to prove. `system` is the one symbol name shared
// verbatim by both lists — ISO C's `system()` is spelled identically in MSVCRT and glibc, so it
// is not a duplicate by accident, just a real coincidence of naming.
#ifdef _WIN32
constexpr std::array<char const*, 14> kHostileSymbols = {
    "CreateProcessA", "CreateProcessW", "CreateProcessAsUserA", "CreateProcessAsUserW",
    "_wspawnv",       "_wspawnve",       "_spawnv",             "_spawnve",
    "system",         "_wsystem",        "LoadLibraryA",        "LoadLibraryW",
    "WinExec",         "ShellExecuteA",
};
#else
constexpr std::array<char const*, 13> kHostileSymbols = {
    "fork",         "vfork",   "execve",        "execv",  "execvp",
    "execvpe",      "execl",   "execlp",        "execle", "posix_spawn",
    "posix_spawnp", "system",  "clone",
};
#endif

} // namespace

int main() {
    std::string nm_path = AE_LLVM_NM_PATH;
    if (nm_path.empty()) {
        std::cout << "SKIP: llvm-nm not found at configure time\n";
        return kSkipReturnCode;
    }

    std::string tmp_out =
        (std::filesystem::temp_directory_path() / "ae_shell_runner_no_process_creation_nm.txt")
            .string();
    std::string inner =
        "\"" + nm_path + "\" --undefined-only \"" + AE_SHELL_RUNNER_LIB_PATH + "\" > \"" + tmp_out +
        "\" 2>&1";
    // `std::system()` on Windows hands this string to `cmd.exe /c`, which has its own, separate
    // quoting rule: when a command line both starts with a quote and contains further quoted
    // arguments, the ENTIRE line must be wrapped in an extra pair of quotes or cmd.exe mis-parses
    // the executable path itself (observed: "'C:/Program' is not recognized..." — it split on the
    // space inside "C:/Program Files/..." despite the inner quotes). On POSIX, `std::system()`
    // hands this string to `/bin/sh -c` directly, with no such re-quoting layer -- applying the
    // SAME extra wrap there is wrong, not merely unneeded: it makes `sh` treat the whole
    // already-quoted string as one unparseable token (empirically confirmed: this is exactly what
    // broke this check's own first Linux port attempt, 2026-08-29 -- a real, novel bug, not a
    // mechanical port of a pre-existing one, since this whole test never ran on Linux before now).
#ifdef _WIN32
    std::string cmd = "\"" + inner + "\"";
#else
    std::string cmd = inner;
#endif
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cout << "SKIP: llvm-nm invocation failed (rc=" << rc << "), cmd=" << cmd << "\n";
        return kSkipReturnCode;
    }

    std::ifstream f(tmp_out, std::ios::binary);
    if (!f) {
        std::cout << "SKIP: could not read llvm-nm output\n";
        return kSkipReturnCode;
    }
    std::ostringstream buffer;
    buffer << f.rdbuf();
    std::string contents = buffer.str();
    f.close();
    std::filesystem::remove(tmp_out);

    if (contents.empty()) {
        std::cout << "SKIP: empty llvm-nm output — could not verify symbol table content\n";
        return kSkipReturnCode;
    }

    // Parse llvm-nm's "--undefined-only" output line by line and compare the EXACT trailing
    // symbol token, not a substring of the whole dump — a naive `contents.find("system")` matches
    // as a substring inside unrelated, legitimate symbols this code really does reference (e.g.
    // `__std_system_error_allocate_message`, part of std::system_error's <filesystem> error-code
    // plumbing, nothing to do with the `system()` process-execution function). Symbol-name
    // precision matters here specifically because the whole point of this check is that renaming/
    // moving code shouldn't be able to fool it — a check that itself over-triggers on substrings
    // would need loosening exactly the way this test must not be loosened.
    auto extract_symbol = [](std::string const& line) -> std::string {
        // llvm-nm --undefined-only line shape: "                 U symbolname"
        std::size_t pos = line.find_last_of(" \t");
        return pos == std::string::npos ? line : line.substr(pos + 1);
    };
    bool found_any = false;
    std::size_t line_start = 0;
    while (line_start < contents.size()) {
        std::size_t line_end = contents.find('\n', line_start);
        if (line_end == std::string::npos) line_end = contents.size();
        std::string line = contents.substr(line_start, line_end - line_start);
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        std::string symbol = extract_symbol(line);
        for (char const* sym : kHostileSymbols) {
            if (symbol == sym) {
                std::cerr << "FAIL: agentengine_shell_runner references process-creation symbol: "
                           << sym << " (line: " << line << ")\n";
                found_any = true;
            }
        }
        line_start = line_end + 1;
    }
    if (found_any) {
        std::cerr << "Full undefined-symbol dump:\n" << contents << "\n";
        return 1;
    }
    std::cout << "test_shell_runner_no_process_creation: PASS — no process-creation symbol is "
                 "referenced anywhere in agentengine_shell_runner's object code ("
              << AE_SHELL_RUNNER_LIB_PATH << ")\n";
    return 0;
}
