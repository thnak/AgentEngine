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
// No thread-safety: intended for the same single-threaded, process-scoped "shared function-local
// static" idiom `tools/cli_chat.cpp` already uses for `shared_python_runner()`/`shared_exec_state()` --
// this type itself is agnostic to how a caller shares one instance across a tool's `invoke()` and a
// later turn's `ContextProvider::on_context()`; it does not impose that idiom, just doesn't preclude it.

#include <algorithm>
#include <string>
#include <vector>

namespace agentengine {

class MountedSkillsState {
public:
    // Idempotent: mounting an already-mounted skill is a no-op, never an error -- the agent may
    // reasonably call mount_skill again for a skill it's unsure about the state of.
    void mount(std::string name) {
        if (!is_mounted(name)) mounted_.push_back(std::move(name));
    }

    [[nodiscard]] bool is_mounted(std::string const& name) const noexcept {
        return std::ranges::find(mounted_, name) != mounted_.end();
    }

    [[nodiscard]] std::vector<std::string> const& all() const noexcept { return mounted_; }

private:
    std::vector<std::string> mounted_;
};

}  // namespace agentengine
