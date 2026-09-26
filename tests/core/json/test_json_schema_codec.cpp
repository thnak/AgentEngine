// Proves M2 Phase B task B2's JSON codec half: AE_JSON_SCHEMA(Type, member...) generates
// ae_to_json/ae_from_json alongside the schema string (core/json_schema.hpp), so the tool pipeline
// (006 §3 steps 2/9) has a real, dependency-free way to turn a raw json::Value into a typed Args
// and a typed Reply back into a json::Value -- without linking nlohmann::json into product code.
//
// Checks: round-trip preserves values (including nested struct + vector + optional-present/
// -absent); a missing required field is a contract error, not a silently-defaulted value; a
// present-but-wrong-typed field is a contract error, never coerced (006 §3 step 2's "reject, do
// not coerce" -- e.g. a string where an integer is declared does NOT get parsed as a number); a
// std::optional field omits its key entirely on the way out (never serializes as JSON null) and
// accepts either an absent key OR an explicit JSON null on the way in.

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

struct Hit {
    std::string url;
    double score;
};
AE_JSON_SCHEMA(Hit, url, score)

struct SearchArgs {
    std::string query;
    int max_results;
    std::optional<std::string> after_cursor;
};
AE_JSON_SCHEMA(SearchArgs, query, max_results, after_cursor)

struct SearchReply {
    std::vector<Hit> hits;
    bool truncated;
};
AE_JSON_SCHEMA(SearchReply, hits, truncated)

}  // namespace

int main() {
    namespace schema = agentengine::schema;
    namespace json = agentengine::json;

    // -- to_json: optional-present, then round-trip through from_json --------------------------
    {
        SearchArgs args{"hello", 3, std::string{"cursor-1"}};
        json::Value v = schema::to_json(args);
        check(v.find("query")->as_string() == "hello", "to_json: query serializes");
        check(v.find("max_results")->as_number() == 3.0, "to_json: max_results serializes");
        check(v.find("after_cursor") != nullptr && v.find("after_cursor")->as_string() == "cursor-1",
              "to_json: present optional serializes its value");

        auto round_tripped = schema::from_json<SearchArgs>(v);
        check(round_tripped.has_value(), "from_json: round-trip succeeds");
        if (round_tripped) {
            check(round_tripped->query == "hello", "round-trip preserves query");
            check(round_tripped->max_results == 3, "round-trip preserves max_results");
            check(round_tripped->after_cursor.has_value() && *round_tripped->after_cursor == "cursor-1",
                  "round-trip preserves optional value");
        }
    }

    // -- to_json: optional-absent is OMITTED, not serialized as null ---------------------------
    {
        SearchArgs args{"hello", 3, std::nullopt};
        json::Value v = schema::to_json(args);
        check(v.find("after_cursor") == nullptr,
              "to_json: absent optional field is omitted, not emitted as JSON null");

        auto round_tripped = schema::from_json<SearchArgs>(v);
        check(round_tripped.has_value() && !round_tripped->after_cursor.has_value(),
              "from_json: missing key on an optional field decodes to nullopt");
    }

    // -- from_json: an explicit JSON null also decodes an optional to nullopt ------------------
    {
        auto v = json::parse(R"({"query":"q","max_results":1,"after_cursor":null})");
        check(v.has_value(), "fixture parses");
        auto decoded = schema::from_json<SearchArgs>(*v);
        check(decoded.has_value() && !decoded->after_cursor.has_value(),
              "from_json: explicit JSON null on an optional field decodes to nullopt");
    }

    // -- from_json: missing REQUIRED field is a contract error, not a default-initialized value -
    {
        auto v = json::parse(R"({"max_results":1})");
        check(v.has_value(), "fixture parses");
        auto decoded = schema::from_json<SearchArgs>(*v);
        check(!decoded.has_value(), "missing required 'query' field is rejected");
        if (!decoded) {
            check(decoded.error().klass == agentengine::failure_class::contract,
                  "missing-required-field error is classified 'contract'");
            check(decoded.error().code == "json.missing_required_field",
                  "missing-required-field error carries the expected machine-readable code");
        }
    }

    // -- from_json: wrong-typed field is rejected, NEVER coerced --------------------------------
    {
        auto v = json::parse(R"({"query":"q","max_results":"not-a-number"})");
        check(v.has_value(), "fixture parses");
        auto decoded = schema::from_json<SearchArgs>(*v);
        check(!decoded.has_value(), "a string where an integer is declared is rejected, not coerced");
        if (!decoded) {
            check(decoded.error().code == "json.field_type_mismatch",
                  "type-mismatch error carries the expected machine-readable code");
        }
    }

    // -- nested struct + vector round-trip -------------------------------------------------------
    {
        SearchReply reply{{Hit{"https://a", 0.9}, Hit{"https://b", 0.5}}, true};
        json::Value v = schema::to_json(reply);
        auto decoded = schema::from_json<SearchReply>(v);
        check(decoded.has_value(), "SearchReply round-trips");
        if (decoded) {
            check(decoded->hits.size() == 2, "vector<Hit> round-trips its length");
            check(decoded->hits[0].url == "https://a" && decoded->hits[0].score == 0.9,
                  "nested struct field values round-trip");
            check(decoded->hits[1].url == "https://b" && decoded->hits[1].score == 0.5,
                  "second vector element round-trips");
            check(decoded->truncated == true, "bool field round-trips");
        }
    }

    // -- from_json: input that isn't even a JSON object is rejected ------------------------------
    {
        auto v = json::parse(R"(["not","an","object"])");
        check(v.has_value(), "array fixture parses");
        auto decoded = schema::from_json<SearchArgs>(*v);
        check(!decoded.has_value(), "a JSON array where an object is expected is rejected");
        if (!decoded) {
            check(decoded.error().code == "json.expected_object",
                  "not-an-object error carries the expected machine-readable code");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stdout, "test_json_schema_codec: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_json_schema_codec: %d check(s) failed\n", g_failures);
    return 1;
}
