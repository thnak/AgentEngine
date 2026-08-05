// Implements cgroup_limits.hpp. See that header for the spec citations this satisfies.

#include "backends/native_jail/cgroup_limits.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <utility>

namespace agentengine::native_jail {

namespace {

std::unexpected<ae::error> errno_error(char const* what, failure_class klass, char const* code) {
    int e = errno;
    return std::unexpected(
        ae::error{klass, std::string(what) + " failed: " + std::strerror(e), code});
}

// Cgroup v2 control files generally require the whole value written in a single write(2) call
// (partial writes across multiple syscalls are rejected by several controllers) -- POSIX
// open/write/close, not buffered iostreams, is deliberate here.
result<void> write_control_file(std::string const& path, std::string const& value) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno_error(("open(" + path + ")").c_str(), failure_class::fatal,
                            "cgroup.open_control_file_failed");
    }
    ssize_t written = ::write(fd, value.data(), value.size());
    int write_errno = errno;
    ::close(fd);
    if (written < 0 || static_cast<std::size_t>(written) != value.size()) {
        errno = write_errno;
        return errno_error(("write(" + path + ")").c_str(), failure_class::fatal,
                            "cgroup.write_control_file_failed");
    }
    return {};
}

result<std::string> read_control_file(std::string const& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno_error(("open(" + path + ")").c_str(), failure_class::fatal,
                            "cgroup.open_control_file_failed");
    }
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            int e = errno;
            ::close(fd);
            errno = e;
            return errno_error(("read(" + path + ")").c_str(), failure_class::fatal,
                                "cgroup.read_control_file_failed");
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

// Parses a cgroup "flat keyed" file (cpu.stat, memory.events -- one "key value\n" pair per line)
// for `key`. Returns 0 if the key is absent rather than an error -- controllers this class doesn't
// depend on being enabled should not turn a query into a hard failure.
std::uint64_t parse_keyed_field(std::string const& content, std::string const& key) {
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream line_stream(line);
        std::string field_key;
        std::uint64_t field_value = 0;
        if (line_stream >> field_key >> field_value && field_key == key) return field_value;
    }
    return 0;
}

}  // namespace

CgroupLimits::~CgroupLimits() { destroy_now(); }

CgroupLimits::CgroupLimits(CgroupLimits&& other) noexcept : path_(std::move(other.path_)) {
    other.path_.clear();
}

CgroupLimits& CgroupLimits::operator=(CgroupLimits&& other) noexcept {
    if (this != &other) {
        destroy_now();
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

void CgroupLimits::destroy() { destroy_now(); }

void CgroupLimits::destroy_now() {
    if (!path_.empty()) {
        ::rmdir(path_.c_str());  // best-effort; a non-empty cgroup fails harmlessly (caller's bug)
        path_.clear();
    }
}

result<void> CgroupLimits::create(std::string const& delegated_root, std::string const& name,
                                   ResourceLimits const& limits) {
    if (!path_.empty()) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "CgroupLimits::create called twice on the same instance",
                                          "cgroup.already_created"});
    }

    std::string dir = delegated_root + "/" + name;
    if (::mkdir(dir.c_str(), 0755) != 0) {
        return errno_error(("mkdir(" + dir + ")").c_str(), failure_class::fatal,
                            "cgroup.mkdir_failed");
    }
    path_ = dir;

    if (limits.memory_bytes > 0) {
        auto set = write_control_file(path_ + "/memory.max", std::to_string(limits.memory_bytes));
        if (!set.has_value()) { destroy_now(); return set; }
        // memory.max alone caps RESIDENT usage only -- without also capping swap, the kernel can
        // (and, measured directly in this harness, sometimes does) spill overflow into swap
        // instead of invoking the OOM killer, letting a guest that should be denied quietly
        // succeed anyway, just slower. ResourceLimits::memory_bytes is meant as a hard ceiling
        // (matching the Windows side's Job Object memory limit, which has no swap-overflow
        // concept at all) -- memory.swap.max=0 closes that escape hatch. Best-effort: some
        // kernels/cgroup configurations don't expose memory.swap.max (e.g. swap accounting
        // disabled at boot); a missing file is not treated as fatal, since memory.max alone still
        // provides real, if incomplete, containment on those hosts.
        auto swap_set = write_control_file(path_ + "/memory.swap.max", "0");
        (void)swap_set;  // best-effort; see comment above
    }
    if (limits.pids > 0) {
        auto set = write_control_file(path_ + "/pids.max", std::to_string(limits.pids));
        if (!set.has_value()) { destroy_now(); return set; }
    }
    return {};
}

result<void> CgroupLimits::add_process(int pid) const {
    if (path_.empty()) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "CgroupLimits::add_process called before create()",
                                          "cgroup.not_created"});
    }
    return write_control_file(path_ + "/cgroup.procs", std::to_string(pid));
}

result<CgroupUsage> CgroupLimits::query_usage() const {
    if (path_.empty()) {
        return std::unexpected(ae::error{failure_class::contract,
                                          "CgroupLimits::query_usage called before create()",
                                          "cgroup.not_created"});
    }
    CgroupUsage usage;

    auto memory_current = read_control_file(path_ + "/memory.current");
    if (memory_current.has_value()) {
        try {
            usage.peak_memory_bytes = std::stoull(*memory_current);
        } catch (...) {
            usage.peak_memory_bytes = 0;
        }
    }
    // memory.peak (kernel >= 5.19) is the true high-water mark; fall back to memory.current
    // (already captured above) on older kernels where the file doesn't exist.
    auto memory_peak = read_control_file(path_ + "/memory.peak");
    if (memory_peak.has_value()) {
        try {
            usage.peak_memory_bytes = std::stoull(*memory_peak);
        } catch (...) {
            // keep the memory.current fallback already captured
        }
    }

    auto cpu_stat = read_control_file(path_ + "/cpu.stat");
    if (cpu_stat.has_value()) usage.cpu_usage_usec = parse_keyed_field(*cpu_stat, "usage_usec");

    auto memory_events = read_control_file(path_ + "/memory.events");
    if (memory_events.has_value()) usage.oom_kill_count = parse_keyed_field(*memory_events, "oom_kill");

    return usage;
}

}  // namespace agentengine::native_jail
