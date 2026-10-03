#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §9 D5 -- `pal::usable_cpus()`: how many
// CPUs this process may actually run on, which is what the default lane count is derived from
// (`max(2, usable_cpus() / 2)`, rt/runtime.hpp). `std::thread::hardware_concurrency()` alone reports the
// host's cores: inside a `--cpus=1` container or under `taskset -c 0` it overstates the CPUs by up to the
// whole machine (round 2, G-d).
//
//   - Windows: the process affinity mask (GetProcessAffinityMask). A process whose affinity is the whole
//     system mask may run in every processor group (Windows 11), so it counts every active processor across
//     groups; a narrowed mask counts the mask's bits (one group, by construction).
//   - Linux: the calling thread's affinity (sched_getaffinity, which is what `taskset` sets for the whole
//     process), capped by the cgroup CPU quota: cgroup v2 `cpu.max` along the process's cgroup path up to the
//     root (the tightest wins), else cgroup v1 `cpu.cfs_quota_us` / `cpu.cfs_period_us`. A quota of q CPUs
//     counts as ceil(q).
//   - Never less than 1.
//
// The OS calls are compiled once in src/pal/usable_cpus.cpp so <windows.h> stays out of every includer. The
// pure parsing helpers are here so they can be tested on every OS against fixed inputs.
//
// Not covered (documented, ADR §14 step 3): Windows Job-object CPU rate caps (JOBOBJECT_CPU_RATE_CONTROL),
// cgroup cpusets narrower than the affinity mask (the kernel already applies them to sched_getaffinity).

#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace agentengine::pal {

// ae-naming-lint: allow usable_cpus — ADR-237 §9 D5: new runtime vocabulary, 027 §4 row added when the ADR is Judged
[[nodiscard]] unsigned usable_cpus() noexcept;

namespace cpu_detail {

// CPUs granted by a quota of `quota` per `period` (both > 0), rounded up, at least 1.
[[nodiscard]] inline unsigned cpus_from_quota(double quota, double period) noexcept {
    if (!(quota > 0.0) || !(period > 0.0)) return 0;
    double const cpus = std::ceil(quota / period);
    return cpus < 1.0 ? 1U : static_cast<unsigned>(cpus);
}

[[nodiscard]] inline std::optional<long long> parse_ll(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    long long v = 0;
    auto const [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
}

// cgroup v2 `cpu.max`: "max 100000" (no limit) or "<quota> <period>". The CPU count, or nullopt for no limit
// or an unreadable value.
[[nodiscard]] inline std::optional<unsigned> parse_cgroup2_cpu_max(std::string_view content) noexcept {
    std::size_t const sp = content.find(' ');
    if (sp == std::string_view::npos) return std::nullopt;
    std::string_view const q = content.substr(0, sp);
    if (q == "max") return std::nullopt;
    auto const quota  = parse_ll(q);
    auto const period = parse_ll(content.substr(sp + 1));
    if (!quota || !period) return std::nullopt;
    unsigned const n = cpus_from_quota(static_cast<double>(*quota), static_cast<double>(*period));
    if (n == 0) return std::nullopt;
    return n;
}

// cgroup v1 `cpu.cfs_quota_us` (-1: no limit) and `cpu.cfs_period_us`.
[[nodiscard]] inline std::optional<unsigned> parse_cgroup1_quota(std::string_view quota_us,
                                                                std::string_view period_us) noexcept {
    auto const quota  = parse_ll(quota_us);
    auto const period = parse_ll(period_us);
    if (!quota || !period || *quota <= 0 || *period <= 0) return std::nullopt;
    return cpus_from_quota(static_cast<double>(*quota), static_cast<double>(*period));
}

// `/proc/self/cgroup`: the v2 path (line "0::<path>") and the v1 cpu controller's path (a line whose
// controller list contains "cpu", e.g. "4:cpu,cpuacct:<path>").
struct CgroupPaths {
    std::optional<std::string> v2;
    std::optional<std::string> v1_cpu;
};

[[nodiscard]] inline CgroupPaths parse_proc_self_cgroup(std::string_view content) {
    CgroupPaths out;
    while (!content.empty()) {
        std::size_t const nl   = content.find('\n');
        std::string_view  line = content.substr(0, nl);
        content = nl == std::string_view::npos ? std::string_view{} : content.substr(nl + 1);
        std::size_t const c1 = line.find(':');
        if (c1 == std::string_view::npos) continue;
        std::size_t const c2 = line.find(':', c1 + 1);
        if (c2 == std::string_view::npos) continue;
        std::string_view const id          = line.substr(0, c1);
        std::string_view const controllers = line.substr(c1 + 1, c2 - c1 - 1);
        std::string_view const path        = line.substr(c2 + 1);
        if (id == "0" && controllers.empty()) {
            out.v2 = std::string(path);
            continue;
        }
        std::string_view rest = controllers;
        while (!rest.empty()) {
            std::size_t const comma = rest.find(',');
            std::string_view const name = rest.substr(0, comma);
            if (name == "cpu") {
                out.v1_cpu = std::string(path);
                break;
            }
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        }
    }
    return out;
}

// min(affinity, quota), at least 1. `quota` absent: no quota.
[[nodiscard]] inline unsigned combine(unsigned affinity, std::optional<unsigned> quota) noexcept {
    unsigned n = affinity;
    if (quota && *quota < n) n = *quota;
    return n == 0 ? 1U : n;
}

}  // namespace cpu_detail

}  // namespace agentengine::pal
