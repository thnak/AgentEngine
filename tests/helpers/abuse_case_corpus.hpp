#pragma once
// M2 Phase C task C4 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md): the
// shared hostile-probe corpus 008-Sandbox-and-Isolation.md §9 G1 requires -- "one hostile test
// corpus runs against every available backend on every platform in the current target set (021 §2
// -- Windows now, Linux next) and produces the same outcome classification for every case."
//
// This header is that "one hostile test corpus": ONE array, included verbatim by both
// tests/test_native_jail_parity_windows.cpp and tests/test_native_jail_parity_linux.cpp, so parity
// is a structural fact (the same table, compiled into two different binaries against two different
// SandboxBackend implementations) rather than an assertion that happens to agree because two
// independently-written test files were kept in sync by hand.
//
// Deliberately narrower than C3's own corpus (tests/test_native_jail_abuse_corpus_{windows,linux}.cpp):
// G1 is specifically about `exec_outcome_class` parity for a single exec() call, which fork-bomb
// (succeeded-count) and fs-escape (ESCAPE_OK/DENIED string, Windows-only per the tracked Linux
// containment gap, GitHub issue #5) don't naturally reduce to -- those stay in C3's own corpus,
// each with its own G2 positive control. This corpus covers the cases that DO reduce to a single
// exec_outcome_class: well-behaved, ordinary failure, infinite loop, OOM, unbounded output.
//
// wall_ms is intentionally NOT part of this table: ADR-004 §10.5 and this project's own C2/C3
// measurements found real, large, platform-specific timing asymmetries (Windows Job Object memory
// limit ~14-22ms vs. Linux cgroups v2 OOM-kill ~2-8s for the identical workload) -- baking one
// number into a "shared" table would either be needlessly tight for Windows or flaky on Linux. Each
// platform's consuming test supplies wall_ms per case; what stays identical (the actual G1 claim)
// is the probe command and the expected exec_outcome_class.

#include <cstdint>

#include "agentengine/sandbox/sandbox.hpp"

namespace agentengine::native_jail::test {

struct AbuseCorpusCase {
    char const* name;
    char const* probe_args;  // argv passed to the hostile_child(_posix) binary
    std::uint64_t memory_bytes;
    std::uint32_t pids;
    std::uint64_t output_bytes;
    exec_outcome_class expected;
};

inline constexpr AbuseCorpusCase kAbuseCorpus[] = {
    {"well_behaved", "sleep 50", 64ull * 1024 * 1024, 4, 1024 * 1024, exec_outcome_class::ok},
    {"ordinary_failure", "fail 7", 64ull * 1024 * 1024, 4, 1024 * 1024, exec_outcome_class::crash},
    {"infinite_loop", "spin", 64ull * 1024 * 1024, 4, 1024 * 1024, exec_outcome_class::timeout},
    // 512 MB request against a 32 MB cap -- the same shape C2's own tests used on both platforms.
    {"oom", "alloc 512", 32ull * 1024 * 1024, 4, 1024 * 1024, exec_outcome_class::oom},
    {"unbounded_output", "flood", 64ull * 1024 * 1024, 4, 4096, exec_outcome_class::timeout},
};

inline constexpr std::size_t kAbuseCorpusSize = sizeof(kAbuseCorpus) / sizeof(kAbuseCorpus[0]);

[[nodiscard]] inline char const* to_string(exec_outcome_class klass) {
    switch (klass) {
        case exec_outcome_class::ok: return "ok";
        case exec_outcome_class::timeout: return "timeout";
        case exec_outcome_class::oom: return "oom";
        case exec_outcome_class::crash: return "crash";
        case exec_outcome_class::policy_violation: return "policy_violation";
        case exec_outcome_class::escape_attempt: return "escape_attempt";
    }
    return "unknown";
}

}  // namespace agentengine::native_jail::test
