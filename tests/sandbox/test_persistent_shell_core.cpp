// ADR-209 (GitHub issue #146) -- the live shell's shared core, OFFLINE: the snapshot diff and its caps, the sh and
// pwsh replay scripts' quoting (the host-side half of C9; the live half runs the scripts in a real sh/pwsh), and
// the Docker client-reply parser's bounds. No daemon, no process.
//
//   S1  make_snapshot() diffs against the open-time environment: changed and new names are set, removed ones
//       unset, unchanged ones left out.
//   S2  the denylist (IFS, ENV, BASH_ENV, LD_*, PS*, SHELLOPTS, BASHOPTS, PROMPT_COMMAND, PWD, OLDPWD, SHLVL)
//       and non-identifier names never enter a snapshot.
//   S3  caps: more than 64 variables or 32 KiB is truncated (flagged), a cwd over 4 KiB is dropped (flagged),
//       never cut.
//   Q1  sh_single_quote(): every hostile value round-trips through a POSIX single-quote parse (a reference
//       parser in this test) to itself -- `'`, U+2019, newline, `$(...)`, backquote, backslash.
//   Q2  sh_replay_script() splices a name only if it is an identifier.
//   Q3  pwsh_replay_script() carries no value as a literal: every value is base64 (no quote of any kind, ASCII
//       or U+2018-U+201B, appears in the script outside the fixed template).
//   P1  parse_client_reply(): a well-formed reply parses (exit code, cwd with a trailing-newline strip, env,
//       base env, output).
//   P2  a missing header is not ok and carries the marker (AE_SHELL_LOST).
//   P3  lengths past the end of the reply, a non-numeric or >255 exit status, a record without NULs: not ok,
//       never an out-of-bounds read.
//
// Positive controls (planted by hand, recorded in ADR-209 §15): escaping `'` as `\'` (the classic mistake)
// fails Q1; dropping the `xlen > body.size()` check fails P3 under the debug iterator checks.

#include "agentengine/sandbox/docker_persistent_shell_surface.hpp"
#include "agentengine/sandbox/persistent_shell.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace agentengine;
namespace ps = agentengine::persistent_shell;
namespace dps = agentengine::docker_persistent_shell_detail;

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

// A reference POSIX parse of ONE word made only of single-quoted segments and `\'` escapes -- exactly the
// grammar sh_single_quote() may emit. Returns nullopt on anything else (an unquoted metacharacter included).
std::optional<std::string> parse_sh_word(std::string_view w) {
    std::string out;
    std::size_t i = 0;
    while (i < w.size()) {
        if (w[i] == '\'') {
            std::size_t const end = w.find('\'', i + 1);
            if (end == std::string_view::npos) return std::nullopt;
            out.append(w.substr(i + 1, end - i - 1));
            i = end + 1;
        } else if (w[i] == '\\' && i + 1 < w.size() && w[i + 1] == '\'') {
            out.push_back('\'');
            i += 2;
        } else {
            return std::nullopt;
        }
    }
    return out;
}

using Env = std::vector<std::pair<std::string, std::string>>;

}  // namespace

int main() {
    // ---- S1
    {
        Env const base{{"HOME", "/root"}, {"KEEP", "1"}, {"GONE", "x"}, {"PATH", "/bin"}};
        Env const after{{"HOME", "/root"}, {"KEEP", "1"}, {"PATH", "/venv/bin:/bin"}, {"NEW", "v"}};
        auto s = ps::make_snapshot("/workspace/src", after, base);
        bool const path_set = std::find(s.env_set.begin(), s.env_set.end(),
                                        std::pair<std::string, std::string>{"PATH", "/venv/bin:/bin"}) != s.env_set.end();
        bool const new_set = std::find(s.env_set.begin(), s.env_set.end(),
                                       std::pair<std::string, std::string>{"NEW", "v"}) != s.env_set.end();
        check(s.cwd == "/workspace/src" && path_set && new_set && s.env_set.size() == 2,
              "S1: changed and new variables are set, unchanged ones left out");
        check(s.env_unset == std::vector<std::string>{"GONE"} && !s.truncated, "S1: a removed variable is unset");
    }
    // ---- S2
    {
        Env const after{{"IFS", "x"},       {"ENV", "/x"},     {"BASH_ENV", "/x"}, {"LD_PRELOAD", "/evil.so"},
                        {"PS1", "$ "},      {"PS4", "+"},      {"SHELLOPTS", "x"}, {"BASHOPTS", "x"},
                        {"PROMPT_COMMAND", "x"}, {"PWD", "/etc"}, {"OLDPWD", "/"}, {"SHLVL", "9"},
                        {"BASH_FUNC_f%%", "() { :; }"}, {"1BAD", "x"}, {"OK_NAME", "fine"}};
        auto s = ps::make_snapshot("/w", after, {});
        check(s.env_set.size() == 1 && s.env_set[0].first == "OK_NAME",
              "S2: denylisted and non-identifier names never enter a snapshot");
        // ADR-209 §15.5: case-insensitive (Windows names are), and .NET's injection points (pwsh is .NET).
        Env const dotnet{{"psmodulepath", "C:\\evil"}, {"ld_preload", "/e.so"}, {"DOTNET_STARTUP_HOOKS", "C:\\h.dll"},
                         {"COR_ENABLE_PROFILING", "1"},  {"COR_PROFILER_PATH", "C:\\p.dll"}, {"CORECLR_PROFILER", "x"},
                         {"COMPlus_EnableDiagnostics", "1"}, {"OK_NAME", "fine"}};
        auto s2 = ps::make_snapshot("/w", dotnet, {});
        check(s2.env_set.size() == 1 && s2.env_set[0].first == "OK_NAME",
              "S2: lower-case denylisted names and .NET startup-hook/profiler variables never enter a snapshot");
    }
    // ---- S3
    {
        Env many;
        for (int i = 0; i < 100; ++i) many.emplace_back("V" + std::to_string(1000 + i), "x");
        auto s = ps::make_snapshot("/w", many, {});
        check(s.env_set.size() == kShellSnapshotMaxVars && s.truncated, "S3: more than 64 variables is capped and flagged");
        Env big{{"A", std::string(20000, 'a')}, {"B", std::string(20000, 'b')}};
        auto s2 = ps::make_snapshot("/w", big, {});
        check(s2.env_set.size() == 1 && s2.truncated, "S3: more than 32 KiB of environment is capped and flagged");
        auto s3 = ps::make_snapshot(std::string(5000, 'd'), {}, {});
        check(s3.cwd.empty() && s3.truncated, "S3: a cwd over 4 KiB is dropped, never cut");
    }
    // ---- Q1
    {
        std::vector<std::string> const hostile{"it's",
                                               "\xE2\x80\x99quoted\xE2\x80\x99",
                                               "line1\nline2\n\n",
                                               "$(touch /tmp/pwned)",
                                               "`id`",
                                               "back\\slash\\",
                                               "''''",
                                               "",
                                               "a'\"b\"'c$HOME;rm -rf /"};
        bool all = true;
        for (auto const& v : hostile) {
            auto q = ps::sh_single_quote(v);
            auto parsed = parse_sh_word(q);
            if (!parsed || *parsed != v) {
                all = false;
                std::printf("       value did not round-trip: [%s] -> [%s]\n", v.c_str(), q.c_str());
            }
        }
        check(all, "Q1: every hostile value round-trips through a POSIX single-quote parse to itself");
    }
    // ---- Q2
    {
        ShellSnapshot s;
        s.cwd = "/w";
        s.env_set = {{"GOOD", "1"}, {"BAD;rm", "x"}, {"LD_PRELOAD", "/e.so"}};
        s.env_unset = {"ALSO_BAD$(x)"};
        auto script = ps::sh_replay_script(s);
        check(script.find("export GOOD='1'") != std::string::npos && script.find("BAD;rm") == std::string::npos &&
                  script.find("LD_PRELOAD") == std::string::npos && script.find("ALSO_BAD") == std::string::npos,
              "Q2: only identifier, replayable names are spliced into the sh replay script");
    }
    // ---- Q3
    {
        ShellSnapshot s;
        s.cwd = "C:\\Users\\it's \xE2\x80\x98quoted\xE2\x80\x99";
        s.env_set = {{"VAL", "x' ; Remove-Item -Recurse C:\\ ; '\xE2\x80\x9B"}};
        auto script = ps::pwsh_replay_script(s);
        bool const no_value_text = script.find("Remove-Item") == std::string::npos &&
                                   script.find("quoted") == std::string::npos &&
                                   script.find("\xE2\x80") == std::string::npos;
        check(no_value_text, "Q3: the pwsh replay carries every value as base64, never as a literal");
    }
    // ---- P1
    {
        std::string record = std::string("7") + '\0' + "/workspace/sub\n" + '\0' + "PATH=/bin" + '\0' + "X=1" + '\0';
        std::string base = std::string("PATH=/bin") + '\0';
        std::string raw = "AE1 " + std::to_string(record.size()) + " 11 " + std::to_string(base.size()) + "\n" +
                          record + base + "hello\nworld";
        auto r = dps::parse_client_reply(raw, true);
        check(r.ok && r.exit_code == 7 && r.cwd == "/workspace/sub" && r.env.size() == 2 && r.base_env.size() == 1 &&
                  r.output == "hello\nworld" && r.output_bytes == 11,
              "P1: a well-formed reply parses");
        auto r2 = dps::parse_client_reply("AE1   " + std::to_string(record.size()) + "      5       0\n" + record + "abcde",
                                          false);
        check(r2.ok && r2.output == "abcde", "P1: space-padded `wc -c` counts parse");
    }
    // ---- P2
    {
        auto r = dps::parse_client_reply("AE_SHELL_LOST\n", false);
        check(!r.ok && r.marker == "AE_SHELL_LOST", "P2: a missing header is not ok and carries the marker");
    }
    // ---- P3
    {
        check(!dps::parse_client_reply("AE1 999999 0 0\nshort", false).ok, "P3: a record length past the end is rejected");
        std::string rec = std::string("x") + '\0' + "/w" + '\0';
        check(!dps::parse_client_reply("AE1 " + std::to_string(rec.size()) + " 0 0\n" + rec, false).ok,
              "P3: a non-numeric exit status is rejected");
        std::string rec2 = std::string("300") + '\0' + "/w" + '\0';
        check(!dps::parse_client_reply("AE1 " + std::to_string(rec2.size()) + " 0 0\n" + rec2, false).ok,
              "P3: an exit status above 255 is rejected");
        check(!dps::parse_client_reply("AE1 3 0 0\nabc", false).ok, "P3: a record without NUL separators is rejected");
        check(!dps::parse_client_reply("AE1 1 2\nx", false).ok, "P3: a header with the wrong field count is rejected");
        std::string rec3 = std::string("0") + '\0' + "/w" + '\0';
        check(!dps::parse_client_reply("AE1 " + std::to_string(rec3.size()) + " 0 99\n" + rec3, true).ok,
              "P3: a base length past the end is rejected");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
