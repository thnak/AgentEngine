#pragma once
// Test helper for decisions/ADR-244-required-choices-step-1b.md §2.4 (G-C4): `authorize_spec()` checks every
// host-path mount unconditionally, so a test that mounts a path must also grant it. This grants exactly the
// mounts the spec lists -- each one, at its own host and guest path and polarity -- and nothing else. It
// REPLACES `spec.capabilities`; a test that needs other capabilities too builds the set itself.

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/sandbox/sandbox.hpp"
#include "agentengine/trust/capability.hpp"

namespace ae_test {

inline void grant_listed_mounts(agentengine::SandboxSpec& spec) {
    std::vector<agentengine::Capability> grants;
    for (agentengine::MountSpec const& mount : spec.mounts) {
        if (auto const* host = std::get_if<std::string>(&mount.source)) {
            grants.push_back(agentengine::cap::SandboxMount{*host, mount.guest_path, mount.read_write});
        }
    }
    spec.capabilities = agentengine::CapabilitySet::grant_root(std::move(grants));
}

}  // namespace ae_test
