// PROVE-PHASE PROBE, ad hoc (2026-08-28): connects `DockerSandboxBackend`
// (sandbox_backend_tree_refinement.hpp) to the REAL, production `agentengine::SandboxBackendRegistry`
// (sandbox/sandbox_backend_registry.hpp) -- the next step the project-owner conversation asked for
// after the standalone refinement probe compiled and ran clean. Runs against this session's real,
// running Docker Desktop daemon, not a mock.
//
// THE REAL FINDING THIS PROBE EXISTS TO SURFACE (predicted before writing it, from reading
// `RegisteredSandboxBackend`'s own fields, then confirmed live below): `register_backend()` closes
// over exactly THREE verbs -- `create`/`exec`/`destroy` -- into `RegisteredSandboxBackend`'s
// `std::function` members. Nothing about `TreeCapableSandboxBackend`'s `reset()`/`drain_to()`
// refinement survives type erasure through the registry. A caller holding only what
// `resolve_named()`/`resolve_strict()` hand back (`RegisteredSandboxBackend const*`) can create/exec/
// destroy a real container but has NO way to reach `reset()`/`drain_to()` at all -- it would need a
// SEPARATE, independent reference to the concrete `shared_ptr<DockerSandboxBackend>` instance kept
// outside the registry. This is exactly the same shape ADR-100 F4 already names for
// `NativeJailBackend::create_python_worker()`/`exec_session()` ("additive, non-concept methods...
// RegisteredSandboxBackend's type erasure structurally cannot carry [them], by design, not by
// oversight") -- this probe is the first place that finding is demonstrated for THIS refinement
// specifically, with working code, not just reasoned about.
//
// UPDATE (same day, after three independent adversarial red-team passes on the first version -- see
// sandbox_backend_tree_refinement.hpp's own top banner for the full list): the SHOULD-FIX finding
// that this probe's own `strength=0`/`resolve_strict()` demonstration contradicted its own comment
// (a sole strict-eligible candidate wins trivially regardless of declared strength) is fixed
// STRUCTURALLY here, not by adjusting a number: registration now goes through
// `register_hardware_isolation_backend()` (`named_only`), the same entry point `KataBackend` uses in
// `default_sandbox_registry.cpp` for an identical reason -- this backend can no longer win `Strict`
// resolution regardless of what else is or isn't registered alongside it. Step 4 below is rewritten
// to prove the NEW, correct behavior: `resolve_strict()` over a registry containing ONLY a
// `named_only` entry finds no strict-eligible candidate at all and fails closed.
//
// Also updated: reset()/drain_to() now take `EffectContext&` (the FATAL security fix -- see the
// refinement header's own banner), so step 7 constructs a real `cap::SandboxMount` grant before
// calling them, and a forged-handle check is added to prove `entry->exec()` (the registry's own
// type-erased surface) rejects a handle this backend never created, not just that the concrete type
// does.
//
// Build: same env/flags as probe_sandbox_backend_tree_refinement.cpp (see that file's own banner).
//   cl /std:c++latest /EHsc /nologo /I "<repo>/include" /I .. \
//       probe_docker_sandbox_backend_registry.cpp /Fe:probe_registry.exe

#include "sandbox_backend_tree_refinement.hpp"

#include "agentengine/sandbox/sandbox_backend_registry.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {
template <class T>
concept has_reset_method = requires(T& e, agentengine::SandboxHandle const& h,
                                     std::filesystem::path const& p) { e.reset(h, p); };
}  // namespace

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
    using namespace agentengine;

    auto docker_backend = std::make_shared<probe::DockerSandboxBackend>();
    // Kept as a SEPARATE, independent reference to the concrete type -- see this file's own banner.
    // A real host that only kept the registry's own `RegisteredSandboxBackend const*` would have no
    // way to obtain this at all once `resolve_named()`/`resolve_strict()` are the only paths in.

    SandboxBackendRegistry registry;

    // 1. register_hardware_isolation_backend() through the REAL registry, real template-constrained
    //    on SandboxBackend<B> (compiles at all only because DockerSandboxBackend satisfies the
    //    concept -- proven already by sandbox_backend_tree_refinement.hpp's own static_assert,
    //    exercised here as a real template instantiation, not just a static_assert in isolation).
    //    `named_only` -- see this file's own updated top banner for why plain register_backend()'s
    //    default `eligible` mode is the wrong choice for this conformer today.
    auto reg_r = registry.register_hardware_isolation_backend("docker", docker_backend);
    CHECK(reg_r.has_value());

    // 2. A duplicate name is rejected, matching every other real registry precedent
    //    (default_sandbox_registry.cpp's own duplicate-name path).
    auto dup_r = registry.register_hardware_isolation_backend("docker", docker_backend);
    CHECK(!dup_r.has_value());
    CHECK(dup_r.error().code == "sandbox_backend_registry.duplicate_name");

    // 3. resolve_named() finds it -- named_only entries exist specifically to be reachable here.
    HostSandboxSelection const selection("docker");
    auto resolved_r = registry.resolve_named(selection);
    CHECK(resolved_r.has_value());
    RegisteredSandboxBackend const* entry = *resolved_r;
    CHECK(entry->name == "docker");

    // 4. resolve_strict() does NOT find it -- a named_only entry never competes for Strict
    //    resolution, so a registry containing ONLY this one entry has no strict-eligible candidate
    //    at all and fails closed, exactly 008 §3's "no fallback -> startup fails" rule. This is the
    //    corrected version of what this step originally (wrongly) demonstrated -- see this file's
    //    own top banner.
    auto strict_r = registry.resolve_strict(current_platform());
    CHECK(!strict_r.has_value());
    CHECK(strict_r.error().code == "sandbox_backend_registry.no_strict_candidate");

    // 5. The type-erased create/exec/destroy on `entry` (NOT on `docker_backend` directly) actually
    //    drive a real container end to end -- this is what a caller holding ONLY a
    //    `RegisteredSandboxBackend const*` (the registry's real public surface) can do.
    SandboxSpec spec;
    EffectContext ctx;  // no capabilities -- create()/exec() need none (mounts/net already rejected
                          // structurally by DockerSandboxBackend::create() itself when requested)
    auto handle_r = entry->create(spec, ctx);
    CHECK(handle_r.has_value());
    SandboxHandle handle = *handle_r;

    ExecRequest req;
    req.source = "echo -n hello-through-registry";
    auto exec_r = entry->exec(handle, req, ctx);
    CHECK(exec_r.has_value());
    CHECK(exec_r->stdout_text == "hello-through-registry");

    // 5b. A forged handle is rejected through the REGISTRY's own type-erased surface too, not just
    //     when calling the concrete type directly -- the handle-ownership check lives inside the
    //     conformer itself, so it applies no matter which surface reaches it.
    SandboxHandle forged_handle{"not-a-real-container-id"};
    ExecRequest const forged_req{.source = "echo should-not-run"};
    auto forged_exec = entry->exec(forged_handle, forged_req, ctx);
    CHECK(!forged_exec.has_value());
    CHECK(forged_exec.error().code == "docker_sandbox_backend.unknown_handle");

    // 6. THE FINDING, demonstrated positively: `entry` (RegisteredSandboxBackend const*) has no
    //    `.reset()`/`.drain_to()` member at all -- this is a compile-time fact, not something a
    //    runtime check could probe, so it is asserted here as a concept-gated `requires` clause
    //    rather than executed. Tree-materialization is reachable ONLY through the separately-held
    //    `docker_backend` shared_ptr, never through anything the registry itself hands back.
    //    (A bare, non-template `requires(...) { ... }` used directly as a `static_assert` operand
    //    hard-errors on MSVC instead of evaluating false for an ill-formed member access --
    //    apparently not properly treated as an unevaluated/SFINAE context outside a template. Routing
    //    it through an actual `concept` -- which IS a template -- sidesteps that MSVC-specific
    //    quirk.)
    static_assert(!has_reset_method<RegisteredSandboxBackend>,
                  "RegisteredSandboxBackend must NOT expose reset() -- if this ever starts compiling, "
                  "the type erasure changed and this probe's own central finding is stale");

    // 7. Tree materialization still works, but only via the side-channel reference kept in step 1 --
    //    proving the WORKAROUND this finding forces on any real caller, not just the gap itself. Also
    //    needs a real cap::SandboxMount grant now (the FATAL security fix, see the refinement
    //    header's own banner) -- a bare EffectContext with no capabilities would be rejected exactly
    //    like probe_sandbox_backend_tree_refinement.cpp's own "unauthorized" checks 2a/5a prove.
    fs::path const host_dir = fs::temp_directory_path() / "ae_registry_probe_seed";
    std::error_code ec;
    fs::remove_all(host_dir, ec);
    fs::create_directories(host_dir, ec);
    {
        std::ofstream f(host_dir / "seed.txt", std::ios::binary | std::ios::trunc);
        f << "seeded-via-side-channel\n";
    }
    auto caps = std::make_shared<CapabilitySet>(CapabilitySet::grant_root(
        {Capability{cap::SandboxMount{
            .host_path_prefix = host_dir.generic_string(), .guest_path_prefix = "", .read_write = true}}}));
    EffectContext tree_ctx;
    tree_ctx.capabilities = caps;
    auto reset_r = docker_backend->reset(handle, host_dir, tree_ctx);  // NOT entry->reset(...) --
                                                                          // doesn't exist
    CHECK(reset_r.has_value());
    ExecRequest read_req;
    read_req.source = "cat /workspace/seed.txt";
    auto read_r = entry->exec(handle, read_req, ctx);  // back through the registry-typed entry
    CHECK(read_r.has_value());
    CHECK(read_r->stdout_text == "seeded-via-side-channel\n");

    entry->destroy(handle);
    fs::remove_all(host_dir, ec);

    std::printf("=== all checks passed: DockerSandboxBackend registers into the REAL "
                "SandboxBackendRegistry (named_only) for create/exec/destroy -- never wins Strict --"
                " reset()/drain_to() are structurally unreachable through anything the registry hands"
                " back, and every verb enforces real capability/handle-ownership checks regardless of"
                " which surface reaches it ===\n");
    return 0;
}
