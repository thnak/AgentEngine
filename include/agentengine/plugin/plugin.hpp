#pragma once
// Implements rfcs/009-Plugin-and-Extension-System.md §3 — the manifest shape. "The manifest declares,
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
    // ADR-244 slice 5: stated, not defaulted. A manifest says what the plugin is held to, or `unbounded()`; an
    // omitted field does not compile and `0` is not "no limit" (`check_bound` refuses `at_most(0)`).
    cap::Limit                memory_bytes_limit;
    cap::Limit                wall_ms_limit;
};

} // namespace agentengine
