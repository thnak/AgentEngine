#pragma once
// Implements ADR-182 §12 C-3's file-fixture rule (decisions/ADR-182-agent-test-driver-mcp.md §21): a
// fixture file is accepted only if git tracks it and it has no uncommitted change. A fixture decides a
// session's tools and instructions, so it is host authority; a tester that could write one could hand
// itself a different agent. Checked at session_start, every time, not cached.
//
// This is the host's check, wired by agentengine_test_driver's main(). The scenario runner does not use
// it (a person or CI runs that, not the tester), and tests inject their own.

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace agentengine::test_driver {

// Returns why the file is refused, or nothing when git tracks it and it matches HEAD. The path comes
// from a host-fixed root plus a name already checked against [a-z0-9_-]{1,64}, so it needs no shell
// escaping beyond the quotes (the root is the host's own command-line argument).
[[nodiscard]] inline std::optional<std::string> git_fixture_trust_check(std::filesystem::path const& file) {
#ifdef _WIN32
    constexpr char const* kNull = "NUL";
#else
    constexpr char const* kNull = "/dev/null";
#endif
    std::filesystem::path const abs = std::filesystem::absolute(file);
    std::string const dir = abs.parent_path().string();
    std::string const name = abs.filename().string();
    std::string const quiet = std::string(" >") + kNull + " 2>&1";
    std::string const tracked = "git -C \"" + dir + "\" ls-files --error-unmatch -- \"" + name + "\"" + quiet;
    if (std::system(tracked.c_str()) != 0) return std::string("not tracked by git");
    std::string const clean = "git -C \"" + dir + "\" diff --quiet HEAD -- \"" + name + "\"" + quiet;
    if (std::system(clean.c_str()) != 0) return std::string("has uncommitted changes");
    return std::nullopt;
}

}  // namespace agentengine::test_driver
