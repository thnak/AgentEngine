// PROVE-PHASE PROBE, ad hoc (2026-08-28): compiles and runs `DockerSandboxBackend`
// (sandbox_backend_tree_refinement.hpp) end to end -- create() a real container through the REAL
// `agentengine::SandboxBackend` conformance path, reset()/exec()/drain_to()/destroy() it through the
// additive `TreeCapableSandboxBackend` refinement -- against a real, running Docker Desktop daemon
// (confirmed real and running this session, docker_backend.hpp's own header comment).
//
// Build (Windows, MSVC, this session's environment -- see memory "Windows MSVC build env setup" for
// how to construct INCLUDE/LIB without a sourced vcvars64.bat). From Git Bash specifically,
// MSYS_NO_PATHCONV=1 is required or MSYS rewrites `/EHsc`/`/I` as bogus Windows paths:
//   export MSYS_NO_PATHCONV=1
//   cd docs/planning/proofs/execution_surface
//   cl /std:c++latest /EHsc /nologo /I "<repo>/include" /I .. \
//       probe_sandbox_backend_tree_refinement.cpp /Fe:probe_sbtr.exe
//   ./probe_sbtr.exe
//
// UPDATE (same day, after three independent adversarial red-team passes on the first version --
// see sandbox_backend_tree_refinement.hpp's own top banner for the full list): this probe now also
// exercises the fixes those passes forced -- a real `cap::SandboxMount` grant is required before
// reset()/drain_to() succeed (checks 2b/5b below are POSITIVE CONTROLS proving the unauthorized path
// is actually rejected, not merely that the authorized path works), a forged/unknown SandboxHandle is
// rejected by every verb, and the container's real network is confirmed genuinely unreachable
// (`--network none`, not merely requested and silently ignored).

#include "sandbox_backend_tree_refinement.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::exit(1);                                                                       \
        }                                                                                        \
        std::printf("[ok] %s\n", #cond);                                                        \
    } while (0)

int main() {
    namespace fs = std::filesystem;
    using namespace probe;

    fs::path const host_dir = fs::temp_directory_path() / "ae_sandbox_backend_tree_refinement_probe";
    std::error_code ec;
    fs::remove_all(host_dir, ec);
    fs::create_directories(host_dir, ec);
    {
        std::ofstream f(host_dir / "seed.txt", std::ios::binary | std::ios::trunc);
        f << "seeded-from-host\n";
    }

    DockerSandboxBackend backend;
    agentengine::SandboxSpec spec;  // default: no mounts, no capabilities -- authorize_spec() is a
                                     // no-op for this shape (sandbox.hpp's own comment)

    // A ctx with NO capabilities granted at all -- used for the positive controls proving
    // reset()/drain_to() fail closed without authorization.
    agentengine::EffectContext ctx_no_caps;

    // A ctx carrying a real cap::SandboxMount grant covering host_dir, read_write=true (satisfies
    // both reset()'s read-shaped requirement and drain_to()'s write-shaped one).
    auto caps = std::make_shared<agentengine::CapabilitySet>(agentengine::CapabilitySet::grant_root(
        {agentengine::Capability{agentengine::cap::SandboxMount{
            .host_path_prefix = host_dir.generic_string(), .guest_path_prefix = "", .read_write = true}}}));
    agentengine::EffectContext ctx;
    ctx.capabilities = caps;

    // A ctx carrying a READ-ONLY grant (read_write=false) covering the SAME host_dir -- proves the
    // write-polarity branch in authorize_tree_path() is real, not just present: reset() (read-shaped)
    // must still succeed under this grant, drain_to() (write-shaped) must still be rejected. Added
    // after an independent verification pass found the original probe only ever exercised
    // read_write=true, so a regression that dropped the polarity check entirely would have passed
    // unnoticed.
    auto read_only_caps = std::make_shared<agentengine::CapabilitySet>(agentengine::CapabilitySet::grant_root(
        {agentengine::Capability{agentengine::cap::SandboxMount{
            .host_path_prefix = host_dir.generic_string(), .guest_path_prefix = "", .read_write = false}}}));
    agentengine::EffectContext ctx_read_only;
    ctx_read_only.capabilities = read_only_caps;

    // 1. create() through the REAL SandboxBackend path.
    auto handle_r = backend.create(spec, ctx);
    CHECK(handle_r.has_value());
    agentengine::SandboxHandle handle = *handle_r;
    CHECK(!handle.opaque_id.empty());

    // 2a. reset() WITHOUT a covering capability grant fails closed (positive control for the FATAL
    //     security fix -- proves the authorization gate actually rejects, not just that it exists).
    auto unauthorized_reset = backend.reset(handle, host_dir, ctx_no_caps);
    CHECK(!unauthorized_reset.has_value());
    CHECK(unauthorized_reset.error().code == "docker_sandbox_backend.reset_not_authorized");

    // 2b. reset() through the TreeCapableSandboxBackend refinement, WITH a real grant: seeds the
    //     container from host_dir.
    auto reset_r = backend.reset(handle, host_dir, ctx);
    CHECK(reset_r.has_value());

    // 2c. A forged/unknown SandboxHandle is rejected by reset() (positive control for the
    //     handle-hijack fix) -- even with a fully valid capability grant.
    agentengine::SandboxHandle forged_handle{"not-a-real-container-id"};
    auto forged_reset = backend.reset(forged_handle, host_dir, ctx);
    CHECK(!forged_reset.has_value());
    CHECK(forged_reset.error().code == "docker_sandbox_backend.unknown_handle");

    // 2d. Read/write POLARITY is real, not just "some grant exists": a read_write=false grant
    //     covering host_dir still authorizes reset() (read-shaped) but NOT drain_to() (write-shaped).
    //     Added after an independent verification pass found the original probe only ever exercised
    //     read_write=true, which would not have caught a regression dropping this check entirely.
    auto reset_read_only_r = backend.reset(handle, host_dir, ctx_read_only);
    CHECK(reset_read_only_r.has_value());
    auto drain_read_only_r = backend.drain_to(handle, host_dir / "should-not-write", ctx_read_only);
    CHECK(!drain_read_only_r.has_value());
    CHECK(drain_read_only_r.error().code == "docker_sandbox_backend.drain_not_authorized");

    // 2e. A '..'-laden path is rejected outright even when its literal string starts with a granted
    //     prefix (positive control for the FATAL path-traversal finding an independent verification
    //     pass found in the first fix round's own new authorize_tree_path() code: a lexical '..' can
    //     defeat a plain string-prefix check with no filesystem access at all, letting a narrow grant
    //     be used to reach a host path completely outside it).
    fs::path const traversal_path = fs::path(host_dir.generic_string() + "/../escaped_outside_grant");
    auto traversal_reset = backend.reset(handle, traversal_path, ctx);
    CHECK(!traversal_reset.has_value());
    CHECK(traversal_reset.error().code == "docker_sandbox_backend.reset_not_authorized");
    auto traversal_drain = backend.drain_to(handle, traversal_path, ctx);
    CHECK(!traversal_drain.has_value());
    CHECK(traversal_drain.error().code == "docker_sandbox_backend.drain_not_authorized");

    // 3. exec() through the REAL SandboxBackend path: reads what reset() seeded.
    agentengine::ExecRequest req;
    req.language = "shell";
    req.source = "cat /workspace/seed.txt";
    auto exec_r = backend.exec(handle, req, ctx);
    CHECK(exec_r.has_value());
    CHECK(exec_r->klass == agentengine::exec_outcome_class::ok);
    CHECK(exec_r->stdout_text == "seeded-from-host\n");

    // 3b. A forged handle is rejected by exec() too, never silently running against someone else's
    //     (or a nonexistent) container.
    agentengine::ExecRequest const forged_req{.source = "echo should-not-run"};
    auto forged_exec = backend.exec(forged_handle, forged_req, ctx);
    CHECK(!forged_exec.has_value());
    CHECK(forged_exec.error().code == "docker_sandbox_backend.unknown_handle");

    // 3c. The real container has NO network access -- confirmed live, not merely requested.
    //     `create()`'s only accepted policy (NetPolicy{deny_all=true, allowlist={}}, spec's own
    //     default) now maps to a real `--network none` container.
    agentengine::ExecRequest net_req;
    net_req.source = "ping -c1 -W1 8.8.8.8 2>&1; wget -T2 -q -O- http://example.com 2>&1; echo DONE";
    auto net_r = backend.exec(handle, net_req, ctx);
    CHECK(net_r.has_value());
    bool const network_blocked =
        net_r->stdout_text.find("Network unreachable") != std::string::npos ||
        net_r->stdout_text.find("bad address") != std::string::npos ||
        net_r->stdout_text.find("Network is unreachable") != std::string::npos;
    CHECK(network_blocked);

    // 4. A second exec() writes a new file INSIDE the container.
    agentengine::ExecRequest write_req;
    write_req.source = "echo written-in-container > /workspace/out.txt";
    auto write_r = backend.exec(handle, write_req, ctx);
    CHECK(write_r.has_value());

    // 5a. drain_to() WITHOUT a covering capability grant fails closed.
    fs::path const drain_dir = host_dir / "drained";
    auto unauthorized_drain = backend.drain_to(handle, drain_dir, ctx_no_caps);
    CHECK(!unauthorized_drain.has_value());
    CHECK(unauthorized_drain.error().code == "docker_sandbox_backend.drain_not_authorized");

    // 5b. drain_to() through the refinement, WITH a real grant, pulls it back onto real host disk.
    auto drain_r = backend.drain_to(handle, drain_dir, ctx);
    CHECK(drain_r.has_value());
    std::ifstream out(drain_dir / "out.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(out)), std::istreambuf_iterator<char>());
    CHECK(content == "written-in-container\n");

    // 6. destroy() through the REAL SandboxBackend path.
    backend.destroy(handle);

    // 6b. A second destroy() on the now-untracked handle is a harmless no-op (destroy() returns
    //     void by the real, locked SandboxBackend contract -- this just confirms no crash/UB).
    backend.destroy(handle);

    // 7. A SandboxSpec carrying a real mount is rejected outright (fail-closed, not silently ignored).
    agentengine::SandboxSpec mount_spec;
    mount_spec.mounts.push_back(agentengine::MountSpec{
        .source = std::string("C:/somewhere"), .guest_path = "/mnt", .read_write = false});
    auto rejected_r = backend.create(mount_spec, ctx);
    CHECK(!rejected_r.has_value());
    CHECK(rejected_r.error().code == "docker_sandbox_backend.mounts_unsupported");

    // 8. A SandboxSpec requesting anything other than full network deny is rejected outright (the
    //    fix for the "authorize_spec() passes but nothing is enforced" finding).
    agentengine::SandboxSpec net_spec;
    net_spec.net.deny_all = false;
    auto net_rejected_r = backend.create(net_spec, ctx);
    CHECK(!net_rejected_r.has_value());
    CHECK(net_rejected_r.error().code == "docker_sandbox_backend.net_policy_unsupported");

    fs::remove_all(host_dir, ec);

    std::printf("=== all checks passed: DockerSandboxBackend satisfies both the real "
                "agentengine::SandboxBackend concept AND TreeCapableSandboxBackend, with real "
                "capability authorization, handle-ownership checks, and network denial enforced ===\n");
    return 0;
}
