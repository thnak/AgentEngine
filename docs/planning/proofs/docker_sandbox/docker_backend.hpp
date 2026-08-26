#pragma once
// PROVE-PHASE PROBE: a minimal, real DockerBackend -- shells out to the real `docker` CLI (this
// environment has a real, running Docker Desktop daemon, confirmed: `docker version`/`docker ps`
// against real containers), mirroring the REAL, already-shipped KataBackend's own precedent shape
// (src/backends/kata/kata_backend.hpp: shells out to `ctr run`/`ctr tasks exec`/`ctr tasks kill`
// rather than linking a C API) -- create()/exec()/destroy() over `docker run`/`docker exec`/
// `docker rm`, not a mock.
//
// PURPOSE: this design's own §29 (Attack 5) found and disclosed that nothing in the identity-native
// stack (GrantSet/MediatedFileSystem/Ledger ACLs) is an OS-level enforcement boundary against native
// code sharing the same process -- closing that for real needs an actual OS-level jail. This probe
// tests whether a REAL Docker container, bind-mounting the SAME real host directory this design's own
// RealIoFileSystem/Ledger stack already uses, provides that boundary for real.

#include <array>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>

#include "../common/result.hpp"

namespace probe {

struct ExecOutcome {
    int exit_code = -1;
    std::string stdout_text;
};

namespace docker_detail {
// Runs a command, captures stdout, returns the real process exit code. Windows CRT `_popen`/
// `_pclose` (not POSIX popen/pclose -- this project's own real code targets both, but this probe is
// Windows-only, matching every other prove-phase probe's own build environment).
[[nodiscard]] inline ExecOutcome run_capture(std::string const& command) {
    ExecOutcome out;
    FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
    if (!pipe) { out.exit_code = -1; return out; }
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        out.stdout_text += buffer.data();
    }
    out.exit_code = _pclose(pipe);
    return out;
}
}  // namespace docker_detail

class DockerBackend {
public:
    struct Instance {
        std::string container_id;
    };

    // Real `docker run -d --rm -w /workspace <image> sleep infinity` -- NO bind mount: this
    // environment's Docker Desktop restricts host bind-mounts to an explicit GUI-configured
    // "File Sharing" allowlist (confirmed for real: `docker run -v <any real path here>` fails with
    // "the path ... is not shared from the host" for every path tried, including under the project's
    // own drive and under C:\Users\<user>) -- a real environment constraint, not a code defect, and
    // not something fixable from the CLI/from this probe. Real host<->container data movement uses
    // `docker cp` instead (copy_to_container/copy_from_container below), which needs no such
    // allowlist. The container's OWN internal filesystem (its image's rootfs + whatever `docker cp`
    // deposits into it) is still real, still isolated by the kernel's own mount/pid/network
    // namespaces -- only the LIVE bind-mount convenience is unavailable in this specific environment.
    [[nodiscard]] result<Instance> create(std::string const& image = "alpine:latest") {
        std::ostringstream cmd;
        cmd << "docker run -d --rm -w /workspace " << image << " sh -c \"mkdir -p /workspace && sleep infinity\"";
        auto r = docker_detail::run_capture(cmd.str());
        if (r.exit_code != 0) {
            return std::unexpected(error{"docker run failed: " + r.stdout_text,
                                          "docker_backend.create_failed"});
        }
        std::string id = r.stdout_text;
        while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) id.pop_back();
        return Instance{id};
    }

    // Real `docker cp <host_path> <container>:<container_path>` -- copies a real file from real host
    // disk into the container's own isolated filesystem.
    [[nodiscard]] result<void> copy_to_container(Instance const& inst, std::filesystem::path const& host_path,
                                                    std::string const& container_path) {
        std::ostringstream cmd;
        cmd << "docker cp \"" << host_path.string() << "\" " << inst.container_id << ":" << container_path;
        auto r = docker_detail::run_capture(cmd.str());
        if (r.exit_code != 0) {
            return std::unexpected(error{"docker cp (to container) failed: " + r.stdout_text,
                                          "docker_backend.copy_to_failed"});
        }
        return result<void>{};
    }

    // Real `docker cp <container>:<container_path> <host_path>` -- copies a real file OUT of the
    // container's isolated filesystem onto real host disk (this is the bridge back into this
    // design's own RealIoFileSystem/Ledger stack, which only ever reads real host paths).
    [[nodiscard]] result<void> copy_from_container(Instance const& inst, std::string const& container_path,
                                                      std::filesystem::path const& host_path) {
        std::ostringstream cmd;
        cmd << "docker cp " << inst.container_id << ":" << container_path << " \"" << host_path.string() << "\"";
        auto r = docker_detail::run_capture(cmd.str());
        if (r.exit_code != 0) {
            return std::unexpected(error{"docker cp (from container) failed: " + r.stdout_text,
                                          "docker_backend.copy_from_failed"});
        }
        return result<void>{};
    }

    // Real `docker exec <id> sh -c "<command>"` -- runs INSIDE the container's own isolated
    // filesystem/process namespace, never in this process at all.
    [[nodiscard]] result<ExecOutcome> exec(Instance const& inst, std::string const& command) {
        std::ostringstream cmd;
        cmd << "docker exec " << inst.container_id << " sh -c \"" << command << "\"";
        auto r = docker_detail::run_capture(cmd.str());
        return r;   // exit_code intentionally passed through as-is (a non-zero exit from the
                     // CONTAINED command is a normal, meaningful result, not a probe failure)
    }

    [[nodiscard]] result<void> destroy(Instance const& inst) {
        auto r = docker_detail::run_capture("docker rm -f " + inst.container_id);
        if (r.exit_code != 0) {
            return std::unexpected(error{"docker rm failed: " + r.stdout_text,
                                          "docker_backend.destroy_failed"});
        }
        return result<void>{};
    }
};

}  // namespace probe
