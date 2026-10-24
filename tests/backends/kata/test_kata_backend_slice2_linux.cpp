// Proves KataBackend Slice 2 (kata_backend.hpp's own header comment) against a REAL Kata sandbox --
// MountSpec/NetPolicy/ResourceLimits are now inspected and enforced, not silently ignored the way
// Slice 1 left them. Same "REQUIRES a real Kata/containerd deployment" precondition as
// test_kata_backend_linux.cpp; see that file's own header comment for the exact setup. Gated behind
// the same AGENTENGINE_KATA_SANDBOX_TESTS flag.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <string>

#include "../../support/sandbox_grants.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/core/effect_context.hpp"
#include "backends/kata/kata_backend.hpp"

using namespace agentengine;
using namespace agentengine::kata;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "ok: %s\n", what);
    }
}

EffectContext make_ctx() {
    EffectContext ctx{.deadline = agentengine::Deadline::none()};
    ctx.trace_id = "kata-backend-slice2-test";
    ctx.span_id = "span-1";
    return ctx;
}

}  // namespace

int main() {
    EffectContext ctx = make_ctx();

    // ---- 1. NetPolicy: default (deny_all=true) still creates cleanly -- the property Slice 1's
    //         own test already covers implicitly; re-asserted here as this Slice's own baseline. ---
    {
        KataBackend backend;
        SandboxSpec spec;  // NetPolicy default: deny_all=true, empty allowlist
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        check(handle.has_value(), "create(): default NetPolicy (deny_all=true) still creates cleanly");
        if (handle.has_value()) agentengine::rt::block_on(backend.destroy(*handle));
    }

    // ---- 2. NetPolicy: deny_all=false fails closed, not silently granted or silently denied. ------
    // STALE-EXPECTATION FIX (found via this session's first-ever real deployment run): both cases
    // below used to assert `kata_backend.net_allowlist_unsupported`, a Slice 2-era error code SLICE 10
    // (ADR-093, kata_backend.cpp's own §Slice 10 comment) superseded with a real, capability-gated
    // NetPolicy mechanism -- `spec.net.deny_all=false`/a nonempty `allowlist` with ZERO
    // `cap::SandboxNetOut` grants (the default `SandboxSpec` both cases below construct) now fails
    // closed with `sandbox.net_not_authorized (ADR-244: authorize_spec() is unconditional)` instead (kata_backend.cpp's own real, current
    // rejection for "NetPolicy requests real network access... but the caller holds no
    // cap::SandboxNetOut grant at all"). `net_allowlist_unsupported` no longer appears anywhere in
    // kata_backend.cpp at all (grep-confirmed) -- this assertion was never actually exercised against
    // a live deployment before today, so the drift went undetected until this session's real run.
    {
        KataBackend backend;
        SandboxSpec spec;
        spec.net.deny_all = false;
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        check(!handle.has_value() &&
                  handle.error().code == "sandbox.net_not_authorized",
              "create(): NetPolicy{deny_all=false} with no cap::SandboxNetOut grant fails closed with "
              "sandbox.net_not_authorized (ADR-244: authorize_spec() is unconditional), not silently ignored");
    }
    {
        KataBackend backend;
        SandboxSpec spec;
        spec.net.allowlist.push_back("example.com:443:https");
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        check(!handle.has_value() &&
                  handle.error().code == "sandbox.net_not_authorized",
              "create(): a nonempty NetPolicy::allowlist with no cap::SandboxNetOut grant also fails "
              "closed the same way");
    }

    // ---- 3. MountSpec: a BlobRef source fails closed. -----------------------------------------------
    {
        KataBackend backend;
        SandboxSpec spec;
        MountSpec m{.source = {}, .guest_path = {}, .read_write = false,
                       .quota_bytes = cap::Limit::unbounded()};
        m.source = BlobRef{"sha256:deadbeef", "text/plain", 4, "test-store"};
        m.guest_path = "/mnt/blob";
        spec.mounts.push_back(m);
        ae_test::grant_listed_mounts(spec);
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        check(!handle.has_value() && handle.error().code == "kata_backend.blob_mount_unsupported",
              "create(): MountSpec::source as a BlobRef fails closed with "
              "kata_backend.blob_mount_unsupported");
    }

    // ---- 3a. SLICE 11 (red-team finding, kata-backend-config-pipeline-pids-disk-net-redesign-
    //          draft.md §5a'): a MountSpec host path targeting this backend's own workdir root is
    //          rejected outright, unconditionally -- EVEN WITH a covering cap::SandboxMount grant
    //          held. (The finding was found with zero grants, when authorize_spec() skipped itself;
    //          ADR-244 made that check unconditional, so the zero-grant shape is now refused earlier
    //          with sandbox.mount_not_authorized. Granting the mount is the stronger form: the
    //          workdir is forbidden whatever the caller holds.) Runs before any resource is acquired (same as the
    //          BlobRef/',' checks above), so -- unlike most of this file's cases -- this one does NOT
    //          require a live Kata/containerd deployment to prove for real. -----------------------
    {
        KataBackend backend;
        SandboxSpec spec;  // the mount below IS granted (grant_listed_mounts): the workdir check must still fire
        MountSpec m{.source = {}, .guest_path = {}, .read_write = false,
                       .quota_bytes = cap::Limit::unbounded()};
        m.source = std::string("/run/agentengine-kata");
        m.guest_path = "/mnt/workdir";
        m.read_write = true;
        spec.mounts.push_back(m);
        ae_test::grant_listed_mounts(spec);
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        check(!handle.has_value() && handle.error().code == "kata_backend.workdir_mount_forbidden",
              "create(): a MountSpec host path equal to this backend's own workdir root is rejected "
              "with kata_backend.workdir_mount_forbidden, even with a covering cap::SandboxMount grant");
    }
    {
        KataBackend backend;
        SandboxSpec spec;
        MountSpec m{.source = {}, .guest_path = {}, .read_write = false,
                       .quota_bytes = cap::Limit::unbounded()};
        m.source = std::string("/run/agentengine-kata/some-instance-id/upper.img");
        m.guest_path = "/mnt/quota-file";
        m.read_write = true;
        spec.mounts.push_back(m);
        ae_test::grant_listed_mounts(spec);
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        check(!handle.has_value() && handle.error().code == "kata_backend.workdir_mount_forbidden",
              "create(): a MountSpec host path CONTAINED WITHIN this backend's own workdir root "
              "(e.g. a live disk-quota loop-backing file) is rejected the same way, not just an "
              "exact-match on the root itself");
    }
    {
        KataBackend backend;
        SandboxSpec spec;
        MountSpec m{.source = {}, .guest_path = {}, .read_write = false,
                       .quota_bytes = cap::Limit::unbounded()};
        // A sibling directory that merely shares the workdir root as a STRING PREFIX (not a real
        // path-component ancestor) must NOT be rejected -- proves the check is component-aware, not
        // a naive substring match that would over-reject legitimate, unrelated host paths.
        m.source = std::string("/run/agentengine-kata-unrelated-sibling");
        m.guest_path = "/mnt/sibling";
        m.read_write = false;
        spec.mounts.push_back(m);
        ae_test::grant_listed_mounts(spec);
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        check(!handle.has_value() && handle.error().code != "kata_backend.workdir_mount_forbidden",
              "create(): a host path that only shares the workdir root as a string PREFIX (a sibling "
              "directory, not a real path-component descendant) is NOT rejected by the workdir "
              "exclusion check -- it is component-aware, not a naive substring match "
              "(this case still fails create() overall in this environment for the ordinary "
              "no-live-deployment reason every other case in this file does -- only the DIAGNOSTIC "
              "CODE is asserted here, not overall success)");
    }

    // ---- 4. MountSpec: a real host-path grant is a REAL bind mount into the guest VM -- read-only
    //         grant is readable but not writable (EROFS), read-write grant is writable. -------------
    {
        char const* host_dir = "/tmp/ae_kata_slice2_mount_test";
        { std::error_code ec; std::filesystem::remove_all(host_dir, ec); std::filesystem::create_directories(host_dir, ec); }
        std::ofstream(std::string(host_dir) + "/probe.txt") << "kata_mount_probe_content";

        KataBackend backend;
        SandboxSpec spec;
        MountSpec ro_mount{.source = {}, .guest_path = {}, .read_write = false,
                       .quota_bytes = cap::Limit::unbounded()};
        ro_mount.source = std::string(host_dir);
        ro_mount.guest_path = "/mnt/ro";
        ro_mount.read_write = false;
        spec.mounts.push_back(ro_mount);
        ae_test::grant_listed_mounts(spec);

        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        check(handle.has_value(), "create(): a real host-path MountSpec grant creates cleanly");
        if (handle.has_value()) {
            ExecRequest read_req;
            read_req.source = "cat /mnt/ro/probe.txt";
            auto read_out = agentengine::rt::block_on(backend.exec(*handle, read_req, ctx));
            check(read_out.has_value() && read_out->klass == exec_outcome_class::ok &&
                      read_out->stdout_text.find("kata_mount_probe_content") != std::string::npos,
                  "REAL bind mount: content written on the HOST is readable inside the GUEST VM "
                  "through the granted mount -- not a no-op, an actual virtiofs bind mount");

            ExecRequest write_req;
            write_req.source = "echo should_not_write > /mnt/ro/probe.txt 2>&1; echo EXIT:$?";
            auto write_out = agentengine::rt::block_on(backend.exec(*handle, write_req, ctx));
            check(write_out.has_value() &&
                      write_out->stdout_text.find("EXIT:0") == std::string::npos,
                  "a read_write=false mount rejects a guest write attempt (nonzero exit, not "
                  "silently succeeding)");

            agentengine::rt::block_on(backend.destroy(*handle));
        }

        // Read-write grant: the guest CAN write, and the write is visible back on the HOST --
        // proving the mount is a real, live bind, not a copy-on-create snapshot.
        KataBackend rw_backend;
        SandboxSpec rw_spec;
        MountSpec rw_mount{.source = {}, .guest_path = {}, .read_write = false,
                       .quota_bytes = cap::Limit::unbounded()};
        rw_mount.source = std::string(host_dir);
        rw_mount.guest_path = "/mnt/rw";
        rw_mount.read_write = true;
        rw_spec.mounts.push_back(rw_mount);
        ae_test::grant_listed_mounts(rw_spec);
        auto rw_handle = agentengine::rt::block_on(rw_backend.create(rw_spec, ctx));
        check(rw_handle.has_value(), "create(): a read_write=true MountSpec grant creates cleanly");
        if (rw_handle.has_value()) {
            ExecRequest write_req;
            write_req.source = "echo written_from_guest > /mnt/rw/from_guest.txt";
            auto write_out = agentengine::rt::block_on(rw_backend.exec(*rw_handle, write_req, ctx));
            check(write_out.has_value() && write_out->klass == exec_outcome_class::ok,
                  "a read_write=true mount accepts a guest write");
            agentengine::rt::block_on(rw_backend.destroy(*rw_handle));

            std::ifstream host_check(std::string(host_dir) + "/from_guest.txt");
            std::string line;
            std::getline(host_check, line);
            check(line.find("written_from_guest") != std::string::npos,
                  "REGRESSION-style proof: a file the GUEST wrote through the rw mount is visible "
                  "on the HOST filesystem afterward -- a real live bind, not an ephemeral VM-local "
                  "copy that would silently discard the write");
        }
    }

    // ---- 5. ResourceLimits::memory_bytes is REAL VM memory sizing, not a cosmetic no-op. -----------
    {
        KataBackend backend;
        SandboxSpec spec;
        spec.limits.memory_bytes = 256ull * 1024 * 1024;  // 256 MiB, well under the ~2GiB default
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        check(handle.has_value(), "create(): a small ResourceLimits::memory_bytes creates cleanly");
        if (handle.has_value()) {
            ExecRequest mem_req;
            mem_req.source = "free -m | awk '/Mem:/{print $2}'";
            auto mem_out = agentengine::rt::block_on(backend.exec(*handle, mem_req, ctx));
            check(mem_out.has_value() && mem_out->klass == exec_outcome_class::ok,
                  "exec(): reading guest total memory succeeds");
            if (mem_out.has_value()) {
                long total_mb = std::strtol(mem_out->stdout_text.c_str(), nullptr, 10);
                std::fprintf(stderr, "  guest total memory: %ld MiB (requested cap: 256 MiB)\n",
                             total_mb);
                check(total_mb > 0 && total_mb < 1024,
                      "REAL enforcement: the guest VM's own total memory is sized to the "
                      "requested 256 MiB cap (well under the ~2GiB unconfigured default), proving "
                      "--memory-limit actually resizes the VM rather than being cosmetic");
            }
            agentengine::rt::block_on(backend.destroy(*handle));
        }
    }

    // ---- 6. ResourceLimits::fds maps to a real `ctr run --rlimit-nofile` (SLICE 7,
    //         kata_backend.hpp's own header comment has the full story). This is the ONE test in this
    //         tree probing the exact open question that fix disclosed rather than assumed: does the
    //         container's initial-process RLIMIT_NOFILE also reach a LATER `ctr tasks exec`-spawned
    //         process (this backend's actual per-call workload path), or only the `sleep infinity`
    //         placeholder `create()` itself starts? `ulimit -n` inside the exec'd shell reports the
    //         open-file-descriptor limit of THAT process, directly answering the question -- not
    //         guessed at, and not claimed either way ahead of a real run.
    {
        KataBackend backend;
        SandboxSpec spec;
        spec.limits.fds = 123;  // an unusual, specific number -- if it appears verbatim in the
                                 // exec'd process's own ulimit output, that's real evidence of
                                 // propagation, not a coincidence with some other default.
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        check(handle.has_value(), "create(): a ResourceLimits::fds cap creates cleanly");
        if (handle.has_value()) {
            ExecRequest fds_req;
            fds_req.source = "ulimit -n";
            auto fds_out = agentengine::rt::block_on(backend.exec(*handle, fds_req, ctx));
            check(fds_out.has_value() && fds_out->klass == exec_outcome_class::ok,
                  "exec(): reading the exec'd process's own RLIMIT_NOFILE succeeds");
            if (fds_out.has_value()) {
                std::fprintf(stderr,
                             "  measured: exec'd process ulimit -n = '%s' (requested cap: 123) -- "
                             "this is the open question SLICE 7 disclosed rather than assumed: does "
                             "the container's own RLIMIT_NOFILE reach a LATER `ctr tasks exec` call, "
                             "not just the initial `sleep infinity` process?\n",
                             fds_out->stdout_text.c_str());
                long const observed = std::strtol(fds_out->stdout_text.c_str(), nullptr, 10);
                check(observed == 123,
                      "SLICE 7's disclosed open question, answered by this run: the exec'd "
                      "process's own RLIMIT_NOFILE matches the requested cap exactly -- real "
                      "propagation to ctr tasks exec, not just the container's initial process "
                      "(if this ever fails on a real run, it's real evidence the other way, not a "
                      "flake -- the whole point of this case is to stop assuming either answer)");
            }
            agentengine::rt::block_on(backend.destroy(*handle));
        }
    }

    // ---- 7. ResourceLimits::wall_ms overrides run_ctr()'s own fixed default timeout, per instance. -
    {
        KataBackend backend;
        SandboxSpec spec;
        spec.limits.wall_ms = 3000;  // 3s -- much tighter than the 30s fixed default
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        check(handle.has_value(), "create(): a tight ResourceLimits::wall_ms creates cleanly");
        if (handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest slow_req;
            slow_req.source = "sleep 10";
            auto slow_out = agentengine::rt::block_on(backend.exec(*handle, slow_req, ctx));
            auto t1 = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(t1 - t0).count();
            std::fprintf(stderr, "  exec('sleep 10') under wall_ms=3000 took %.2fs\n", secs);
            check(slow_out.has_value() && slow_out->klass == exec_outcome_class::timeout,
                  "exec(): classified exec_outcome_class::timeout under the per-instance wall_ms");
            check(secs < 10.0,
                  "REAL per-instance override: exec() returned well before the guest's own 10s "
                  "sleep finished and well before run_ctr()'s 30s fixed default -- wall_ms=3000 "
                  "was actually honored, not just accepted and ignored");
            agentengine::rt::block_on(backend.destroy(*handle));
        }
    }

    { std::error_code ec; std::filesystem::remove_all("/tmp/ae_kata_slice2_mount_test", ec); }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_kata_backend_slice2_linux: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_kata_backend_slice2_linux: %d FAILURE(S)\n", g_failures);
    return 1;
}
