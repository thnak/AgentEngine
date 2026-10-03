// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §9 D5 -- the OS half of
// `pal::usable_cpus()` (include/agentengine/pal/cpu.hpp, whose comment states the rules). Compiled here so
// <windows.h> never reaches the headers that include the runtime.

#include "agentengine/pal/cpu.hpp"

#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sched.h>

#include <fstream>
#include <sstream>
#endif

namespace agentengine::pal {

namespace {

unsigned fallback_count() noexcept {
    unsigned const n = std::thread::hardware_concurrency();
    return n == 0 ? 1U : n;
}

#if defined(_WIN32)

unsigned affinity_count() noexcept {
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask  = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) == 0 || process_mask == 0) {
        return fallback_count();
    }
    if (process_mask == system_mask) {
        // Unrestricted: on Windows 11 the process's threads may run in every processor group.
        DWORD const all = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (all != 0) return static_cast<unsigned>(all);
    }
    return static_cast<unsigned>(std::popcount(static_cast<std::uint64_t>(process_mask)));
}

std::optional<unsigned> quota_count() noexcept { return std::nullopt; }

#else

unsigned affinity_count() noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return fallback_count();
    int const n = CPU_COUNT(&set);
    return n > 0 ? static_cast<unsigned>(n) : fallback_count();
}

std::optional<std::string> read_file(std::string const& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<unsigned> tighter(std::optional<unsigned> a, std::optional<unsigned> b) noexcept {
    if (!a) return b;
    if (!b) return a;
    return *a < *b ? a : b;
}

// v2: the tightest `cpu.max` from the process's cgroup up to the mount root.
std::optional<unsigned> cgroup2_quota(std::string path) {
    std::string const root = "/sys/fs/cgroup";
    std::optional<unsigned> best;
    for (;;) {
        std::string const dir = path.empty() || path == "/" ? root : root + path;
        if (auto c = read_file(dir + "/cpu.max")) best = tighter(best, cpu_detail::parse_cgroup2_cpu_max(*c));
        if (path.empty() || path == "/") break;
        std::size_t const slash = path.find_last_of('/');
        path                    = slash == 0 || slash == std::string::npos ? std::string("/") : path.substr(0, slash);
    }
    return best;
}

// v1: the cpu controller's quota at the process's path, or at the mount root (a container whose cgroup
// namespace makes its own cgroup the root).
std::optional<unsigned> cgroup1_quota(std::string const& path) {
    for (char const* mount : {"/sys/fs/cgroup/cpu", "/sys/fs/cgroup/cpu,cpuacct"}) {
        for (std::string const& sub : {path, std::string()}) {
            std::string const dir = std::string(mount) + (sub == "/" ? std::string() : sub);
            auto const q = read_file(dir + "/cpu.cfs_quota_us");
            auto const p = read_file(dir + "/cpu.cfs_period_us");
            if (q && p) return cpu_detail::parse_cgroup1_quota(*q, *p);
        }
    }
    return std::nullopt;
}

std::optional<unsigned> quota_count() noexcept {
    try {
        auto const self = read_file("/proc/self/cgroup");
        if (!self) return std::nullopt;
        cpu_detail::CgroupPaths const paths = cpu_detail::parse_proc_self_cgroup(*self);
        std::optional<unsigned>       q;
        if (paths.v2) q = cgroup2_quota(*paths.v2);
        if (!q && paths.v1_cpu) q = cgroup1_quota(*paths.v1_cpu);
        return q;
    } catch (...) {  // an allocation failure reading /proc: no quota is known
        return std::nullopt;
    }
}

#endif

}  // namespace

unsigned usable_cpus() noexcept { return cpu_detail::combine(affinity_count(), quota_count()); }

}  // namespace agentengine::pal
