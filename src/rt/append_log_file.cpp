// ADR-037 / ADR-181 E32: the OS half of `rt::FileAppendLogStore` (include/agentengine/rt/append_log_store.hpp)
// -- the locked log file and the quarantine sidecar. It lives in a .cpp so <windows.h> and the POSIX file
// headers never reach the many headers that include append_log_store.hpp (E32 red team round 3, MAJOR: the
// header-only version broke consumer code through windows.h's macros). See that header's banner for the
// locking, torn-tail and quarantine rules this file implements.

#include "agentengine/rt/append_log_store.hpp"

#include <algorithm>
#include <random>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agentengine::rt::detail {

namespace {

[[nodiscard]] std::unexpected<error> fail_open(char const* what, std::filesystem::path const& path) {
    return std::unexpected(error{failure_class::transient, std::string(what) + ": " + path.string(),
                                 "rt.append_log_store.file_open_failed"});
}

[[nodiscard]] std::unexpected<error> fail_io(char const* what) {
    return std::unexpected(error{failure_class::transient, what, "rt.append_log_store.file_write_failed"});
}

#if defined(_WIN32)
HANDLE as_handle(std::intptr_t h) { return reinterpret_cast<HANDLE>(h); }

// The `\\?\` form of `path`, which the Win32 file API does not limit to MAX_PATH (round 3: a sidecar name a
// few characters longer than its log crossed 260 and made every later append fail).
std::wstring long_form(std::filesystem::path const& path) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (ec) return path.wstring();
    std::wstring w = abs.make_preferred().wstring();
    if (w.starts_with(LR"(\\?\)")) return w;
    if (w.starts_with(LR"(\\)")) return LR"(\\?\UNC\)" + w.substr(2);
    return LR"(\\?\)" + w;
}
#else
int as_fd(std::intptr_t h) { return static_cast<int>(h); }
#endif

}  // namespace

result<std::optional<LockedAppendLogFile>> LockedAppendLogFile::open(std::filesystem::path const& path,
                                                                     bool writer) {
    LockedAppendLogFile f;
#if defined(_WIN32)
    HANDLE const h = ::CreateFileW(long_form(path).c_str(), writer ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   writer ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD const why = ::GetLastError();
        if (!writer && (why == ERROR_FILE_NOT_FOUND || why == ERROR_PATH_NOT_FOUND)) return std::nullopt;
        return fail_open("could not open append log", path);
    }
    f.handle_ = reinterpret_cast<std::intptr_t>(h);
    OVERLAPPED whole{};
    if (!::LockFileEx(h, writer ? LOCKFILE_EXCLUSIVE_LOCK : 0, 0, MAXDWORD, MAXDWORD, &whole)) {
        return fail_open("could not lock append log", path);
    }
#else
    int const fd = ::open(path.c_str(), writer ? (O_RDWR | O_CREAT | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC), 0644);
    if (fd < 0) {
        if (!writer && errno == ENOENT) return std::nullopt;
        return fail_open("could not open append log", path);
    }
    f.handle_ = fd;
    while (::flock(fd, writer ? LOCK_EX : LOCK_SH) != 0) {
        if (errno != EINTR) return fail_open("could not lock append log", path);
    }
#endif
    f.locked_ = true;
    return std::optional<LockedAppendLogFile>(std::move(f));
}

result<std::vector<std::byte>> LockedAppendLogFile::read_all() {
    std::vector<std::byte> out;
#if defined(_WIN32)
    HANDLE const h = as_handle(handle_);
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) return fail_io("could not size append log");
    out.resize(static_cast<std::size_t>(size.QuadPart));
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) return fail_io("could not seek append log");
    std::size_t got = 0;
    while (got < out.size()) {
        DWORD const want = static_cast<DWORD>((std::min<std::size_t>)(out.size() - got, 1u << 30));
        DWORD n = 0;
        if (!::ReadFile(h, out.data() + got, want, &n, nullptr)) return fail_io("could not read append log");
        if (n == 0) break;
        got += n;
    }
    out.resize(got);
#else
    int const fd = as_fd(handle_);
    struct stat st{};
    if (::fstat(fd, &st) != 0) return fail_io("could not size append log");
    out.resize(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < out.size()) {
        ssize_t const n = ::pread(fd, out.data() + got, out.size() - got, static_cast<off_t>(got));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return fail_io("could not read append log");
        if (n == 0) break;
        got += static_cast<std::size_t>(n);
    }
    out.resize(got);
#endif
    return out;
}

result<void> LockedAppendLogFile::truncate_and_seek(std::size_t length) {
#if defined(_WIN32)
    HANDLE const h = as_handle(handle_);
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(length);
    if (!::SetFilePointerEx(h, at, nullptr, FILE_BEGIN) || !::SetEndOfFile(h)) {
        return fail_io("could not truncate append log");
    }
#else
    int const fd = as_fd(handle_);
    if (::ftruncate(fd, static_cast<off_t>(length)) != 0) return fail_io("could not truncate append log");
    if (::lseek(fd, static_cast<off_t>(length), SEEK_SET) < 0) return fail_io("could not seek append log");
#endif
    return {};
}

result<void> LockedAppendLogFile::write_all(std::vector<std::byte> const& bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
#if defined(_WIN32)
        DWORD const want = static_cast<DWORD>((std::min<std::size_t>)(bytes.size() - done, 1u << 30));
        DWORD n = 0;
        if (!::WriteFile(as_handle(handle_), bytes.data() + done, want, &n, nullptr) || n == 0) {
            return fail_io("failed appending record");
        }
        done += n;
#else
        ssize_t const n = ::write(as_fd(handle_), bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail_io("failed appending record");
        done += static_cast<std::size_t>(n);
#endif
    }
    return {};
}

void LockedAppendLogFile::close() noexcept {
    if (handle_ == kNoHandle) return;
#if defined(_WIN32)
    HANDLE const h = as_handle(handle_);
    if (locked_) {
        OVERLAPPED whole{};
        ::UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &whole);
    }
    ::CloseHandle(h);
#else
    int const fd = as_fd(handle_);
    if (locked_) ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
    handle_ = kNoHandle;
    locked_ = false;
}

result<std::filesystem::path> create_new_file(std::filesystem::path const& path, std::byte const* data,
                                              std::size_t size) {
    auto refused = [&path](bool exists) {
        return std::unexpected(error{failure_class::transient,
                                     std::string(exists ? "refusing to write through an existing name: "
                                                        : "could not create file: ") + path.string(),
                                     exists ? "rt.append_log_store.file_exists" : "rt.append_log_store.file_open_failed"});
    };
    bool wrote = true;
#if defined(_WIN32)
    std::filesystem::path const made(long_form(path));  // so a caller's remove() is not held to MAX_PATH
    // CREATE_NEW fails on any existing name, a symlink included; OPEN_REPARSE_POINT is belt and braces.
    HANDLE const h = ::CreateFileW(made.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD const why = ::GetLastError();
        return refused(why == ERROR_FILE_EXISTS || why == ERROR_ALREADY_EXISTS);
    }
    std::size_t done = 0;
    while (done < size) {
        DWORD const want = static_cast<DWORD>((std::min<std::size_t>)(size - done, 1u << 30));
        DWORD n = 0;
        if (!::WriteFile(h, data + done, want, &n, nullptr) || n == 0) {
            wrote = false;
            break;
        }
        done += n;
    }
    if (!::CloseHandle(h)) wrote = false;
#else
    std::filesystem::path const& made = path;
    // O_EXCL with O_CREAT fails on any existing name, a dangling symlink included (POSIX).
    int const fd = ::open(made.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return refused(errno == EEXIST);
    std::size_t done = 0;
    while (done < size) {
        ssize_t const n = ::write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            wrote = false;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) wrote = false;
#endif
    if (!wrote) {
        std::error_code ec;
        std::filesystem::remove(made, ec);  // ours: we created it
        return std::unexpected(error{failure_class::transient, "could not write file: " + path.string(),
                                     "rt.append_log_store.file_write_failed"});
    }
    return made;
}

result<std::filesystem::path> quarantine_append_log_tail(std::filesystem::path const& log,
                                                         std::vector<std::byte> const& bytes, std::size_t from) {
    std::random_device rd;
    for (int attempt = 0; attempt < 16; ++attempt) {
        char suffix[17];
        std::uint64_t const r = (std::uint64_t{rd()} << 32) ^ rd();
        for (int i = 0; i < 16; ++i) suffix[i] = "0123456789abcdef"[(r >> (4 * i)) & 0xf];
        suffix[16] = '\0';
        std::filesystem::path side = log;
        side += ".quarantine-" + std::to_string(from) + "-" + suffix;
        auto made = create_new_file(side, bytes.data() + from, bytes.size() - from);
        if (made) return made;
        if (made.error().code != "rt.append_log_store.file_exists") break;
    }
    return std::unexpected(error{failure_class::transient, "could not quarantine the unreadable tail of: " + log.string(),
                                 "rt.append_log_store.quarantine_failed"});
}

}  // namespace agentengine::rt::detail
