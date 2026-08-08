// Milestone 7 Phase D3 (012-A2A-Conformance.md §1, docs/planning/milestone-7-protocol-conformance-
// breakdown.md). Proves the `ContentItem`/`Message` (003, internal) <-> `Part`/`Message` (012, wire)
// projection (protocol/a2a/mapping.hpp) at the unit level, independent of any real `AgentSession` --
// the narrow `Text`/`Data`/`Error` mapping, the `UnknownPart` fallback reuse for every other
// `ContentItem` kind, and the lossy-but-honest role mapping in both directions.

#include <cstdio>
#include <string>

#include "agentengine/protocol/a2a/mapping.hpp"

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

namespace ae   = agentengine;
namespace a2a  = agentengine::a2a;
namespace json = agentengine::json;

}  // namespace

int main() {
    // --- Text <-> TextPart -----------------------------------------------------------------------
    {
        ae::ContentItem item;
        item.value = ae::Text{"hello"};
        a2a::Part p = a2a::content_item_to_part(item);
        auto const* t = std::get_if<a2a::TextPart>(&p.value);
        check(t && t->text == "hello", "Text maps to TextPart with the exact same string");
        ae::ContentItem back = a2a::part_to_content_item(p);
        auto const* t2 = std::get_if<ae::Text>(&back.value);
        check(t2 && t2->text == "hello", "TextPart maps back to Text");
        check(back.tainted && back.origin == ae::content_origin::external,
              "an inbound Part always becomes tainted, externally-originated content (012 §3: a peer "
              "agent is exactly as trusted as a web page)");
    }

    // --- Data <-> DataPart, including the parse-failure fallback ------------------------------------
    {
        ae::ContentItem item;
        item.value = ae::Data{"{\"n\":42}", std::nullopt};
        a2a::Part p = a2a::content_item_to_part(item);
        auto const* d = std::get_if<a2a::DataPart>(&p.value);
        check(d && d->data.find("n") && d->data.find("n")->as_number() == 42,
              "well-formed Data JSON maps to a real DataPart with the parsed value");

        ae::ContentItem malformed;
        malformed.value = ae::Data{"not json at all", std::nullopt};
        a2a::Part p2 = a2a::content_item_to_part(malformed);
        auto const* t = std::get_if<a2a::TextPart>(&p2.value);
        check(t && t->text == "not json at all",
              "malformed Data JSON degrades to a legible TextPart of the raw string, never dropped");
    }

    // --- Error <-> TextPart (Error has no A2A wire counterpart, flattened to text) ------------------
    {
        ae::ContentItem item;
        item.value = ae::Error{"something failed"};
        a2a::Part p = a2a::content_item_to_part(item);
        auto const* t = std::get_if<a2a::TextPart>(&p.value);
        check(t && t->text == "something failed", "Error flattens to a TextPart carrying its message");
    }

    // --- Every other ContentItem kind falls back to UnknownPart, reusing the SAME forward-compat ---
    // --- shape types.hpp built for an unrecognized WIRE Part kind -- not a second mechanism.       ---
    {
        ae::ContentItem item;
        item.value = ae::Reasoning{"internal reasoning text", false};
        a2a::Part p = a2a::content_item_to_part(item);
        check(std::holds_alternative<a2a::UnknownPart>(p.value),
              "a ContentItem kind with no real A2A mapping (Reasoning) falls back to UnknownPart, "
              "never dropped or fabricated as if it were text");
    }

    // --- Part -> ContentItem: DataPart/UrlPart/RawPart/UnknownPart all map to something real --------
    {
        a2a::Part dp;
        dp.value = a2a::DataPart{json::Value::make_object({{"x", json::Value::make_number(1)}})};
        ae::ContentItem back = a2a::part_to_content_item(dp);
        auto const* d = std::get_if<ae::Data>(&back.value);
        check(d && d->json.find('1') != std::string::npos, "DataPart maps to a real Data ContentItem");

        a2a::Part up;
        up.value = a2a::UrlPart{"https://example.invalid/x"};
        up.media_type = "text/html";
        ae::ContentItem back_u = a2a::part_to_content_item(up);
        auto const* m = std::get_if<ae::Media>(&back_u.value);
        check(m && m->media_type == "text/html" && std::holds_alternative<std::string>(m->payload) &&
                  std::get<std::string>(m->payload) == "https://example.invalid/x",
              "UrlPart maps to Media carrying the URI and its mediaType");

        a2a::Part unk;
        unk.value = a2a::UnknownPart{"future_kind", json::Value::make_string("v")};
        ae::ContentItem back_unk = a2a::part_to_content_item(unk);
        auto const* c = std::get_if<ae::Custom>(&back_unk.value);
        check(c && c->type_id == "a2a:future_kind",
              "an UnknownPart maps to Custom, namespaced so its A2A origin is traceable");
    }

    // --- Role mapping: lossy in a NAMED direction, never silently coerced to the wrong speaker ------
    {
        check(a2a::to_a2a_role(ae::role::user) == a2a::a2a_role::user &&
                  a2a::to_a2a_role(ae::role::assistant) == a2a::a2a_role::agent &&
                  a2a::to_a2a_role(ae::role::system) == a2a::a2a_role::unspecified &&
                  a2a::to_a2a_role(ae::role::tool) == a2a::a2a_role::unspecified,
              "system/tool (no A2A equivalent) map to unspecified, never silently promoted to user/agent");
        check(a2a::from_a2a_role(a2a::a2a_role::agent) == ae::role::assistant &&
                  a2a::from_a2a_role(a2a::a2a_role::user) == ae::role::user &&
                  a2a::from_a2a_role(a2a::a2a_role::unspecified) == ae::role::user,
              "inbound A2A roles only ever become user/assistant -- unspecified defaults to the "
              "conservative \"external caller input\" reading");
    }

    // --- Full Message round trip, including message_id and multiple parts ---------------------------
    {
        ae::ContentItem a;
        a.value = ae::Text{"part one"};
        ae::ContentItem b;
        b.value = ae::Text{"part two"};
        ae::Message m;
        m.role       = ae::role::user;
        m.message_id = "m-123";
        m.content    = {a, b};

        a2a::Message wire = a2a::to_a2a_message(m, std::string("task-1"), std::string("ctx-1"));
        check(wire.message_id == "m-123" && wire.task_id == "task-1" && wire.context_id == "ctx-1" &&
                  wire.role == a2a::a2a_role::user && wire.parts.size() == 2,
              "to_a2a_message carries message_id/taskId/contextId/role/parts through exactly");

        ae::Message back = a2a::from_a2a_message(wire);
        check(back.message_id == "m-123" && back.role == ae::role::user && back.content.size() == 2,
              "from_a2a_message round-trips message_id/role/parts");
    }

    if (g_failures == 0) {
        std::printf("test_a2a_mapping: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_a2a_mapping: %d failure(s)\n", g_failures);
    return 1;
}
