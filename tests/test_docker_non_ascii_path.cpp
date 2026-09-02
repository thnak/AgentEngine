// code-review finding (post-ADR-164, docker-execution-surface-argv-hardening): on Windows,
// std::filesystem::path::string() narrows via the process's ACTIVE CODE PAGE, never UTF-8, but the new
// run_argv()'s widen() explicitly expects UTF-8 -- a host_path/container_path containing a non-ASCII
// character would silently mis-decode, corrupting the generated `docker cp` argv element. Fixed by
// docker_cli_detail::path_to_utf8() (converts a path's true wstring()/native() content, never its
// ACP-narrowed string()). This test proves the round trip works for real, on a live Docker daemon, not
// merely that the conversion function looks correct in isolation.
//
// REQUIRES a running Docker daemon reachable via `docker` on PATH, same posture as
// test_sandbox_runtime.cpp/test_docker_orphan_reap.cpp.

#include "agentengine/sandbox/docker_execution_surface.hpp"

#include <cstdio>
#include <fstream>
#include <string>

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

}  // namespace

int main() {
    std::printf("=== copy_to_container()/copy_from_container() round-trip a non-ASCII host path ===\n");

    // A real host directory whose NAME itself contains non-ASCII characters (Vietnamese + CJK + an
    // emoji) -- exactly the class of path the code-review finding named as silently corrupted before
    // the fix. Placed under the system temp directory, cleaned up at the end.
    std::filesystem::path const base = std::filesystem::temp_directory_path() /
                                        L"ae_docker_utf8_probe_tiếng_Việt_中文_\U0001F600";
    std::filesystem::path const in_dir = base / "in";
    std::filesystem::path const out_dir = base / "out";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(in_dir, ec);
    check(!ec, "create_directories(in_dir) succeeds for a non-ASCII host path");

    std::string const expected_content = "utf8-roundtrip-payload-12345";
    {
        std::ofstream f(in_dir / "payload.txt", std::ios::binary);
        f << expected_content;
    }

    agentengine::DockerCliBackend docker;
    auto inst = docker.create("alpine:latest");
    check(inst.has_value(), "create() succeeds");
    if (!inst.has_value()) {
        std::printf("=== %d checks, %d failed ===\n", g_checks, g_failed);
        return 1;
    }

    // Real `docker cp <non-ASCII host dir> <container>:/workspace/in` -- exercises the exact
    // copy_to_container() path the code-review finding flagged.
    auto copied_in = docker.copy_to_container(*inst, in_dir, "/workspace/in");
    check(copied_in.has_value(),
          copied_in.has_value() ? "copy_to_container() succeeds for a non-ASCII host_path"
                                 : ("copy_to_container() FAILED: " + copied_in.error().message));

    if (copied_in.has_value()) {
        auto read_back = docker.exec(*inst, "cat /workspace/in/payload.txt");
        check(read_back.has_value() && read_back->exit_code == 0,
              "exec() reads the file copy_to_container() placed");
        check(read_back.has_value() && read_back->stdout_text == expected_content,
              "content round-tripped byte-for-byte through the non-ASCII host_path");
    }

    // Real `docker cp <container>:/workspace/out <non-ASCII host dir>` -- exercises
    // copy_from_container()'s own identical fix.
    auto wrote = docker.exec(*inst, "mkdir -p /workspace/out && printf '%s' 'drain-side-payload' > "
                                     "/workspace/out/drained.txt");
    check(wrote.has_value() && wrote->exit_code == 0, "setup: container writes a file to drain out");

    auto copied_out = docker.copy_from_container(*inst, "/workspace/out", out_dir);
    check(copied_out.has_value(),
          copied_out.has_value() ? "copy_from_container() succeeds into a non-ASCII host_path"
                                  : ("copy_from_container() FAILED: " + copied_out.error().message));

    if (copied_out.has_value()) {
        std::ifstream f(out_dir / "drained.txt", std::ios::binary);
        std::string const drained_content((std::istreambuf_iterator<char>(f)),
                                            std::istreambuf_iterator<char>());
        check(drained_content == "drain-side-payload",
              "drained content round-tripped byte-for-byte into the non-ASCII host_path");
    }

    auto destroyed = docker.destroy(*inst);
    check(destroyed.has_value(), "destroy() cleanup succeeds");
    std::filesystem::remove_all(base, ec);

    std::printf("=== %d checks, %d failed ===\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
