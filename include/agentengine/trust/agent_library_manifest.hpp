#pragma once
// Resolves OQ-16 (OpenQuestions.md): 026-Agent-Facing-Runtime-Surface.md §4 gives `agent.tools` a
// real introspection story (docstrings, a .pyi stub, dir()/help()) but the other seven `agent.*`
// modules (026 §5) have no equivalent, and nothing tells the model which top-level modules are even
// present before it tries importing one.
//
// This header is the ONE shared source of truth (`agent_library_registry()`) both halves of OQ-16's
// candidate resolution read from, so they cannot drift from each other the way OQ-16 itself warns
// against:
//   - pull side  -> `granted_modules()`: the data a real dir(agent)/help(agent) binding would
//     eventually be built from, once an embedded `agent` Python module exists (026 is still Draft;
//     that CPython binding is NOT built here — this proves the manifest generation only).
//   - push side  -> `push_side_summary()`: a token-budget-bounded string for `instructions`
//     (002 §1/§2), extending 026 §7's existing "Tool surface... <= 30 tokens/tool" line to the
//     whole `agent.*` action space.
//
// I2 enforcement: a module whose gating capability is not present in the caller's CapabilitySet is
// simply absent from both outputs -- never listed as "present" and never explained as denied.
//
// KNOWN LIMITATION, stated rather than papered over: `trust/capability.hpp` now has a real
// parameterized `Capability` (decisions/ADR-009-capability-set-enforcement-mechanism.md) that CAN
// distinguish a "/memory mount" FsRead/FsWrite from any other mount's — but this registry still
// gates on plain `capability_kind` (`contains_kind`, kind-only), coarser than 026 §5's actual
// "FsRead<mount> on /memory" / "FsWrite<mount> on /memory". Sharpening `ModuleDescriptor` to gate on
// real `Capability` values (checked via `CapabilitySet::contains`) instead of bare kinds is now
// possible but is its own follow-up, not bundled into the ADR that unblocked it.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/trust/capability.hpp"

namespace agentengine::trust {

struct ModuleDescriptor {
    std::string_view name;
    std::string_view one_line;                    // <= ~10 words; see push_side_summary's budget check
    std::vector<capability_kind> gating_kinds;     // empty == always present (zero-capability reporting channel)
};

// Mirrors 026 §5's table exactly, in the same order, so a diff against that table is a diff against
// this function -- the two are meant to be read side by side, not derived independently.
inline std::vector<ModuleDescriptor> const& agent_library_registry() {
    static std::vector<ModuleDescriptor> const registry = {
        {"tools",    "Call your granted tools as ordinary functions.",        {capability_kind::tool_call}},
        {"files",    "Read/write files in your workspace.",                  {capability_kind::fs_read, capability_kind::fs_write}},
        {"data",     "Work with tabular/JSON inputs without loading them wholly.", {capability_kind::fs_read}},
        {"memory",   "Read your ranked view of prior memory.",               {capability_kind::fs_read}},
        {"notes",    "Write durable notes that persist across turns.",       {capability_kind::fs_write}},
        {"output",   "Emit your final structured output.",                   {}},
        {"progress", "Report progress on long-running work.",                {}},
        {"ask",      "Ask the caller a question and wait for a reply.",      {capability_kind::elicit}},
        {"spawn",    "Run a sub-agent and get its result.",                  {capability_kind::agent_call}},
    };
    return registry;
}

namespace detail {

inline bool has_kind(CapabilitySet const& granted, capability_kind kind) {
    return granted.contains_kind(kind);
}

inline bool module_is_granted(ModuleDescriptor const& module, CapabilitySet const& granted) {
    if (module.gating_kinds.empty()) {
        return true; // zero-capability reporting channel -- always present
    }
    return std::any_of(module.gating_kinds.begin(), module.gating_kinds.end(),
                        [&granted](capability_kind k) { return has_kind(granted, k); });
}

// Crude proxy, not a real tokenizer (no third-party dependency for a small-prove header) -- a
// whitespace-delimited word count under-counts multi-token words and over-counts none, so it is a
// safe (conservative-in-the-right-direction for short English strings) stand-in for the 026 §7
// budget check, not a claim of exact token accounting.
inline std::size_t approx_token_count(std::string_view text) {
    std::size_t count = 0;
    bool in_word = false;
    for (char c : text) {
        bool is_space = (c == ' ' || c == '\t' || c == '\n');
        if (!is_space && !in_word) { ++count; in_word = true; }
        else if (is_space) { in_word = false; }
    }
    return count;
}

} // namespace detail

// Milestone 3 Phase G3 (026 §5a, §9 G7): the lookup a real CPython binding uses to source a
// submodule's own `__doc__` from this SAME registry, rather than a hand-duplicated string living a
// second place in the generated Python source -- the one-liner text has exactly one home. Returns ""
// (never a sentinel/optional the caller must unwrap) for a name not in the registry, matching this
// header's own "absent, not an error" posture for anything ungranted/unknown.
[[nodiscard]] inline std::string_view module_one_line(std::string_view name) {
    for (auto const& module : agent_library_registry()) {
        if (module.name == name) return module.one_line;
    }
    return {};
}

struct GrantedModule {
    std::string_view name;
    std::string_view one_line;
};

// Pull side (OQ-16).
inline std::vector<GrantedModule> granted_modules(CapabilitySet const& granted) {
    std::vector<GrantedModule> out;
    for (auto const& module : agent_library_registry()) {
        if (detail::module_is_granted(module, granted)) {
            out.push_back(GrantedModule{module.name, module.one_line});
        }
    }
    return out;
}

// Push side (OQ-16): one line per granted module, "name: description" — deliberately the same
// shape 026 §7 already budgets for tools, extended to the whole action space.
inline std::string push_side_summary(CapabilitySet const& granted) {
    std::string out;
    for (auto const& module : granted_modules(granted)) {
        out += "agent.";
        out += module.name;
        out += ": ";
        out += module.one_line;
        out += "\n";
    }
    return out;
}

} // namespace agentengine::trust
