#pragma once
// Implements agentengine::FileSystemAdapter (agentengine/sandbox/filesystem_adapter.hpp) --
// Milestone 3 Phase E3. A genuinely new adapter, built directly on `core/worktree_mount_fs.hpp`'s
// `open_within_mount_root` (ADR-014, Phase C2/C4, Judged) -- that primitive's own header names
// itself as exactly what a future `FileSystemAdapter` implementation is expected to call, so
// reusing it here is the documented intent (only the ADR-001 spike's `real_filesystem_adapter.
// {hpp,cpp}` is off-limits to reuse, per decision 4). Every method is TOCTOU-safe by the same
// construction ADR-014 established: the object verified is the object acted on, never a path
// re-derived and re-resolved between a check and a use.
//
// `remove`/`rename` use handle-anchored Win32 APIs (`SetFileInformationByHandle` with
// `FileDispositionInfo`/`FileRenameInfo`) rather than path-based `RemoveDirectoryW`/`MoveFileExW`,
// specifically so the already-verified handle -- not a re-resolved path -- is what the operation
// acts on. `list_directory` uses `GetFileInformationByHandleEx`/`FileFullDirectoryInfo` for the
// identical reason, rather than `FindFirstFileW` (which re-resolves a path string).

#include <optional>
#include <string>
#include <string_view>

#include "agentengine/sandbox/filesystem_adapter.hpp"

namespace agentengine::native_jail::mediated_shell {

class MediatedFileSystemAdapter final : public FileSystemAdapter {
public:
    [[nodiscard]] static result<MediatedFileSystemAdapter> create(std::wstring root);

    result<std::vector<std::byte>> read_file(std::string_view path) override;
    result<void> write_file(std::string_view path, std::span<std::byte const> data, bool append) override;
    result<void> remove(std::string_view path, bool recursive) override;
    result<void> rename(std::string_view from, std::string_view to) override;
    result<void> copy_file(std::string_view from, std::string_view to) override;
    result<void> make_directory(std::string_view path, bool parents) override;
    result<std::vector<DirEntry>> list_directory(std::string_view path) override;
    result<bool> exists(std::string_view path) override;
    result<std::string> canonicalize(std::string_view path) override;
    result<std::optional<MountUsage>> usage() override;

private:
    explicit MediatedFileSystemAdapter(std::wstring root) : root_(std::move(root)) {}

    std::wstring root_;
};

}  // namespace agentengine::native_jail::mediated_shell
