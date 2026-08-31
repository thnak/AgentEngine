#pragma once
// PROVE-PHASE PROBE, ad hoc (2026-08-28, project-owner conversation): closes the "third,
// uncoordinated tree-materialization vocabulary" gap `sandbox_backend_tree_refinement.hpp`'s own top
// banner disclosed rather than fixed -- an independent architecture-fit red-team pass on that file
// found it built `TreeCapableSandboxBackend` (refining the real, production `agentengine::
// SandboxBackend`) alongside the ALREADY-REAL, already-proven `ExecutionSurface`/`SandboxRuntime`
// track (`execution_surface.hpp`/`sandbox_runtime.hpp`) with NO reconciliation between them --
// `DockerExecutionSurface` and `DockerSandboxBackend` independently implement near-identical
// reset()/drain_to() bodies over the SAME underlying `probe::DockerBackend`, with no bridge.
//
// THE RECONCILIATION, chosen after weighing the alternatives: not merging the two concepts (rejected
// earlier in this same design conversation -- `SandboxBackend`'s handle-scoped, multi-instance shape
// and `ExecutionSurface`'s object-scoped, single-instance shape are genuinely different signatures
// for genuinely different callers, see `sandbox_backend_tree_refinement.hpp`'s own comment on
// `TreeCapableSandboxBackend`). Instead: a generic ADAPTER, `TreeBackendExecutionSurface<Backend>`,
// that drives ANY `TreeCapableSandboxBackend` conformer through the object-scoped `ExecutionSurface`
// shape by lazily minting and re-minting exactly one `SandboxHandle` internally. This makes EVERY
// current and future `TreeCapableSandboxBackend` conformer automatically usable as an
// `ExecutionSurface` -- and therefore automatically composable with `SandboxRuntime`'s real
// `Ledger`/`AsyncQuota` integration -- with zero new code needed per conformer. `DockerSandboxBackend`
// becomes, through this adapter, a SECOND real path to exactly what `DockerExecutionSurface` already
// proves, but capability-gated (real `cap::SandboxMount` authorization, ADR-099's own I2/I3 concerns)
// and handle-ownership-checked (this file's own earlier fix round) in a way `DockerExecutionSurface`
// itself is not.
//
// THIS ALSO ANSWERS one of ADR-099 §7's own still-open questions directly, empirically, for the first
// time: "whether the three-verb `ExecutionSurface` shape generalizes past Docker... unverified --
// only one [now two, with `ContainerdExecutionSurface`] conformer exists." This adapter demonstrates
// a THIRD path to `ExecutionSurface` conformance -- not a new bespoke conformer, but a GENERIC one
// that works for any `TreeCapableSandboxBackend`, meaning the shape generalizes at the
// `SandboxBackend`-refinement layer, not just at the direct-conformer layer.
//
// HONEST, DISCLOSED COST, not smoothed over: `agentengine::ExecOutcome` (the real, production,
// locked type `TreeCapableSandboxBackend::exec()` must return, sandbox.hpp §2) has NO field for a
// raw process exit code -- `sandbox_backend_tree_refinement.hpp`'s own `DockerSandboxBackend::exec()`
// already discloses this (`klass` cannot distinguish a non-zero in-container exit from a zero one
// through that conformer). `probe::ExecOutcome` (this directory's own, `common/exec_outcome.hpp`),
// which `ExecutionSurface::run()` is REQUIRED to return, DOES carry a real `int exit_code` --
// `probe_execution_surface.cpp`'s own check `r3->exec.exit_code == 7` proves callers of
// `SandboxRuntime::run()` genuinely rely on it. This adapter therefore CANNOT losslessly bridge the
// two: for an ORDINARY (`klass == ok`) outcome, `run()` below always reports `exit_code = 0`
// regardless of what actually happened inside the container, a real, structural fidelity loss, not a
// bug to be fixed later -- a caller of `SandboxRuntime::run()` that needs real exit-code fidelity
// from a Docker-shaped surface must use `DockerExecutionSurface` directly, not this adapter. The two
// are NOT fully interchangeable; this adapter trades exit-code fidelity for capability-gating and
// handle-ownership checking.
//
// UPDATE (same day, an independent red-team pass): the first version of this file went further than
// the paragraph above discloses -- it collapsed EVERY `klass` value, not just an ordinary nonzero
// exit, into that same fabricated `exit_code=0`. `agentengine::ExecOutcome::klass` can legitimately
// be `timeout`/`oom`/`crash`/`policy_violation`/`escape_attempt`/`ask_pending` while `exec()` still
// returns a VALUE (this whole design's own "a non-ok outcome is a normal result, not a `result<>`
// error" convention, execution_surface.hpp's own doc comment on `run()`) -- reporting THOSE as
// `exit_code=0` doesn't just lose precision, it makes a policy block, an OOM kill, or a timeout look
// EXACTLY like ordinary success to any caller reading `exit_code==0`. Real, not merely dormant: it
// was dormant only because `DockerSandboxBackend::exec()` today hardcodes `klass=ok` always -- this
// adapter is explicitly generic over ANY `TreeCapableSandboxBackend` conformer, and a future one
// that correctly populates a non-ok `klass` would have hit this for real.
//
// FIRST FIX ATTEMPT, ALSO WRONG, caught by a SECOND independent verification pass before landing:
// making `run()` return a real `result<>` ERROR for `klass != ok` looked like the obvious close, but
// `SandboxRuntime::run()` step 4 (sandbox_runtime.hpp) treats a `surface.run()` error as "the command
// was never even ATTEMPTED" and refunds `run_quota` accordingly -- exactly wrong for a non-ok klass,
// which DID genuinely execute and consume real resources. That would have let a principal trigger a
// non-ok outcome repeatedly and run for real, for free, every time -- the EXACT "run for free" bug
// class `RunCost` (sandbox_runtime.hpp's own header comment) exists to prevent, reopened through a
// different path by the FIRST fix for THIS finding.
//
// ACTUAL FIX: `run()` below returns a VALUE for a non-ok klass too (preserving "this was genuinely
// attempted, keep the charge" semantics), reusing `probe::ExecOutcome::exit_code`'s own documented
// default (`common/exec_outcome.hpp`: `int exit_code = -1`, i.e. "no real code available") as the
// sentinel, with the real outcome class folded into `stdout_text` as text -- the only remaining
// signal channel this adapter has for it. Neither "fabricate success" nor "fabricate never-attempted"
// -- a caller reading `exit_code == -1` learns nothing false, only that this specific conformer
// cannot represent the real outcome any more precisely than that.
//
// UPDATE (same day, same red-team pass): also found a real container-leak path in `reset()` -- the
// real, locked `SandboxBackend::destroy()` returns `void` (008 §2), so the first version of this
// file had no way to know whether a cleanup `destroy()` call actually succeeded, and unconditionally
// forgot the handle regardless -- silently orphaning a real, running Docker container on every
// transient `destroy()` failure, on every turn after the first (`SandboxRuntime::run()` calls
// `surface.reset()` unconditionally every call, so this was not a one-off edge case). FIXED:
// `TreeCapableSandboxBackend` now also requires a checkable `try_destroy()` (added to
// `sandbox_backend_tree_refinement.hpp` this same round) -- `reset()` below uses it and, on a
// confirmed failure, KEEPS the handle instead of clearing it, matching `DockerExecutionSurface`'s own
// already-proven "only untrack on confirmed success" discipline exactly (that sibling's own header
// comment) -- a discipline the void-returning `destroy()` alone structurally could not support.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "../common/result.hpp"
#include "execution_surface.hpp"
#include "sandbox_backend_tree_refinement.hpp"

namespace probe {

// Named `adapter_to_probe_error`, not `to_probe_error` -- `worktree_io/real_io_filesystem.hpp`
// (transitively pulled in by sandbox_runtime.hpp, which every real user of this adapter also
// includes) already defines a same-signature `probe::to_probe_error(agentengine::error const&)` with
// IDENTICAL behavior (message+code, verbatim, no relabeling) -- reusing that name here produced a
// real MSVC C2084 redefinition error the first time this file was written, caught by actually
// compiling it, not by inspection. A distinct name sidesteps the collision without depending on
// which header happens to be included first.
[[nodiscard]] inline probe::error adapter_to_probe_error(agentengine::error const& e) {
    return probe::error{e.message, e.code};
}

// Diagnostic-only helper for `TreeBackendExecutionSurface::run()`'s own klass-collapse fix (see that
// method's own comment) -- folds a real `exec_outcome_class` into text since `probe::ExecOutcome` has
// no field to carry it as data.
[[nodiscard]] inline char const* execution_surface_adapter_klass_name(
        agentengine::exec_outcome_class klass) {
    switch (klass) {
        case agentengine::exec_outcome_class::ok: return "ok";
        case agentengine::exec_outcome_class::timeout: return "timeout";
        case agentengine::exec_outcome_class::oom: return "oom";
        case agentengine::exec_outcome_class::crash: return "crash";
        case agentengine::exec_outcome_class::policy_violation: return "policy_violation";
        case agentengine::exec_outcome_class::escape_attempt: return "escape_attempt";
        case agentengine::exec_outcome_class::ask_pending: return "ask_pending";
    }
    return "unknown";
}

// `Backend` must satisfy `TreeCapableSandboxBackend` (sandbox_backend_tree_refinement.hpp). Drives it
// through exactly one live `SandboxHandle` at a time, minted lazily on first `reset()` and re-minted
// (destroy old, create fresh) on every SUBSEQUENT `reset()` call -- matching
// `DockerExecutionSurface::reset()`'s own already-proven "wipe any prior state, then materialize
// fresh" discipline (execution_surface.hpp's own doc comment on `ExecutionSurface::reset()`) exactly,
// not a weaker "reuse if already created" interpretation.
//
// NON-COPYABLE, NON-MOVABLE -- constructed once as a local and passed by reference into
// `SandboxRuntime::run()` (which itself takes `Surface&`, never by value), the same usage shape
// `probe_execution_surface.cpp` already establishes for `DockerExecutionSurface`. Simpler than
// re-deriving `DockerExecutionSurface`'s own two-round move-semantics fix history (this file's own
// top-of-directory precedent for exactly this bug class) for a type that never needs to move.
template <class Backend>
    requires TreeCapableSandboxBackend<Backend>
class TreeBackendExecutionSurface {
public:
    TreeBackendExecutionSurface(std::shared_ptr<Backend> backend, agentengine::SandboxSpec spec,
                                 agentengine::EffectContext ctx)
        : backend_(std::move(backend)), spec_(std::move(spec)), ctx_(std::move(ctx)) {}

    ~TreeBackendExecutionSurface() {
        if (handle_) backend_->destroy(*handle_);
    }
    TreeBackendExecutionSurface(TreeBackendExecutionSurface const&) = delete;
    TreeBackendExecutionSurface& operator=(TreeBackendExecutionSurface const&) = delete;
    TreeBackendExecutionSurface(TreeBackendExecutionSurface&&) = delete;
    TreeBackendExecutionSurface& operator=(TreeBackendExecutionSurface&&) = delete;

    [[nodiscard]] probe::result<void> reset(std::filesystem::path const& host_dir) {
        if (handle_) {
            auto destroyed = backend_->try_destroy(*handle_);
            if (!destroyed.has_value()) {
                // KEEP the handle -- do NOT clear it. The real fix for the container-leak finding
                // this file's own top banner discloses: propagate the failure and give a later
                // reset() call another real attempt at destroying the SAME handle, rather than
                // silently forgetting the only reference to a possibly-still-running container.
                return std::unexpected(adapter_to_probe_error(destroyed.error()));
            }
            handle_.reset();
        }
        auto created = backend_->create(spec_, ctx_);
        if (!created.has_value()) return std::unexpected(adapter_to_probe_error(created.error()));
        handle_ = *created;
        auto reset_r = backend_->reset(*handle_, host_dir, ctx_);
        if (!reset_r.has_value()) {
            // The handle DID get created for real (a real container exists) even though seeding it
            // failed -- attempt cleanup, but keep the handle if that ALSO fails (same discipline as
            // above) so a later reset() call gets a real retry rather than losing the only reference.
            // The tree-seed error (the primary, more informative cause) is what's returned either way.
            if (backend_->try_destroy(*handle_).has_value()) handle_.reset();
            return std::unexpected(adapter_to_probe_error(reset_r.error()));
        }
        return probe::result<void>{};
    }

    [[nodiscard]] probe::result<probe::ExecOutcome> run(std::string const& command) {
        if (!handle_) {
            return std::unexpected(probe::error{"reset() must be called before run()",
                                                   "tree_backend_execution_surface.not_reset"});
        }
        agentengine::ExecRequest req;
        req.source = command;
        auto outcome = backend_->exec(*handle_, req, ctx_);
        if (!outcome.has_value()) return std::unexpected(adapter_to_probe_error(outcome.error()));
        // SECOND FIX (this file's own top banner -- a verification pass on the FIRST fix's own
        // "return a result<> error for non-ok klass" approach found it opened a real quota-refund
        // gap): `SandboxRuntime::run()` step 4 (sandbox_runtime.hpp) treats a `surface.run()` ERROR
        // as "the command was never even ATTEMPTED" and refunds `run_quota` accordingly -- exactly
        // wrong for a non-ok klass, which DID genuinely execute and consume real resources (a
        // timeout/oom/crash/policy_violation/escape_attempt is an outcome of a real attempt, not a
        // rejection before one). Returning an error here would let a principal trigger a non-ok
        // outcome repeatedly and run for real, for free, every time -- the EXACT "run for free" bug
        // class `RunCost` (sandbox_runtime.hpp's own header comment) exists to prevent, reopened
        // through a different path. FIXED by returning a VALUE instead (preserving "this was
        // genuinely attempted, keep the charge" semantics), reusing `probe::ExecOutcome::exit_code`'s
        // own documented default (`common/exec_outcome.hpp`: `int exit_code = -1`, i.e. "no real
        // code available") as the sentinel, with the real outcome class folded into `stdout_text` --
        // the only remaining signal channel this adapter has, matching the same "stdout_text is the
        // one thing a caller of this specific conformer can still rely on" posture the ordinary
        // exit-code-fidelity-loss disclosure below already establishes for a ROUTINE nonzero exit.
        if (outcome->klass != agentengine::exec_outcome_class::ok) {
            return probe::ExecOutcome{
                /*exit_code=*/-1,
                "[TreeBackendExecutionSurface: non-ok outcome class '" +
                    std::string(execution_surface_adapter_klass_name(outcome->klass)) +
                    "', not representable as a real exit code through this adapter] " +
                    outcome->stdout_text};
        }
        // FIDELITY LOSS, disclosed in this file's own top banner: for an ORDINARY (klass==ok)
        // outcome, agentengine::ExecOutcome still carries no raw exit code, so this adapter cannot
        // report one -- always 0, regardless of what the in-container command's real exit code was.
        // A caller needing real exit-code fidelity must use DockerExecutionSurface directly.
        return probe::ExecOutcome{/*exit_code=*/0, outcome->stdout_text};
    }

    [[nodiscard]] probe::result<void> drain_to(std::filesystem::path const& host_dir) {
        if (!handle_) {
            return std::unexpected(probe::error{"reset() must be called before drain_to()",
                                                   "tree_backend_execution_surface.not_reset"});
        }
        auto drained = backend_->drain_to(*handle_, host_dir, ctx_);
        if (!drained.has_value()) return std::unexpected(adapter_to_probe_error(drained.error()));
        return probe::result<void>{};
    }

private:
    std::shared_ptr<Backend> backend_;
    agentengine::SandboxSpec spec_;
    agentengine::EffectContext ctx_;
    std::optional<agentengine::SandboxHandle> handle_;
};

static_assert(ExecutionSurface<TreeBackendExecutionSurface<DockerSandboxBackend>>,
              "TreeBackendExecutionSurface<DockerSandboxBackend> must satisfy the real, unmodified "
              "ExecutionSurface concept -- the whole point of this adapter");

}  // namespace probe
