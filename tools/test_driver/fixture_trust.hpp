#pragma once
// Implements ADR-182 §12 C-3's file-fixture rule (decisions/ADR-182-agent-test-driver-mcp.md §21, §22): a
// fixture file is used only as git has it committed. A fixture decides a session's tools and
// instructions, so it is host authority; a tester that could write one could hand itself a different agent.
//
// §22 (red-team): checking the working file and then reading it trusted the wrong bytes. With
// `git update-index --assume-unchanged` (or --skip-worktree) a modified file passes `git diff --quiet`;
// a committed symlink passes while its untracked target is what gets read; and the file can change
// between the check and the read. So the bytes parsed are now the committed blob itself
// (`git show HEAD:./<name>`), read through git, never the working file. The tracked and clean checks stay,
// to tell the host why a file is refused.
//
// This is the host's reader, wired by agentengine_test_driver's main(). The scenario runner does not use
// it (a person or CI runs that, not the tester), and tests inject their own.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"

namespace agentengine::test_driver {

namespace fixture_trust_detail {

// The command goes through the platform shell (cmd.exe / sh), so a directory containing a character
// that shell would interpret is refused rather than quoted. The directory is the host's own
// --fixtures-root; the file name was already checked against [a-z0-9_-]{1,64} plus ".yaml".
[[nodiscard]] inline bool shell_safe(std::string_view s) {
    for (char c : s) {
        if (c == '"' || c == '%' || c == '!' || c == '^' || c == '&' || c == '|' || c == '<' || c == '>' ||
            c == '`' || c == '$' || c == '\n' || c == '\r')
            return false;
    }
    return true;
}

[[nodiscard]] inline error refuse(std::string why) {
    return error{failure_class::policy, std::move(why), "test.fixture_untrusted"};
}

}  // namespace fixture_trust_detail

// Returns the committed bytes of `file`, or why it is refused.
[[nodiscard]] inline result<std::string> git_committed_fixture(std::filesystem::path const& file) {
    namespace d = fixture_trust_detail;
#ifdef _WIN32
    constexpr char const* kNull = "NUL";
#else
    constexpr char const* kNull = "/dev/null";
#endif
    std::error_code ec;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(file, ec))) {
        return std::unexpected(d::refuse("is a symbolic link"));
    }
    std::filesystem::path const abs = std::filesystem::absolute(file, ec);
    std::string const dir = abs.parent_path().string();
    std::string const name = abs.filename().string();
    if (!d::shell_safe(dir) || !d::shell_safe(name)) {
        return std::unexpected(d::refuse("its path contains a character the shell would interpret"));
    }
    std::string const quiet = std::string(" >") + kNull + " 2>&1";
    std::string const git = "git -C \"" + dir + "\" ";
    if (std::system((git + "rev-parse --git-dir" + quiet).c_str()) != 0) {
        return std::unexpected(d::refuse("git is not available or the directory is not in a git repository"));
    }
    if (std::system((git + "ls-files --error-unmatch -- \"" + name + "\"" + quiet).c_str()) != 0) {
        return std::unexpected(d::refuse("not tracked by git"));
    }
    if (std::system((git + "diff --quiet HEAD -- \"" + name + "\"" + quiet).c_str()) != 0) {
        return std::unexpected(d::refuse("has uncommitted changes"));
    }
    std::string const show = git + "show \"HEAD:./" + name + "\"";
#ifdef _WIN32
    std::FILE* pipe = ::_popen(show.c_str(), "rb");
#else
    std::FILE* pipe = ::popen(show.c_str(), "r");
#endif
    if (pipe == nullptr) return std::unexpected(d::refuse("could not run git show"));
    std::string bytes;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), pipe)) > 0;) bytes.append(buf, n);
#ifdef _WIN32
    int const status = ::_pclose(pipe);
#else
    int const status = ::pclose(pipe);
#endif
    if (status != 0) return std::unexpected(d::refuse("is not in the committed tree (HEAD)"));
    return bytes;
}

}  // namespace agentengine::test_driver
