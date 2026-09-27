#pragma once
// TEST ORACLE (ADR-203, #120 S8) -- do not use outside
// tests/sandbox/execution_surface/test_process_identity_equivalence.cpp.
// Verbatim copies of the two OLD process-identity implementations as of aa2573f, extracted by script (ADR-203 §6):
//   - include/agentengine/sandbox/docker_execution_surface.hpp lines 728-734 (`current_pid`) and 753-929
//     (`kOrphanNamePrefix` through `parse_orphan_identity`, both platforms), inside namespace
//     `agentengine::oracle_docker_identity` (added);
//   - include/agentengine/sandbox/containerd_execution_surface.hpp lines 298-424 (`kOrphanIdPrefix` through
//     `parse_orphan_identity`; POSIX only, as that header is), inside namespace `agentengine::oracle_ctr_identity`
//     (added).
// The includes are the ones those lines need, taken from the two headers. Nothing else is changed.

#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace agentengine::oracle_docker_identity {

[[nodiscard]] inline long current_pid() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

// The discoverable marker every container `DockerCliBackend::create()` starts now carries via
// `docker run --name`, mirroring the naming scheme `ContainerdExecutionSurface::reset()` already uses
// for `ctr` container ids (`ae_ces_<pid>_<seq>`) -- kept a DIFFERENT prefix (`ae_des_`, not `ae_ces_`)
// so `reap_orphans()` on one backend can never accidentally match a name only the OTHER backend's own
// scheme produced, even though nothing stops both from running against the same host.
inline constexpr char const* kOrphanNamePrefix = "ae_des_";

// POSIX counterpart of the pid-liveness check `reap_orphans()` needs -- see the Windows overload
// below (under `#ifdef _WIN32`) for why this exists on both platforms, unlike most of this file's
// other platform splits. `kill(pid, 0)` sends no signal, only asks the kernel whether `pid` could be
// signaled at all; `ESRCH` is the one answer that actually means "no such process" -- every other
// outcome (success, or a failure this process lacks permission to fully diagnose, e.g. `EPERM` for a
// pid reused by a different, differently-owned process) is treated as "still alive", failing CLOSED:
// this function is the one gate standing between a caller and destroying a real container, so a wrong
// "dead" answer is the only wrong answer that has a real consequence.
#ifndef _WIN32
[[nodiscard]] inline bool process_is_alive(long pid) {
    if (pid <= 0) return true;
    if (::kill(static_cast<::pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;
}
#else
// Windows analog: `OpenProcess()` failing with `ERROR_INVALID_PARAMETER` is the one answer Microsoft
// documents as meaning the pid names no process at all; every other failure (most commonly
// `ERROR_ACCESS_DENIED` for a pid this process cannot fully query) fails closed as "assume alive",
// same posture as the POSIX side. A handle that DOES open is further checked via
// `GetExitCodeProcess()` -- a pid can remain a valid, openable handle for a zombie-equivalent
// not-yet-reaped exited process on Windows too, and `STILL_ACTIVE` is the only value that actually
// means "running". DISCLOSED, not solved: pid reuse (the kernel recycling a dead process's pid for an
// unrelated new one before this check runs) is a real, inherent race in ANY pid-liveness check on any
// platform, not specific to this function -- narrowing it further (e.g. a boot-id-scoped identity)
// is real follow-on work, not attempted here.
[[nodiscard]] inline bool process_is_alive(long pid) {
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
#endif

// ADR-108 §7 pid-reuse-race fix (narrows, not just disclosed): a plain pid-liveness check alone
// cannot tell "the ORIGINAL process that created this container is still running" from "the pid was
// later reused by a completely unrelated process" -- the second case reads as "alive" under
// `process_is_alive()` alone, so a genuinely orphaned container silently stays unreapable forever
// once its pid happens to get recycled by something else. Fixed by embedding, alongside the pid, a
// per-process-INSTANCE "start key" that is (for all practical purposes) never the same across two
// different process instances, even ones sharing the same pid: POSIX reads `/proc/<pid>/stat`'s own
// `starttime` field (ticks since boot -- man proc(5)), Windows reads `GetProcessTimes()`'s
// `lpCreationTime`. `check_process_identity()` below then answers a strictly more specific question
// than plain liveness: "is the SAME process instance that minted this key still running," not just
// "is SOME process running at this pid."
#ifndef _WIN32
// Reads `/proc/<pid>/stat`'s field 22 (`starttime`). Field 2 (`comm`, the process name) is the one
// field in this file that can itself contain spaces or parentheses (man proc(5)) -- every robust
// parser's own convention, reused here, is to find the LAST `)` in the line and count fields from
// there, rather than naively splitting on whitespace from the start. After that `)`, `state` (field
// 3) is the first token; `starttime` (field 22) is therefore the 20th token counting from there.
[[nodiscard]] inline std::optional<std::uint64_t> read_process_start_ticks(long pid) {
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
[[nodiscard]] inline std::uint64_t current_process_start_key() {
    return read_process_start_ticks(current_pid()).value_or(0);
}
// nullopt here means "could not read a start-key for this pid right now" -- deliberately NOT the
// same thing as "no such process" (`process_is_alive()`, unchanged, is still the one function that
// answers that). A transient read failure (the process exiting between the caller's own
// `process_is_alive()` check and this call) must resolve to "unknown", not "gone" -- the caller
// (`check_process_identity()` below) is the one place that decides what an unreadable key means.
[[nodiscard]] inline std::optional<std::uint64_t> process_start_key_for(long pid) {
    return read_process_start_ticks(pid);
}
#else
[[nodiscard]] inline std::uint64_t current_process_start_key() {
    FILETIME creation{}, exit_t{}, kernel_t{}, user_t{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_t, &kernel_t, &user_t)) return 0;
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}
[[nodiscard]] inline std::optional<std::uint64_t> process_start_key_for(long pid) {
    if (pid <= 0) return std::nullopt;
    HANDLE const h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) return std::nullopt;
    FILETIME creation{}, exit_t{}, kernel_t{}, user_t{};
    bool const ok = GetProcessTimes(h, &creation, &exit_t, &kernel_t, &user_t) != 0;
    CloseHandle(h);
    if (!ok) return std::nullopt;
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}
#endif

enum class ProcessMatch { kAliveSameProcess, kGoneOrReplaced, kUnknown };

// The real, narrowed identity check `reap_orphans()` uses in place of plain `process_is_alive()`.
// Layered ON TOP of that already-audited function, not a replacement for its own "ESRCH/
// ERROR_INVALID_PARAMETER is the one unambiguous 'gone' signal" logic: if no process at all is alive
// at `pid`, the creator is unambiguously gone regardless of any key comparison. Only when a process
// genuinely IS alive at that exact pid does the start-key comparison run, to tell "still the same
// process" apart from "the pid got recycled by something else" -- and if THAT read itself fails
// (e.g. a Windows `ERROR_ACCESS_DENIED` for a different-owner process, or a race where the process
// exits between the two checks), the outcome is `kUnknown`, which `reap_orphans()` treats exactly
// like `kAliveSameProcess` -- fail closed, never destroy on an ambiguous answer.
[[nodiscard]] inline ProcessMatch check_process_identity(long pid, std::uint64_t recorded_start_key) {
    if (!process_is_alive(pid)) return ProcessMatch::kGoneOrReplaced;
    auto const current_key = process_start_key_for(pid);
    if (!current_key.has_value()) return ProcessMatch::kUnknown;
    return *current_key == recorded_start_key ? ProcessMatch::kAliveSameProcess
                                               : ProcessMatch::kGoneOrReplaced;
}

// Extracts the (pid, start_key) `create()` embedded in `name` -- the two decimal segments between
// `kOrphanNamePrefix` and the SECOND following `_` (anything after that, including a third `_`, is
// the free-form seq suffix and not parsed further). Returns nullopt -- not a best-effort partial
// parse -- for anything that isn't a plain, purely decimal run of digits in EITHER segment: a name
// sharing this prefix by coincidence (never actually produced by this class, but not provably
// impossible on a shared host) must never be treated as one of ours just because it starts with the
// right characters. Fails closed (skip, don't reap) rather than guessing.
struct OrphanIdentity {
    long pid = 0;
    std::uint64_t start_key = 0;
};

[[nodiscard]] inline std::optional<OrphanIdentity> parse_orphan_identity(std::string const& name) {
    std::string const prefix = kOrphanNamePrefix;
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
        // REAL, independent-red-team-found finding (ADR-108 §5): `process_is_alive()` casts this
        // value to `pid_t` (POSIX, 32-bit) / `DWORD` (Windows, 32-bit), but `long` is 64-bit on
        // LP64 Linux -- a decimal run that fits in `long` yet exceeds INT32_MAX (no real pid ever
        // reaches that range) silently TRUNCATES on that cast, and the truncated value can
        // coincidentally read as a dead pid even when the original, untruncated value was never a
        // pid at all. Empirically proven exploitable: a foreign container on a shared daemon named
        // e.g. `ae_des_10000000000_x` (a value no real create() call could ever produce, but nothing
        // stops another party from naming a container that on a shared host) got misclassified
        // "confirmed dead" and destroyed. Rejecting anything outside a real pid's possible 32-bit
        // range closes this before the value ever reaches a liveness check, not after.
        if (pid <= 0 || pid > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        std::uint64_t const key = std::stoull(key_str);
        return OrphanIdentity{pid, key};
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace agentengine::oracle_docker_identity

#ifndef _WIN32
namespace agentengine::oracle_ctr_identity {

// The naming scheme `ContainerdExecutionSurface::reset()` already uses for every container id it
// creates (`"ae_ces_" + pid + "_" + seq`, unchanged by this addition) -- reused here, not invented,
// as the discoverable marker `reap_orphans()` below scans `ctr containers list` for. Any id NOT
// starting with this exact prefix is never touched by reap_orphans(), regardless of what else is
// running on the host (I2: this mechanism only ever acts on containers this class's own naming
// scheme produced).
inline constexpr char const* kOrphanIdPrefix = "ae_ces_";

// Real `kill(pid, 0)` liveness probe -- sends no signal, only asks the kernel whether `pid` could be
// signaled at all. `ESRCH` is the one answer that actually means "no such process" -- every other
// outcome (success, or a failure this process lacks permission to fully diagnose e.g. `EPERM` for a
// pid now reused by a different, differently-owned process) is treated as "still alive", failing
// CLOSED: this function is the one gate standing between a caller and destroying a real container, so
// a wrong "dead" answer is the only wrong answer that has a real consequence.
[[nodiscard]] inline bool process_is_alive(long pid) {
    if (pid <= 0) return true;
    if (::kill(static_cast<::pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;
}

// ADR-108 §7 pid-reuse-race fix (narrows, not just disclosed) -- identical rationale and shape to
// `docker_execution_surface.hpp`'s own fix: a plain pid-liveness check alone cannot tell "the
// ORIGINAL process that created this container is still running" from "the pid was later reused by
// a completely unrelated process" -- the second case reads as "alive" under `process_is_alive()`
// alone, so a genuinely orphaned container silently stays unreapable forever once its pid happens to
// get recycled by something else. Fixed by embedding, alongside the pid, a per-process-INSTANCE
// "start key" read from `/proc/<pid>/stat`'s own `starttime` field (ticks since boot -- man proc(5)),
// which is (for all practical purposes) never the same across two different process instances, even
// ones sharing the same pid. Field 2 (`comm`, the process name) is the one field in that file that can
// itself contain spaces or parentheses -- every robust parser's own convention, reused here, is to
// find the LAST `)` in the line and count fields from there, rather than naively splitting on
// whitespace from the start. After that `)`, `state` (field 3) is the first token; `starttime` (field
// 22) is therefore the 20th token counting from there.
[[nodiscard]] inline std::optional<std::uint64_t> read_process_start_ticks(long pid) {
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
[[nodiscard]] inline std::uint64_t current_process_start_key() {
    return read_process_start_ticks(static_cast<long>(::getpid())).value_or(0);
}
// nullopt here means "could not read a start-key for this pid right now" -- deliberately NOT the
// same thing as "no such process" (`process_is_alive()`, unchanged, is still the one function that
// answers that). A transient read failure (the process exiting between the caller's own
// `process_is_alive()` check and this call) must resolve to "unknown", not "gone" --
// `check_process_identity()` below is the one place that decides what an unreadable key means.
[[nodiscard]] inline std::optional<std::uint64_t> process_start_key_for(long pid) {
    return read_process_start_ticks(pid);
}

enum class ProcessMatch { kAliveSameProcess, kGoneOrReplaced, kUnknown };

// The real, narrowed identity check `reap_orphans()` uses in place of plain `process_is_alive()`.
// Layered ON TOP of that already-audited function, not a replacement for its own "ESRCH is the one
// unambiguous 'gone' signal" logic: if no process at all is alive at `pid`, the creator is
// unambiguously gone regardless of any key comparison. Only when a process genuinely IS alive at that
// exact pid does the start-key comparison run, to tell "still the same process" apart from "the pid
// got recycled by something else" -- and if THAT read itself fails (e.g. a race where the process
// exits between the two checks), the outcome is `kUnknown`, which `reap_orphans()` treats exactly
// like `kAliveSameProcess` -- fail closed, never destroy on an ambiguous answer.
[[nodiscard]] inline ProcessMatch check_process_identity(long pid, std::uint64_t recorded_start_key) {
    if (!process_is_alive(pid)) return ProcessMatch::kGoneOrReplaced;
    auto const current_key = process_start_key_for(pid);
    if (!current_key.has_value()) return ProcessMatch::kUnknown;
    return *current_key == recorded_start_key ? ProcessMatch::kAliveSameProcess
                                               : ProcessMatch::kGoneOrReplaced;
}

// Extracts the (pid, start_key) `reset()` embedded in `id` -- the two decimal segments between
// `kOrphanIdPrefix` and the SECOND following `_` (anything after that, including a third `_`, is the
// free-form seq suffix and not parsed further). Returns nullopt -- not a best-effort partial parse --
// for anything that isn't a plain, purely decimal run of digits in EITHER segment: a name sharing this
// prefix by coincidence (never actually produced by this class, but not provably impossible on a
// shared host) must never be treated as one of ours just because it starts with the right characters.
// Fails closed (skip, don't reap) rather than guessing.
struct OrphanIdentity {
    long pid = 0;
    std::uint64_t start_key = 0;
};

[[nodiscard]] inline std::optional<OrphanIdentity> parse_orphan_identity(std::string const& id) {
    std::string const prefix = kOrphanIdPrefix;
    if (id.rfind(prefix, 0) != 0) return std::nullopt;
    std::string const rest = id.substr(prefix.size());
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
        // REAL, independent-red-team-found finding (ADR-108 §5), identical to the Docker side's own
        // fix: `process_is_alive()` below casts this value to `pid_t` (32-bit), but `long` is 64-bit
        // on LP64 Linux -- a decimal run that fits in `long` yet exceeds INT32_MAX (no real pid ever
        // reaches that range) silently TRUNCATES on that cast, and the truncated value can
        // coincidentally read as a dead pid even though the original value was never a real pid.
        // Rejecting anything outside a real pid's possible 32-bit range closes this before the value
        // ever reaches a liveness check, not after.
        if (pid <= 0 || pid > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        std::uint64_t const key = std::stoull(key_str);
        return OrphanIdentity{pid, key};
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace agentengine::oracle_ctr_identity
#endif
