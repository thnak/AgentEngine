// Smoke test for core/json_value.hpp -- the dependency-free JSON value/parser/serializer the tool
// pipeline (006 §3 steps 2/9) needs at runtime, since nlohmann::json stays test-only
// (CONVENTIONS.md dependency-tier discipline). Round-trips a representative document through
// parse()/dump() and checks parser rejection on malformed input (006 §3's "reject, do not coerce").

#include <cstdio>
#include <string>

#include "agentengine/core/json_value.hpp"

namespace {
int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}
}  // namespace

int main() {
    using namespace agentengine::json;

    // -- round trip: object with string/number/bool/null/array/nested-object members ------------
    std::string doc =
        R"({"query":"hello \"world\"","max_results":5,"active":true,"cursor":null,)"
        R"("tags":["a","b"],"nested":{"x":1.5}})";
    auto parsed = parse(doc);
    check(parsed.has_value(), "well-formed document parses");
    if (parsed) {
        check(parsed->is_object(), "root is an object");
        check(parsed->find("query")->as_string() == "hello \"world\"", "escaped string round-trips");
        check(parsed->find("max_results")->as_number() == 5.0, "number parses");
        check(parsed->find("active")->as_bool() == true, "bool parses");
        check(parsed->find("cursor")->is_null(), "null parses");
        check(parsed->find("tags")->as_array().size() == 2, "array parses");
        check(parsed->find("tags")->as_array()[0].as_string() == "a", "array element parses");
        check(parsed->find("nested")->find("x")->as_number() == 1.5, "nested object parses");
        check(parsed->find("missing") == nullptr, "find() on absent key returns nullptr");

        std::string redumped = dump(*parsed);
        auto reparsed = parse(redumped);
        check(reparsed.has_value(), "re-dumped document re-parses");
        if (reparsed) {
            check(reparsed->find("query")->as_string() == "hello \"world\"",
                  "round-trip preserves escaped string content");
            check(reparsed->find("nested")->find("x")->as_number() == 1.5,
                  "round-trip preserves nested numbers");
        }
    }

    // -- unicode escape ---------------------------------------------------------------------------
    auto unicode = parse(R"("café")");
    check(unicode.has_value() && unicode->as_string() == "caf\xc3\xa9",
          "\\u escape decodes to UTF-8 bytes");

    // -- malformed input is rejected, not coerced -------------------------------------------------
    check(!parse("{").has_value(), "unterminated object rejected");
    check(!parse("[1,2,]").has_value(), "trailing comma in array rejected");
    check(!parse("{\"a\":1,}").has_value(), "trailing comma in object rejected");
    check(!parse("nul").has_value(), "truncated literal rejected");
    check(!parse("123abc").has_value(), "trailing garbage after number rejected");
    check(!parse("").has_value(), "empty input rejected");
    check(!parse(R"("unterminated)").has_value(), "unterminated string rejected");

    if (g_failures == 0) {
        std::fprintf(stdout, "test_json_value: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_json_value: %d check(s) failed\n", g_failures);
    return 1;
}
