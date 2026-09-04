// Proof for decisions/ADR-171-docker-execution-surface-isolation.md (GitHub issue #63).
//
// Before ADR-171, grepping docker_execution_surface.hpp for `--network`, `--memory`, `--pids-limit`
// or `--cpus` returned NOTHING: `DockerCliBackend::create()` emitted a bare `docker run -d --rm -w
// /workspace <image> ...`, so every container it produced got Docker's defaults -- unfiltered bridge
// egress, unbounded memory, unbounded pids, unbounded CPU, the default Linux capability set. 008 §2
// makes enforced resource limits mandatory and 008 §4 makes egress host-mediated; neither was wired.
//
// The offline half (I1-I5) proves the argv and the 008-vocabulary mapping without a daemon. The live
// half (L1-L4) proves the kernel ACTUALLY applied the limits, by reading the container's own cgroup
// values and network interfaces FROM INSIDE IT -- a flag string on a command line proves that the
// flag was passed, not that anything is contained.
//
//   I1 -- a default-constructed ContainerIsolation produces deny-all argv: --network none, real
//         memory/pids/cpu ceilings, --cap-drop ALL, --security-opt no-new-privileges.
//   I2 -- opting in changes exactly what was opted into (--network bridge), and dropping the
//         capability/privilege flags omits them rather than emitting an empty value.
//   I3 -- --cpus is formatted from an integer milli-CPU, so the argv is byte-stable and
//         locale-independent (std::to_string(double) is neither).
//   I4 -- container_isolation_from() maps 008 §2's ResourceLimits/NetPolicy, and an UNSET (zero)
//         limit maps to the conservative default, NEVER to "unlimited" -- the fail-closed direction.
//   I5 (the security-critical negative control) -- a NetPolicy carrying an allowlist is REFUSED, not
//         silently widened to `--network bridge`. Mapping a 3-entry allowlist onto unfiltered egress
//         is the single most dangerous thing that function could do, so it must fail closed.
//   L1 -- LIVE: a container created with the defaults has ONLY a loopback interface. Reads
//         /sys/class/net from inside the container.
//   L2 (positive control for L1) -- the same code path with network_enabled = true DOES get eth0, so
//         L1 is a real, falsifiable containment result and not a probe that always reports "denied".
//   L3 -- LIVE: the container's own cgroup files report exactly the requested memory/pids/cpu. The
//         kernel applied them; they were not merely passed.
//   L4 -- LIVE: DockerExecutionSurface (not just DockerCliBackend) carries its isolation through
//         reset(), so the surface SandboxRuntime actually drives is contained too.
//
// REQUIRES a running Docker daemon reachable via the `docker` CLI on PATH -- same posture as
// test_docker_orphan_reap.cpp/test_sandbox_runtime.cpp, not a special opt-in flag.
//
// MACHINE SAFETY (CLAUDE.md): every live container is `--rm`, deny-all-network by default, capped at
// or below the defaults, and destroyed explicitly. Nothing here forks a bomb or spawns unbounded
// work -- containment is proven by READING the kernel's own limit values, which is both stronger
// evidence and incapable of taking the machine with it.

#include "agentengine/sandbox/docker_execution_surface.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
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

// Index of `needle` in `argv`, or npos. Used to assert both PRESENCE and the value that follows --
// a flag whose value landed somewhere else would pass a naive "contains" check.
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

[[nodiscard]] bool contains(std::string const& haystack, std::string const& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    using agentengine::ContainerIsolation;
    using agentengine::DockerCliBackend;
    using agentengine::DockerExecutionSurface;
    using agentengine::NetPolicy;
    using agentengine::ResourceLimits;
    using agentengine::container_isolation_from;
    using agentengine::docker_isolation_argv;

    // ================= OFFLINE: the argv and the 008-vocabulary mapping =========================

    // ---- I1: the defaults are containment ------------------------------------------------------
    {
        ContainerIsolation const iso{};
        std::vector<std::string> const argv = docker_isolation_argv(iso);

        check(flag_value_is(argv, "--network", "none"),
              "I1: a DEFAULT-constructed ContainerIsolation denies the network -- containment is what "
              "a caller who passes nothing gets, so loosening is the visible act, not securing");
        check(flag_value_is(argv, "--memory", "536870912b"),
              "I1: a real memory ceiling, stated in explicit bytes rather than a bare number whose "
              "unit the daemon has to guess");
        check(flag_value_is(argv, "--pids-limit", "128"), "I1: a real pids ceiling");
        check(flag_value_is(argv, "--cpus", "1.000"), "I1: a real CPU ceiling");
        check(flag_value_is(argv, "--cap-drop", "ALL"),
              "I1: every Linux capability is dropped by default (008 §2 rule 1's direction)");
        check(flag_value_is(argv, "--security-opt", "no-new-privileges"),
              "I1: and privilege escalation inside the container is forbidden");
    }

    // ---- I2: opting in changes exactly what was opted into ---------------------------------------
    {
        ContainerIsolation iso{};
        iso.network_enabled = true;
        check(flag_value_is(docker_isolation_argv(iso), "--network", "bridge"),
              "I2: an explicit opt-in produces --network bridge");

        ContainerIsolation loose{};
        loose.drop_all_capabilities = false;
        loose.no_new_privileges      = false;
        std::vector<std::string> const argv = docker_isolation_argv(loose);
        check(index_of(argv, "--cap-drop") == std::string::npos &&
                  index_of(argv, "--security-opt") == std::string::npos,
              "I2: turning those two off OMITS the flags entirely rather than emitting an empty value "
              "-- `--cap-drop ''` would be a malformed docker invocation, not a looser one");
        check(flag_value_is(argv, "--network", "none"),
              "I2: and loosening one axis never silently loosens another");
    }

    // ---- I3: --cpus formatting is byte-stable ----------------------------------------------------
    {
        ContainerIsolation iso{};
        iso.cpu_milli = 500;
        check(flag_value_is(docker_isolation_argv(iso), "--cpus", "0.500"),
              "I3: 500 milli-CPU formats as 0.500 -- built from an integer, so no locale can turn the "
              "decimal point into a comma and no float rounding can drift the value");
        iso.cpu_milli = 2250;
        check(flag_value_is(docker_isolation_argv(iso), "--cpus", "2.250"),
              "I3: and a value above 1.0 with a non-zero fraction is correct too");
        iso.cpu_milli = 2000;
        check(flag_value_is(docker_isolation_argv(iso), "--cpus", "2.000"),
              "I3: a whole number still carries its three-digit fraction, not a bare '2.'");
    }

    // ---- I4: the 008 §2 vocabulary maps, and an UNSET limit fails closed --------------------------
    {
        ResourceLimits limits{};
        limits.memory_bytes = 64ull * 1024 * 1024;
        limits.pids         = 16;
        NetPolicy net{};  // deny_all == true by default in the type itself

        auto mapped = container_isolation_from(limits, net);
        check(mapped.has_value(), "I4: a deny-all NetPolicy with real limits maps cleanly");
        if (mapped.has_value()) {
            check(!mapped->network_enabled, "I4: deny_all becomes --network none");
            check(mapped->memory_bytes == 64ull * 1024 * 1024 && mapped->pids == 16,
                  "I4: the real limits are carried through");
        }

        // The load-bearing half: a ZERO field means "unset", and must never become "unlimited".
        ResourceLimits unset{};
        auto defaulted = container_isolation_from(unset, net);
        check(defaulted.has_value(), "I4: an all-zero ResourceLimits still maps");
        if (defaulted.has_value()) {
            check(defaulted->memory_bytes == ContainerIsolation{}.memory_bytes &&
                      defaulted->pids == ContainerIsolation{}.pids,
                  "I4: an UNSET limit falls back to the conservative default, never to 'unlimited' -- "
                  "the fail-closed direction, since 0 is exactly what an untouched SandboxSpec carries");
        }

        NetPolicy open{};
        open.deny_all = false;
        auto opened = container_isolation_from(limits, open);
        check(opened.has_value() && opened->network_enabled,
              "I4: an explicitly non-deny_all policy with NO allowlist maps to bridge -- the caller "
              "asked for unfiltered egress and gets exactly that, visibly");
    }

    // ---- I5: an allowlist is REFUSED, never silently widened -------------------------------------
    {
        NetPolicy allowlisted{};
        allowlisted.deny_all  = false;
        allowlisted.allowlist = {"api.example.com:443:https", "registry.example.com:443:https"};

        auto mapped = container_isolation_from(ResourceLimits{}, allowlisted);
        check(!mapped.has_value(),
              "I5: a NetPolicy carrying an allowlist is REFUSED -- `docker run --network bridge` "
              "grants unfiltered egress, which is not an allowlist by any reading, and silently "
              "treating one as the other is the single most dangerous thing this mapping could do "
              "(008 §4: egress is always host-mediated)");
        if (!mapped.has_value()) {
            check(mapped.error().klass == agentengine::failure_class::policy,
                  "I5: refused as a POLICY failure, not a contract/fatal one -- this is a denial, not "
                  "a malformed input or a broken daemon");
            check(mapped.error().code == "docker_execution_surface.netpolicy_allowlist_unsupported",
                  "I5: with a stable, machine-matchable code");
            check(contains(mapped.error().message, "net_egress_proxy"),
                  "I5: and the message names the real alternative, so the refusal is actionable "
                  "rather than a dead end");
        }
    }

    // ================= LIVE: what the kernel actually applied ====================================

    // ---- L1/L3: defaults, verified from INSIDE the container -------------------------------------
    {
        DockerCliBackend backend;
        auto inst = backend.create("alpine:latest");
        check(inst.has_value(), "L1 setup: a default-isolation container is created");
        if (inst.has_value()) {
            auto net = backend.exec(*inst, "ls /sys/class/net");
            check(net.has_value(), "L1 setup: the interface list is readable");
            if (net.has_value()) {
                check(contains(net->stdout_text, "lo") && !contains(net->stdout_text, "eth0"),
                      "L1: the container has ONLY loopback -- no eth0. Read from inside its own "
                      "network namespace, so this is what the kernel did, not what a flag claimed");
            }

            // The cgroup files are the kernel's own record of the applied limits.
            auto mem = backend.exec(*inst, "cat /sys/fs/cgroup/memory.max");
            check(mem.has_value() && contains(mem->stdout_text, "536870912"),
                  "L3: memory.max inside the container is exactly the requested 512 MiB -- the limit "
                  "is enforced by the kernel, not merely present on a command line");
            auto pids = backend.exec(*inst, "cat /sys/fs/cgroup/pids.max");
            check(pids.has_value() && contains(pids->stdout_text, "128"),
                  "L3: pids.max is exactly the requested 128 -- a fork bomb has a hard ceiling, "
                  "proven by reading the ceiling rather than by detonating one");
            auto cpu = backend.exec(*inst, "cat /sys/fs/cgroup/cpu.max");
            check(cpu.has_value() && contains(cpu->stdout_text, "100000 100000"),
                  "L3: cpu.max is 1.0 CPU (quota == period)");

            auto destroyed = backend.destroy(*inst);
            check(destroyed.has_value(), "L1/L3 teardown: the container is destroyed");
        }
    }

    // ---- L2: the positive control -- opting in genuinely produces a network ----------------------
    // Without this, L1 could be passing because the probe is broken rather than because the container
    // is contained. CLAUDE.md: a test that cannot fail proves nothing.
    {
        ContainerIsolation open{};
        open.network_enabled = true;
        DockerCliBackend backend;
        auto inst = backend.create("alpine:latest", open);
        check(inst.has_value(), "L2 setup: a network-enabled container is created");
        if (inst.has_value()) {
            auto net = backend.exec(*inst, "ls /sys/class/net");
            check(net.has_value() && contains(net->stdout_text, "eth0"),
                  "L2 (POSITIVE CONTROL): the SAME code path WITH network_enabled does get eth0 -- so "
                  "L1's absence of eth0 is a real containment result this probe is capable of "
                  "reporting the opposite of");
            auto destroyed = backend.destroy(*inst);
            check(destroyed.has_value(), "L2 teardown: the container is destroyed");
        }
    }

    // ---- L4: DockerExecutionSurface carries its isolation through reset() -------------------------
    // DockerCliBackend is the low-level wrapper; the type SandboxRuntime actually drives is the
    // SURFACE, and its container is created inside reset(), not by the caller. If the surface dropped
    // the isolation on the floor, every check above would still pass and nothing real would be
    // contained.
    {
        std::filesystem::path const scratch =
            std::filesystem::temp_directory_path() / "ae_docker_isolation_test";
        std::filesystem::remove_all(scratch);
        std::filesystem::create_directories(scratch);
        {
            std::ofstream out(scratch / "hello.txt", std::ios::binary | std::ios::trunc);
            out << "seed";
        }

        ContainerIsolation tight{};
        tight.memory_bytes = 268435456;  // 256 MiB -- deliberately NOT the default, so a surface that
        tight.pids         = 32;          // silently used the defaults would fail these checks.
        DockerExecutionSurface surface("alpine:latest", tight);

        auto reset = surface.reset(scratch);
        check(reset.has_value(), "L4 setup: the surface materializes the directory into a container");
        if (reset.has_value()) {
            auto net = surface.run("ls /sys/class/net");
            check(net.has_value() && contains(net->stdout_text, "lo") &&
                      !contains(net->stdout_text, "eth0"),
                  "L4: the surface's own container is network-denied too");
            auto mem = surface.run("cat /sys/fs/cgroup/memory.max");
            check(mem.has_value() && contains(mem->stdout_text, "268435456"),
                  "L4: and carries the CALLER's memory ceiling, not the type's default -- proving the "
                  "surface genuinely threads its isolation into reset()'s create() call");
            auto pids = surface.run("cat /sys/fs/cgroup/pids.max");
            check(pids.has_value() && contains(pids->stdout_text, "32"),
                  "L4: and the caller's pids ceiling");
            auto seeded = surface.run("cat /workspace/hello.txt");
            check(seeded.has_value() && contains(seeded->stdout_text, "seed"),
                  "L4: while the surface's real job -- materializing the tree -- still works under "
                  "containment; this is not a test that passes by breaking the feature");
        }
        std::filesystem::remove_all(scratch);
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
