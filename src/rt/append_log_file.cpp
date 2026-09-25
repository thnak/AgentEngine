// ADR-037 / ADR-187 E32 (+ ADR-181 phase 0's format, merged 2026-09-25): the OS half of `rt::FileAppendLogStore` (include/agentengine/rt/append_log_store.hpp)
// -- the locked log file. It lives in a .cpp so <windows.h> and the POSIX file
// headers never reach the many headers that include append_log_store.hpp (E32 red team round 3, MAJOR: the
// header-only version broke consumer code through windows.h's macros). See that header's banner for the
// locking and torn-tail rules this file implements.

#include "agentengine/rt/append_log_store.hpp"

#include <algorithm>
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

#else
int as_fd(std::intptr_t h) { return static_cast<int>(h); }
#endif

}  // namespace

result<std::optional<LockedAppendLogFile>> LockedAppendLogFile::open(std::filesystem::path const& path,
                                                                     bool writer) {
    LockedAppendLogFile f;
#if defined(_WIN32)
    HANDLE const h = ::CreateFileW(path.c_str(), writer ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
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

result<void> LockedAppendLogFile::sync_to_disk() {
#if defined(_WIN32)
    if (!::FlushFileBuffers(as_handle(handle_))) return fail_io("could not sync append log to disk");
#else
    int rc = 0;
    do {
        rc = ::fsync(as_fd(handle_));
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) return fail_io("could not sync append log to disk");
#endif
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

}  // namespace agentengine::rt::detail
