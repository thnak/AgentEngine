// Windows half of `agentengine::RealIoFileSystem::write_verified()`/`read_verified()`
// (sandbox/real_io_filesystem.hpp) -- decisions/ADR-104-real-io-filesystem-linux-parity.md. Kept
// verbatim from the original, entirely-header-only implementation (ADR-102 Phase 3) -- this pass
// only moves the two Win32-specific bodies out-of-line, unchanged, so real_io_filesystem.hpp itself
// no longer needs to include `core/worktree_mount_fs.hpp` (and, transitively, `<windows.h>`).

#include "agentengine/sandbox/real_io_filesystem.hpp"

#include "agentengine/core/worktree_mount_fs.hpp"

namespace agentengine {

result<void> RealIoFileSystem::write_verified(std::string const& relative_path,
                                                 std::vector<std::byte> const& bytes) {
    auto handle = open_within_mount_root(host_root_.wstring(), relative_path, GENERIC_WRITE, CREATE_ALWAYS);
    if (!handle.has_value()) return std::unexpected(handle.error());
    DWORD written = 0;
    BOOL const ok = bytes.empty()
        ? TRUE
        : WriteFile(handle->get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    if (!ok || (!bytes.empty() && written != bytes.size())) {
        return std::unexpected(error{failure_class::fatal,
                                       "WriteFile failed on a verified handle: " + relative_path,
                                       "real_io.write_failed"});
    }
    return result<void>{};
}

result<std::vector<std::byte>> RealIoFileSystem::read_verified(std::string const& relative_path) const {
    auto handle = open_within_mount_root(host_root_.wstring(), relative_path, GENERIC_READ, OPEN_EXISTING);
    if (!handle.has_value()) return std::unexpected(handle.error());
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle->get(), &size) || size.QuadPart < 0) {
        return std::unexpected(error{failure_class::fatal,
                                       "GetFileSizeEx failed on a verified handle: " + relative_path,
                                       "real_io.read_failed"});
    }
    std::vector<std::byte> out(static_cast<std::size_t>(size.QuadPart));
    if (!out.empty()) {
        DWORD read_bytes = 0;
        BOOL const ok = ReadFile(handle->get(), out.data(), static_cast<DWORD>(out.size()), &read_bytes, nullptr);
        if (!ok || read_bytes != out.size()) {
            return std::unexpected(error{failure_class::fatal,
                                           "ReadFile failed on a verified handle: " + relative_path,
                                           "real_io.read_failed"});
        }
    }
    return out;
}

}  // namespace agentengine
