#pragma once
// Implements 009-Plugin-and-Extension-System.md §3 — the manifest shape. "The manifest declares,
// the operator grants" (009 §3) — this header has no load/verify/instantiate logic; that is the
// WASM component host, a seam backend with its own review cycle.

#include <cstdint>
#include <string>
#include <vector>

#include "agentengine/trust/capability.hpp"

namespace agentengine {

enum class plugin_world { tool, skill, provider, memory, filter, codec };  // ae-naming-lint: allow plugin_world — pre-existing M0 scaffolding, reconcile at owning milestone

struct PluginManifest {  // ae-naming-lint: allow PluginManifest — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string              id;
    std::string              version;   // semver
    plugin_world              world;
    std::vector<Capability>   requested_capabilities;  // request, not grant (009 §3)
    std::uint64_t             memory_bytes_limit = 0;
    std::uint64_t             wall_ms_limit = 0;
};

} // namespace agentengine
