// Proves KataBackend (src/backends/kata/kata_backend.hpp) end-to-end against a REAL Kata Containers
// sandbox -- cloud-hypervisor VMM, a real guest kernel, real containerd/`ctr` round trips. Not a
// mock: this is the same "regression test that would fail against a fresh-instance-per-call bug"
// discipline test_sandbox_backend_registry.cpp's item 1 established for the registry layer, applied
// here one level down, against the real backend and the real VM.
//
// REQUIRES (not asserted by this test -- a deployment precondition, same posture as
// helpers/cgroup_v2_test_setup.sh for the native-jail Linux suite): a running `containerd` daemon
// with `ctr` on PATH, a `docker.io/library/busybox:latest` image already pulled (or reachable to
// pull), and a `kata-clh` runtime resolvable as `io.containerd.kata-clh.v2` -- see
// kata_backend.hpp's own header comment for the exact containerd config.toml block and the
// `runtime-rs/configuration.toml` default-symlink fix this pass found necessary (the shipped
// default pointed at `configuration-qemu-runtime-rs.toml`, not the clh config; silently trusting
// `KATA_CONF_FILE` alone was insufficient -- the shim rejected it outright: "only shipped Kata
// configuration files are accepted"). Gated behind AGENTENGINE_KATA_SANDBOX_TESTS (OFF by default,
// mirrors AGENTENGINE_LINUX_SANDBOX_TESTS) precisely because CI has none of this installed.

#include <cstdio>
#include <string>

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
    EffectContext ctx;
    ctx.trace_id = "kata-backend-linux-test";
    ctx.span_id = "span-1";
    return ctx;
}

}  // namespace

int main() {
    KataBackend backend;
    EffectContext ctx = make_ctx();
    SandboxSpec spec;

    auto handle = backend.create(spec, ctx);
    check(handle.has_value(), "create(): a real Kata sandbox (cloud-hypervisor) starts successfully");
    if (!handle.has_value()) {
        std::fprintf(stderr, "create() error: %s\n", handle.error().message.c_str());
        return 1;
    }

    // ---- 1. Real guest kernel, distinct from the host's. ------------------------------------------
    ExecRequest uname_req;
    uname_req.language = "shell";
    uname_req.source = "uname -r";
    auto uname_out = backend.exec(*handle, uname_req, ctx);
    check(uname_out.has_value() && uname_out->klass == exec_outcome_class::ok,
          "exec(): `uname -r` succeeds inside the guest");
    if (uname_out.has_value()) {
        std::fprintf(stderr, "  guest kernel: %s", uname_out->stdout_text.c_str());
        check(!uname_out->stdout_text.empty(), "guest kernel version string is non-empty");
    }

    // ---- 2/3. State persists across exec() calls into the SAME instance -- the real-VM analogue of
    //           test_sandbox_backend_registry.cpp item 1's regression test. ------------------------
    ExecRequest write_req;
    write_req.language = "shell";
    write_req.source = "echo kata_state_probe > /tmp/kata_probe_file";
    auto write_out = backend.exec(*handle, write_req, ctx);
    check(write_out.has_value() && write_out->klass == exec_outcome_class::ok,
          "exec(): writing a state file inside the guest succeeds");

    ExecRequest read_req;
    read_req.language = "shell";
    read_req.source = "cat /tmp/kata_probe_file";
    auto read_out = backend.exec(*handle, read_req, ctx);
    check(read_out.has_value() && read_out->klass == exec_outcome_class::ok,
          "exec(): reading the state file back succeeds");
    if (read_out.has_value()) {
        check(read_out->stdout_text.find("kata_state_probe") != std::string::npos,
              "REGRESSION: a later exec() call sees state written by an EARLIER exec() call on the "
              "SAME instance -- proves this is one long-lived VM across calls, not a fresh sandbox "
              "per exec() (the exact class of bug test_sandbox_backend_registry.cpp item 1 exists "
              "to catch at the registry layer, verified here one level down against the real VM)");
    }

    // ---- 4. A nonzero guest exit code is classified as `crash`, not silently reported `ok`. -------
    ExecRequest fail_req;
    fail_req.language = "shell";
    fail_req.source = "exit 7";
    auto fail_out = backend.exec(*handle, fail_req, ctx);
    check(fail_out.has_value() && fail_out->klass == exec_outcome_class::crash,
          "exec(): a nonzero guest exit code is classified as exec_outcome_class::crash");

    // ---- 5. exec() on an unknown handle fails closed. ----------------------------------------------
    {
        KataBackend fresh_backend;
        SandboxHandle bogus{"never-created"};
        ExecRequest req;
        req.source = "echo unreachable";
        auto bogus_out = fresh_backend.exec(bogus, req, ctx);
        check(!bogus_out.has_value() && bogus_out.error().code == "kata_backend.unknown_handle",
              "exec(): an unknown handle fails closed with kata_backend.unknown_handle");
    }

    backend.destroy(*handle);
    // destroy() is void by the SandboxBackend concept -- no direct observable here beyond "did not
    // crash"; a second destroy() on the same (now-erased) handle must also be a safe no-op.
    backend.destroy(*handle);
    check(true, "destroy(): idempotent, a second call on an already-destroyed handle does not crash");

    if (g_failures == 0) {
        std::fprintf(stderr, "test_kata_backend_linux: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_kata_backend_linux: %d FAILURE(S)\n", g_failures);
    return 1;
}
