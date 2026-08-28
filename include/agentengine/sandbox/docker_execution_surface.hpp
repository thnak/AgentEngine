#pragma once
// Implements ADR-102 Phase 3 -- `DockerCliBackend`/`DockerExecutionSurface`, the one real
// `ExecutionSurface` conformer this phase ports: a real Docker container, driven entirely by
// shelling out to the real `docker` CLI (`docker run`/`docker cp`/`docker exec`/`docker rm`), never
// a mock.
//
// Ported from docs/planning/proofs/{docker_sandbox/docker_backend.hpp,
// execution_surface/docker_execution_surface.hpp} (ADR-099's own standalone, red-teamed,
// live-Docker-tested prove-phase originals -- kept as-is, these are new files). Real changes made
// during the port:
//   - `probe::DockerBackend` -> `agentengine::DockerCliBackend`, deliberately NOT bare
//     `DockerBackend` -- this type does not conform to (and is not meant to imply conformance to)
//     the real, production `agentengine::SandboxBackend` concept (`sandbox.hpp`, 008 §2a); ADR-101's
//     own, separate, still-Proposed/unjudged `DockerSandboxBackend` wraps the SAME underlying `docker`
//     CLI shape as a REAL `SandboxBackend` conformer -- a real, disclosed future consolidation
//     opportunity (both could eventually share one production Docker-CLI wrapper), not acted on in
//     this phase, which stays deliberately independent of ADR-101 per its own scope decision.
//   - `probe::result<T>`/`probe::error{message, code}` -> the real `agentengine::result<T>`/
//     `agentengine::error{failure_class, message, code}` -- `policy` for the shell-injection-defense
//     rejections (a caller-supplied value that could break out of the intended quoting is refused,
//     matching this codebase's own `failure_class::policy` convention for "denied by policy, never
//     from a model", I3), `fatal` for a `docker` CLI invocation itself failing.
//   - `probe::ExecOutcome` -> `agentengine::SurfaceRunOutcome` (execution_surface.hpp, this phase's
//     own naming decision -- see that file's own top comment for why).

#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/execution_surface.hpp"

namespace agentengine {

namespace docker_cli_detail {
// Runs a command, captures stdout+stderr (merged), returns the real process exit code. Windows CRT
// `_popen`/`_pclose` -- matching this whole phase's own disclosed Windows-only scope (see
// `real_io_filesystem.hpp`'s own top comment for the same disclosure on its own Win32 dependency).
[[nodiscard]] inline SurfaceRunOutcome run_capture(std::string const& command) {
    SurfaceRunOutcome out;
    FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
    if (!pipe) { out.exit_code = -1; return out; }
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        out.stdout_text += buffer.data();
    }
    out.exit_code = _pclose(pipe);
    return out;
}
}  // namespace docker_cli_detail

// Every method below builds a `cmd.exe`-interpreted command STRING by concatenation -- a
// caller-supplied value containing a double-quote, `&`, `|`, `^`, or similar could otherwise break
// out of the intended quoting and execute attacker-controlled commands on the HOST via `_popen`,
// defeating the very isolation boundary this type exists to provide. Defended by REJECTING (not
// attempting to escape) any such value outright -- a NECESSARY, not sufficient, defense; a fully
// general cmd.exe escaper is its own hard problem this type does not attempt to solve in full.
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_chars(std::string const& value,
                                                                           char const* what,
                                                                           char const* dangerous) {
    if (value.find_first_of(dangerous) != std::string::npos) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            std::string("refusing to build a shell command: '") + what +
                "' contains a character that could break out of the surrounding quoting",
            "docker_cli_backend.unsafe_shell_argument"});
    }
    return agentengine::result<void>{};
}

// For image names/paths: NONE of `"&|<>^%` are ever legitimately needed, so reject the whole set.
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_unsafe_for_shell(
        std::string const& value, char const* what) {
    return docker_cli_reject_chars(value, what, "\"&|<>^%\r\n");
}

// For exec()'s own `command` argument specifically: it is legitimately a shell command meant for the
// CONTAINER's inner `sh` and needs `&|<>` for its own real use (pipes/redirects/backgrounding) --
// rejecting those would defeat exec()'s whole purpose. Only the characters that actually break the
// OUTER cmd.exe quoting this string sits inside (a literal double-quote; `%`/`^`, cmd.exe's own
// escape/expansion quirks) are rejected here.
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_shell_breakout(
        std::string const& value, char const* what) {
    return docker_cli_reject_chars(value, what, "\"%^\r\n");
}

// A thin, real wrapper over the `docker` CLI -- create()/exec()/destroy() over `docker run`/
// `docker exec`/`docker rm`, `copy_to_container()`/`copy_from_container()` over `docker cp`.
// Deliberately NOT a `SandboxBackend` conformer (see this file's own top comment).
class DockerCliBackend {
public:
    struct Instance {
        std::string container_id;
    };

    // Real `docker run -d --rm -w /workspace <image> sleep infinity` -- NO bind mount: Docker
    // Desktop restricts host bind-mounts to an explicit GUI-configured "File Sharing" allowlist on
    // many real developer machines (a real environment constraint, not a code defect). Real
    // host<->container data movement uses `docker cp` instead (below), which needs no such
    // allowlist. The container's OWN internal filesystem is still real, still isolated by the
    // kernel's own mount/pid/network namespaces regardless.
    [[nodiscard]] agentengine::result<Instance> create(std::string const& image = "alpine:latest") {
        if (auto safe = docker_cli_reject_unsafe_for_shell(image, "image"); !safe.has_value())
            return std::unexpected(safe.error());
        std::ostringstream cmd;
        cmd << "docker run -d --rm -w /workspace " << image
            << " sh -c \"mkdir -p /workspace && sleep infinity\"";
        auto r = docker_cli_detail::run_capture(cmd.str());
        if (r.exit_code != 0) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                          "docker run failed: " + r.stdout_text,
                                                          "docker_cli_backend.create_failed"});
        }
        std::string id = r.stdout_text;
        while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) id.pop_back();
        return Instance{id};
    }

    // Real `docker cp <host_path> <container>:<container_path>`.
    [[nodiscard]] agentengine::result<void> copy_to_container(Instance const& inst,
                                                                  std::filesystem::path const& host_path,
                                                                  std::string const& container_path) {
        if (auto safe = docker_cli_reject_unsafe_for_shell(host_path.string(), "host_path"); !safe.has_value())
            return std::unexpected(safe.error());
        if (auto safe = docker_cli_reject_unsafe_for_shell(container_path, "container_path"); !safe.has_value())
            return std::unexpected(safe.error());
        std::ostringstream cmd;
        cmd << "docker cp \"" << host_path.string() << "\" " << inst.container_id << ":" << container_path;
        auto r = docker_cli_detail::run_capture(cmd.str());
        if (r.exit_code != 0) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                          "docker cp (to container) failed: " + r.stdout_text,
                                                          "docker_cli_backend.copy_to_failed"});
        }
        return agentengine::result<void>{};
    }

    // Real `docker cp <container>:<container_path> <host_path>`.
    [[nodiscard]] agentengine::result<void> copy_from_container(Instance const& inst,
                                                                    std::string const& container_path,
                                                                    std::filesystem::path const& host_path) {
        if (auto safe = docker_cli_reject_unsafe_for_shell(container_path, "container_path"); !safe.has_value())
            return std::unexpected(safe.error());
        if (auto safe = docker_cli_reject_unsafe_for_shell(host_path.string(), "host_path"); !safe.has_value())
            return std::unexpected(safe.error());
        std::ostringstream cmd;
        cmd << "docker cp " << inst.container_id << ":" << container_path << " \"" << host_path.string() << "\"";
        auto r = docker_cli_detail::run_capture(cmd.str());
        if (r.exit_code != 0) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                          "docker cp (from container) failed: " + r.stdout_text,
                                                          "docker_cli_backend.copy_from_failed"});
        }
        return agentengine::result<void>{};
    }

    // Real `docker exec <id> sh -c "<command>"` -- runs INSIDE the container's own isolated
    // filesystem/process namespace, never in this process at all.
    [[nodiscard]] agentengine::result<SurfaceRunOutcome> exec(Instance const& inst, std::string const& command) {
        if (auto safe = docker_cli_reject_shell_breakout(command, "command"); !safe.has_value())
            return std::unexpected(safe.error());
        std::ostringstream cmd;
        cmd << "docker exec " << inst.container_id << " sh -c \"" << command << "\"";
        return docker_cli_detail::run_capture(cmd.str());   // exit_code intentionally passed through
                                                                // as-is -- a non-zero exit from the
                                                                // CONTAINED command is a normal,
                                                                // meaningful result, never itself a
                                                                // result<>-level error
    }

    [[nodiscard]] agentengine::result<void> destroy(Instance const& inst) {
        auto r = docker_cli_detail::run_capture("docker rm -f " + inst.container_id);
        if (r.exit_code != 0) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                          "docker rm failed: " + r.stdout_text,
                                                          "docker_cli_backend.destroy_failed"});
        }
        return agentengine::result<void>{};
    }
};

// The one real `ExecutionSurface` conformer this phase ports -- wraps `DockerCliBackend` behind the
// generic `reset()`/`run()`/`drain_to()` shape `SandboxRuntime` drives.
//
// HONEST RESIDUAL, disclosed not solved: containers are started via `docker run -d --rm ...
// sleep infinity` -- `--rm` fires on the container's own exit, which `sleep infinity` never triggers
// on its own. If the hosting process crashes/aborts between a successful `reset()` and this
// destructor running, the container is orphaned and keeps running indefinitely, with no id
// persisted anywhere and no reclaim mechanism analogous to `Ledger`'s own `orphaned_branches()`/A7
// design.
//
// CORRECTED (2026-08-28, an independent red-team pass on this port found the prior wording here
// overclaimed): this is NOT limited to an actual process crash. The destructor below discards
// `destroy()`'s own result via `(void)`, with no retry and nothing to retry it later -- a perfectly
// ORDINARY destructor call whose `docker rm -f` transiently fails (daemon contention, a network
// hiccup -- the same transient-failure class `reset()` itself already defends against, by NOT
// clearing `instance_` on a failed `destroy()` there, since a live caller can retry) leaks the
// container just as silently, on a completely normal, non-crash exit path. Confirmed consistent with
// a real, pre-existing orphaned container observed on this development host during this same
// red-team pass (an `alpine:latest` container running the exact `create()` command this class emits,
// with no crash known to have produced it). Not fixed in this pass -- the fix (persisting instance
// ids somewhere reclaimable, mirroring `Ledger`'s own orphan-branch design) is real follow-on work,
// named accurately here rather than left understated a second time.
class DockerExecutionSurface {
public:
    explicit DockerExecutionSurface(std::string image = "alpine:latest") : image_(std::move(image)) {}

    ~DockerExecutionSurface() {
        if (instance_) { (void)docker_.destroy(*instance_); }
    }
    DockerExecutionSurface(DockerExecutionSurface const&) = delete;
    DockerExecutionSurface& operator=(DockerExecutionSurface const&) = delete;

    // Move constructor resets the moved-FROM `instance_` explicitly -- `std::optional`'s own
    // move-construction semantics only move the CONTAINED value, `has_value()` is unchanged by
    // default, which would otherwise fire a malformed `docker rm -f ` (empty id) from the moved-from
    // object's own destructor.
    DockerExecutionSurface(DockerExecutionSurface&& other) noexcept
        : image_(std::move(other.image_)), docker_(std::move(other.docker_)),
          instance_(std::move(other.instance_)) {
        other.instance_.reset();
    }
    // Move assignment SWAPS rather than overwrite-then-discard: `a = std::move(b)` where `a` already
    // owns a live container must not silently leak `a`'s own instance by simply copying `b`'s state
    // over it with no cleanup attempt. Swapping means `other` (almost always an about-to-be-destroyed
    // moved-from temporary) ends up owning what `this` used to own, and `other`'s own, already-correct
    // destructor performs the real cleanup when it goes out of scope -- no possibly-failing `destroy()`
    // call happens inside this operator at all, so there is no failure path here to mishandle.
    DockerExecutionSurface& operator=(DockerExecutionSurface&& other) noexcept {
        if (this != &other) {
            image_.swap(other.image_);
            std::swap(docker_, other.docker_);
            instance_.swap(other.instance_);
        }
        return *this;
    }

    // `instance_` is only cleared once `destroy()` has actually succeeded -- a transient failure
    // (daemon contention, a network hiccup) leaves it in place, so the object still remembers the
    // leak and a caller retrying `reset()` gets another real attempt at destroying the SAME
    // container, rather than silently starting a second one alongside an orphaned first.
    [[nodiscard]] agentengine::result<void> reset(std::filesystem::path const& host_dir) {
        if (instance_) {
            auto destroyed = docker_.destroy(*instance_);
            if (!destroyed.has_value()) return std::unexpected(destroyed.error());
            instance_.reset();
        }
        auto inst = docker_.create(image_);
        if (!inst.has_value()) return std::unexpected(inst.error());
        instance_ = *inst;

        if (!std::filesystem::exists(host_dir)) return agentengine::result<void>{};  // nothing to seed yet
        // Trailing "/." copies host_dir's CONTENTS into /workspace (which create() already made),
        // not host_dir itself as a nested subdirectory. `generic_string()`, not `string()`, so the
        // result uses forward slashes throughout even on Windows -- `string()` + a literal "/."
        // would produce a mixed-separator path fragile against docker CLI's own path normalization.
        std::filesystem::path const source(host_dir.generic_string() + "/.");
        auto copied = docker_.copy_to_container(*instance_, source, "/workspace");
        if (!copied.has_value()) return std::unexpected(copied.error());
        return agentengine::result<void>{};
    }

    [[nodiscard]] agentengine::result<SurfaceRunOutcome> run(std::string const& command) {
        if (!instance_) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "reset() must be called before run()",
                                                          "docker_execution_surface.not_reset"});
        }
        return docker_.exec(*instance_, "cd /workspace && " + command);
    }

    [[nodiscard]] agentengine::result<void> drain_to(std::filesystem::path const& host_dir) {
        if (!instance_) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "reset() must be called before drain_to()",
                                                          "docker_execution_surface.not_reset"});
        }
        std::filesystem::create_directories(host_dir);
        // Same "/." convention in the other direction: copies /workspace's CONTENTS onto host_dir.
        auto copied = docker_.copy_from_container(*instance_, "/workspace/.", host_dir);
        if (!copied.has_value()) return std::unexpected(copied.error());
        return agentengine::result<void>{};
    }

private:
    std::string image_;
    DockerCliBackend docker_;
    std::optional<DockerCliBackend::Instance> instance_;
};

static_assert(ExecutionSurface<DockerExecutionSurface>,
              "DockerExecutionSurface must satisfy the real ExecutionSurface concept");

}  // namespace agentengine
