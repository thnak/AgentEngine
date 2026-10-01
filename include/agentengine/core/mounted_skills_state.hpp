#pragma once
// Implements 009-Plugin-and-Extension-System.md §8c's Phase 3 addendum (decisions/ADR-024) --
// on-demand, agent-triggered skill mounting. Distinguishes "resolved" (every configured skill's files
// are materialized unconditionally at session start, core/skill_provider.hpp's own scope, §8b) from
// "mounted" (a SUBSET of resolved skills the agent has explicitly activated this run, tracked here).
//
// Deliberately a plain, ordinary mutable set -- not a cryptographic token, not a capability. Mounting a
// skill grants NO new authority (I3: a model-triggered call can never mint authority it wasn't already
// pre-provisioned) -- the underlying `cap::FsRead` for every resolved skill's files is already granted
// unconditionally by the operator, and stays that way regardless of mount state (see
// skill_provider.hpp's own top comment: "resolved" and "mounted" are independent concerns). What
// mounting actually gates is narrower and purely informational/visibility-shaped: which tools get
// declared to the model and accepted by `invoke_tool` (`core/skill_tool_scoping.hpp`), and which
// skills' full body gets re-injected into context. Both of those are already-authorized capabilities
// the agent is merely CHOOSING to activate, not new ones it is granting itself.
//
// PROVENANCE (decisions/ADR-224): every mount records WHO made it -- `skill_mount_origin::model` (the
// model's own `mount_skill` call, `mount()`) or `skill_mount_origin::host` (host code acting on its own
// configuration before the run, `mount_by_host()`, see core/skill_premount.hpp). The two entry points
// are separate methods rather than one method with an origin argument so that a `mount_skill` tool
// implementation cannot pass `host` by accident: the model-reachable path names no origin at all.
// The FIRST mount of a name fixes its origin; a later mount of the same name by either party is a
// no-op and does not rewrite it -- so a model re-mounting a host-mounted skill cannot make it look
// model-earned, and nothing can make a model mount look host-made after the fact. Provenance is
// attribution only: nothing may read it to decide a permission (I3), since mount state itself grants
// none.
//
// No thread-safety: intended for the same single-threaded, process-scoped "shared function-local
// static" idiom `tools/cli_chat.cpp` already uses for `shared_python_runner()`/`shared_exec_state()` --
// this type itself is agnostic to how a caller shares one instance across a tool's `invoke()` and a
// later turn's `ContextProvider::on_context()`; it does not impose that idiom, just doesn't preclude it.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agentengine {

// ADR-224: who mounted a skill. Attribution only -- never an input to a permission decision.
enum class skill_mount_origin : std::uint8_t { model, host };

// ae-naming-lint: allow MountedSkillsState — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class MountedSkillsState {
public:
    // The model-reachable path: what a `mount_skill` tool implementation calls. Idempotent: mounting an
    // already-mounted skill is a no-op, never an error -- the agent may reasonably call mount_skill
    // again for a skill it's unsure about the state of. Records `skill_mount_origin::model` for a NEW
    // mount only.
    void mount(std::string name) { add(std::move(name), skill_mount_origin::model); }

    // Host-only (ADR-224): host code mounting a skill from its own configuration, never from anything a
    // model said. Same idempotence as `mount()`; records `skill_mount_origin::host` for a NEW mount only.
    void mount_by_host(std::string name) { add(std::move(name), skill_mount_origin::host); }

    [[nodiscard]] bool is_mounted(std::string const& name) const noexcept {
        return std::ranges::find(mounted_, name) != mounted_.end();
    }

    // Who mounted `name`, or nullopt when it is not mounted.
    [[nodiscard]] std::optional<skill_mount_origin> origin_of(std::string const& name) const noexcept {
        auto const it = std::ranges::find(mounted_, name);
        if (it == mounted_.end()) return std::nullopt;
        return origins_[static_cast<std::size_t>(it - mounted_.begin())];
    }

    // In mount order.
    [[nodiscard]] std::vector<std::string> const& all() const noexcept { return mounted_; }

private:
    void add(std::string name, skill_mount_origin origin) {
        if (is_mounted(name)) return;
        mounted_.push_back(std::move(name));
        origins_.push_back(origin);
    }

    std::vector<std::string> mounted_;
    std::vector<skill_mount_origin> origins_;  // parallel to mounted_
};

}  // namespace agentengine
