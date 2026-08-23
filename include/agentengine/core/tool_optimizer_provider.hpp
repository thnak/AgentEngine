#pragma once
// Implements docs/planning/tool-optimizer-provider-design-draft.md (issue #15) and
// decisions/ADR-065-tool-optimizer-provider.md -- an ordinary composite ContextProvider that gates
// MCP/WASM-plugin/agent tool exposure the same way core/skill_tool_scoping.hpp already gates skill
// tools: a small always-on surface plus three always-on, zero-capability management tools
// (search_tools/mount_tool/unmount_tool) to grow it on demand, agent-driven, mid-run.
//
// NOT a reopening of OpenQuestions.md's OQ-18 (already resolved): this composes raw ToolDescriptor
// SOURCES directly, as plain function calls inside its own on_context() -- never another
// ContextProvider's ContextContribution -- the exact ComposedContextProvider idiom OQ-18's own
// resolution prescribes for a real reactive need, not the rejected generic chained-provider seam
// (context_assembly.hpp's own file-top comment). Confirmed against the real MAF source this
// session re-read (D:\GitSrc\agent-framework's AIContextProvider.cs:174-176,
// WithAgentRequestMessageSource, and its own InvokingContext.AIContext doc comment on chaining) --
// OQ-18's citations hold.
//
// Two real divergences from mount_skill's own precedent, named here rather than left implicit:
//   - search_tools has NO precedent -- 009 §8b explicitly names "advertise cheaply, load lazily, not
//     search" as this codebase's (and MAF's) prior answer. Implemented here as a cheap, deliberately
//     non-semantic substring/keyword heuristic over name+description, not embedding search --
//     matching that spirit rather than importing a new ML dependency for a ranking need this small.
//   - unmount_tool has NO precedent -- mount_skill has no unmount counterpart, and ADR-024 §8 names
//     "no expiry/unmount within a run" as an open, never-closed residual for skills specifically.
//     Closed HERE, for this mechanism only -- core/mounted_skills_state.hpp is untouched.
//
// THE INTEGRATION INVARIANT (skill_tool_scoping.hpp's own warning, inherited verbatim): whatever
// ToolTable a caller declares to the model must be the SAME table -- recomputed from the SAME
// mounted_ state, at the SAME cadence -- as whatever table authorizes invoke_tool()/CodeAct's own
// bridge. agent_session.hpp's run_rounds() already builds exactly one ToolTable per turn from
// on_context()'s own output and reuses it for every invoke_tool() call that turn (ADR-024 §8 proved
// this closes the "hidden but still callable" defect class for skills) -- so as long as
// ToolOptimizerProvider::on_context() is the SOLE source feeding a caller's declared tools, this
// class inherits that invariant for free. A caller that ALSO feeds a separate CodeAct-bridge union
// (core/codeact_tool_union.hpp) must source that union's mcp_tools/wasm_tools arguments from THIS
// SAME instance's currently_scoped_tools() (below), never from an independently-refetched universe --
// getting this wrong reopens exactly the defect class this comment warns about.

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/codeact_tool_union.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/skill_tool_scoping.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine {

// ---- The three always-on, zero-capability management tools ------------------------------------
// Same trust shape as MountSkillTool (tools/cli_chat.cpp:311-330): Tool<T, EffectClass<pure>>, no
// Capabilities<...> -- these grant no new capability, they only move the visibility window over an
// already-authorized set (009 §8c's own documented mount_skill property, inherited here). invoke()
// is a dead poison stub; real logic lives in ToolOptimizerProvider's own member functions, reached
// via make_tool_descriptor_with_invoke()'s closure -- MountSkillTool's exact pattern.

// ae-naming-lint: allow SearchToolsArgs — mechanical request DTO for the vocabularied SearchToolsTool (027 §3); not itself a distinct concept.
struct SearchToolsArgs {
    std::string query;
};
AE_JSON_SCHEMA(SearchToolsArgs, query)

// ae-naming-lint: allow SearchToolsReply — mechanical reply DTO for the vocabularied SearchToolsTool (027 §3); not itself a distinct concept.
struct SearchToolsReply {
    std::vector<std::string> names;
};
AE_JSON_SCHEMA(SearchToolsReply, names)

struct SearchToolsTool : Tool<SearchToolsTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "search_tools";
    static constexpr std::string_view description =
        "Search the full universe of tools available this run (including ones not currently "
        "mounted) by name/description keyword match. Read-only -- does not mount anything. Call "
        "mount_tool with a name from the results before you can actually invoke it.";
    using Args = SearchToolsArgs;
    using Reply = SearchToolsReply;
    static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(error{failure_class::fatal,
                                      "SearchToolsTool::invoke() must never run directly -- reached "
                                      "only through ToolOptimizerProvider::real_search_tools()",
                                      "tool_optimizer.dead_static_invoke_path"});
    }
};

// ae-naming-lint: allow MountToolArgs — mechanical request DTO for the vocabularied MountToolTool (027 §3); not itself a distinct concept.
struct MountToolArgs {
    std::string name;
};
AE_JSON_SCHEMA(MountToolArgs, name)

// ae-naming-lint: allow MountToolReply — mechanical reply DTO for the vocabularied MountToolTool (027 §3); not itself a distinct concept.
struct MountToolReply {
    bool ok = false;
    std::string message;
};
AE_JSON_SCHEMA(MountToolReply, ok, message)

struct MountToolTool : Tool<MountToolTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "mount_tool";
    static constexpr std::string_view description =
        "Activate a tool you've seen from search_tools (or already know the name of), so it becomes "
        "callable -- starting next turn. Grants no new capability: the tool must already be "
        "pre-authorized by the operator; this only moves the visibility window over it.";
    using Args = MountToolArgs;
    using Reply = MountToolReply;
    static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(error{failure_class::fatal,
                                      "MountToolTool::invoke() must never run directly -- reached "
                                      "only through ToolOptimizerProvider::real_mount_tool()",
                                      "tool_optimizer.dead_static_invoke_path"});
    }
};

// ae-naming-lint: allow UnmountToolArgs — mechanical request DTO for the vocabularied UnmountToolTool (027 §3); not itself a distinct concept.
struct UnmountToolArgs {
    std::string name;
};
AE_JSON_SCHEMA(UnmountToolArgs, name)

// ae-naming-lint: allow UnmountToolReply — mechanical reply DTO for the vocabularied UnmountToolTool (027 §3); not itself a distinct concept.
struct UnmountToolReply {
    bool ok = false;
    std::string message;
};
AE_JSON_SCHEMA(UnmountToolReply, ok, message)

struct UnmountToolTool : Tool<UnmountToolTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "unmount_tool";
    static constexpr std::string_view description =
        "Deactivate a currently-mounted tool, shrinking your own declared tool surface back down -- "
        "starting next turn. An always-on tool cannot be unmounted.";
    using Args = UnmountToolArgs;
    using Reply = UnmountToolReply;
    static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(error{failure_class::fatal,
                                      "UnmountToolTool::invoke() must never run directly -- reached "
                                      "only through ToolOptimizerProvider::real_unmount_tool()",
                                      "tool_optimizer.dead_static_invoke_path"});
    }
};

// A closure fetching a tool source's currently-available ToolDescriptors, fresh, every on_context()
// call -- never cached across turns, so a server that connects/disconnects mid-run, or changes its
// own exposed tools, is reflected the same turn (the declare/invoke cadence this whole class exists
// to preserve). Wraps e.g. protocol/mcp/mcp_tool_bridge.hpp's mcp_tools_as_descriptors(client) or
// src/backends/wasm/wasm_tool_bridge.hpp's wasm_tools_as_descriptors_from(backend, handle, id, ctx)
// -- a caller with no MCP/plugin connection passes no_tool_source() below.
using ToolSourceFetch = std::function<result<std::vector<ToolDescriptor>>(EffectContext&)>;

[[nodiscard]] inline ToolSourceFetch no_tool_source() {
    return [](EffectContext&) -> result<std::vector<ToolDescriptor>> {
        return std::vector<ToolDescriptor>{};
    };
}

class ToolOptimizerProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3: required for this conformer
    // to be composed via ComposedContextProvider<Ms...> (HasContextProviderName)
    // -- missed when this class first landed (ADR-065), caught by a full-tree rebuild against
    // ADR-066's own requirement.
    static constexpr std::string_view name = "tool_optimizer";  // ae-naming-lint: allow name — ADR-033's HasMiddlewareName precedent, reused verbatim per ADR-066 §3

    ToolOptimizerProvider(ToolTable agent_tools, ToolSourceFetch mcp_tools_fetch,
                           ToolSourceFetch plugin_tools_fetch, std::vector<std::string> always_on = {})
        : agent_tools_(std::move(agent_tools)),
          mcp_tools_fetch_(std::move(mcp_tools_fetch)),
          plugin_tools_fetch_(std::move(plugin_tools_fetch)),
          always_on_(std::move(always_on)) {}

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext& ctx) {
        auto universe = build_universe(ctx);
        if (!universe) co_return std::unexpected(universe.error());

        ToolTable const scoped = scope_tools_to_mounted_skills(*universe, mounted_, always_on_);
        ContextContribution contribution;
        contribution.tools = scoped.descriptors();

        contribution.tools.push_back(make_tool_descriptor_with_invoke<SearchToolsTool>(
            [this](SearchToolsArgs a, EffectContext& c) { return real_search_tools(std::move(a), c); }));
        contribution.tools.push_back(make_tool_descriptor_with_invoke<MountToolTool>(
            [this](MountToolArgs a, EffectContext& c) { return real_mount_tool(std::move(a), c); }));
        contribution.tools.push_back(make_tool_descriptor_with_invoke<UnmountToolTool>(
            [this](UnmountToolArgs a, EffectContext& c) { return real_unmount_tool(std::move(a), c); }));
        co_return contribution;
    }

    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

    // Exposed for a caller that ALSO needs to feed union_codeact_tools's mcp_tools/wasm_tools
    // parameters for a CodeAct bridge -- MUST be sourced from here, recomputed the same way, never
    // independently refetched, per this file's own top-comment integration invariant (§3 of the
    // design draft).
    [[nodiscard]] result<ToolTable> currently_scoped_tools(EffectContext& ctx) {
        auto universe = build_universe(ctx);
        if (!universe) return std::unexpected(universe.error());
        return scope_tools_to_mounted_skills(*universe, mounted_, always_on_);
    }

private:
    [[nodiscard]] static bool is_management_tool_name(std::string const& name) {
        return name == std::string(SearchToolsTool::name) || name == std::string(MountToolTool::name) ||
               name == std::string(UnmountToolTool::name);
    }

    // Reuses union_codeact_tools's already-tested cross-source collision rejection (empty
    // skill-unlocked-tools slot -- skills stay SkillsProvider's own concern, never this class's)
    // rather than reimplementing dedup logic, then additionally rejects any source tool that shadows
    // one of THIS provider's own reserved management-tool names.
    [[nodiscard]] result<ToolTable> build_universe(EffectContext& ctx) {
        auto mcp = mcp_tools_fetch_(ctx);
        if (!mcp) return std::unexpected(mcp.error());
        auto plugin = plugin_tools_fetch_(ctx);
        if (!plugin) return std::unexpected(plugin.error());

        auto universe = union_codeact_tools(agent_tools_, ToolTable::from_tools<>(), *mcp, *plugin);
        if (!universe) return std::unexpected(universe.error());

        for (ToolDescriptor const& d : universe->descriptors()) {
            if (is_management_tool_name(d.name)) {
                return std::unexpected(error{
                    failure_class::contract,
                    "tool '" + d.name +
                        "' collides with one of ToolOptimizerProvider's own always-on management "
                        "tool names",
                    "tool_optimizer.reserved_name_collision"});
            }
        }
        return universe;
    }

    [[nodiscard]] static std::string to_lower(std::string s) {
        std::ranges::transform(s, s.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Splits on whitespace, dropping empty tokens (repeated spaces, leading/trailing whitespace).
    [[nodiscard]] static std::vector<std::string> split_words(std::string const& lower) {
        std::vector<std::string> words;
        std::string word;
        for (char c : lower) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!word.empty()) { words.push_back(std::move(word)); word.clear(); }
            } else {
                word += c;
            }
        }
        if (!word.empty()) words.push_back(std::move(word));
        return words;
    }

    // Token-overlap match, not a whole-query substring match: a live model run against a multi-tool
    // pool showed this heuristic's earlier whole-phrase form failing every natural multi-word query
    // ("code execution compute calculation", "python calculator math", "run code script compute
    // liters", ...) for a tool whose name/description never happened to contain that exact phrase
    // verbatim, causing the model to give up searching for a tool that was in fact present. Matching
    // if ANY query word appears in a tool's name+description is what "search" is actually expected to
    // mean; a single-word query (this class's own R4 test, tests/test_tool_optimizer_provider.cpp)
    // behaves identically either way, so this is a strict widening, not a behavior change for it.
    [[nodiscard]] result<SearchToolsReply> real_search_tools(SearchToolsArgs args, EffectContext& ctx) {
        auto universe = build_universe(ctx);
        if (!universe) return std::unexpected(universe.error());
        std::vector<std::string> const words = split_words(to_lower(args.query));
        std::vector<std::string> names;
        for (ToolDescriptor const& d : universe->descriptors()) {
            std::string const haystack = to_lower(d.name) + " " + to_lower(d.description);
            bool const matches = words.empty() ||
                std::ranges::any_of(words, [&](std::string const& w) {
                    return haystack.find(w) != std::string::npos;
                });
            if (matches) names.push_back(d.name);
        }
        return SearchToolsReply{std::move(names)};
    }

    [[nodiscard]] result<MountToolReply> real_mount_tool(MountToolArgs args, EffectContext& ctx) {
        auto universe = build_universe(ctx);
        if (!universe) return std::unexpected(universe.error());
        if (universe->find(args.name) == nullptr) {
            return std::unexpected(error{failure_class::contract, "unknown tool: " + args.name,
                                          "tool_optimizer.unknown_name"});
        }
        if (std::ranges::find(mounted_, args.name) == mounted_.end()) mounted_.push_back(args.name);
        return MountToolReply{true, "mounted: " + args.name};
    }

    [[nodiscard]] result<UnmountToolReply> real_unmount_tool(UnmountToolArgs args, EffectContext&) {
        if (std::ranges::find(always_on_, args.name) != always_on_.end()) {
            return std::unexpected(error{failure_class::contract,
                                          "'" + args.name + "' is always-on and cannot be unmounted",
                                          "tool_optimizer.cannot_unmount_always_on"});
        }
        auto it = std::ranges::find(mounted_, args.name);
        if (it != mounted_.end()) mounted_.erase(it);
        return UnmountToolReply{true, "unmounted: " + args.name};
    }

    ToolTable agent_tools_;
    ToolSourceFetch mcp_tools_fetch_;
    ToolSourceFetch plugin_tools_fetch_;
    std::vector<std::string> always_on_;
    std::vector<std::string> mounted_;
};

static_assert(ContextProvider<ToolOptimizerProvider>,
              "ToolOptimizerProvider must satisfy ContextProvider (005 §5)");

}  // namespace agentengine
