// Linux half of `agentengine::RealIoFileSystem::write_verified()`/`read_verified()`
// (sandbox/real_io_filesystem.hpp) -- decisions/ADR-104-real-io-filesystem-linux-parity.md, parity
// pass over the Windows-only original (ADR-102 Phase 3). Same containment guarantee, POSIX shape:
// `open_within_mount_root` (core/worktree_mount_fs_posix.hpp, already-Judged ADR-014 Design B)
// returns an already-verified file descriptor; `::write`/`::fstat`+`::read` act on that SAME
// descriptor, never a re-parsed path -- no window between "checked" and "used", matching the
// Windows sibling's own invariant exactly.
//
// `O_CREAT | O_TRUNC | O_WRONLY` is the POSIX analogue of Windows' `GENERIC_WRITE` +
// `CREATE_ALWAYS`: create if absent, truncate to empty if present, open for writing only -- the
// same "always start from a clean, empty file" contract on both platforms.
//
// `::write`/`::read` are not guaranteed to transfer the whole buffer in one call (a short write/read
// is a normal, documented POSIX outcome, not just an EINTR corner case) -- both loops below keep
// calling until the full byte count is transferred, retrying transparently on `EINTR`, matching
// `WriteFile`/`ReadFile`'s own "fully synchronous, no short transfer" guarantee on the Windows side.

#include "agentengine/sandbox/real_io_filesystem.hpp"

#include "agentengine/core/worktree_mount_fs_posix.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace agentengine {

result<void> RealIoFileSystem::write_verified(std::string const& relative_path,
                                                 std::vector<std::byte> const& bytes) {
    auto handle = open_within_mount_root(host_root_.string(), relative_path,
                                            O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (!handle.has_value()) return std::unexpected(handle.error());

    std::size_t written = 0;
    while (written < bytes.size()) {
        ssize_t const n = ::write(handle->get(), bytes.data() + written, bytes.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(error{failure_class::fatal,
                                           "write() failed on a verified fd: " + relative_path,
                                           "real_io.write_failed", errno});
        }
        written += static_cast<std::size_t>(n);
    }
    return result<void>{};
}

result<std::vector<std::byte>> RealIoFileSystem::read_verified(std::string const& relative_path) const {
    auto handle = open_within_mount_root(host_root_.string(), relative_path, O_RDONLY, 0);
    if (!handle.has_value()) return std::unexpected(handle.error());

    struct stat st{};
    if (::fstat(handle->get(), &st) != 0) {
        return std::unexpected(error{failure_class::fatal,
                                       "fstat() failed on a verified fd: " + relative_path,
                                       "real_io.read_failed", errno});
    }
    if (st.st_size < 0) {
        return std::unexpected(error{failure_class::fatal,
                                       "fstat() reported a negative size: " + relative_path,
                                       "real_io.read_failed"});
    }

    std::vector<std::byte> out(static_cast<std::size_t>(st.st_size));
    std::size_t total_read = 0;
    while (total_read < out.size()) {
        ssize_t const n = ::read(handle->get(), out.data() + total_read, out.size() - total_read);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(error{failure_class::fatal,
                                           "read() failed on a verified fd: " + relative_path,
                                           "real_io.read_failed", errno});
        }
        if (n == 0) break;  // EOF earlier than fstat()'s reported size (a real, concurrent shrink) --
                             // fall through to the length check below rather than spin forever.
        total_read += static_cast<std::size_t>(n);
    }
    if (total_read != out.size()) {
        return std::unexpected(error{failure_class::fatal,
                                       "read() returned fewer bytes than fstat() reported (file changed "
                                       "concurrently): " + relative_path,
                                       "real_io.read_short"});
    }
    return out;
}

}  // namespace agentengine
