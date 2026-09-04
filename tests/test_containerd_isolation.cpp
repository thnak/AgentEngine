// Proof for decisions/ADR-172-containerd-execution-surface-isolation.md (GitHub issue #66) -- the
// containerd sibling of ADR-171, which closed the identical gap for Docker.
//
// Before ADR-172, `ContainerdCliBackend::create()` emitted
// `ctr run -d --mount ... <image> <id> sleep infinity` with no memory, CPU or capability flags at
// all. What makes this NOT a copy of the Docker fix, and why every difference below is real rather
// than cosmetic -- each established by running `ctr` against a live containerd and reading the result
// back, never from its --help text:
//
//   * `ctr run`'s NETWORK default is already isolated (a fresh netns with only `lo`). This is the one
//     axis containerd was never in Docker's pre-ADR-171 state on. So the default emits no network
//     flag at all, and the opt-in is `--net-host` -- HOST networking, strictly BROADER than Docker's
//     NAT bridge, which is why it is the most dangerous field on the struct.
//   * `--cap-drop ALL` DOES NOT EXIST here: `ctr` rejects it with "capabilities must be specified
//     with 'CAP_' prefix". Dropping everything means enumerating containerd's own 14-entry OCI
//     default set, and C1/L3 are what make that list's completeness a checked claim rather than a
//     hopeful one.
//   * There is NO pids limit on this path at all. `containerd_isolation_from()` therefore REFUSES a
//     non-zero `ResourceLimits::pids` (C6) instead of accepting it and emitting nothing -- a caller
//     must never believe a fork bomb has a ceiling it does not have.
//
//   C1 -- the default argv: no network flag, a real memory ceiling, a real CPU ceiling, and all 14
//         capability drops.
//   C2 -- opting into host networking emits --net-host; disabling the capability drop omits all 14
//         rather than emitting a malformed empty value.
//   C3 -- --cpus is formatted from integer milli-CPU, so the argv is byte-stable and
//         locale-independent (std::to_string(double) is neither).
//   C4 -- container_isolation_from()'s containerd analogue maps 008 §2's vocabulary, and an UNSET
//         (zero) limit maps to the conservative default, NEVER to "unlimited".
//   C5 -- a NetPolicy allowlist is REFUSED, not silently widened (008 §4).
//   C6 -- a pids limit is REFUSED. The honest difference from the Docker surface, which can map it.
//   L1 -- LIVE: a default container has ONLY loopback, read from /sys/class/net inside it.
//   L2 (positive control for L1) -- host_network = true DOES produce eth0, so L1 is a falsifiable
//         containment result and not a probe that always reports "denied".
//   L3 -- LIVE: CapBnd inside the container is exactly 0000000000000000 -- proving the 14-name drop
//         list is COMPLETE for this containerd/kernel pair, which no offline test can establish.
//   L4 -- LIVE: the container's own cgroup on the HOST reports exactly the requested memory/CPU. Read
//         host-side, not from inside: unlike Docker, `ctr run` does not mount cgroupfs into the
//         container, so /sys/fs/cgroup/<namespace>/<id> is where the kernel's record actually is.
//         Stated rather than glossed -- it is a different vantage than ADR-171's L3, not the same one.
//   L5 -- LIVE: ContainerdExecutionSurface (the type SandboxRuntime drives, whose container is
//         created inside reset()) threads the CALLER's isolation through -- proven by the caller's
//         host_network opt-in genuinely producing eth0 -- while the surface's real job, the bind
//         mount, still works under containment.
//
// REQUIRES Linux, a running containerd reachable via `ctr` on PATH, and enough privilege to talk to
// its socket -- same posture as test_containerd_execution_surface.cpp.
//
// MACHINE SAFETY (CLAUDE.md): every container is `sleep infinity`, capped at or below the defaults,
// and destroyed explicitly. Containment is proven by READING the kernel's own limit records rather
// than by trying to breach them.

#include "agentengine/sandbox/containerd_execution_surface.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

[[nodiscard]] std::size_t index_of(std::vector<std::string> const& argv, std::string const& needle) {
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (argv[i] == needle) return i;
    }
    return std::string::npos;
}

[[nodiscard]] bool flag_value_is(std::vector<std::string> const& argv, std::string const& flag,
                                   std::string const& value) {
    std::size_t const i = index_of(argv, flag);
    return i != std::string::npos && i + 1 < argv.size() && argv[i + 1] == value;
}

// `flag_value_is` above finds only the FIRST occurrence, which is wrong for `--cap-drop`: it repeats
// 14 times, so asking it about any drop but `CAP_CHOWN` reports a false negative. (A real bug in the
// first draft of this file, caught by the test failing, not by review.)
[[nodiscard]] bool any_flag_value_is(std::vector<std::string> const& argv, std::string const& flag,
                                       std::string const& value) {
    for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == flag && argv[i + 1] == value) return true;
    }
    return false;
}

[[nodiscard]] std::size_t count_of(std::vector<std::string> const& argv, std::string const& needle) {
    std::size_t n = 0;
    for (auto const& s : argv) {
        if (s == needle) ++n;
    }
    return n;
}

[[nodiscard]] bool contains(std::string const& haystack, std::string const& needle) {
    return haystack.find(needle) != std::string::npos;
}

// The container's own cgroup on the HOST. `ctr run` places a container at
// /sys/fs/cgroup/<namespace>/<id> under the default cgroupfs driver; "default" is the namespace this
// backend uses (it passes no -n/--namespace). Returns an empty string when the file is unreadable,
// so a caller's assertion fails loudly rather than passing on an empty match.
[[nodiscard]] std::string host_cgroup_value(std::string const& container_id, char const* file) {
    std::filesystem::path const p =
        std::filesystem::path("/sys/fs/cgroup/default") / container_id / file;
    std::ifstream in(p);
    if (!in) return {};
    std::string line;
    std::getline(in, line);
    return line;
}

// A unique-per-run id, so a leftover container from an earlier failed run can never make this one
// look like it passed (or fail it spuriously).
[[nodiscard]] std::string unique_id(char const* tag) {
    return std::string("ae_ciso_") + tag + "_" + std::to_string(static_cast<long>(::getpid()));
}

}  // namespace

int main() {
    using agentengine::ContainerdCliBackend;
    using agentengine::ContainerdExecutionSurface;
    using agentengine::ContainerdIsolation;
    using agentengine::NetPolicy;
    using agentengine::ResourceLimits;
    using agentengine::containerd_isolation_argv;
    using agentengine::containerd_isolation_from;
    using agentengine::kDefaultDroppedCapabilities;

    // ================= OFFLINE: the argv and the 008-vocabulary mapping =========================

    // ---- C1: the defaults ------------------------------------------------------------------------
    {
        std::vector<std::string> const argv = containerd_isolation_argv(ContainerdIsolation{});

        check(index_of(argv, "--net-host") == std::string::npos,
              "C1: the default emits NO network flag -- containerd already gives the container its "
              "own netns with only loopback, so the guarantee comes from containerd and this type's "
              "job is to keep it, not to establish it");
        check(flag_value_is(argv, "--memory-limit", "536870912"),
              "C1: a real memory ceiling (bare bytes -- `ctr` takes no unit suffix, unlike docker)");
        check(flag_value_is(argv, "--cpus", "1.000"), "C1: a real CPU ceiling");
        check(count_of(argv, "--cap-drop") == kDefaultDroppedCapabilities.size(),
              "C1: every one of containerd's 14 OCI-default capabilities is dropped -- there is no "
              "`--cap-drop ALL` on this CLI, so 'drop everything' must be enumerated");
        check(any_flag_value_is(argv, "--cap-drop", "CAP_SYS_CHROOT") &&
                  any_flag_value_is(argv, "--cap-drop", "CAP_NET_RAW") &&
                  any_flag_value_is(argv, "--cap-drop", "CAP_CHOWN"),
              "C1: and each drop is a real CAP_-prefixed name `ctr` accepts");
        bool every_drop_is_prefixed = true;
        for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
            if (argv[i] == "--cap-drop" && argv[i + 1].rfind("CAP_", 0) != 0) {
                every_drop_is_prefixed = false;
            }
        }
        check(every_drop_is_prefixed,
              "C1: EVERY drop carries the CAP_ prefix -- `ctr` rejects the whole `create()` on a "
              "single unprefixed name, so one bad entry in the table would disable containment "
              "entirely rather than degrade it");
    }

    // ---- C2: opting in changes exactly what was opted into ---------------------------------------
    {
        ContainerdIsolation host_net{};
        host_net.host_network = true;
        check(index_of(containerd_isolation_argv(host_net), "--net-host") != std::string::npos,
              "C2: the network opt-in emits --net-host -- which is the HOST's own namespace, broader "
              "than Docker's bridge, not narrower");

        ContainerdIsolation keep_caps{};
        keep_caps.drop_default_capabilities = false;
        std::vector<std::string> const argv = containerd_isolation_argv(keep_caps);
        check(count_of(argv, "--cap-drop") == 0,
              "C2: turning the capability drop off OMITS all 14 flags rather than emitting an empty "
              "value -- `--cap-drop ''` is a rejected invocation, not a looser one");
        check(flag_value_is(argv, "--memory-limit", "536870912") &&
                  index_of(argv, "--net-host") == std::string::npos,
              "C2: and loosening one axis never silently loosens another");
    }

    // ---- C3: --cpus formatting is byte-stable ----------------------------------------------------
    {
        ContainerdIsolation iso{};
        iso.cpu_milli = 500;
        check(flag_value_is(containerd_isolation_argv(iso), "--cpus", "0.500"),
              "C3: 500 milli-CPU formats as 0.500 -- integer-derived, so no locale turns the decimal "
              "point into a comma and no float rounding drifts the value");
        iso.cpu_milli = 2250;
        check(flag_value_is(containerd_isolation_argv(iso), "--cpus", "2.250"),
              "C3: and a value above 1.0 with a non-zero fraction is correct too");
    }

    // ---- C4: the 008 §2 vocabulary maps, and an UNSET limit fails closed --------------------------
    {
        ResourceLimits limits{};
        limits.memory_bytes = 268435456;
        NetPolicy net{};  // deny_all == true by default in the type itself

        auto mapped = containerd_isolation_from(limits, net);
        check(mapped.has_value(), "C4: a deny-all NetPolicy with a real memory limit maps cleanly");
        if (mapped.has_value()) {
            check(!mapped->host_network, "C4: deny_all means no host networking");
            check(mapped->memory_bytes == 268435456, "C4: the real limit is carried through");
        }

        auto defaulted = containerd_isolation_from(ResourceLimits{}, net);
        check(defaulted.has_value() &&
                  defaulted->memory_bytes == ContainerdIsolation{}.memory_bytes,
              "C4: an UNSET (zero) limit falls back to the conservative default, never to "
              "'unlimited' -- zero is exactly what an untouched SandboxSpec carries");

        NetPolicy open{};
        open.deny_all = false;
        auto opened = containerd_isolation_from(limits, open);
        check(opened.has_value() && opened->host_network,
              "C4: an explicitly non-deny_all policy with NO allowlist maps to host networking -- the "
              "caller asked to leave the netns and gets exactly that, visibly");
    }

    // ---- C5: an allowlist is REFUSED -------------------------------------------------------------
    {
        NetPolicy allowlisted{};
        allowlisted.deny_all  = false;
        allowlisted.allowlist = {"api.example.com:443:https"};
        auto mapped = containerd_isolation_from(ResourceLimits{}, allowlisted);
        check(!mapped.has_value(),
              "C5: a NetPolicy allowlist is REFUSED -- and the stakes are higher here than on the "
              "Docker side: the only opt-in `ctr run` offers is --net-host, so silently honoring an "
              "allowlist would hand the container the HOST's network namespace (008 §4)");
        if (!mapped.has_value()) {
            check(mapped.error().klass == agentengine::failure_class::policy &&
                      mapped.error().code ==
                          "containerd_execution_surface.netpolicy_allowlist_unsupported",
                  "C5: refused as a policy denial with a stable, machine-matchable code");
            check(contains(mapped.error().message, "net_egress_proxy"),
                  "C5: and the message names the real alternative");
        }
    }

    // ---- C6: a pids limit is REFUSED -- the honest containerd-specific difference -----------------
    {
        ResourceLimits limits{};
        limits.pids = 64;
        auto mapped = containerd_isolation_from(limits, NetPolicy{});
        check(!mapped.has_value(),
              "C6: a non-zero pids limit is REFUSED. `ctr run` has no --pids-limit and this backend "
              "never uses --config/OCI-spec mode, so accepting one and emitting nothing would leave a "
              "caller believing a fork bomb has a ceiling it does not have -- strictly worse than "
              "saying no");
        if (!mapped.has_value()) {
            check(mapped.error().code == "containerd_execution_surface.pids_limit_unsupported",
                  "C6: with its own stable code, distinct from the allowlist refusal");
            check(contains(mapped.error().message, "Docker surface"),
                  "C6: and the message points at a surface that CAN enforce it, so the refusal is "
                  "actionable rather than a dead end");
        }
        // Positive control: the same limits WITHOUT pids must still map, or C6 would be passing
        // because the mapping refuses everything.
        ResourceLimits no_pids{};
        no_pids.memory_bytes = 268435456;
        check(containerd_isolation_from(no_pids, NetPolicy{}).has_value(),
              "C6 (positive control): the same call without a pids limit maps fine -- the refusal is "
              "specific, not a blanket failure");
    }

    // ================= LIVE: what the kernel actually applied ====================================

    // ---- L1/L3/L4: the defaults, verified from inside AND from the host cgroup --------------------
    {
        std::filesystem::path const work = "/tmp/ae_containerd_isolation_default";
        std::filesystem::remove_all(work);
        std::filesystem::create_directories(work);

        std::string const id = unique_id("def");
        ContainerdCliBackend backend;
        auto inst = backend.create(id, work);
        check(inst.has_value(), "L1 setup: a default-isolation container is created");
        if (inst.has_value()) {
            auto net = backend.exec(*inst, "ls /sys/class/net");
            check(net.has_value() && contains(net->stdout_text, "lo") &&
                      !contains(net->stdout_text, "eth0"),
                  "L1: the container has ONLY loopback -- read from inside its own network namespace");

            auto caps = backend.exec(*inst, "grep CapBnd /proc/self/status");
            check(caps.has_value() && contains(caps->stdout_text, "0000000000000000"),
                  "L3: CapBnd inside the container is exactly zero -- proving the 14-name drop list "
                  "is COMPLETE for this containerd/kernel pair, which no offline test can establish");

            check(host_cgroup_value(id, "memory.max") == "536870912",
                  "L4: the container's cgroup ON THE HOST reports exactly the requested 512 MiB -- "
                  "the kernel applied it. Read host-side because `ctr run`, unlike docker, does not "
                  "mount cgroupfs into the container");
            check(host_cgroup_value(id, "cpu.max") == "100000 100000",
                  "L4: and cpu.max is 1.0 CPU (quota == period)");
            // The honest negative: this path cannot bound pids, and the cgroup says so out loud.
            check(host_cgroup_value(id, "pids.max") == "max",
                  "L4 (the disclosed gap, asserted rather than described): pids.max really is "
                  "unbounded here -- which is exactly why C6 refuses a pids limit instead of "
                  "pretending to honor one");

            auto destroyed = backend.destroy(*inst);
            check(destroyed.has_value(), "L1/L3/L4 teardown: the container is destroyed");
        }
        std::filesystem::remove_all(work);
    }

    // ---- L2: the positive control ----------------------------------------------------------------
    {
        std::filesystem::path const work = "/tmp/ae_containerd_isolation_hostnet";
        std::filesystem::remove_all(work);
        std::filesystem::create_directories(work);

        ContainerdIsolation host_net{};
        host_net.host_network = true;
        std::string const id = unique_id("net");
        ContainerdCliBackend backend;
        auto inst = backend.create(id, work, "docker.io/library/alpine:latest", host_net);
        check(inst.has_value(), "L2 setup: a host-networked container is created");
        if (inst.has_value()) {
            auto net = backend.exec(*inst, "ls /sys/class/net");
            check(net.has_value() && contains(net->stdout_text, "eth0"),
                  "L2 (POSITIVE CONTROL): the SAME code path WITH host_network does see the host's "
                  "interfaces -- so L1's absence of eth0 is a real containment result this probe is "
                  "capable of reporting the opposite of");
            auto destroyed = backend.destroy(*inst);
            check(destroyed.has_value(), "L2 teardown: the container is destroyed");
        }
        std::filesystem::remove_all(work);
    }

    // ---- L5: the surface threads the caller's isolation through reset() ---------------------------
    {
        std::filesystem::path const work = "/tmp/ae_containerd_isolation_surface";
        std::filesystem::remove_all(work);
        std::filesystem::create_directories(work);
        {
            std::ofstream out(work / "hello.txt", std::ios::binary | std::ios::trunc);
            out << "seed";
        }

        // Default surface: contained, and its real job still works.
        {
            ContainerdExecutionSurface surface;
            auto reset = surface.reset(work);
            check(reset.has_value(), "L5 setup: the default surface bind-mounts the directory");
            if (reset.has_value()) {
                auto net = surface.run("ls /sys/class/net");
                check(net.has_value() && !contains(net->stdout_text, "eth0"),
                      "L5: the surface's own container is network-isolated too");
                auto caps = surface.run("grep CapBnd /proc/self/status");
                check(caps.has_value() && contains(caps->stdout_text, "0000000000000000"),
                      "L5: and runs with a zeroed capability bounding set");
                auto seeded = surface.run("cat hello.txt");
                check(seeded.has_value() && contains(seeded->stdout_text, "seed"),
                      "L5: while the surface's real job -- the bind mount -- still works under "
                      "containment; this is not a test that passes by breaking the feature");
            }
        }

        // A caller-supplied isolation must actually reach reset()'s own create() call. Proven with
        // host_network specifically because it is observable from INSIDE the container, needing no
        // access to the id the surface generates privately.
        {
            ContainerdIsolation host_net{};
            host_net.host_network = true;
            ContainerdExecutionSurface surface("docker.io/library/alpine:latest", host_net);
            auto reset = surface.reset(work);
            check(reset.has_value(), "L5 setup: a host-networked surface resets");
            if (reset.has_value()) {
                auto net = surface.run("ls /sys/class/net");
                check(net.has_value() && contains(net->stdout_text, "eth0"),
                      "L5: the CALLER's isolation genuinely reaches reset()'s own create() -- a "
                      "surface that dropped it on the floor would show only loopback here and every "
                      "other check in this file would still have passed");
            }
        }
        std::filesystem::remove_all(work);
    }

    std::printf("\n=== %d checks, %d failed ===\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
