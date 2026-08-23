#pragma once
// Implements ADR-001-shellrunner-grammar-and-dispatch.md §2.1 / §2.5.5, with the FULL path-escape
// list §2.5.5 requires (a floor that must match or exceed 025-Worktree-and-Virtual-Filesystem.md §5
// verbatim): `..` traversal, absolute-path redirection, symlink/junction/reparse-point escape,
// Alternate Data Streams, UNC paths, extended-length `\\?\` prefixes, reserved device names, and
// case-fold-consistent comparison between the capability-scope check and the actual I/O call.
//
// Stale-claim correction (2026-08-22, component-role-audit-tracker.md Finding N): this used to call
// itself "the only `FileSystemAdapter` implementation this project builds today" and "this project's
// actual near-term implementation target" -- no longer true. `MediatedFileSystemAdapter`
// (`mediated_filesystem_adapter.hpp`, ADR-014, Judged) supersedes this for every real path and is
// what every production caller (shell dispatch, `MediatedPythonRunner`'s `open()` bridge) actually
// uses; grep confirms zero non-test production instantiation of this type. It stays in the default
// build ON PURPOSE, not by oversight: `agentengine_shell_runner` (CMakeLists.txt) is a deliberately
// separate STATIC library so ADR-001 §7 finding 1's fix -- Sh-S1's "zero references to a
// process-creation primitive" check -- can be verified at link-target granularity against the real
// built artifact (`tests/test_shell_runner_proof.cpp`), not against prose. This file is "off-limits
// to reuse" (ADR-001 decision 4, CMakeLists.txt's own comment) precisely so the mediated replacement
// shares no source with that proof target.
//
// Backend: native_jail (008 §1b, §3).

#include <filesystem>
#include <string>
#include <string_view>

#include "agentengine/sandbox/filesystem_adapter.hpp"

namespace agentengine::native_jail {

// Rooted at a fixed host directory handed to it at construction (must already exist and be
// canonicalizable — construction fails closed via `create` otherwise). Every `path` argument to
// every virtual method is interpreted as a slash-or-backslash-separated path RELATIVE to that
// root ("/" and "" both mean the root itself); nothing this adapter accepts can name a host path
// outside the root, by construction of `validate_and_resolve` (real_filesystem_adapter.cpp).
class RealFileSystemAdapter final : public FileSystemAdapter {
public:
    // Factory rather than a public constructor: computing the root's canonical form is fallible
    // (the root might not exist, might not be a directory, might itself be a reparse point whose
    // target can't be resolved) and a `FileSystemAdapter` must never be constructed in a
    // half-valid state that later validation silently trusts.
    [[nodiscard]] static result<RealFileSystemAdapter> create(std::filesystem::path const& root);

    result<std::vector<std::byte>> read_file(std::string_view path) override;
    result<void> write_file(std::string_view path, std::span<std::byte const> data,
                             bool append) override;
    result<void> remove(std::string_view path, bool recursive) override;
    result<void> rename(std::string_view from, std::string_view to) override;
    result<void> copy_file(std::string_view from, std::string_view to) override;
    result<void> make_directory(std::string_view path, bool parents) override;
    result<std::vector<DirEntry>> list_directory(std::string_view path) override;
    result<bool> exists(std::string_view path) override;
    result<std::string> canonicalize(std::string_view path) override;

    // Exposed for tests: the validated-and-resolved *host* path a given virtual path would map
    // to, without performing any I/O beyond canonicalizing whatever already exists on disk. This
    // is the single choke point §2.5.5 requires — every public method above funnels through it,
    // so a test can assert the escape list directly instead of only indirectly through read/write
    // side effects.
    [[nodiscard]] result<std::filesystem::path> validate_and_resolve(std::string_view virtual_path) const;

private:
    explicit RealFileSystemAdapter(std::filesystem::path canonical_root)
        : root_(std::move(canonical_root)) {}

    std::filesystem::path root_; // canonical, absolute, case as returned by the OS
};

} // namespace agentengine::native_jail
