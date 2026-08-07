// Follow-up to Milestone 5 (006-Tool-and-Function-Plane.md §1): proves `Described<T, "text">`
// (core/json_schema.hpp) closes the gap the user found -- OpenAI's and Anthropic's own "tools" wire
// formats carry a `description` on EVERY parameter, and this project's schema generator had no
// channel for that until now. Covers:
//  (1) a described field's generated JSON Schema fragment carries "description", an undescribed
//      field's does not.
//  (2) Described<std::optional<T>, "..."> is still detected as NOT required -- described-ness and
//      optional-ness compose, neither overrides the other's detection.
//  (3) to_json/from_json round-trip a Described field transparently (reads/writes like plain T).
//  (4) the description survives OpenAI's and Anthropic's translate_tool byte-for-byte, end to end
//      from a real ToolDescriptor -- not just asserted at the schema-string layer.

#include <cstdio>
#include <optional>
#include <string>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

struct SearchArgs {
    Described<std::string, "The search query text"> query;
    int max_results = 5;  // no description -- must be unaffected
    Described<std::optional<std::string>, "Optional locale filter, e.g. 'en-US'"> locale;
};
AE_JSON_SCHEMA(SearchArgs, query, max_results, locale)

}  // namespace

int main() {
    // ---- (1) description present on a described field, absent on a plain one --------------------
    {
        std::string const s = schema::json_schema_of<SearchArgs>();
        auto parsed = json::parse(s);
        check(parsed.has_value(), "schema parses as JSON");
        if (parsed) {
            auto const* props = parsed->find("properties");
            check(props && props->is_object(), "schema has a properties object");
            if (props) {
                auto const* query_prop = props->find("query");
                check(query_prop && query_prop->is_object(), "query property present");
                if (query_prop) {
                    auto const* desc = query_prop->find("description");
                    check(desc && desc->is_string() && desc->as_string() == "The search query text",
                          "R1: Described<std::string,\"...\"> emits the exact description text into "
                          "the property's own JSON Schema object");
                }
                auto const* max_results_prop = props->find("max_results");
                check(max_results_prop && max_results_prop->is_object(), "max_results property present");
                if (max_results_prop) {
                    check(max_results_prop->find("description") == nullptr,
                          "R2: a plain (non-Described) field carries NO description key -- "
                          "undescribed fields are byte-for-byte unaffected by this feature");
                }
            }
        }
    }

    // ---- (2) Described<std::optional<T>, "..."> is still detected as optional ---------------------
    {
        std::string const s = schema::json_schema_of<SearchArgs>();
        auto parsed = json::parse(s);
        if (parsed) {
            auto const* required = parsed->find("required");
            bool locale_in_required = false;
            bool query_in_required = false;
            if (required && required->is_array()) {
                for (auto const& item : required->as_array()) {
                    if (item.is_string() && item.as_string() == "locale") locale_in_required = true;
                    if (item.is_string() && item.as_string() == "query") query_in_required = true;
                }
            }
            check(query_in_required, "R3: a Described<T,\"...\"> (non-optional inner) IS required");
            check(!locale_in_required,
                  "R4: Described<std::optional<T>,\"...\"> is NOT required -- described-ness and "
                  "optional-ness compose independently, wrapping in Described does not defeat the "
                  "existing optional-detection rule");

            auto const* props = parsed->find("properties");
            auto const* locale_prop = props ? props->find("locale") : nullptr;
            check(locale_prop != nullptr, "locale property present");
            if (locale_prop) {
                auto const* desc = locale_prop->find("description");
                check(desc && desc->is_string() && desc->as_string() == "Optional locale filter, e.g. 'en-US'",
                      "R5: the description still comes through for an optional described field");
            }
        }
    }

    // ---- (3) to_json/from_json round-trip transparently --------------------------------------------
    {
        SearchArgs args;
        args.query = std::string("hello world");  // assignment through the conversion path
        args.max_results = 10;
        args.locale = std::string("en-US");

        json::Value v = schema::to_json(args);
        auto back = schema::from_json<SearchArgs>(v);
        check(back.has_value(), "R6: a Described-bearing struct round-trips through to_json/from_json");
        if (back) {
            check(std::string(back->query) == "hello world",
                  "R7: the described field's VALUE round-trips (the wrapper is transparent, not just "
                  "the schema)");
            check(back->max_results == 10, "R8: the plain field round-trips unaffected");
            check(back->locale.value.has_value() && *back->locale.value == "en-US",
                  "R9: Described<std::optional<T>,...>'s value round-trips too");
        }

        // Omitting the optional described field entirely must still parse (matches the existing
        // optional-omission contract, now reached through Described).
        SearchArgs args_no_locale;
        args_no_locale.query = std::string("q");
        args_no_locale.max_results = 1;
        json::Value v2 = schema::to_json(args_no_locale);
        check(v2.find("locale") == nullptr,
              "R10: an unset Described<std::optional<T>,...> is OMITTED from the serialized object, "
              "same as a plain std::optional<T> would be -- not emitted as JSON null");
        auto back2 = schema::from_json<SearchArgs>(v2);
        check(back2.has_value() && !back2->locale.value.has_value(),
              "R11: parsing an object with the key entirely absent succeeds and leaves the described "
              "optional field unset");
    }

    // ---- (4) the description survives real translate_tool(), both backends -----------------------
    {
        ToolDescriptor td;
        td.name = "search";
        td.description = "Search for something.";
        td.args_schema_json = schema::json_schema_of<SearchArgs>();

        auto openai_tool = openai::detail::translate_tool(td);
        check(openai_tool.has_value(), "OpenAI translate_tool succeeds");
        if (openai_tool) {
            auto const* fn = openai_tool->find("function");
            auto const* params = fn ? fn->find("parameters") : nullptr;
            auto const* props = params ? params->find("properties") : nullptr;
            auto const* query_prop = props ? props->find("query") : nullptr;
            auto const* desc = query_prop ? query_prop->find("description") : nullptr;
            check(desc && desc->is_string() && desc->as_string() == "The search query text",
                  "R12: the per-parameter description survives OpenAI's real translate_tool() "
                  "end to end, from a real ToolDescriptor to the actual wire-shaped JSON");
        }

        auto anthropic_tool = anthropic::detail::translate_tool(td, /*cache_this_one=*/false);
        check(anthropic_tool.has_value(), "Anthropic translate_tool succeeds");
        if (anthropic_tool) {
            auto const* input_schema = anthropic_tool->find("input_schema");
            auto const* props = input_schema ? input_schema->find("properties") : nullptr;
            auto const* query_prop = props ? props->find("query") : nullptr;
            auto const* desc = query_prop ? query_prop->find("description") : nullptr;
            check(desc && desc->is_string() && desc->as_string() == "The search query text",
                  "R13: the per-parameter description survives Anthropic's real translate_tool() "
                  "end to end too");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_json_schema_described: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_json_schema_described: %d FAILURE(S)\n", g_failures);
    return 1;
}
