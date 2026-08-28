#pragma once
// PROVE-PHASE PROBE (A3, §36.5): `ContainerdExecutionSurface` -- the SECOND real `ExecutionSurface`
// conformer this design tree now has, closing §36.4's own residual ("whether the three-verb shape
// generalizes past Docker... unverified -- only one conformer exists"). Built on `ctr run`'s
// convenience-flag path (never `--config` mode) with a BIND MOUNT replacing `DockerExecutionSurface`'s
// `docker cp`-based copy-in/copy-out entirely, per docs/planning/oci-execution-surface-design-draft.md
// Design C (§2/§3) and identity-native-sandbox-worktree-design.md §36.5.
//
// TIER REACHED (state this precisely, not optimistically): a real, standalone C++ class satisfying
// the REAL `ExecutionSurface` concept (execution_surface.hpp, unmodified, `static_assert` below
// proves it compiles against the actual concept) -- compiled and run for real against a live
// containerd 2.2.2 + runc 1.4.0 deployment (WSL2 Ubuntu). NOT yet driven through the real
// `SandboxRuntime`/`Ledger`/`RealIoFileSystem` stack (sandbox_runtime.hpp) -- this class is exercised
// directly by its own accompanying probe (probe_containerd_execution_surface.cpp), the same relationship
// `docker_sandbox/probe_docker_sandbox.cpp` originally had to `DockerBackend` before
// `DockerExecutionSurface`/`SandboxRuntime` existed at all. See the final report for the exact scope.
//
// THE central architectural finding (§36.5, C1/C2): because `/workspace` inside the container is a
// LIVE bind mount of the real host directory (never a copy), `reset()`'s copy-in and `drain_to()`'s
// copy-out both disappear when draining back to the SAME directory that was mounted -- writes inside
// the container land on real host disk the whole time it runs. This is proven below to be a true,
// harmless no-op for the common case (drain_to(same host_dir) after reset(host_dir)), while still
// handling the general `ExecutionSurface` contract (draining to a DIFFERENT directory than the one
// mounted) via a real recursive copy -- disclosed as the one place this conformer is NOT trivial,
// not silently assumed away.
//
// Linux-only, matching KataBackend's own real scope and this design's own §3 decision.

#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "../common/result.hpp"
#include "containerd_ctr_backend.hpp"
#include "execution_surface.hpp"

namespace probe {

class ContainerdExecutionSurface {
public:
    explicit ContainerdExecutionSurface(std::string image = "docker.io/library/alpine:latest")
        : image_(std::move(image)) {}

    ~ContainerdExecutionSurface() {
        if (instance_) { (void)ctr_.destroy(*instance_); }
    }
    ContainerdExecutionSurface(ContainerdExecutionSurface const&) = delete;
    ContainerdExecutionSurface& operator=(ContainerdExecutionSurface const&) = delete;

    // Same two REAL C++ correctness findings `DockerExecutionSurface`'s own header comment documents
    // (docker_execution_surface.hpp) -- the default move constructor would leave the moved-FROM
    // `instance_` still engaged (a moved-from `std::optional` keeps `has_value()==true`, only the
    // CONTAINED value moves), so its destructor would still fire `ctr_.destroy()` against an
    // empty/moved-from `container_id`; the default move ASSIGNMENT would silently leak `this`'s own
    // previously-live container by overwriting `instance_` without destroying it first. Fixed
    // identically: reset-the-source on move-construct, swap-not-overwrite on move-assign (the same
    // "no possibly-failing destroy() call happens inside the assignment operator" reasoning
    // `docker_execution_surface.hpp` already worked out and disclosed).
    ContainerdExecutionSurface(ContainerdExecutionSurface&& other) noexcept
        : image_(std::move(other.image_)), instance_(std::move(other.instance_)),
          mounted_at_(std::move(other.mounted_at_)), seq_(other.seq_) {
        other.instance_.reset();
    }
    ContainerdExecutionSurface& operator=(ContainerdExecutionSurface&& other) noexcept {
        if (this != &other) {
            image_.swap(other.image_);
            instance_.swap(other.instance_);
            mounted_at_.swap(other.mounted_at_);
            std::swap(seq_, other.seq_);
        }
        return *this;
    }

    // reset(): destroys any previous container (if this instance already owned one -- exactly
    // DockerExecutionSurface::reset()'s own "destroy-then-recreate" shape, reused for API parity even
    // though the underlying mechanism differs completely), then starts a FRESH container bind-mounted
    // directly at host_dir. Unlike DockerExecutionSurface::reset(), there is no copy_to_container()
    // step at all -- ExecutionSurface's own "materialize host_dir's CURRENT real content into the
    // surface's own isolated view" contract is satisfied TRIVIALLY, by construction, the moment the
    // bind-mounted container starts (execution_surface.hpp's own concept comment names this exact
    // degree of freedom).
    //
    // A caller re-`reset()`ing the SAME instance at the SAME host_dir path is exactly the ordering
    // scenario `probe_bind_mount_ordering_hazard.sh` already empirically proved benign for this
    // containerd/runc/kernel combination (a real prior container still bind-mounted at that path
    // when the host recreates it, then destroyed and replaced) -- this method's own
    // destroy-old-then-create-new sequence is the SAME sequence that probe validated, not a new,
    // unverified one.
    [[nodiscard]] result<void> reset(std::filesystem::path const& host_dir) {
        if (instance_) {
            auto destroyed = ctr_.destroy(*instance_);
            if (!destroyed.has_value()) return std::unexpected(destroyed.error());
            instance_.reset();
        }
        std::filesystem::create_directories(host_dir);
        std::string const id = "ae_ces_" + std::to_string(::getpid()) + "_" + std::to_string(++seq_);
        auto inst = ctr_.create(id, host_dir, image_);
        if (!inst.has_value()) return std::unexpected(inst.error());
        instance_ = *inst;
        mounted_at_ = host_dir;
        return result<void>{};
    }

    [[nodiscard]] result<ExecOutcome> run(std::string const& command) {
        if (!instance_) {
            return std::unexpected(error{"reset() must be called before run()",
                                          "execution_surface.not_reset"});
        }
        return ctr_.exec(*instance_, "cd /workspace && " + command);
    }

    // drain_to(): when host_dir IS the same directory reset() bind-mounted (the common case, and the
    // only case this class's own accompanying probe exercises), this is a true no-op -- the bytes are
    // already there, matching execution_surface.hpp's own "this concept's own job ends at 'the bytes
    // are back on real disk'" framing vacuously. Still requires an instance to exist (same not_reset
    // contract every other verb enforces). For the general contract (draining to a DIFFERENT
    // directory than the one mounted -- NOT exercised by this pass's own probe, disclosed rather than
    // silently assumed identical), falls back to a real recursive host-side copy.
    [[nodiscard]] result<void> drain_to(std::filesystem::path const& host_dir) {
        if (!instance_) {
            return std::unexpected(error{"reset() must be called before drain_to()",
                                          "execution_surface.not_reset"});
        }
        if (host_dir == mounted_at_) return result<void>{};
        std::error_code ec;
        std::filesystem::create_directories(host_dir, ec);
        std::filesystem::copy(mounted_at_, host_dir,
                               std::filesystem::copy_options::recursive |
                                   std::filesystem::copy_options::overwrite_existing,
                               ec);
        if (ec) {
            return std::unexpected(error{"drain_to(): host-side copy from " + mounted_at_.string() +
                                              " to " + host_dir.string() + " failed: " + ec.message(),
                                          "execution_surface.drain_copy_failed"});
        }
        return result<void>{};
    }

private:
    std::string image_;
    ContainerdBackend ctr_;
    std::optional<ContainerdBackend::Instance> instance_;
    std::filesystem::path mounted_at_;
    std::uint64_t seq_ = 0;
};

static_assert(ExecutionSurface<ContainerdExecutionSurface>);

}  // namespace probe
