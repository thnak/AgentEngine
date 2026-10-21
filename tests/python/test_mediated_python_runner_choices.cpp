// Proves decisions/ADR-244-required-choices-step-1b.md §2.4 (G-C5 a, b, d, f) against the real jailed worker:
// the Python runner never grants itself paths, serves one session only, and never lets a deployment path
// become writable.
//
//   A  -- `initialize` with a host grant that lacks an `extra_sys_path` entry fails `sandbox.mount_not_authorized`; the
//         same runner config with the full `required_mounts()` grant initializes (positive control).
//   B  -- the runner binds the session nonce of its first run: a run without a nonce is refused
//         (`codeact.no_session`), a second session's nonce is refused (`codeact.runner_bound_to_other_session`),
//         a later epoch of the bound session (a clear, restore or fork) is refused
//         (`codeact.runner_session_moved`), and the bound nonce keeps running (positive control).
//   D  -- read-only deployment grants are deduplicated: the DACL entry count on `python_home` is unchanged
//         across repeated worker creates. Positive control: the counter does see growth -- three read-write
//         grants of one fresh directory (read-write grants are not deduplicated) add entries.
//   F  -- a read-write mount over a deployment path is refused (`sandbox.deployment_path_writable`) even
//         though the host grant covers it, through the runner and through a plain `NativeJailBackend::create`.
//
// Windows only (the jailed worker is), and needs the vendored CPython (AE_PYTHON_HOME).

#include <windows.h>

#include <aclapi.h>

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

// Total DACL entry count on `path` (same technique as test_native_jail_ro_mount_dedup_windows.cpp).
DWORD dacl_entry_count(std::wstring const& path) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS || dacl == nullptr) return 0;
    ACL_SIZE_INFORMATION size_info{};
    GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation);
    if (sd != nullptr) LocalFree(sd);
    return size_info.AceCount;
}

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
        EffectContext ctx;
        ctx.session_nonce = ae_test::kTestSessionNonce;
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
        MediatedPythonRunner runner(base_config(scratch), backend);
        AE_CHECK(ae_test::initialize_runner(runner).has_value(), "B setup: runner initializes");
        ExecState state{};

        EffectContext none;
        AE_CHECK(refusal(runner, state, none) == "codeact.no_session", "B: a run carrying no session nonce is refused");

        EffectContext first;
        first.session_nonce = SessionNonce{0xB0001, 1};
        AE_CHECK(ran_ok(runner, state, first), "B (positive control): the first session's run binds and runs");

        EffectContext other;
        other.session_nonce = SessionNonce{0xB0002, 1};
        AE_CHECK(refusal(runner, state, other) == "codeact.runner_bound_to_other_session",
                 "B: a second session's nonce is refused");

        EffectContext moved;
        moved.session_nonce = SessionNonce{0xB0001, 2};
        AE_CHECK(refusal(runner, state, moved) == "codeact.runner_session_moved",
                 "B: a later epoch of the bound session (clear/restore/fork) is refused");

        AE_CHECK(ran_ok(runner, state, first), "B (positive control): the bound incarnation keeps running");
    }

    // ---- D: read-only deployment grants are deduplicated ---------------------------------------------
    {
        std::wstring const python_home = std::filesystem::path(AE_PYTHON_HOME).wstring();
        {
            MediatedPythonRunner warm(base_config(scratch), backend);
            AE_CHECK(ae_test::initialize_runner(warm).has_value(), "D setup: a first worker creates");
        }
        DWORD const after_first = dacl_entry_count(python_home);
        for (int i = 0; i < 5; ++i) {
            MediatedPythonRunner again(base_config(scratch), backend);
            (void)ae_test::initialize_runner(again);
        }
        AE_CHECK(after_first != 0 && dacl_entry_count(python_home) == after_first,
                 "D: the DACL entry count on python_home is unchanged across 5 more worker creates");

        std::filesystem::path const fresh = std::filesystem::temp_directory_path() / "ae_runner_choices_rw_growth";
        std::filesystem::remove_all(fresh, ec);
        std::filesystem::create_directories(fresh, ec);
        DWORD const before_rw = dacl_entry_count(fresh.wstring());
        for (int i = 0; i < 3; ++i) {
            SandboxSpec spec;
            spec.mounts.push_back(MountSpec{.source = fresh.string(), .guest_path = "/rw", .read_write = true});
            spec.capabilities = CapabilitySet::grant_root({Capability{cap::SandboxMount{fresh.string(), "/rw", true}}});
            EffectContext ctx;
            auto h = rt::block_on(backend.create(spec, ctx));
            if (h.has_value()) rt::block_on(backend.destroy(*h));
        }
        AE_CHECK(dacl_entry_count(fresh.wstring()) > before_rw,
                 "D (positive control): the counter sees growth -- repeated read-write grants add entries");
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
        spec.mounts.push_back(MountSpec{.source = home, .guest_path = "/home", .read_write = true});
        spec.capabilities = CapabilitySet::grant_root({Capability{cap::SandboxMount{home, "/home", true}}});
        EffectContext ctx;
        auto h = rt::block_on(backend.create(spec, ctx));
        AE_CHECK(!h.has_value() && h.error().code == "sandbox.deployment_path_writable",
                 "F: a plain sandbox's read-write mount over a registered deployment path is refused");
        if (h.has_value()) rt::block_on(backend.destroy(*h));
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
