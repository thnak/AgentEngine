#pragma once
// ADR-176 §14/§15. A gate over a provider's CONTRIBUTED tool descriptors: every tool must declare image
// provenance in its reply -- `image`, `image_digest`, `image_digest_kind`, all three required strings --
// unless its name is on an explicit, justified exemption list the caller passes in.
//
// Why this exists. ADR-176 §6 carried a residual with no test behind it: "nothing proves future callers
// go through `bound_image()`". The provider's half of the answer is structural -- `with_image_provenance()`
// (mandatory_sandbox_provider.hpp) stamps every contributed tool's reply after the body returns, so a tool
// body cannot forget to fill the fields, and a reply that declares `image` without the other two now fails
// to compile. This gate is the other half, and it reads the WIRE: `reply_schema_json` is what
// `Tool<>::reply_schema()` published and what an MCP/A2A client is actually told.
//
// THREE THINGS §15's RED-TEAM ROUND CHANGED, each of which the first version got wrong:
//
// 1. **Declaring no image at all used to PASS.** That inverted the residual. The realistic sixth tool is
//    one that runs a command in the sandbox and carries no provenance whatsoever -- it declares no
//    `image`, so the old gate skipped it and returned ok. The rarer case (declared `image`, forgot a
//    companion) was the only one caught. The default is now inverted: every tool a sandbox provider
//    contributes must carry provenance, and a tool that genuinely must not has to be NAMED in the
//    caller's exemption list. That list is a review artefact -- someone has to write down which tool is
//    exempt and why -- which is the whole point. `unknown_exemptions()` then reports names on the list
//    that no longer match a contributed tool, so the list cannot quietly rot into permission for a tool
//    nobody meant to exempt.
//
// 2. **It looked only at top-level `properties`.** `core/json_schema.hpp` emits `{"type":"array","items":
//    <fragment>}` for a `std::vector<T>` and recurses into a nested AE_JSON_SCHEMA type, so a plausible
//    `struct BatchRunReply { std::vector<RunCommandReply> runs; };` declared `image` three levels down
//    where neither this gate nor the C++ stamp could see it. The walk is recursive now, through
//    `properties` and `items`, and reports the path it found the offender at.
//
// 3. **It accepted `"type":"string"` as proof the C++ side can stamp it.** It cannot: `std::optional<T>`
//    collapses to T's fragment and `Described<T, "...">` splices a description onto it, so both publish a
//    plain string while failing the stamp's `std::same_as<std::string&>` test. The C++ side now
//    `static_assert`s instead of silently skipping, which is where that hole is actually closed; this
//    gate additionally requires all three names to appear in the schema's `"required"` array, which is
//    what an optional field would fail from the wire side.
//
// It checks SHAPE, not value. Values are checked where a real surface exists to compare against:
// test_mandatory_sandbox_provider (the stamped reply equals `bound_image()`) and
// test_execution_surface_image_identity (what the kind is, measured against an independent oracle).

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_descriptor.hpp"

namespace agentengine::test_support {

struct ProvenanceShapeResult {
    bool ok = true;
    std::string detail;  // names the offending tool and what it was missing; empty when ok
};

namespace provenance_shape_detail {

// Every property name reachable from `schema`, as a dotted path: "image", "runs[].image", and so on.
// Deliberately a flat list rather than a tree walk with a callback -- the gate needs to answer "does the
// name `image` appear ANYWHERE under this reply" and "is it required where it appears", and a path string
// is what makes a failure message point at the right place.
inline void collect_paths(agentengine::json::Value const& schema, std::string const& prefix,
                          std::vector<std::string>& out, std::vector<std::string>& required_out,
                          std::vector<std::string>& string_typed_out, int depth = 0) {
    if (depth > 8) return;  // a self-referential schema cannot be built by AE_JSON_SCHEMA, but stop anyway
    if (auto const* items = schema.find("items"); items != nullptr && items->is_object()) {
        collect_paths(*items, prefix + "[]", out, required_out, string_typed_out, depth + 1);
    }
    auto const* properties = schema.find("properties");
    if (properties == nullptr || !properties->is_object()) return;

    std::vector<std::string> required_here;
    if (auto const* required = schema.find("required"); required != nullptr && required->is_array()) {
        for (auto const& name : required->as_array()) {
            if (name.is_string()) required_here.push_back(name.as_string());
        }
    }

    for (auto const& [name, value] : properties->as_object()) {
        std::string const path = prefix.empty() ? name : prefix + "." + name;
        out.push_back(path);
        if (std::find(required_here.begin(), required_here.end(), name) != required_here.end()) {
            required_out.push_back(path);
        }
        if (value.is_object()) {
            auto const* type = value.find("type");
            if (type != nullptr && type->is_string() && type->as_string() == "string") {
                string_typed_out.push_back(path);
            }
            collect_paths(value, path, out, required_out, string_typed_out, depth + 1);
        }
    }
}

[[nodiscard]] inline bool contains(std::vector<std::string> const& haystack, std::string const& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

// The path an `image` was found at, minus the trailing name: "" for a top-level one, "runs[]" for one
// inside an array of objects. Its companions must live in the same place.
[[nodiscard]] inline std::string parent_of(std::string const& path) {
    auto const dot = path.rfind('.');
    return dot == std::string::npos ? std::string{} : path.substr(0, dot);
}

[[nodiscard]] inline std::string join(std::string const& parent, char const* leaf) {
    return parent.empty() ? std::string(leaf) : parent + "." + leaf;
}

}  // namespace provenance_shape_detail

// `tools` is a `ContextContribution::tools` vector, verbatim. `exempt` names the tools that legitimately
// carry no image provenance -- each one a deliberate, reviewable statement, not a default.
[[nodiscard]] inline ProvenanceShapeResult image_provenance_shape(
    std::vector<agentengine::ToolDescriptor> const& tools,
    std::initializer_list<std::string_view> exempt = {}) {
    for (auto const& tool : tools) {
        bool const is_exempt =
            std::find(exempt.begin(), exempt.end(), std::string_view(tool.name)) != exempt.end();
        if (is_exempt) continue;

        if (tool.reply_schema_json.empty()) {
            // Publishing no schema at all is not a provenance violation -- four in-tree providers build
            // descriptors by hand and set no reply schema. Non-empty garbage below still fails.
            continue;
        }
        auto parsed = agentengine::json::parse(tool.reply_schema_json);
        if (!parsed) {
            return {false, "tool `" + tool.name + "`: its reply schema is not empty and does not parse "
                                                  "as JSON"};
        }

        std::vector<std::string> paths;
        std::vector<std::string> required;
        std::vector<std::string> string_typed;
        provenance_shape_detail::collect_paths(*parsed, "", paths, required, string_typed);

        // Every place an `image` appears -- top level or nested -- must carry its two companions, and all
        // three must be required. A nested one matters as much: an element of a batch reply that names an
        // image without a digest is the same false claim, one level down.
        std::vector<std::string> image_sites;
        for (auto const& path : paths) {
            if (path == "image" || (path.size() > 6 && path.compare(path.size() - 6, 6, ".image") == 0)) {
                image_sites.push_back(path);
            }
        }
        if (image_sites.empty()) {
            return {false,
                    "tool `" + tool.name +
                        "`: its reply declares NO image provenance at all. Every tool a sandbox provider "
                        "contributes runs somewhere, and ADR-176 §14 makes that attributable by default; "
                        "if this one genuinely must not say where, name it in the exemption list at the "
                        "call site, with a reason"};
        }
        for (auto const& site : image_sites) {
            std::string const parent = provenance_shape_detail::parent_of(site);
            for (char const* companion : {"image_digest", "image_digest_kind"}) {
                std::string const expected = provenance_shape_detail::join(parent, companion);
                if (!provenance_shape_detail::contains(paths, expected)) {
                    return {false, "tool `" + tool.name + "`: `" + site + "` has no `" + expected +
                                       "` beside it -- a reference with no resolved identity"};
                }
                if (!provenance_shape_detail::contains(required, expected)) {
                    return {false, "tool `" + tool.name + "`: `" + expected +
                                       "` is not REQUIRED, so it is optional or wrapped on the wire -- a "
                                       "shape that publishes a string and silently defeats "
                                       "`stamp_image_provenance()`"};
                }
                if (!provenance_shape_detail::contains(string_typed, expected)) {
                    return {false, "tool `" + tool.name + "`: `" + expected +
                                       "` is not a string on the wire, so `stamp_image_provenance()` "
                                       "cannot fill it and a consumer cannot read it"};
                }
            }
            if (!provenance_shape_detail::contains(required, site) ||
                !provenance_shape_detail::contains(string_typed, site)) {
                return {false, "tool `" + tool.name + "`: `" + site +
                                   "` is not a REQUIRED string on the wire"};
            }
        }
    }
    return {};
}

// Names on an exemption list that no contributed tool answers to. A stale exemption is how a list like
// this turns into blanket permission: a tool gets renamed, its entry stops matching, and the entry sits
// there looking like someone thought about it.
[[nodiscard]] inline std::vector<std::string> unknown_exemptions(
    std::vector<agentengine::ToolDescriptor> const& tools,
    std::initializer_list<std::string_view> exempt) {
    std::vector<std::string> stale;
    for (auto const& name : exempt) {
        auto const matches = [&](agentengine::ToolDescriptor const& t) { return t.name == name; };
        if (std::find_if(tools.begin(), tools.end(), matches) == tools.end()) {
            stale.emplace_back(name);
        }
    }
    return stale;
}

}  // namespace agentengine::test_support
