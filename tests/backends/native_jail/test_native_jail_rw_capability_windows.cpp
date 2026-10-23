// Proves decisions/ADR-244-required-choices-step-1b.md §2.4's grant model on the real shared AppContainer profile:
// read-write mounts go to a per-sandbox capability SID, and a deployment path is never writable by a sandbox, in
// this process or another.
//
//   W1  a sandbox writes its own read-write mount (positive control) and cannot write another sandbox's.
//   W2  the write grant exists while the sandbox lives and is gone once it is destroyed.
//   L2  a deployment path whose DACL gives write to a capability SID (a live or crashed sandbox elsewhere) or to the
//       profile SID (an older engine's grant) is refused; a clean one gets past every check (positive control: the
//       call then fails only at launching the fake worker binary).
//   L3  a second PROCESS asking for a read-write mount over a path this process recorded as a deployment path is
//       refused; the same child on an unrecorded path succeeds (positive control).
//   L1  in this process, a read-write mount spelled `\\?\...` over a recorded deployment path is refused.
//
// `create_python_worker` is driven with a worker path that does not exist: every check runs before the launch, so
// `native_jail.worker_create_process_failed` means "admitted". No Python is needed.

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "agentengine/rt/block_on.hpp"
#include "backends/native_jail/app_container_profile.hpp"
#include "backends/native_jail/native_jail_backend.hpp"

using namespace agentengine;
using agentengine::native_jail::AppContainerProfile;
using agentengine::native_jail::NativeJailBackend;
using agentengine::native_jail::OwnedSid;
using agentengine::native_jail::PythonWorkerSessionConfig;

namespace {

int g_failures = 0;
void check(bool cond, std::string const& what) {
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "  ok" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

namespace fs = std::filesystem;

fs::path fresh_dir(char const* name) {
    fs::path const p = fs::temp_directory_path() / "ae_rw_capability_test" / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

SandboxSpec rw_spec(fs::path const& dir) {
    SandboxSpec spec;
    spec.mounts.push_back(MountSpec{.source = dir.string(), .guest_path = "/rw", .read_write = true, .quota_bytes = cap::Limit::unbounded()});
    spec.capabilities = CapabilitySet::grant_root({Capability{cap::SandboxMount{dir.string(), "/rw", true}}});
    spec.limits.wall_ms = 10000;
    return spec;
}

// Runs `cmd.exe /c echo x > target` in the sandbox; true if the file exists afterwards.
bool sandbox_writes(NativeJailBackend& backend, SandboxHandle& h, fs::path const& target) {
    std::error_code ec;
    fs::remove(target, ec);
    EffectContext ctx;
    ExecRequest req{.language = "native",
                    .source = "C:\\Windows\\System32\\cmd.exe /c echo x > \"" + target.string() + "\""};
    (void)rt::block_on(backend.exec(h, req, ctx));
    return fs::exists(target, ec);
}

// create_python_worker with a fake worker binary in `worker_dir` and `deployment` as its one extra_sys_path entry.
std::string worker_outcome(NativeJailBackend& backend, fs::path const& worker_dir, fs::path const& deployment) {
    PythonWorkerSessionConfig session;
    session.worker_exe_path = (worker_dir / "no_such_worker.exe").wstring();
    session.extra_sys_path.push_back(deployment.wstring());
    SandboxSpec spec;
    spec.capabilities = CapabilitySet::grant_root({
        Capability{cap::SandboxMount{worker_dir.string(), worker_dir.string(), false}},
        Capability{cap::SandboxMount{deployment.string(), deployment.string(), false}},
    });
    EffectContext ctx;
    auto h = backend.create_python_worker(spec, std::move(session), ctx);
    if (h.has_value()) {
        rt::block_on(backend.destroy(*h));
        return "<created>";
    }
    return h.error().code;
}

// Child mode: asks for a read-write mount over argv[2]. Exit 0 = refused as a deployment path, 2 = created.
int child_main(char const* path) {
    NativeJailBackend backend;
    EffectContext ctx;
    auto h = rt::block_on(backend.create(rw_spec(path), ctx));
    if (h.has_value()) {
        rt::block_on(backend.destroy(*h));
        return 2;
    }
    std::fprintf(stderr, "  child refused: %s\n", h.error().code.c_str());
    return h.error().code == "sandbox.deployment_path_writable" ? 0 : 1;
}

DWORD run_child(fs::path const& path) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" rw-child \"" + path.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return 99;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "rw-child") return child_main(argv[2]);

    NativeJailBackend backend;
    auto profile = AppContainerProfile::ensure(L"AgentEngine.NativeJail", L"AgentEngine Native Jail",
                                               L"AgentEngine native-jail sandbox AppContainer profile (008 SS1b, ADR-004)");
    if (!profile.has_value()) {
        std::fprintf(stderr, "cannot open the shared profile: %s\n", profile.error().message.c_str());
        return 1;
    }

    // ---- W1, W2 -------------------------------------------------------------------------------------
    {
        fs::path const dir_a = fresh_dir("a");
        fs::path const dir_b = fresh_dir("b");
        EffectContext ctx;
        auto a = rt::block_on(backend.create(rw_spec(dir_a), ctx));
        auto b = rt::block_on(backend.create(rw_spec(dir_b), ctx));
        check(a.has_value() && b.has_value(), "W setup: two sandboxes with read-write mounts create");
        if (a.has_value() && b.has_value()) {
            check(sandbox_writes(backend, *a, dir_a / "own.txt"), "W1 (positive control): a sandbox writes its own mount");
            check(!sandbox_writes(backend, *a, dir_b / "other.txt"), "W1: a sandbox cannot write another sandbox's mount");

            auto live = native_jail::appcontainer_writer(dir_a.wstring(), profile->sid());
            check(live.has_value() && live->has_value(), "W2 (positive control): the write grant exists while the sandbox lives");
            rt::block_on(backend.destroy(*a));
            auto gone = native_jail::appcontainer_writer(dir_a.wstring(), profile->sid());
            check(gone.has_value() && !gone->has_value(), "W2: the write grant is revoked when the sandbox is destroyed");
            rt::block_on(backend.destroy(*b));
        }
    }

    // ---- L2 -----------------------------------------------------------------------------------------
    fs::path const worker_dir = fresh_dir("worker");
    {
        fs::path const leftover = fresh_dir("leftover_capability");
        auto cap = OwnedSid::derive_capability(L"agentengine.test.crashed-sandbox");
        check(cap.has_value() && native_jail::grant_sid_path(leftover.wstring(), cap->get(), true).has_value(),
              "L2 setup: a capability SID holds write on a directory (as a crashed sandbox's grant would)");
        check(worker_outcome(backend, worker_dir, leftover) == "sandbox.deployment_path_writable",
              "L2: a deployment path some capability SID can write is refused");

        fs::path const legacy = fresh_dir("leftover_profile");
        check(profile->grant_path(legacy.wstring(), true).has_value(),
              "L2 setup: the profile SID holds write on a directory (as an older engine's grant would)");
        check(worker_outcome(backend, worker_dir, legacy) == "sandbox.deployment_path_writable",
              "L2: a deployment path the profile SID can write is refused");
    }
    fs::path const deployment = fresh_dir("deployment");
    check(worker_outcome(backend, worker_dir, deployment) == "native_jail.worker_create_process_failed",
          "L2 (positive control): a clean deployment path passes every check, failing only at the launch");

    // ---- L3 -----------------------------------------------------------------------------------------
    check(run_child(deployment) == 0, "L3: another process's read-write mount over a recorded deployment path is refused");
    fs::create_directories(deployment / "sub");
    check(run_child(deployment / "sub") == 0, "L3: another process's read-write mount inside a deployment path is refused");
    check(run_child(fresh_dir("unrecorded")) == 2,
          "L3 (positive control): the same child mounts an unrecorded directory read-write");

    // ---- L1 -----------------------------------------------------------------------------------------
    {
        std::string const verbatim = "\\\\?\\" + deployment.string();
        SandboxSpec spec;
        spec.mounts.push_back(MountSpec{.source = verbatim, .guest_path = "/rw", .read_write = true, .quota_bytes = cap::Limit::unbounded()});
        spec.capabilities = CapabilitySet::grant_root({Capability{cap::SandboxMount{verbatim, "/rw", true}}});
        EffectContext ctx;
        auto h = rt::block_on(backend.create(spec, ctx));
        check(!h.has_value() && h.error().code == "sandbox.deployment_path_writable",
              "L1: a read-write mount spelled \\\\?\\... over a recorded deployment path is refused");
        if (h.has_value()) rt::block_on(backend.destroy(*h));
    }

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "ae_rw_capability_test", ec);
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
