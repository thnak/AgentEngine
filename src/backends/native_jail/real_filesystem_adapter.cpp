// Implements ADR-001-shellrunner-grammar-and-dispatch.md §2.5.5 (RealFileSystemAdapter's full
// path-escape list). Backend: native_jail (008 §1b, §3).

#include "backends/native_jail/real_filesystem_adapter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <system_error>

namespace agentengine::native_jail {

namespace {

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// True iff `child` (already ASCII-lowercased, generic-separator form) is `root` itself or a
// descendant of it, compared on the SAME case-folded, canonical representation the actual I/O
// call will use — §2.5.5's "computed once, never compared as two independently-derived strings"
// requirement.
bool is_within_root(std::string const& child_lower, std::string const& root_lower) {
    if (child_lower == root_lower) return true;
    if (child_lower.size() <= root_lower.size()) return false;
    if (child_lower.compare(0, root_lower.size(), root_lower) != 0) return false;
    char sep = child_lower[root_lower.size()];
    return sep == '/' || sep == '\\';
}

std::string generic_lower(std::filesystem::path const& p) {
    std::string s = p.generic_string();
    return to_lower_ascii(s);
}

// Windows reserved device names (025 §5 / ADR-001 §2.5.5): a component is reserved if its stem
// (text before the first '.') case-insensitively matches one of these, REGARDLESS of extension —
// "NUL.txt" is just as reserved as "NUL".
bool is_reserved_device_stem(std::string const& component_lower) {
    std::string stem = component_lower.substr(0, component_lower.find('.'));
    static constexpr std::array<std::string_view, 22> kReserved = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5",
        "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
        "lpt6", "lpt7", "lpt8", "lpt9"};
    // Not std::array<...,22> with 9+9+4 = 22 entries — kept literal rather than computed so the
    // list is auditable at a glance against 025 §5's own list.
    for (auto name : kReserved) {
        if (stem == name) return true;
    }
    return false;
}

ae::error path_error(std::string message, std::string code) {
    return ae::error{failure_class::policy, std::move(message), std::move(code)};
}

} // namespace

result<RealFileSystemAdapter> RealFileSystemAdapter::create(std::filesystem::path const& root) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec) {
        return std::unexpected(path_error("filesystem root does not exist: " + root.string(),
                                           "shell.fs.bad_root"));
    }
    if (!std::filesystem::is_directory(root, ec) || ec) {
        return std::unexpected(
            path_error("filesystem root is not a directory: " + root.string(), "shell.fs.bad_root"));
    }
    std::filesystem::path canonical_root = std::filesystem::canonical(root, ec);
    if (ec) {
        return std::unexpected(path_error("filesystem root cannot be canonicalized: " + root.string(),
                                           "shell.fs.bad_root"));
    }
    return RealFileSystemAdapter(std::move(canonical_root));
}

result<std::filesystem::path>
RealFileSystemAdapter::validate_and_resolve(std::string_view virtual_path) const {
    // 1. Reject embedded NUL — a string that legitimately terminates early against every C API
    //    a downstream OS call might use, while std::string_view happily carries the rest.
    if (virtual_path.find('\0') != std::string_view::npos) {
        return std::unexpected(path_error("path contains an embedded NUL", "shell.fs.bad_path"));
    }

    // 2. UNC paths and the `\\?\` extended-length prefix both start with two consecutive
    //    separators (of either kind) — reject before any component splitting. A single leading
    //    separator is the ordinary, expected form of a mount-rooted virtual path ("/work/sub")
    //    and is NOT an escape by itself.
    if (virtual_path.size() >= 2 &&
        (virtual_path[0] == '/' || virtual_path[0] == '\\') &&
        (virtual_path[1] == '/' || virtual_path[1] == '\\')) {
        return std::unexpected(
            path_error("UNC / extended-length paths are rejected: " + std::string(virtual_path),
                       "shell.fs.escape_unc"));
    }

    // 3. Drive-letter absolute redirection ("C:\...", "C:/...", or bare "C:").
    if (virtual_path.size() >= 2 && virtual_path[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(virtual_path[0]))) {
        return std::unexpected(path_error(
            "absolute drive-letter paths are rejected: " + std::string(virtual_path),
            "shell.fs.escape_absolute"));
    }

    // 4. Component-wise validation: split on '/' and '\', reject `..`, reject reserved device
    //    names (stem-wise), reject any component carrying a ':' (Alternate Data Stream marker —
    //    a *non-leading* colon can't be a drive letter, so any colon reaching here is an ADS).
    std::filesystem::path relative;
    std::string component;
    auto flush_component = [&]() -> result<void> {
        if (component.empty() || component == ".") {
            component.clear();
            return {};
        }
        if (component == "..") {
            return std::unexpected(
                path_error("`..` traversal is rejected: " + std::string(virtual_path),
                           "shell.fs.escape_dotdot"));
        }
        std::string lower = to_lower_ascii(component);
        if (lower.find(':') != std::string::npos) {
            return std::unexpected(path_error(
                "Alternate Data Stream syntax is rejected: " + std::string(virtual_path),
                "shell.fs.escape_ads"));
        }
        if (is_reserved_device_stem(lower)) {
            return std::unexpected(path_error(
                "reserved device name is rejected: " + component, "shell.fs.escape_device_name"));
        }
        relative /= component;
        component.clear();
        return {};
    };
    for (char c : virtual_path) {
        if (c == '/' || c == '\\') {
            if (auto r = flush_component(); !r) return std::unexpected(r.error());
        } else {
            component.push_back(c);
        }
    }
    if (auto r = flush_component(); !r) return std::unexpected(r.error());

    std::filesystem::path target = root_ / relative;

    // 5. Symlink/junction/reparse-point escape check: walk upward from `target` to the longest
    //    prefix that actually exists, canonicalize THAT (which follows any symlink/junction along
    //    the way), and verify the canonical ancestor is still inside the canonical root, compared
    //    on the same case-folded representation used everywhere else in this function. Any
    //    component beyond the existing ancestor is, by construction above, free of `..`, drive
    //    letters, and UNC markers, so appending it lexically cannot itself introduce an escape.
    std::filesystem::path existing_ancestor = target;
    std::filesystem::path remaining;
    std::error_code ec;
    while (existing_ancestor != existing_ancestor.root_path() &&
           !std::filesystem::exists(existing_ancestor, ec)) {
        // NOTE: `path / path` unconditionally appends a preferred-separator before the right-hand
        // side, even when that side is empty — `path("x") / path("")` yields "x/" (a trailing
        // separator with nothing after it), which downstream I/O calls can interpret as "x" being
        // a directory rather than a file. Guard the empty case explicitly rather than relying on
        // `operator/`'s empty-path behavior.
        remaining = remaining.empty() ? existing_ancestor.filename()
                                       : existing_ancestor.filename() / remaining;
        auto parent = existing_ancestor.parent_path();
        if (parent == existing_ancestor) break; // reached a root with nothing existing
        existing_ancestor = parent;
    }
    std::filesystem::path canonical_ancestor = existing_ancestor;
    if (std::filesystem::exists(existing_ancestor, ec)) {
        canonical_ancestor = std::filesystem::canonical(existing_ancestor, ec);
        if (ec) {
            return std::unexpected(
                path_error("path cannot be canonicalized: " + std::string(virtual_path),
                           "shell.fs.bad_path"));
        }
    }
    std::string root_lower      = generic_lower(root_);
    std::string ancestor_lower  = generic_lower(canonical_ancestor);
    if (!is_within_root(ancestor_lower, root_lower)) {
        return std::unexpected(
            path_error("path escapes the filesystem root via symlink/junction: " +
                           std::string(virtual_path),
                       "shell.fs.escape_symlink"));
    }

    // Same empty-path/trailing-separator guard as above: if `target` already existed in full,
    // `remaining` is empty and `canonical_ancestor / remaining` would otherwise append a
    // trailing separator to an ordinary file path.
    std::filesystem::path final_path =
        remaining.empty() ? canonical_ancestor : canonical_ancestor / remaining;

    // 6. Defense in depth against a TOCTOU-planted symlink landing exactly on `remaining` between
    //    the walk above and this check: if the assembled path now fully exists, re-canonicalize
    //    it and re-verify containment on the same case-folded form before trusting it.
    if (std::filesystem::exists(final_path, ec)) {
        std::filesystem::path recanon = std::filesystem::canonical(final_path, ec);
        if (!ec) {
            std::string recanon_lower = generic_lower(recanon);
            if (!is_within_root(recanon_lower, root_lower)) {
                return std::unexpected(path_error(
                    "path escapes the filesystem root via symlink/junction: " +
                        std::string(virtual_path),
                    "shell.fs.escape_symlink"));
            }
            final_path = recanon;
        }
    }

    return final_path;
}

result<std::vector<std::byte>> RealFileSystemAdapter::read_file(std::string_view path) {
    auto resolved = validate_and_resolve(path);
    if (!resolved) return std::unexpected(resolved.error());
    std::ifstream in(*resolved, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::unexpected(
            ae::error{failure_class::contract, "cannot open file: " + std::string(path),
                      "shell.fs.not_found"});
    }
    auto size = in.tellg();
    if (size < 0) {
        return std::unexpected(
            ae::error{failure_class::contract, "cannot stat file: " + std::string(path),
                      "shell.fs.io_error"});
    }
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return data;
}

result<void> RealFileSystemAdapter::write_file(std::string_view path,
                                                std::span<std::byte const> data, bool append) {
    auto resolved = validate_and_resolve(path);
    if (!resolved) return std::unexpected(resolved.error());
    auto mode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
    std::ofstream out(*resolved, mode);
    if (!out) {
        return std::unexpected(
            ae::error{failure_class::contract, "cannot open file for write: " + std::string(path),
                      "shell.fs.io_error"});
    }
    if (!data.empty()) {
        out.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return {};
}

result<void> RealFileSystemAdapter::remove(std::string_view path, bool recursive) {
    auto resolved = validate_and_resolve(path);
    if (!resolved) return std::unexpected(resolved.error());
    std::error_code ec;
    if (recursive) {
        std::filesystem::remove_all(*resolved, ec);
    } else {
        std::filesystem::remove(*resolved, ec);
    }
    if (ec) {
        return std::unexpected(
            ae::error{failure_class::contract, "remove failed: " + std::string(path),
                      "shell.fs.io_error"});
    }
    return {};
}

result<void> RealFileSystemAdapter::rename(std::string_view from, std::string_view to) {
    auto rf = validate_and_resolve(from);
    if (!rf) return std::unexpected(rf.error());
    auto rt = validate_and_resolve(to);
    if (!rt) return std::unexpected(rt.error());
    std::error_code ec;
    std::filesystem::rename(*rf, *rt, ec);
    if (ec) {
        return std::unexpected(ae::error{failure_class::transient,
                                          "cross-device or failed rename: " + std::string(from),
                                          "shell.fs.rename_failed"});
    }
    return {};
}

result<void> RealFileSystemAdapter::copy_file(std::string_view from, std::string_view to) {
    auto rf = validate_and_resolve(from);
    if (!rf) return std::unexpected(rf.error());
    auto rt = validate_and_resolve(to);
    if (!rt) return std::unexpected(rt.error());
    std::error_code ec;
    std::filesystem::copy_file(*rf, *rt, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "copy failed: " + std::string(from), "shell.fs.io_error"});
    }
    return {};
}

result<void> RealFileSystemAdapter::make_directory(std::string_view path, bool parents) {
    auto resolved = validate_and_resolve(path);
    if (!resolved) return std::unexpected(resolved.error());
    std::error_code ec;
    bool ok = parents ? std::filesystem::create_directories(*resolved, ec)
                       : std::filesystem::create_directory(*resolved, ec);
    if (ec || (!ok && !std::filesystem::exists(*resolved))) {
        return std::unexpected(
            ae::error{failure_class::contract, "mkdir failed: " + std::string(path),
                      "shell.fs.io_error"});
    }
    return {};
}

result<std::vector<DirEntry>> RealFileSystemAdapter::list_directory(std::string_view path) {
    auto resolved = validate_and_resolve(path);
    if (!resolved) return std::unexpected(resolved.error());
    std::error_code ec;
    if (!std::filesystem::is_directory(*resolved, ec)) {
        return std::unexpected(
            ae::error{failure_class::contract, "not a directory: " + std::string(path),
                      "shell.fs.not_a_directory"});
    }
    std::vector<DirEntry> entries;
    for (auto const& e : std::filesystem::directory_iterator(*resolved, ec)) {
        DirEntry de;
        de.name         = e.path().filename().string();
        de.is_directory = e.is_directory(ec);
        de.size_bytes   = de.is_directory ? 0 : static_cast<std::uint64_t>(e.file_size(ec));
        entries.push_back(std::move(de));
    }
    return entries;
}

result<bool> RealFileSystemAdapter::exists(std::string_view path) {
    auto resolved = validate_and_resolve(path);
    if (!resolved) return std::unexpected(resolved.error());
    std::error_code ec;
    return std::filesystem::exists(*resolved, ec);
}

result<std::string> RealFileSystemAdapter::canonicalize(std::string_view path) {
    auto resolved = validate_and_resolve(path);
    if (!resolved) return std::unexpected(resolved.error());
    std::string root_generic = root_.generic_string();
    std::string full_generic = resolved->generic_string();
    // Return a mount-relative, forward-slash form ("/" for the root itself, "/sub/dir" below it)
    // so callers (ExecState.cwd) never see the host path (026 §2).
    if (full_generic.size() <= root_generic.size()) return std::string("/");
    std::string rel = full_generic.substr(root_generic.size());
    if (rel.empty() || rel.front() != '/') rel = "/" + rel;
    return rel;
}

} // namespace agentengine::native_jail
