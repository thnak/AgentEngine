// Proves decisions/ADR-244-required-choices-step-1b.md §2.4 (G-C5 a, b, d, f) against the real jailed worker:
// the Python runner never grants itself paths, serves one session only, and never lets a deployment path
// become writable.
//
//   A  -- `initialize` with a host grant that lacks an `extra_sys_path` entry fails `sandbox.mount_not_authorized`; the
//         same runner config with the full `required_mounts()` grant initializes (positive control).
//   B  -- the runner binds the session it is created for: creating it with no nonce is refused
//         (`codeact.no_session`); a run with no nonce, another session's nonce, or a later epoch of the bound
//         session (a clear, restore or fork) is refused (`codeact.no_session`,
//         `codeact.runner_bound_to_other_session`, `codeact.runner_session_moved`); the bound nonce runs
//         (positive control).
//   F  -- a read-write mount over a deployment path is refused (`sandbox.deployment_path_writable`) even
//         though the host grant covers it, through the runner and through a plain `NativeJailBackend::create`.
//
// G-C5(d) (read-only grants written once) and the cross-process deployment checks are proven without Python, by
// test_native_jail_ro_mount_dedup_windows and test_native_jail_rw_capability_windows.
//
// Windows only (the jailed worker is), and needs the vendored CPython (AE_PYTHON_HOME).

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "../support/python_runner_grants.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"

using namespace agentengine;
using agentengine::native_jail::MediatedPythonConfig;
using agentengine::native_jail::MediatedPythonRunner;
using agentengine::native_jail::NativeJailBackend;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s at %s:%d\n", (label), __FILE__, __LINE__);              \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::printf("  ok: %s\n", (label));                                                    \
        }                                                                                           \
    } while (0)

[[nodiscard]] MediatedPythonConfig base_config(std::filesystem::path const& scratch) {
    MediatedPythonConfig cfg;
    cfg.python_home = AE_PYTHON_HOME;
    cfg.mount_roots["work"] = scratch.wstring();
    return cfg;
}

[[nodiscard]] CapabilitySet grant_without(MediatedPythonConfig const& cfg, std::string const& host_path) {
    std::vector<Capability> grants;
    for (auto const& m : MediatedPythonRunner::required_mounts(cfg)) {
        if (m.host_path_prefix != host_path) grants.push_back(m);
    }
    return CapabilitySet::grant_root(std::move(grants));
}

[[nodiscard]] bool ran_ok(MediatedPythonRunner& runner, ExecState& state, EffectContext& ctx) {
    auto out = rt::block_on(runner.run(ExecRequest{"python", "print(1)"}, state, ctx));
    return out.has_value() && out->klass == exec_outcome_class::ok;
}

[[nodiscard]] std::string refusal(MediatedPythonRunner& runner, ExecState& state, EffectContext& ctx) {
    auto out = rt::block_on(runner.run(ExecRequest{"python", "print(1)"}, state, ctx));
    return out.has_value() ? std::string("<ran>") : out.error().code;
}

}  // namespace

int main() {
    std::filesystem::path const scratch = std::filesystem::temp_directory_path() / "ae_runner_choices_work";
    std::error_code ec;
    std::filesystem::create_directories(scratch, ec);
    NativeJailBackend backend;

    // ---- A: the host grant is the whole authority ----------------------------------------------------
    {
        // The omitted path must not sit under another deployment path: the worker's own directory is a
        // deployment path, and in this repository's build it is the build root, so its grant covers
        // `python_home` too (a disclosed residual, ADR-244 §6). An extra_sys_path entry outside the tree is not.
        std::filesystem::path const outside = std::filesystem::temp_directory_path() / "ae_runner_choices_sys";
        std::filesystem::create_directories(outside, ec);
        MediatedPythonConfig cfg = base_config(scratch);
        cfg.extra_sys_path.push_back(outside.string());
        CapabilitySet const partial = grant_without(cfg, outside.string());
        MediatedPythonRunner runner(std::move(cfg), backend);
        EffectContext ctx{.deadline = agentengine::Deadline::none()};
        ctx.session_nonce = ae_test::kTestSessionNonce;
        ctx.principal = ae_test::kTestPrincipal;
        auto init = runner.initialize(partial, ctx);
        if (init.has_value()) std::printf("  note: A initialize succeeded\n");
        else std::printf("  note: A refused with %s: %s\n", init.error().code.c_str(), init.error().message.c_str());
        AE_CHECK(!init.has_value() && init.error().code == "sandbox.mount_not_authorized",
                 "A: a host grant lacking an extra_sys_path entry fails sandbox.mount_not_authorized");
    }
    {
        MediatedPythonRunner runner(base_config(scratch), backend);
        AE_CHECK(ae_test::initialize_runner(runner).has_value(),
                 "A (positive control): the full required_mounts() grant initializes");
    }

    // ---- B: one runner per session -------------------------------------------------------------------
    {
        MediatedPythonRunner unbound(base_config(scratch), backend);
        auto refused = ae_test::initialize_runner(unbound, SessionNonce{});
        AE_CHECK(!refused.has_value() && refused.error().code == "codeact.no_session",
                 "B: creating a runner with no session nonce is refused");

        MediatedPythonRunner runner(base_config(scratch), backend);
        AE_CHECK(ae_test::initialize_runner(runner, SessionNonce{0xB0001, 1}).has_value(),
                 "B setup: the runner is created for session 0xB0001");
        ExecState state{};

        EffectContext none{.deadline = agentengine::Deadline::none()};
        AE_CHECK(refusal(runner, state, none) == "codeact.no_session", "B: a run carrying no session nonce is refused");

        EffectContext first{.deadline = agentengine::Deadline::none()};
        first.session_nonce = SessionNonce{0xB0001, 1};
        AE_CHECK(ran_ok(runner, state, first), "B (positive control): the bound session's run runs");

        EffectContext other{.deadline = agentengine::Deadline::none()};
        other.session_nonce = SessionNonce{0xB0002, 1};
        AE_CHECK(refusal(runner, state, other) == "codeact.runner_bound_to_other_session",
                 "B: a second session's nonce is refused");

        EffectContext moved{.deadline = agentengine::Deadline::none()};
        moved.session_nonce = SessionNonce{0xB0001, 2};
        AE_CHECK(refusal(runner, state, moved) == "codeact.runner_session_moved",
                 "B: a later epoch of the bound session (clear/restore/fork) is refused");

        AE_CHECK(ran_ok(runner, state, first), "B (positive control): the bound incarnation keeps running");
    }

    // ---- F: deployment paths are never writable ------------------------------------------------------
    {
        MediatedPythonConfig cfg = base_config(scratch);
        cfg.mount_roots["home"] = std::filesystem::path(AE_PYTHON_HOME).wstring();  // python_home, read-write
        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = ae_test::initialize_runner(runner);  // the grant covers it: required_mounts() lists it rw
        AE_CHECK(!init.has_value() && init.error().code == "sandbox.deployment_path_writable",
                 "F: a read-write mount_root over python_home is refused although the host grant covers it");
    }
    {
        std::string const home = std::filesystem::path(AE_PYTHON_HOME).string();
        SandboxSpec spec;
        spec.mounts.push_back(MountSpec{.source = home, .guest_path = "/home", .read_write = true, .quota_bytes = cap::Limit::unbounded()});
        spec.capabilities = CapabilitySet::grant_root({Capability{cap::SandboxMount{home, "/home", true}}});
        EffectContext ctx{.deadline = agentengine::Deadline::none()};
        auto h = rt::block_on(backend.create(spec, ctx));
        AE_CHECK(!h.has_value() && h.error().code == "sandbox.deployment_path_writable",
                 "F: a plain sandbox's read-write mount over a registered deployment path is refused");
        if (h.has_value()) rt::block_on(backend.destroy(*h));
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
