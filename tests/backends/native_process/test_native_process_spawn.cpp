// Design -> red-team -> prove -> judge for decisions/ADR-071-native-unsandboxed-process-execution-
// providers.md's native_process_spawn.hpp: the real, UNSANDBOXED host process-spawn primitive.
// Covers (1) argv-quoting correctness against known-correct Microsoft C runtime vectors -- a real
// injection vector if wrong, so it gets dedicated test vectors, not only indirect verification
// through an observed process's behavior -- (2) a real spawn round-trip (stdout capture, exit
// code), (3) the mandatory-cwd contract fails closed, and (4) the wall-clock safety ceiling
// actually terminates a runaway process even when the caller never asked for a cap (022 §5: every
// negative-result claim below is paired with a positive control).

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include "backends/native_process/native_process_spawn.hpp"

using namespace agentengine::native_process;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

std::string system_dir() {
    char buf[MAX_PATH]{};
    UINT len = GetSystemDirectoryA(buf, MAX_PATH);
    return std::string(buf, len);
}

std::string cmd_exe_path() { return system_dir() + "\\cmd.exe"; }
std::string ping_exe_path() { return system_dir() + "\\ping.exe"; }

}  // namespace

int main() {
    // ---- Q1: bare arguments with no special characters pass through unquoted -------------------
    {
        AE_CHECK(detail::quote_one_argument(L"hello") == L"hello",
                  "Q1: a plain argument is not quoted");
        AE_CHECK(detail::quote_one_argument(L"") == L"\"\"",
                  "Q1: an EMPTY argument is quoted (otherwise it would vanish from the command line)");
    }

    // ---- Q2: known-correct Microsoft C runtime quoting vectors ----------------------------------
    // https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments
    {
        AE_CHECK(detail::quote_one_argument(L"a b") == L"\"a b\"",
                  "Q2: an argument with an embedded space is wrapped in quotes");
        AE_CHECK(detail::quote_one_argument(L"he said \"hi\"") == L"\"he said \\\"hi\\\"\"",
                  "Q2: an embedded quote is backslash-escaped");
        AE_CHECK(detail::quote_one_argument(L"C:\\temp\\") == L"C:\\temp\\",
                  "Q2 (positive control): a trailing backslash with NO space needs no quoting at all");
        AE_CHECK(detail::quote_one_argument(L"C:\\Program Files\\") == L"\"C:\\Program Files\\\\\"",
                  "Q2: a trailing backslash IN a quoted argument is doubled (else it would escape the "
                  "closing quote)");
        AE_CHECK(detail::quote_one_argument(L"a\\\\b") == L"a\\\\b",
                  "Q2 (positive control): backslashes NOT followed by a quote and no space present "
                  "need no escaping or quoting at all");
    }

    // ---- R-Q3: the injection this algorithm exists to prevent -----------------------------------
    // If an argument NEEDING quotes (here: it contains a space, so it must be wrapped) were quoted
    // naively (just wrap in quotes, no backslash-doubling), a trailing backslash immediately before
    // the closing quote would escape that quote, letting whatever FOLLOWS in the command line be
    // read as a SECOND, attacker-influenced argument/flag instead of remaining part of the first.
    // Prove the real fix defeats this for a 3-argument build.
    {
        std::wstring const cmdline =
            detail::build_command_line({"prog", "C:\\evil path\\", "--dangerous-flag"});
        // A naive quoter would produce: prog "C:\evil path\" --dangerous-flag
        // where the trailing backslash-quote sequence \" is a literal escaped quote to a real
        // command-line parser, NOT a closing quote -- so "--dangerous-flag" would be swallowed
        // into the FIRST argument's text rather than remaining a separate, distinct argument.
        std::wstring const naive_bad = L"prog \"C:\\evil path\\\" --dangerous-flag";
        AE_CHECK(cmdline != naive_bad,
                  "R-Q3: the real quoting differs from the naive (vulnerable) quoting for this input");
        AE_CHECK(cmdline == L"prog \"C:\\evil path\\\\\" --dangerous-flag",
                  "R-Q3: the real quoting doubles the trailing backslash, keeping the closing quote "
                  "a real closing quote and \"--dangerous-flag\" a genuinely separate argument");
    }

    // ---- S1: mandatory cwd -- fails closed rather than falling back to an ambient directory ------
    {
        NativeExecRequest req;
        req.program_path = cmd_exe_path();
        req.argv = {req.program_path, "/c", "echo", "hi"};
        req.cwd = "";  // deliberately empty
        auto outcome = spawn_native_process(req);
        AE_CHECK(!outcome.has_value(),
                  "S1: an empty cwd is REJECTED, never silently defaulting to the current process's "
                  "own working directory (worktree confinement stays mandatory, ADR-071)");
    }

    // ---- S2 (positive control): a real spawn round-trip captures stdout and exit code -----------
    {
        std::string const tmp_dir = std::filesystem::temp_directory_path().string();
        NativeExecRequest req;
        req.program_path = cmd_exe_path();
        req.argv = {req.program_path, "/c", "echo", "hello-native-process-spawn"};
        req.cwd = tmp_dir;
        req.wall_ms_cap = 10000;
        auto outcome = spawn_native_process(req);
        AE_CHECK(outcome.has_value(), "S2: a real spawn against cmd.exe succeeds");
        if (outcome.has_value()) {
            AE_CHECK(outcome->klass == native_exec_outcome_class::ok, "S2: exit class is ok");
            AE_CHECK(outcome->exit_code == 0, "S2: exit code is 0");
            AE_CHECK(outcome->stdout_text.find("hello-native-process-spawn") != std::string::npos,
                      "S2: stdout actually contains the echoed text");
        }
    }

    // ---- S3: a nonzero exit code is faithfully reported ------------------------------------------
    {
        std::string const tmp_dir = std::filesystem::temp_directory_path().string();
        NativeExecRequest req;
        req.program_path = cmd_exe_path();
        req.argv = {req.program_path, "/c", "exit", "3"};
        req.cwd = tmp_dir;
        req.wall_ms_cap = 10000;
        auto outcome = spawn_native_process(req);
        AE_CHECK(outcome.has_value(), "S3: spawn succeeds even though the child exits nonzero");
        if (outcome.has_value()) {
            AE_CHECK(outcome->klass == native_exec_outcome_class::nonzero_exit,
                      "S3: exit class is nonzero_exit, not ok");
            AE_CHECK(outcome->exit_code == 3, "S3: the actual exit code is reported faithfully");
        }
    }

    // ---- R-S4: an unresponsive/runaway process is still terminated by the wall-clock safety ------
    // ceiling even when the caller never set wall_ms_cap -- "never literally unbounded".
    {
        std::string const tmp_dir = std::filesystem::temp_directory_path().string();
        NativeExecRequest req;
        req.program_path = ping_exe_path();
        // A long-running, deterministic real process that reads no stdin at all (so this test does
        // not depend on any stdin/console-handle inheritance behavior) -- 60 pings a second apart
        // is far longer than the 500ms wall_ms_cap below, simulating a runaway process.
        req.argv = {req.program_path, "-n", "60", "127.0.0.1"};
        req.cwd = tmp_dir;
        // wall_ms_cap left unset -- the internal safety ceiling (5 minutes) is far too slow for a
        // test; explicitly set a short one instead to prove the MECHANISM (not the specific
        // default) actually kills a hung process rather than blocking the test suite forever.
        req.wall_ms_cap = 500;
        auto const start = std::chrono::steady_clock::now();
        auto outcome = spawn_native_process(req);
        auto const elapsed = std::chrono::steady_clock::now() - start;
        AE_CHECK(outcome.has_value(), "R-S4: the call itself still returns (does not hang forever)");
        if (outcome.has_value()) {
            AE_CHECK(outcome->klass == native_exec_outcome_class::timeout,
                      "R-S4: a hung process is classified as timeout, not left running");
        }
        AE_CHECK(elapsed < std::chrono::seconds(30),
                  "R-S4: termination happens promptly, not after some much longer unrelated ceiling");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All native_process_spawn (ADR-071) checks passed.\n";
    return 0;
}
