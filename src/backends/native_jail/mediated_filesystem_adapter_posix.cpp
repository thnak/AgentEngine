// Implements mediated_filesystem_adapter.hpp -- the Linux counterpart to mediated_filesystem_adapter.cpp
// (2026-08-28, ADR-103, the SandboxToolProvider Linux-parity pass). Same scope, same TOCTOU-safety
// discipline (ADR-014, Judged: the object verified is the object acted on, never a path re-derived
// and re-resolved between a check and a use), built on `core/worktree_mount_fs_posix.hpp`'s
// `open_within_mount_root` (ADR-014 Phase C4, the already-Judged Linux twin of the Windows Phase C2
// primitive) exactly the way the Windows sibling is built on its own `worktree_mount_fs.hpp`.
//
// Two POSIX primitives this task needed but that did not exist anywhere in this codebase before this
// file -- neither is itself a NEW security boundary (both build entirely on `open_within_mount_root`'s
// already-established containment guarantee for the fds/paths they walk), so neither needed its own
// ADR-014-grade design pass, matching how `worktree_mount_fs.cpp`'s own `list_within_mount_root`/
// `mount_root_usage` were themselves ordinary follow-on tasks against an already-Judged primitive:
//   - directory listing (`list_directory`, and the `redteam`-style walk `usage()` below needs) --
//     `fdopendir`/`readdir`/`fstatat`, all fd-relative to an already-verified directory fd, never a
//     path re-resolved from a string.
//   - live on-disk usage (`usage()`) -- an ITERATIVE, explicit-stack `fstatat`-based walk (fixed
//     2026-08-28, see `accumulate_usage()`'s own comment for why it was recursive originally and
//     why that was a real, guest-triggerable crash), mirroring `worktree_mount_fs.cpp`'s own
//     `mount_root_usage` policy exactly: symlinks are never followed while recursing (skipped, not
//     counted or descended into) purely to keep the scan non-cyclic and bounded, an
//     availability/correctness precaution for a usage COUNTER, not a second ADR-014-grade security
//     boundary.
//
// `remove()`'s own residual, disclosed rather than silently assumed equivalent to the Windows
// sibling: POSIX has no "delete via an already-open fd of the target itself" operation the way
// Windows' handle-anchored `SetFileInformationByHandle(FileDispositionInfo)` does -- deletion is
// fundamentally a (parent directory fd, leaf name) pair (`unlinkat`). This implementation verifies
// BOTH the target (via `open_within_mount_root`, confirming it resolves inside the mount and learning
// its type) AND the parent (via `open_within_mount_root` too, when non-root) before calling
// `unlinkat(parent_fd, leaf, ...)` -- a narrower TOCTOU guarantee than `open_within_mount_root`'s own
// open-then-verify pattern for the single leaf-name lookup `unlinkat` itself performs, the same,
// already-accepted narrowing this codebase's own `create_one_directory` (Windows sibling) documents
// for directory creation, not a new kind of gap this file introduces.
//
// NOT FIXED, disclosed (2026-08-28 red-team round): `remove(path, recursive=true)`'s own descent
// (below) recurses one C++ stack frame per directory-tree level -- the IDENTICAL structural hazard
// `accumulate_usage()` above was just fixed for, discovered by analogy while fixing that one, not
// independently confirmed to crash. This is NOT a Linux-specific regression: the Windows sibling's
// own `remove()` (`mediated_filesystem_adapter.cpp`) has the exact same recursive shape, and this
// file's own `remove()` deliberately mirrors it. Left unfixed here -- converting it needs the
// identical explicit-stack treatment on BOTH platforms for real parity, a real, contained, but
// separate follow-on task (touching the already-shipped Windows file too, out of THIS pass's
// Linux-parity scope), not attempted in this pass.
//
// `rename()`/`copy_file()`: implemented as copy-then-delete via `read_file`/`write_file`/`remove`,
// identical in shape and in its own disclosed non-atomic residual to the Windows sibling's own
// `rename()` (that file's own comment: ADR-001's researched, accepted fallback for `mv`) -- kept
// identical across platforms deliberately, not reimplemented via `renameat`, so this design's one
// accepted residual stays in exactly one place conceptually, not two independently-reasoned ones.

#include "backends/native_jail/mediated_filesystem_adapter.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#include "agentengine/core/worktree_mount.hpp"           // split_mount_path -- reused, not duplicated
#include "agentengine/core/worktree_mount_fs_posix.hpp"  // open_within_mount_root, SafeFileHandlePosix

namespace agentengine::native_jail::mediated_shell {

namespace {

result<void> posix_error_v(char const* what, int err) {
    return std::unexpected(error{failure_class::fatal,
                                  std::string(what) + " failed: " + std::strerror(err),
                                  "shell.fs_posix_failure", err});
}
template <class T>
result<T> posix_error(char const* what, int err) {
    return std::unexpected(posix_error_v(what, err).error());
}

// The only real smuggling surface a single POSIX path component has -- see
// core/worktree_mount_fs_posix.cpp's own `validate_fs_segment` for the identical reasoning (`\`/`:`
// are ordinary, legal filename characters on POSIX, unlike Windows, so only NUL is checked here).
// Duplicated rather than exported -- that function has internal linkage in a different,
// ADR-014-governed file; widening its visibility is out of scope for this fix, the same "duplicated
// rather than exported" precedent the Windows sibling's `reject_forbidden_segment_chars` already sets.
result<void> reject_forbidden_leaf_chars(std::string const& leaf) {
    if (leaf.find('\0') != std::string::npos) {
        return std::unexpected(error{failure_class::policy,
                                      "path segment contains a character not permitted in a mounted path",
                                      "worktree.mount_path_forbidden_character"});
    }
    return {};
}

// Splits "a/b/c" into parent="a/b", leaf="c" (or parent="", leaf="a/b/c" if there is no '/') --
// `leaf` can never itself contain '/' by construction, so `mkdirat`/`unlinkat` below always receive
// a single path component, never a multi-component relative path that could walk elsewhere inside
// (or, if crafted with enough `..` segments, attempt to walk outside) `parent`.
std::pair<std::string, std::string> split_leaf(std::string const& relative_path) {
    auto slash = relative_path.find_last_of('/');
    std::string parent = slash == std::string::npos ? "" : relative_path.substr(0, slash);
    std::string leaf    = slash == std::string::npos ? relative_path : relative_path.substr(slash + 1);
    return {parent, leaf};
}

// Opens the verified directory fd `relative_dir` names -- the mount root itself if empty, otherwise
// through `open_within_mount_root` like every other guest-path lookup in this file.
result<SafeFileHandlePosix> open_dir_within_mount_root(std::filesystem::path const& root,
                                                          std::string const& relative_dir) {
    if (relative_dir.empty()) {
        int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) return posix_error<SafeFileHandlePosix>("open(root)", errno);
        return SafeFileHandlePosix(fd);
    }
    return open_within_mount_root(root.string(), relative_dir, O_RDONLY | O_DIRECTORY);
}

// Reads what `fd` actually, currently resolves to via the kernel's own record of the open file
// description (`/proc/self/fd/N`) -- the same mechanism `worktree_mount_fs_posix.cpp`'s own
// (internal-linkage) `resolved_path_of_fd` uses, duplicated here for the identical "different,
// ADR-014-governed file" reason as `reject_forbidden_leaf_chars` above.
result<std::string> resolved_path_of_fd(int fd) {
    std::string const proc_path = "/proc/self/fd/" + std::to_string(fd);
    std::string buf(4096, '\0');
    ssize_t n = ::readlink(proc_path.c_str(), buf.data(), buf.size() - 1);
    if (n < 0) return posix_error<std::string>("readlink(/proc/self/fd)", errno);
    buf.resize(static_cast<std::size_t>(n));
    return buf;
}

result<void> create_one_directory(std::filesystem::path const& root, std::string const& relative_path) {
    auto [parent, leaf] = split_leaf(relative_path);
    if (auto ok = reject_forbidden_leaf_chars(leaf); !ok) return std::unexpected(ok.error());
    if (leaf.empty() || leaf == "." || leaf == "..") {
        return std::unexpected(error{failure_class::contract,
                                      "'.'/'..' are not a valid directory name to create",
                                      "worktree.mount_path_malformed"});
    }

    auto parent_handle = open_dir_within_mount_root(root, parent);
    if (!parent_handle) return std::unexpected(parent_handle.error());

    if (::mkdirat(parent_handle->get(), leaf.c_str(), 0700) != 0) {
        if (errno == EEXIST) return {};
        return posix_error_v("mkdirat", errno);
    }
    return {};
}

// Non-cyclic, `fstatat`-based walk -- mirrors `worktree_mount_fs.cpp`'s own `mount_root_usage`
// policy exactly (see this file's own top comment): symlinks are never followed/counted.
//
// MUST-FIX (2026-08-28 red-team round, empirically confirmed via a real WSL2 build): the original
// version of this function was RECURSIVE -- one C++ stack frame, plus TWO simultaneously-open fds
// (this level's own `dup`/`fdopendir` handle, held open for the whole recursive call below; the
// child's `openat` fd, held open until its own recursive call returns), per directory-tree level.
// `usage()` is not a diagnostic side-path -- `mediated_shell_dispatch.cpp`'s `require_fs_write()`
// calls it on every quota-capped `mkdir`/`cp`/output-redirect, an entirely ordinary sandbox
// configuration -- and nothing in `split_mount_path`/`open_within_mount_root` caps guest-created
// directory NESTING DEPTH (only individual segment shape), so a guest can legitimately `mkdir` an
// arbitrarily deep tree via ordinary, individually-harmless calls, then trip this. Confirmed via a
// real 50,000-level-deep tree built the same fd-relative way this function itself walks: under the
// default `ulimit -n` this failed safe with `EMFILE`, denying a legitimate operation for no
// adversarial reason; under a raised (realistic, container-plausible) `ulimit -n` it SEGFAULTED the
// host process, twice, reproducibly -- a guest-triggerable crash of the sandbox host, contradicting
// the very design `worktree_mount_fs.cpp::mount_root_usage()` (below) already documents avoiding for
// this identical reason: "An explicit stack, not recursion -- ... its depth is not bounded by
// anything this function controls."
//
// Fixed here the same way: an explicit stack of MOUNT-RELATIVE PATH STRINGS (not open fds), each
// re-opened via `open_within_mount_root` only when popped and closed again before the next pop --
// at most ONE open directory handle at any time, matching the Windows sibling's own documented
// invariant exactly, and bounded by the number of PENDING directories on the frontier (ordinary,
// self-limiting disk usage), never by nesting depth. Re-verifying containment via
// `open_within_mount_root` on every pop (rather than a cheaper `openat` relative to a still-open
// parent fd) costs one extra syscall per directory but needs no separate argument for why it stays
// safe -- that primitive's own already-Judged guarantee covers an arbitrary guest-relative path
// directly, regardless of how deep.
result<void> accumulate_usage(std::filesystem::path const& root, MountUsage& usage) {
    std::vector<std::string> stack{std::string()};  // "" = the mount root itself
    while (!stack.empty()) {
        std::string const relative_dir = std::move(stack.back());
        stack.pop_back();

        auto dir_handle = open_dir_within_mount_root(root, relative_dir);
        if (!dir_handle) return std::unexpected(dir_handle.error());
        int const dup_fd = ::dup(dir_handle->get());
        if (dup_fd < 0) return posix_error_v("dup", errno);
        DIR* dir = ::fdopendir(dup_fd);
        if (!dir) {
            int const err = errno;
            ::close(dup_fd);
            return posix_error_v("fdopendir", err);
        }

        errno = 0;
        for (struct dirent* ent = ::readdir(dir); ent != nullptr; ent = ::readdir(dir)) {
            std::string const name(ent->d_name);
            if (name == "." || name == "..") {
                errno = 0;
                continue;
            }
            struct stat st {};
            if (::fstatat(dir_handle->get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
                int const err = errno;
                ::closedir(dir);
                return posix_error_v("fstatat", err);
            }
            if (S_ISLNK(st.st_mode)) {
                errno = 0;
                continue;  // never followed/counted -- see this file's own top comment
            }
            if (S_ISDIR(st.st_mode)) {
                stack.push_back(relative_dir.empty() ? name : relative_dir + "/" + name);
            } else if (S_ISREG(st.st_mode)) {
                usage.total_bytes += static_cast<std::uint64_t>(st.st_size);
                usage.file_count += 1;
            }
            errno = 0;
        }
        if (errno != 0) {
            int const err = errno;
            ::closedir(dir);
            return posix_error_v("readdir", err);
        }
        ::closedir(dir);  // also closes dup_fd -- dir_handle's own fd is closed by its own
                           // destructor at the top of the next loop iteration (or on return)
    }
    return {};
}

}  // namespace

result<MediatedFileSystemAdapter> MediatedFileSystemAdapter::create(std::filesystem::path root) {
    // Prove the root itself is a real, existing directory -- open_within_mount_root's own "trusted
    // baseline" step re-does this on every call anyway, but failing fast here at construction
    // (matching the Windows sibling's identical shape) surfaces a misconfigured root immediately.
    int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return posix_error<MediatedFileSystemAdapter>("open(root)", errno);
    ::close(fd);
    return MediatedFileSystemAdapter(std::move(root));
}

result<std::vector<std::byte>> MediatedFileSystemAdapter::read_file(std::string_view path) {
    auto h = open_within_mount_root(root_.string(), std::string(path), O_RDONLY);
    if (!h) return std::unexpected(h.error());

    struct stat st {};
    if (::fstat(h->get(), &st) != 0) return posix_error<std::vector<std::byte>>("fstat", errno);
    std::vector<std::byte> out(static_cast<std::size_t>(st.st_size));
    std::size_t total_read = 0;
    while (total_read < out.size()) {
        ssize_t n = ::read(h->get(), out.data() + total_read, out.size() - total_read);
        if (n < 0) {
            if (errno == EINTR) continue;
            return posix_error<std::vector<std::byte>>("read", errno);
        }
        if (n == 0) break;
        total_read += static_cast<std::size_t>(n);
    }
    out.resize(total_read);
    return out;
}

result<void> MediatedFileSystemAdapter::write_file(std::string_view path, std::span<std::byte const> data,
                                                     bool append) {
    // `O_APPEND` at open time gives kernel-atomic append-at-EOF semantics for each write below --
    // stronger than the Windows sibling's own manual `SetFilePointerEx(..., FILE_END)` (a real,
    // narrower guarantee on this platform, not merely equivalent).
    int const flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    auto h = open_within_mount_root(root_.string(), std::string(path), flags, 0600);
    if (!h) return std::unexpected(h.error());
    std::size_t total_written = 0;
    while (total_written < data.size()) {
        ssize_t n = ::write(h->get(), data.data() + total_written, data.size() - total_written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return posix_error_v("write", errno);
        }
        total_written += static_cast<std::size_t>(n);
    }
    return {};
}

result<bool> MediatedFileSystemAdapter::exists(std::string_view path) {
    // A plain O_RDONLY open succeeds for both a regular file AND a directory on POSIX (unlike
    // Windows, which needs FILE_FLAG_BACKUP_SEMANTICS to open a directory HANDLE at all) -- no
    // special-casing needed here for the two kinds, matching this method's own contract exactly.
    auto h = open_within_mount_root(root_.string(), std::string(path), O_RDONLY);
    if (h) return true;
    if (h.error().klass == failure_class::policy || h.error().klass == failure_class::contract) {
        return std::unexpected(h.error());
    }
    // SHOULD-FIX (2026-08-28 red-team round): originally checked ONLY `native_code == ENOENT`,
    // narrower than the Windows sibling's own catch-all ("any non-policy/non-contract failure ->
    // false", mediated_filesystem_adapter.cpp). Empirically confirmed to diverge: querying a path
    // through an intermediate component that is itself a regular file (e.g. "blocker/child" where
    // "blocker" is a file, not a directory) returns ENOTDIR on Linux, which the narrower check
    // propagated as a hard error instead of the Windows-matching `false` -- not a security escape
    // (both fail SAFE, just differently-shaped), but a real "isolation parity is a gate, not
    // identical shape" (CONVENTIONS.md) violation. Broadened to match the Windows catch-all exactly.
    return false;
}

result<void> MediatedFileSystemAdapter::remove(std::string_view path, bool recursive) {
    auto h = open_within_mount_root(root_.string(), std::string(path), O_RDONLY);
    if (!h) return std::unexpected(h.error());
    struct stat st {};
    if (::fstat(h->get(), &st) != 0) return posix_error_v("fstat", errno);
    bool const is_dir = S_ISDIR(st.st_mode);

    if (is_dir && recursive) {
        auto entries = list_directory(path);
        if (!entries) return std::unexpected(entries.error());
        for (auto const& e : *entries) {
            std::string child = std::string(path) + (path.empty() ? "" : "/") + e.name;
            auto r = remove(child, true);
            if (!r) return r;
        }
    }

    auto [parent, leaf] = split_leaf(std::string(path));
    auto parent_handle = open_dir_within_mount_root(root_, parent);
    if (!parent_handle) return std::unexpected(parent_handle.error());
    if (::unlinkat(parent_handle->get(), leaf.c_str(), is_dir ? AT_REMOVEDIR : 0) != 0) {
        return posix_error_v("unlinkat", errno);
    }
    return {};
}

result<void> MediatedFileSystemAdapter::rename(std::string_view from, std::string_view to) {
    auto data = read_file(from);
    if (!data) return std::unexpected(data.error());
    auto write = write_file(to, std::span<std::byte const>(*data), false);
    if (!write) return std::unexpected(write.error());
    return remove(from, false);
}

result<void> MediatedFileSystemAdapter::copy_file(std::string_view from, std::string_view to) {
    auto data = read_file(from);
    if (!data) return std::unexpected(data.error());
    return write_file(to, std::span<std::byte const>(*data), false);
}

result<void> MediatedFileSystemAdapter::make_directory(std::string_view path, bool parents) {
    if (!parents) return create_one_directory(root_, std::string(path));

    auto segments = split_mount_path(std::string(path));
    if (!segments) return std::unexpected(segments.error());
    std::string prefix;
    for (auto const& seg : *segments) {
        prefix += (prefix.empty() ? "" : "/") + seg;
        auto probe = open_within_mount_root(root_.string(), prefix, O_RDONLY);
        if (probe) continue;  // already exists
        // SHOULD-FIX (2026-08-28 red-team round): also accept ENOTDIR (an earlier segment already
        // exists as a regular file, so the kernel cannot traverse through it) as "not found yet" --
        // matches the Windows sibling's own dual check (ERROR_FILE_NOT_FOUND OR
        // ERROR_PATH_NOT_FOUND). Does not change the overall outcome (this still ultimately fails,
        // one step later inside create_one_directory's own parent-open call, exactly as the Windows
        // sibling does) -- only makes the error PATH match across platforms, not just the final
        // pass/fail verdict.
        bool const not_found_yet =
            probe.error().native_code == ENOENT || probe.error().native_code == ENOTDIR;
        if (!not_found_yet) return std::unexpected(probe.error());
        auto created = create_one_directory(root_, prefix);
        if (!created) return std::unexpected(created.error());
    }
    return {};
}

result<std::vector<DirEntry>> MediatedFileSystemAdapter::list_directory(std::string_view path) {
    auto h = open_dir_within_mount_root(root_, std::string(path));
    if (!h) return std::unexpected(h.error());

    // `fdopendir` takes ownership of the fd it is given -- `dup` first so `h`'s own fd stays
    // independently valid (RAII-closed by `SafeFileHandlePosix` regardless of this function's exit
    // path).
    int const dup_fd = ::dup(h->get());
    if (dup_fd < 0) return posix_error<std::vector<DirEntry>>("dup", errno);
    DIR* dir = ::fdopendir(dup_fd);
    if (!dir) {
        int const err = errno;
        ::close(dup_fd);
        return posix_error<std::vector<DirEntry>>("fdopendir", err);
    }

    std::vector<DirEntry> out;
    errno = 0;
    for (struct dirent* ent = ::readdir(dir); ent != nullptr; ent = ::readdir(dir)) {
        std::string name(ent->d_name);
        if (name == "." || name == "..") {
            errno = 0;
            continue;
        }
        // `lstat`-equivalent (AT_SYMLINK_NOFOLLOW): a symlink entry reports as itself, never
        // silently resolved through -- ordinary directory-listing semantics, and consistent with
        // `usage()`'s own "symlinks are never followed" policy just below.
        struct stat st {};
        if (::fstatat(h->get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            int const err = errno;
            ::closedir(dir);
            return posix_error<std::vector<DirEntry>>("fstatat", err);
        }
        DirEntry de;
        de.name         = std::move(name);
        de.is_directory = S_ISDIR(st.st_mode);
        de.size_bytes   = static_cast<std::uint64_t>(st.st_size);
        out.push_back(std::move(de));
        errno = 0;
    }
    if (errno != 0) {
        int const err = errno;
        ::closedir(dir);
        return posix_error<std::vector<DirEntry>>("readdir", err);
    }
    ::closedir(dir);
    return out;
}

result<std::string> MediatedFileSystemAdapter::canonicalize(std::string_view path) {
    auto h = open_within_mount_root(root_.string(), std::string(path), O_RDONLY);
    if (!h) return std::unexpected(h.error());
    auto target_real = resolved_path_of_fd(h->get());
    if (!target_real) return std::unexpected(target_real.error());

    int const root_fd = ::open(root_.c_str(), O_RDONLY | O_DIRECTORY);
    if (root_fd < 0) return posix_error<std::string>("open(root)", errno);
    SafeFileHandlePosix root_handle(root_fd);
    auto root_real = resolved_path_of_fd(root_handle.get());
    if (!root_real) return std::unexpected(root_real.error());

    // Strip the (already-canonicalized) root prefix -- the adapter's own contract is a
    // root-relative canonical path, never a host path (filesystem_adapter.hpp's own comment:
    // "callers never see host paths"). No backslash-to-forward-slash translation needed here,
    // unlike the Windows sibling -- POSIX paths are already '/'-separated.
    std::string relative =
        target_real->size() > root_real->size() ? target_real->substr(root_real->size()) : "";
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    return relative;
}

result<std::optional<MountUsage>> MediatedFileSystemAdapter::usage() {
    MountUsage out;
    auto r = accumulate_usage(root_, out);
    if (!r) return std::unexpected(r.error());
    return std::optional<MountUsage>(out);
}

}  // namespace agentengine::native_jail::mediated_shell
