// Proves M2 Phase B task B1: Tool<Derived, Policies...>'s compile-time JSON-Schema-shape
// derivation from nested Args/Reply types (006 §1). Checks:
//   - primitive field types map to the right JSON Schema "type"
//   - std::optional<T> fields are excluded from "required", everything else is included
//   - std::vector<T> maps to {"type":"array","items":<T's fragment>}
//   - a nested AE_JSON_SCHEMA-described struct recurses correctly (not flattened, not stringified)
//   - Tool<Derived>::args_schema()/reply_schema() route to exactly the same schema as calling
//     schema::json_schema_of<T>() directly -- no separate, divergent code path
//
// Uses nlohmann::json (already a test-only dependency, tests/CMakeLists.txt) to parse the emitted
// string and assert on real structure, not on substring matching -- a schema that's merely
// "textually plausible" but not valid JSON would pass a substring check and fail every real
// consumer (MCP client, JSON Schema validator).

#include <nlohmann/json.hpp>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"

using agentengine::schema::json_schema_of;
using json = nlohmann::json;

namespace {

int g_failures = 0;

void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        // GTest-free harness (project convention, see other M0/M1/M2 tests): print and keep going.
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// -- fixtures -------------------------------------------------------------------------------

struct SearchArgs {
    std::string query;
    int max_results;
    std::optional<std::string> after_cursor;
};
AE_JSON_SCHEMA(SearchArgs, query, max_results, after_cursor)

struct Hit {
    std::string url;
    double score;
};
AE_JSON_SCHEMA(Hit, url, score)

struct SearchReply {
    std::vector<Hit> hits;
    bool truncated;
};
AE_JSON_SCHEMA(SearchReply, hits, truncated)

struct WebSearchTool : agentengine::Tool<WebSearchTool> {
    static constexpr std::string_view name = "web_search";
    using Args = SearchArgs;
    using Reply = SearchReply;
};

}  // namespace

int main() {
    // -- primitive fields + required/optional split --------------------------------------------
    json args_schema = json::parse(json_schema_of<SearchArgs>());
    check(args_schema["type"] == "object", "SearchArgs schema type is object");
    check(args_schema["properties"]["query"]["type"] == "string", "query is string");
    check(args_schema["properties"]["max_results"]["type"] == "integer", "max_results is integer");
    check(args_schema["properties"]["after_cursor"]["type"] == "string",
          "after_cursor (optional<string>) unwraps to string");

    auto const& required = args_schema["required"];
    check(required.is_array(), "required is an array");
    bool query_required = false, max_results_required = false, after_cursor_required = false;
    for (auto const& r : required) {
        if (r == "query") query_required = true;
        if (r == "max_results") max_results_required = true;
        if (r == "after_cursor") after_cursor_required = true;
    }
    check(query_required, "query is required");
    check(max_results_required, "max_results is required");
    check(!after_cursor_required, "after_cursor (std::optional) is NOT required");

    // -- vector<T> + nested described struct ----------------------------------------------------
    json reply_schema = json::parse(json_schema_of<SearchReply>());
    check(reply_schema["type"] == "object", "SearchReply schema type is object");
    check(reply_schema["properties"]["hits"]["type"] == "array", "hits is array");
    auto const& items = reply_schema["properties"]["hits"]["items"];
    check(items["type"] == "object", "hits[] items is a nested object, not flattened/stringified");
    check(items["properties"]["url"]["type"] == "string", "Hit.url is string");
    check(items["properties"]["score"]["type"] == "number", "Hit.score is number");
    check(items["required"].size() == 2, "both Hit fields are required (neither is optional)");
    check(reply_schema["properties"]["truncated"]["type"] == "boolean", "truncated is boolean");

    // -- Tool<Derived>::args_schema()/reply_schema() route to the same schema ------------------
    check(WebSearchTool::args_schema() == json_schema_of<SearchArgs>(),
          "Tool::args_schema() matches schema::json_schema_of<Args>() exactly");
    check(WebSearchTool::reply_schema() == json_schema_of<SearchReply>(),
          "Tool::reply_schema() matches schema::json_schema_of<Reply>() exactly");

    if (g_failures == 0) {
        std::fprintf(stdout, "test_tool_json_schema: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_tool_json_schema: %d check(s) failed\n", g_failures);
    return 1;
}
