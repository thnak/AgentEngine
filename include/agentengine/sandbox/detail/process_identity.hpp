#pragma once
// #120 S8 (decisions/ADR-203-one-base64-one-process-identity.md): the one process-identity check behind orphaned
// container reaping (ADR-108 §5/§7), shared by `DockerCliBackend::reap_orphans()` (docker_execution_surface.hpp,
// ae_des_ names; Windows and POSIX) and `ContainerdCliBackend::reap_orphans()` (containerd_execution_surface.hpp,
// ae_ces_ ids; POSIX only). Before ADR-203 each header carried its own copy; the two agreed line for line apart from
// the name prefix and the Windows branch only Docker has (ADR-203 §4). `docker_cli_detail::` and `ctr_cli_detail::`
// keep every name as a using-declaration or a one-line wrapper.
//
// The answer decides whether a container is destroyed, so every function here fails CLOSED: an ambiguous answer is
// "alive" or `kUnknown`, never "gone", and `reap_orphans()` destroys only on `kGoneOrReplaced`.
//
// The bodies are in src/sandbox/process_identity.cpp (ADR-207, #120 S7). ADR-203 §3 kept them inline because both
// public headers needed <windows.h> or the POSIX headers for their own process spawning anyway; since ADR-207 the
// Docker header's spawning is out of line too, so this header no longer carries any OS header.

#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>


namespace agentengine::detail::process_identity {

[[nodiscard]] long current_pid();

// Liveness: is ANY process running at `pid`?
//
// POSIX: `kill(pid, 0)` sends no signal, only asks the kernel whether `pid` could be signaled at all; `ESRCH` is the
// one answer that actually means "no such process" -- every other outcome (success, or a failure this process lacks
// permission to fully diagnose, e.g. `EPERM` for a pid reused by a different, differently-owned process) is treated
// as "still alive", failing CLOSED: this function is the one gate standing between a caller and destroying a real
// container, so a wrong "dead" answer is the only wrong answer that has a real consequence.
//
// Windows: `OpenProcess()` failing with `ERROR_INVALID_PARAMETER` is the one answer Microsoft documents as meaning
// the pid names no process at all; every other failure (most commonly `ERROR_ACCESS_DENIED` for a pid this process
// cannot fully query) fails closed as "assume alive", same posture as the POSIX side. A handle that DOES open is
// further checked via `GetExitCodeProcess()` -- a pid can remain a valid, openable handle for a
// zombie-equivalent not-yet-reaped exited process on Windows too, and `STILL_ACTIVE` is the only value that
// actually means "running".
//
// Pid reuse (the kernel recycling a dead process's pid for an unrelated new one) makes liveness alone insufficient;
// `check_process_identity()` below narrows it with a per-process-instance start key.
#ifndef _WIN32
[[nodiscard]] bool process_is_alive(long pid);
#else
[[nodiscard]] bool process_is_alive(long pid);
#endif

// ADR-108 §7 pid-reuse-race fix (narrows, not just disclosed): a plain pid-liveness check alone cannot tell "the
// ORIGINAL process that created this container is still running" from "the pid was later reused by a completely
// unrelated process" -- the second case reads as "alive" under `process_is_alive()` alone, so a genuinely orphaned
// container silently stays unreapable forever once its pid happens to get recycled by something else. Fixed by
// embedding, alongside the pid, a per-process-INSTANCE "start key" that is (for all practical purposes) never the
// same across two different process instances, even ones sharing the same pid: POSIX reads `/proc/<pid>/stat`'s own
// `starttime` field (ticks since boot -- man proc(5)), Windows reads `GetProcessTimes()`'s `lpCreationTime`.
// `check_process_identity()` below then answers a strictly more specific question than plain liveness: "is the SAME
// process instance that minted this key still running," not just "is SOME process running at this pid."
#ifndef _WIN32
// Reads `/proc/<pid>/stat`'s field 22 (`starttime`). Field 2 (`comm`, the process name) is the one field in this
// file that can itself contain spaces or parentheses (man proc(5)) -- every robust parser's own convention, reused
// here, is to find the LAST `)` in the line and count fields from there, rather than naively splitting on whitespace
// from the start. After that `)`, `state` (field 3) is the first token; `starttime` (field 22) is therefore the 20th
// token counting from there.
[[nodiscard]] std::optional<std::uint64_t> read_process_start_ticks(long pid);
[[nodiscard]] std::uint64_t current_process_start_key();
// nullopt here means "could not read a start-key for this pid right now" -- deliberately NOT the same thing as "no
// such process" (`process_is_alive()` is still the one function that answers that). A transient read failure (the
// process exiting between the caller's own `process_is_alive()` check and this call) must resolve to "unknown", not
// "gone" -- `check_process_identity()` below is the one place that decides what an unreadable key means.
[[nodiscard]] std::optional<std::uint64_t> process_start_key_for(long pid);
#else
[[nodiscard]] std::uint64_t current_process_start_key();
[[nodiscard]] std::optional<std::uint64_t> process_start_key_for(long pid);
#endif

enum class ProcessMatch { kAliveSameProcess, kGoneOrReplaced, kUnknown };

// The narrowed identity check `reap_orphans()` uses in place of plain `process_is_alive()`. Layered ON TOP of that
// function, not a replacement for its own "ESRCH / ERROR_INVALID_PARAMETER is the one unambiguous 'gone' signal"
// logic: if no process at all is alive at `pid`, the creator is unambiguously gone regardless of any key comparison.
// Only when a process genuinely IS alive at that exact pid does the start-key comparison run, to tell "still the same
// process" apart from "the pid got recycled by something else" -- and if THAT read itself fails (e.g. a Windows
// `ERROR_ACCESS_DENIED` for a different-owner process, or a race where the process exits between the two checks),
// the outcome is `kUnknown`, which `reap_orphans()` treats exactly like `kAliveSameProcess` -- fail closed, never
// destroy on an ambiguous answer.
[[nodiscard]] ProcessMatch check_process_identity(long pid, std::uint64_t recorded_start_key);

// Extracts the (pid, start_key) a backend embedded in a container name -- the two decimal segments between `prefix`
// (`ae_des_` for Docker, `ae_ces_` for containerd) and the SECOND following `_` (anything after that, including a
// third `_`, is the free-form seq suffix and not parsed further). Returns nullopt -- not a best-effort partial parse
// -- for anything that isn't a plain, purely decimal run of digits in EITHER segment: a name sharing the prefix by
// coincidence (never actually produced by the backend, but not provably impossible on a shared host) must never be
// treated as one of ours just because it starts with the right characters. Fails closed (skip, don't reap) rather
// than guessing.
struct OrphanIdentity {
    long pid = 0;
    std::uint64_t start_key = 0;
};

[[nodiscard]] std::optional<OrphanIdentity> parse_orphan_identity(std::string const& name,
                                                                  std::string_view name_prefix);

}  // namespace agentengine::detail::process_identity
