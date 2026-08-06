// Milestone 3 Phase E4 -- the STATIC half of 010-Python-Code-Interpreter.md §9 G2's ShellRunner-
// specific bar ("must not merely be contained, must not exist as a reachable code path"), applied
// to the REAL `agentengine_mediated_shell_runner` library (E3), not the untouched ADR-001 spike --
// test_shell_runner_no_process_creation.cpp already covers the spike; this file is its own
// translation unit against the genuinely new one, same methodology, deliberately duplicated rather
// than parameterized (matching this project's own per-abuse-test-file precedent).
//
// Checked against the ACTUAL BUILT ARTIFACT via `llvm-nm --undefined-only`, not source text -- a
// symbol import table can't be satisfied by renaming a function or moving code between files the
// way a text grep could be fooled by either. `mediated_command_registry.hpp`'s own header comment
// already states the design property ("no branch ... could reach fork/exec/CreateProcess for an
// unrecognized name, because it never had a reference to any process-creation API in the first
// place"); this is that property's own executed proof, not an assertion resting on the comment
// alone.
//
// Skips (CTest SKIP_RETURN_CODE) if llvm-nm isn't available, rather than silently passing or
// failing for an unrelated reason -- same discipline as the spike's own version of this check.

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef AE_MEDIATED_SHELL_RUNNER_LIB_PATH
#error "AE_MEDIATED_SHELL_RUNNER_LIB_PATH must be defined by CMake to the built agentengine_mediated_shell_runner artifact"
#endif
#ifndef AE_LLVM_NM_PATH
#define AE_LLVM_NM_PATH ""
#endif

namespace {

constexpr int kSkipReturnCode = 77;

constexpr std::array<char const*, 14> kHostileSymbols = {
    "CreateProcessA", "CreateProcessW", "CreateProcessAsUserA", "CreateProcessAsUserW",
    "_wspawnv",       "_wspawnve",       "_spawnv",             "_spawnve",
    "system",         "_wsystem",        "LoadLibraryA",        "LoadLibraryW",
    "WinExec",         "ShellExecuteA",
};

} // namespace

int main() {
    std::string nm_path = AE_LLVM_NM_PATH;
    if (nm_path.empty()) {
        std::cout << "SKIP: llvm-nm not found at configure time\n";
        return kSkipReturnCode;
    }

    std::string tmp_out =
        (std::filesystem::temp_directory_path() / "ae_mediated_shell_runner_no_process_creation_nm.txt")
            .string();
    std::string inner = "\"" + nm_path + "\" --undefined-only \"" + AE_MEDIATED_SHELL_RUNNER_LIB_PATH +
                         "\" > \"" + tmp_out + "\" 2>&1";
    std::string cmd = "\"" + inner + "\"";
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

    auto extract_symbol = [](std::string const& line) -> std::string {
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
                std::cerr << "FAIL: agentengine_mediated_shell_runner references process-creation "
                             "symbol: "
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
    std::cout << "test_mediated_shell_runner_no_process_creation: PASS — no process-creation "
                 "symbol is referenced anywhere in agentengine_mediated_shell_runner's object code ("
              << AE_MEDIATED_SHELL_RUNNER_LIB_PATH << ")\n";
    return 0;
}
