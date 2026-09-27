// #120 S8 (ADR-203) and S7 (ADR-207): the bodies of include/agentengine/sandbox/detail/process_identity.hpp, moved
// verbatim out of the header so <windows.h> and the POSIX process headers no longer reach every file that includes
// docker_execution_surface.hpp. The header keeps every declaration and comment; the fail-closed rules each body
// implements are documented at its declaration there.

#include "agentengine/sandbox/detail/process_identity.hpp"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace agentengine::detail::process_identity {

long current_pid() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

}  // namespace agentengine::detail::process_identity

#ifndef _WIN32
namespace agentengine::detail::process_identity {

bool process_is_alive(long pid) {
    if (pid <= 0) return true;
    if (::kill(static_cast<::pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;
}

}  // namespace agentengine::detail::process_identity
#endif  // _WIN32

#ifdef _WIN32
namespace agentengine::detail::process_identity {

bool process_is_alive(long pid) {
    if (pid <= 0) return true;
    HANDLE const h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }
    DWORD exit_code = 0;
    bool const got = GetExitCodeProcess(h, &exit_code) != 0;
    CloseHandle(h);
    return !got || exit_code == STILL_ACTIVE;
}

}  // namespace agentengine::detail::process_identity
#endif  // _WIN32

#ifndef _WIN32
namespace agentengine::detail::process_identity {

std::optional<std::uint64_t> read_process_start_ticks(long pid) {
    if (pid <= 0) return std::nullopt;
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!f || !std::getline(f, line)) return std::nullopt;
    auto const close_paren = line.rfind(')');
    if (close_paren == std::string::npos) return std::nullopt;
    std::istringstream rest(line.substr(close_paren + 1));
    std::string token;
    for (int i = 0; i < 19; ++i) {
        if (!(rest >> token)) return std::nullopt;
    }
    if (!(rest >> token)) return std::nullopt;
    try {
        return static_cast<std::uint64_t>(std::stoull(token));
    } catch (...) {
        return std::nullopt;
    }
}

std::uint64_t current_process_start_key() {
    return read_process_start_ticks(current_pid()).value_or(0);
}

std::optional<std::uint64_t> process_start_key_for(long pid) {
    return read_process_start_ticks(pid);
}

}  // namespace agentengine::detail::process_identity
#endif  // _WIN32

#ifdef _WIN32
namespace agentengine::detail::process_identity {

std::uint64_t current_process_start_key() {
    FILETIME creation{}, exit_t{}, kernel_t{}, user_t{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_t, &kernel_t, &user_t)) return 0;
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

std::optional<std::uint64_t> process_start_key_for(long pid) {
    if (pid <= 0) return std::nullopt;
    HANDLE const h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) return std::nullopt;
    FILETIME creation{}, exit_t{}, kernel_t{}, user_t{};
    bool const ok = GetProcessTimes(h, &creation, &exit_t, &kernel_t, &user_t) != 0;
    CloseHandle(h);
    if (!ok) return std::nullopt;
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

}  // namespace agentengine::detail::process_identity
#endif  // _WIN32

namespace agentengine::detail::process_identity {

ProcessMatch check_process_identity(long pid, std::uint64_t recorded_start_key) {
    if (!process_is_alive(pid)) return ProcessMatch::kGoneOrReplaced;
    auto const current_key = process_start_key_for(pid);
    if (!current_key.has_value()) return ProcessMatch::kUnknown;
    return *current_key == recorded_start_key ? ProcessMatch::kAliveSameProcess
                                               : ProcessMatch::kGoneOrReplaced;
}

std::optional<OrphanIdentity> parse_orphan_identity(std::string const& name,
                                                    std::string_view name_prefix) {
    std::string const prefix(name_prefix);
    if (name.rfind(prefix, 0) != 0) return std::nullopt;
    std::string const rest = name.substr(prefix.size());
    std::string::size_type const sep1 = rest.find('_');
    if (sep1 == std::string::npos) return std::nullopt;
    std::string const pid_str = rest.substr(0, sep1);
    std::string const rest2 = rest.substr(sep1 + 1);
    std::string::size_type const sep2 = rest2.find('_');
    std::string const key_str = (sep2 == std::string::npos) ? rest2 : rest2.substr(0, sep2);
    if (pid_str.empty() || key_str.empty()) return std::nullopt;
    for (char const c : pid_str) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    for (char const c : key_str) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    try {
        long const pid = std::stol(pid_str);
        // REAL, independent-red-team-found finding (ADR-108 §5): `process_is_alive()` casts this value to `pid_t`
        // (POSIX, 32-bit) / `DWORD` (Windows, 32-bit), but `long` is 64-bit on LP64 Linux -- a decimal run that fits
        // in `long` yet exceeds INT32_MAX (no real pid ever reaches that range) silently TRUNCATES on that cast, and
        // the truncated value can coincidentally read as a dead pid even when the original, untruncated value was
        // never a pid at all. Empirically proven exploitable: a foreign container on a shared daemon named e.g.
        // `ae_des_10000000000_x` (a value no real create() call could ever produce, but nothing stops another party
        // from naming a container that on a shared host) got misclassified "confirmed dead" and destroyed. Rejecting
        // anything outside a real pid's possible 32-bit range closes this before the value ever reaches a liveness
        // check, not after.
        if (pid <= 0 || pid > (std::numeric_limits<std::int32_t>::max)()) return std::nullopt;
        std::uint64_t const key = std::stoull(key_str);
        return OrphanIdentity{pid, key};
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace agentengine::detail::process_identity
