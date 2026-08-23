// 008-Sandbox-and-Isolation.md SS9 G2/G3's Linux filesystem-containment half --
// LinuxNativeJailBackend::setup_jail() (linux_native_jail_backend.cpp) now builds a real
// pivot_root + bind-mount jail per exec() call: MS_PRIVATE first, a tmpfs-backed root, exactly the
// granted MountSpec set bind-mounted in (read-only grants remounted MS_RDONLY -- the initial
// MS_BIND call silently ignores that flag), pivot_root (not chroot) into it. This is the Linux
// analogue of test_native_jail_abuse_corpus_windows.cpp's Case 4 (fs-escape) plus a dedicated
// read-only-bind-mount positive/negative pair (design draft MUST-FIX 2) -- both explicitly named
// in docs/planning/linux-native-jail-pivot-root-containment-design-draft.md SS4 as what proving
// this design would need. /proc-namespace-locality (the design draft's other SS4 item) is proven
// in test_native_jail_ambient_authority_linux.cpp's axis 3, already structured for it.
//
// Requires a delegated cgroup v2 root and CAP_SYS_ADMIN, same as every other Linux native_jail
// test -- run via tests/helpers/cgroup_v2_test_setup.sh.

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "backends/native_jail/linux_native_jail_backend.hpp"
#include "helpers/native_jail_linux_toolchain_mounts.hpp"

using namespace agentengine;
using agentengine::native_jail::LinuxNativeJailBackend;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

std::string quote(std::string const& s) { return "\"" + s + "\""; }

std::string hostile_child_cmd(std::string const& args) {
    return quote(AE_HOSTILE_CHILD_POSIX_EXE) + " " + args;
}

void write_file(std::filesystem::path const& path, std::string const& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}

std::string read_file(std::filesystem::path const& path) {
    std::ifstream f(path, std::ios::binary);
    std::string out((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return out;
}

}  // namespace

int main() {
    std::error_code ec;
    std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_fs_containment_test_linux_work";
    std::filesystem::path secret_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_fs_containment_test_linux_secret";
    std::filesystem::path ro_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_fs_containment_test_linux_ro";
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: work_dir exists");
    std::filesystem::create_directories(secret_dir, ec);
    AE_CHECK(!ec, "setup: secret_dir (deliberately NOT granted to any jail below) exists");
    std::filesystem::create_directories(ro_dir, ec);
    AE_CHECK(!ec, "setup: ro_dir exists");

    EffectContext ctx;

    // ---- Case A: fs-escape -- an ungranted host path is unreadable from inside the jail ----------
    {
        std::string const kSecretValue = "host_secret_never_granted_to_any_jail_13579";
        write_file(secret_dir / "secret.txt", kSecretValue);
        write_file(work_dir / "granted.txt", "granted_and_readable_24680");

        LinuxNativeJailBackend backend;
        SandboxSpec spec;
        spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        agentengine::native_jail::test::add_shell_toolchain_mounts(spec);
        spec.limits.wall_ms = 3000;
        spec.limits.memory_bytes = 32ull * 1024 * 1024;
        spec.limits.pids = 4;
        spec.limits.output_bytes = 4096;

        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "fs-escape: create() succeeds");
        if (handle.has_value()) {
            std::string secret_path = (secret_dir / "secret.txt").string();
            ExecRequest req{.language = "native", .source = hostile_child_cmd("probe_read " + quote(secret_path))};
            auto outcome = backend.exec(*handle, req, ctx);
            AE_CHECK(outcome.has_value(), "fs-escape: contained exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: contained probe_read(secret) stdout=" << outcome->stdout_text;
                bool leaked = outcome->stdout_text.find(kSecretValue) != std::string::npos;
                AE_CHECK(!leaked && outcome->stdout_text.find("READ_DENIED") != std::string::npos,
                          "fs-escape: a host path outside every granted MountSpec is unreadable "
                          "from inside the pivot_root jail, even by absolute path (008 SS9 G2)");
            }

            // Positive control 1: the SAME probe against a path INSIDE the granted mount succeeds --
            // proves probe_read/the containment above is real, not the guest failing to run at all.
            ExecRequest granted_req{
                .language = "native",
                .source = hostile_child_cmd("probe_read /work/granted.txt")};
            auto granted_outcome = backend.exec(*handle, granted_req, ctx);
            AE_CHECK(granted_outcome.has_value(), "fs-escape positive control: exec() returns a result");
            if (granted_outcome.has_value()) {
                std::cout << "  measured: contained probe_read(granted) stdout="
                          << granted_outcome->stdout_text;
                AE_CHECK(granted_outcome->stdout_text.find("granted_and_readable_24680") != std::string::npos,
                          "fs-escape positive control: a path INSIDE the granted /work mount IS "
                          "readable (the denial above is real containment, not a broken guest)");
            }
            backend.destroy(*handle);
        }
    }

    // ---- Case B: read-only bind mount -- MUST-FIX 2 (design draft SS3) ----------------------------
    {
        write_file(ro_dir / "ro_file.txt", "original_ro_content");
        write_file(work_dir / "rw_file.txt", "original_rw_content");

        LinuxNativeJailBackend backend;
        SandboxSpec spec;
        spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        spec.mounts.push_back(
            MountSpec{.source = ro_dir.string(), .guest_path = "/ro", .read_write = false});
        agentengine::native_jail::test::add_shell_toolchain_mounts(spec);
        spec.limits.wall_ms = 3000;
        spec.limits.memory_bytes = 32ull * 1024 * 1024;
        spec.limits.pids = 4;
        spec.limits.output_bytes = 4096;

        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "ro-bind: create() succeeds");
        if (handle.has_value()) {
            ExecRequest req{.language = "native", .source = hostile_child_cmd("probe_write /ro/ro_file.txt")};
            auto outcome = backend.exec(*handle, req, ctx);
            AE_CHECK(outcome.has_value(), "ro-bind: contained exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: contained probe_write(ro) stdout=" << outcome->stdout_text;
                AE_CHECK(outcome->stdout_text.find("WRITE_DENIED") != std::string::npos,
                          "ro-bind: writing through a read-only-granted mount is denied (MUST-FIX "
                          "2's required second MS_REMOUNT|MS_RDONLY call is genuinely present, not "
                          "just the first MS_BIND call, which silently ignores MS_RDONLY alone)");
            }
            std::string host_content_after = read_file(ro_dir / "ro_file.txt");
            AE_CHECK(host_content_after == "original_ro_content",
                      "ro-bind: the host file's content is unchanged after the denied write attempt "
                      "(the denial is real, not a probe that silently no-oped)");

            // Positive control: the identical write, against the RW-granted mount, succeeds --
            // proves the write mechanism itself works and the RO denial above is real containment.
            ExecRequest rw_req{.language = "native", .source = hostile_child_cmd("probe_write /work/rw_file.txt")};
            auto rw_outcome = backend.exec(*handle, rw_req, ctx);
            AE_CHECK(rw_outcome.has_value(), "ro-bind positive control: exec() returns a result");
            if (rw_outcome.has_value()) {
                std::cout << "  measured: contained probe_write(rw) stdout=" << rw_outcome->stdout_text;
                AE_CHECK(rw_outcome->stdout_text.find("WRITE_OK") != std::string::npos,
                          "ro-bind positive control: writing through a read-write-granted mount "
                          "succeeds (the RO denial above is real, not every write silently failing)");
            }
            std::string host_rw_content_after = read_file(work_dir / "rw_file.txt");
            AE_CHECK(host_rw_content_after.find("hostile_write_probe") != std::string::npos,
                      "ro-bind positive control: the write through the RW mount is visible on the "
                      "HOST side too (a real bind mount, not an isolated tmpfs-only copy)");
            backend.destroy(*handle);
        }
    }

    std::filesystem::remove_all(work_dir, ec);
    std::filesystem::remove_all(secret_dir, ec);
    std::filesystem::remove_all(ro_dir, ec);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
