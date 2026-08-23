#pragma once
// Small Win32/UTF helpers shared across the native-jail Windows backend and its jailed Python worker
// (008-Sandbox-and-Isolation.md §1b/§3; the jailed-worker design superseding the "Correction
// (2026-08-23)" comment `native_jail_backend.hpp` used to carry — see native_jail_backend.hpp's own
// top comment for the ADR this implements). Lifted out of `native_jail_backend.cpp`'s anonymous
// namespace (where `widen`/`HandleGuard` originated, exec()-path only) so a SECOND translation unit —
// `python_worker_mediation.cpp`/`python_worker_main.cpp`, which run inside the separate worker
// process, not inside `native_jail_backend.cpp`'s own TU — can use the identical UTF-8/UTF-16
// conversion and RAII-handle idioms rather than re-deriving them. Header-only: no new .cpp, no new
// link-time dependency for either consumer.

#include <windows.h>

#include <cstddef>
#include <string>

namespace agentengine::native_jail {

// UTF-8 (this codebase's std::string convention) -> UTF-16, for Win32 *W APIs.
[[nodiscard]] inline std::wstring widen(std::string const& utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

// UTF-16 -> UTF-8, the inverse of widen() above.
[[nodiscard]] inline std::string narrow(std::wstring const& utf16) {
    if (utf16.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr,
                                      0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), needed,
                         nullptr, nullptr);
    return out;
}

// RAII HANDLE close — the same idiom `native_jail_backend.cpp`'s exec() path already uses locally;
// shared here so `create_python_worker()` (same file, but now also `FramedChannel`'s two callers in
// two separate binaries) doesn't hand-roll a second copy.
struct HandleGuard {
    HANDLE h = nullptr;
    HandleGuard() = default;
    explicit HandleGuard(HANDLE handle) : h(handle) {}
    ~HandleGuard() { close_now(); }
    HandleGuard(HandleGuard const&) = delete;
    HandleGuard& operator=(HandleGuard const&) = delete;
    HandleGuard(HandleGuard&& other) noexcept : h(other.h) { other.h = nullptr; }
    HandleGuard& operator=(HandleGuard&& other) noexcept {
        if (this != &other) {
            close_now();
            h = other.h;
            other.h = nullptr;
        }
        return *this;
    }
    void close_now() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = nullptr;
        }
    }
    HANDLE release() {
        HANDLE tmp = h;
        h = nullptr;
        return tmp;
    }
};

}  // namespace agentengine::native_jail
