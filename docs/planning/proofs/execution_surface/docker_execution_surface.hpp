#pragma once
// PROVE-PHASE PROBE (A3): the one real `ExecutionSurface` conformer this design builds and proves --
// wraps the already-proven, already-fixed (§35 finding 9) `probe::DockerBackend` (docker_sandbox/
// docker_backend.hpp) behind the fresh `ExecutionSurface` concept, using `docker cp`'s own native
// whole-directory copy (source/dest path ending in `/.` copies CONTENTS, matching `cp -a src/.
// dst`'s own well-known convention) instead of a hand-rolled per-file loop -- fewer real subprocess
// invocations, and no chance of missing a nested directory the way a naive flat-file loop could.

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "../common/result.hpp"
#include "../docker_sandbox/docker_backend.hpp"
#include "execution_surface.hpp"

namespace probe {

// HONEST RESIDUAL a security-shaped red-team pass named (not closed here): containers are started
// via `docker run -d --rm ... sleep infinity` -- `--rm` fires on the container's own exit, which
// `sleep infinity` never triggers on its own. If the hosting process crashes/aborts between a
// successful `reset()` and this destructor running, the container is orphaned and keeps running
// indefinitely, with no id persisted anywhere and no reclaim mechanism analogous to the Ledger's
// own `orphaned_branches()`/A7 design. Real, but requires an actual process crash (not a normal
// exit path) -- disclosed here rather than solved, matching this document's own established
// "necessary, not sufficient" posture for gaps that would need real new machinery (a persisted
// container-id ledger + a reclaim sweep) to close properly.
class DockerExecutionSurface {
public:
    explicit DockerExecutionSurface(std::string image = "alpine:latest") : image_(std::move(image)) {}

    ~DockerExecutionSurface() {
        if (instance_) { (void)docker_.destroy(*instance_); }
    }
    DockerExecutionSurface(DockerExecutionSurface const&) = delete;
    DockerExecutionSurface& operator=(DockerExecutionSurface const&) = delete;

    // REAL FINDINGS a C++ correctness red-team pass caught (both would have been silent, real
    // resource leaks -- neither is hypothetical): the default move constructor leaves the
    // moved-FROM `instance_` still engaged (`std::optional`'s own move-construction semantics only
    // move the CONTAINED value, `has_value()` is unchanged), so the moved-from object's destructor
    // still fires `docker_.destroy()` -- with a moved-from (empty) `container_id`, producing a
    // malformed, silently-swallowed `docker rm -f ` call instead of a clean no-op. The default move
    // ASSIGNMENT is worse: `std::optional::operator=(optional&&)` when BOTH sides already hold a
    // value does a plain member-wise assignment, never destroying `this`'s own previous container
    // first -- `a = std::move(b)` where `a` already owns a live container silently overwrites
    // `a`'s own `instance_`, permanently leaking that container with no code path left that can
    // ever clean it up.
    DockerExecutionSurface(DockerExecutionSurface&& other) noexcept
        : image_(std::move(other.image_)), docker_(std::move(other.docker_)),
          instance_(std::move(other.instance_)) {
        other.instance_.reset();
    }
    // REAL FINDING an independent round-2 verification pass caught, reintroducing finding 3's own
    // "clear the reference before confirming destroy() succeeded" bug at a DIFFERENT call site: a
    // first attempt at this fix called `docker_.destroy(*instance_)` here too but discarded its
    // result via `(void)` before unconditionally overwriting `instance_` with `other`'s state -- a
    // transient `destroy()` failure (the exact scenario finding 3's own fix was written to handle)
    // would silently lose `this`'s original container reference just as before, only reached via
    // `operator=` instead of `reset()`. Fixed properly this time by SWAPPING rather than
    // overwriting-then-discarding: no possibly-failing `destroy()` call happens INSIDE the
    // assignment operator at all, so there is no failure path here left to mishandle. `other` ends
    // up owning whatever `this` used to own; when `other` (almost always an about-to-be-destroyed
    // moved-from temporary) itself goes out of scope, its OWN already-correct destructor destroys
    // it the same way it always does for any object going out of scope -- no new failure mode, and
    // no reference is ever discarded without at least one real attempt to destroy it.
    //
    // NOTE (round-3 verification, not a bug): `other` deliberately ends up holding what `this` used
    // to own, unlike the move CONSTRUCTOR above which resets the source to empty -- that asymmetry
    // is required, not accidental: `other`'s own destructor is what performs the actual cleanup
    // this operator itself no longer attempts, so `other` must stay non-empty. Also swaps `docker_`
    // (currently a no-op -- `DockerBackend` holds no per-instance state today) for forward safety,
    // so this stays correct if that ever changes.
    DockerExecutionSurface& operator=(DockerExecutionSurface&& other) noexcept {
        if (this != &other) {
            image_.swap(other.image_);
            std::swap(docker_, other.docker_);  // DockerBackend has no `.swap()` member -- the free
                                                    // function works via its (implicit, trivial)
                                                    // move assignment, which is all this needs
            instance_.swap(other.instance_);
        }
        return *this;
    }

    // REAL FINDING a C++ correctness red-team pass caught: this used to clear `instance_`
    // UNCONDITIONALLY even when `docker_.destroy()` itself failed -- a transient failure (daemon
    // contention, a network hiccup) meant the only in-process reference to a possibly-still-running
    // container was discarded right before returning the error, permanently orphaning it with
    // nothing left that could ever attempt to clean it up again. Fixed: `instance_` is only cleared
    // once `destroy()` has actually succeeded; a failed destroy leaves it in place, so the object
    // still remembers the leak, and a caller retrying `reset()` gets another real attempt at
    // destroying the SAME container rather than silently starting a second one alongside it.
    [[nodiscard]] result<void> reset(std::filesystem::path const& host_dir) {
        if (instance_) {
            auto destroyed = docker_.destroy(*instance_);
            if (!destroyed.has_value()) return std::unexpected(destroyed.error());
            instance_.reset();
        }
        auto inst = docker_.create(image_);
        if (!inst.has_value()) return std::unexpected(inst.error());
        instance_ = *inst;

        if (!std::filesystem::exists(host_dir)) return result<void>{};  // nothing to seed yet
        // Trailing "/." copies host_dir's CONTENTS into /workspace (which create() already made),
        // not host_dir itself as a nested subdirectory -- constructing the path via string
        // concatenation (not operator/) so the literal "/." survives to docker cp's own argument,
        // rather than being collapsed by any lexical-normalization step. `generic_string()`, not
        // `string()`, so the result uses forward slashes throughout even on Windows -- a C++
        // correctness red-team pass flagged `string()` + a literal "/." as producing a
        // mixed-separator path (native backslashes + one trailing forward slash) it could not rule
        // out as fragile against Docker CLI's own path normalization on every host path shape.
        std::filesystem::path const source(host_dir.generic_string() + "/.");
        auto copied = docker_.copy_to_container(*instance_, source, "/workspace");
        if (!copied.has_value()) return std::unexpected(copied.error());
        return result<void>{};
    }

    [[nodiscard]] result<ExecOutcome> run(std::string const& command) {
        if (!instance_) {
            return std::unexpected(error{"reset() must be called before run()",
                                          "execution_surface.not_reset"});
        }
        auto r = docker_.exec(*instance_, "cd /workspace && " + command);
        if (!r.has_value()) return std::unexpected(r.error());
        return ExecOutcome{r->exit_code, r->stdout_text};
    }

    [[nodiscard]] result<void> drain_to(std::filesystem::path const& host_dir) {
        if (!instance_) {
            return std::unexpected(error{"reset() must be called before drain_to()",
                                          "execution_surface.not_reset"});
        }
        std::filesystem::create_directories(host_dir);
        // Same "/." convention in the other direction: copies /workspace's CONTENTS onto host_dir,
        // not a nested "workspace" subdirectory inside it.
        auto copied = docker_.copy_from_container(*instance_, "/workspace/.", host_dir);
        if (!copied.has_value()) return std::unexpected(copied.error());
        return result<void>{};
    }

private:
    std::string image_;
    DockerBackend docker_;
    std::optional<DockerBackend::Instance> instance_;
};

static_assert(ExecutionSurface<DockerExecutionSurface>);

}  // namespace probe
