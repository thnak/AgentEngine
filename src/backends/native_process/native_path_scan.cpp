// Implements native_path_scan.hpp. See that header for the spec/ADR citation and scope statement.

#include "backends/native_process/native_path_scan.hpp"

#include <filesystem>
#include <system_error>

#include "agentengine/pal/env.hpp"
#include "agentengine/trust/capability.hpp"

#if defined(_WIN32)
#include <cctype>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agentengine::native_process {

namespace {

#if defined(_WIN32)
constexpr char kPathSeparator = ';';

// PATHEXT-shaped check without actually reading PATHEXT (a host-configurable, rarely-customized
// env var this codebase would otherwise have to trust) -- the fixed, standard set covers every
// executable this ADR's four provider families actually launch (python.exe/node.exe/bash.exe/
// cmd.exe and their .bat/.cmd wrapper-script cousins some installers use).
bool has_recognized_executable_extension(std::string const& filename, std::string& out_stem) {
    static constexpr char const* kExts[] = {".exe", ".cmd", ".bat", ".com"};
    for (char const* ext : kExts) {
        std::size_t const ext_len = std::char_traits<char>::length(ext);
        if (filename.size() <= ext_len) continue;
        std::string suffix = filename.substr(filename.size() - ext_len);
        for (auto& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (suffix == ext) {
            out_stem = filename.substr(0, filename.size() - ext_len);
            return true;
        }
    }
    return false;
}
#else
constexpr char kPathSeparator = ':';
#endif

std::vector<std::string> split_path_env(std::string const& path_value) {
    std::vector<std::string> dirs;
    std::string current;
    for (char c : path_value) {
        if (c == kPathSeparator) {
            if (!current.empty()) dirs.push_back(std::exchange(current, std::string{}));
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) dirs.push_back(current);
    return dirs;
}

bool matches_any_grant(std::string const& candidate, std::vector<std::string> const& granted_patterns) {
    for (auto const& pattern : granted_patterns) {
        if (capability_detail::native_exec_pattern_covers(pattern, candidate)) return true;
    }
    return false;
}

}  // namespace

std::vector<DiscoveredExecutable> scan_path(std::vector<std::string> const& granted_patterns) {
    std::vector<DiscoveredExecutable> found;
    if (granted_patterns.empty()) return found;  // nothing granted -- nothing to discover, ever

    auto path_value = pal::env_var("PATH");
    if (!path_value.has_value()) return found;

    for (std::string const& dir : split_path_env(*path_value)) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec) || ec) continue;  // stale PATH entry -- skip, not an error

        for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec) || ec) continue;

            std::string const filename = entry.path().filename().string();
            std::string short_name;
#if defined(_WIN32)
            if (!has_recognized_executable_extension(filename, short_name)) continue;
#else
            struct stat st{};
            std::string const full = entry.path().string();
            if (::stat(full.c_str(), &st) != 0) continue;
            bool const any_exec_bit = (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
            if (!any_exec_bit) continue;
            short_name = filename;
#endif
            if (short_name.empty()) continue;
            if (!matches_any_grant(short_name, granted_patterns)) continue;

            found.push_back(DiscoveredExecutable{short_name, entry.path().string()});
        }
    }
    return found;
}

}  // namespace agentengine::native_process
