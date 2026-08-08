// Milestone 7 Phase D1 (012-A2A-Conformance.md §0/§A.4, docs/research/2026-a2a-and-agui-detail.md
// Part A, docs/planning/milestone-7-protocol-conformance-breakdown.md). Proves the A2A v1.0 wire
// object model (protocol/a2a/types.hpp) round-trips correctly and specifically that the two named
// v1.0 breaking changes from 0.3.x are honoured: `Part` carries NO `kind` discriminator (the JSON
// member name itself is the discriminator), and enums serialize as full SCREAMING_SNAKE names.

#include <cstddef>
#include <cstdio>
#include <string>

#include "agentengine/protocol/a2a/types.hpp"

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

namespace a2a  = agentengine::a2a;
namespace json = agentengine::json;

}  // namespace

int main() {
    // --- D1-1: TextPart round-trips, and the wire form carries NO "kind" member ---------------------
    {
        a2a::Part p;
        p.value = a2a::TextPart{"hello"};
        json::Value wire = a2a::to_json(p);
        std::string dumped = json::dump(wire);
        check(dumped.find("\"kind\"") == std::string::npos,
              "D1-1: a serialized Part carries no \"kind\" member -- v1.0 removed it");
        check(dumped.find("\"text\":\"hello\"") != std::string::npos,
              "D1-1: the JSON member name \"text\" is itself the discriminator");
        auto parsed = a2a::part_from_json(wire);
        check(parsed.has_value(), "D1-1: TextPart round-trips through from_json");
        if (parsed.has_value()) {
            auto const* t = std::get_if<a2a::TextPart>(&parsed->value);
            check(t && t->text == "hello", "D1-1: the round-tripped text matches exactly");
        }
    }

    // --- D1-2: RawPart round-trips real bytes through base64 -----------------------------------------
    {
        a2a::Part p;
        p.value = a2a::RawPart{{std::byte{0x00}, std::byte{0xFF}, std::byte{0x7A}, std::byte{0x01}}};
        auto parsed = a2a::part_from_json(a2a::to_json(p));
        check(parsed.has_value(), "D1-2: RawPart round-trips");
        if (parsed.has_value()) {
            auto const* r = std::get_if<a2a::RawPart>(&parsed->value);
            check(r && r->bytes.size() == 4 && r->bytes[0] == std::byte{0x00} &&
                      r->bytes[1] == std::byte{0xFF} && r->bytes[2] == std::byte{0x7A} &&
                      r->bytes[3] == std::byte{0x01},
                  "D1-2: the round-tripped bytes match exactly, including a 0x00 and 0xFF byte");
        }
    }

    // --- D1-3: UrlPart, D1-4: DataPart round-trip -----------------------------------------------------
    {
        a2a::Part up;
        up.value = a2a::UrlPart{"https://example.invalid/resource"};
        auto parsed_url = a2a::part_from_json(a2a::to_json(up));
        check(parsed_url.has_value(), "D1-3: UrlPart round-trips");
        if (parsed_url.has_value()) {
            auto const* u = std::get_if<a2a::UrlPart>(&parsed_url->value);
            check(u && u->url == "https://example.invalid/resource", "D1-3: the URL matches exactly");
        }

        a2a::Part dp;
        dp.value = a2a::DataPart{json::Value::make_object(
            {{"n", json::Value::make_number(42)}, {"s", json::Value::make_string("x")}})};
        auto parsed_data = a2a::part_from_json(a2a::to_json(dp));
        check(parsed_data.has_value(), "D1-4: DataPart round-trips");
        if (parsed_data.has_value()) {
            auto const* d = std::get_if<a2a::DataPart>(&parsed_data->value);
            check(d && d->data.find("n") && d->data.find("n")->as_number() == 42,
                  "D1-4: the arbitrary JSON payload survives the round trip");
        }
    }

    // --- D1-5: an unrecognized discriminator ("audio", say -- not yet a kind this build knows) is ---
    // --- PRESERVED verbatim, never dropped -- the same "unknown kinds round-trip" precedent          -
    // --- core/content.hpp's own Custom establishes, applied to A2A's own Part oneof.                 -
    {
        json::Value hand_authored = json::Value::make_object(
            {{"audio", json::Value::make_string("some-future-payload")},
             {"mediaType", json::Value::make_string("audio/mpeg")}});
        auto parsed = a2a::part_from_json(hand_authored);
        check(parsed.has_value(), "D1-5: an unrecognized discriminator does not fail parsing");
        if (parsed.has_value()) {
            auto const* unk = std::get_if<a2a::UnknownPart>(&parsed->value);
            check(unk && unk->member_name == "audio" && unk->raw_value.is_string() &&
                      unk->raw_value.as_string() == "some-future-payload",
                  "D1-5: the unknown member's name and raw value are preserved exactly");
            check(parsed->media_type.has_value() && *parsed->media_type == "audio/mpeg",
                  "D1-5: common fields (mediaType) alongside an unknown discriminator still parse");
            json::Value re_emitted = a2a::to_json(*parsed);
            check(re_emitted.find("audio") && re_emitted.find("audio")->as_string() == "some-future-payload",
                  "D1-5: re-serializing an unknown-kind Part reproduces the original member verbatim");
        }
    }

    // --- D1-6: task_state's full 9-value SCREAMING_SNAKE round trip, and is_terminal() correctness --
    {
        struct Row { a2a::task_state s; char const* wire; bool terminal; };
        Row const rows[] = {
            {a2a::task_state::unspecified, "TASK_STATE_UNSPECIFIED", false},
            {a2a::task_state::submitted, "TASK_STATE_SUBMITTED", false},
            {a2a::task_state::working, "TASK_STATE_WORKING", false},
            {a2a::task_state::completed, "TASK_STATE_COMPLETED", true},
            {a2a::task_state::failed, "TASK_STATE_FAILED", true},
            {a2a::task_state::canceled, "TASK_STATE_CANCELED", true},
            {a2a::task_state::input_required, "TASK_STATE_INPUT_REQUIRED", false},
            {a2a::task_state::rejected, "TASK_STATE_REJECTED", true},
            {a2a::task_state::auth_required, "TASK_STATE_AUTH_REQUIRED", false},
        };
        bool all_ok = true;
        for (auto const& row : rows) {
            if (a2a::to_wire_string(row.s) != row.wire) all_ok = false;
            auto parsed = a2a::task_state_from_wire_string(row.wire);
            if (!parsed.has_value() || *parsed != row.s) all_ok = false;
            if (a2a::is_terminal(row.s) != row.terminal) all_ok = false;
        }
        check(all_ok, "D1-6: all 9 task_state values serialize to their full SCREAMING_SNAKE wire "
                       "name, round-trip, and is_terminal() matches the spec's terminal/interrupted "
                       "split exactly");
    }

    // --- D1-7: a2a_role's 3-value round trip ----------------------------------------------------------
    {
        check(a2a::to_wire_string(a2a::a2a_role::user) == "ROLE_USER" &&
                  a2a::to_wire_string(a2a::a2a_role::agent) == "ROLE_AGENT" &&
                  a2a::to_wire_string(a2a::a2a_role::unspecified) == "ROLE_UNSPECIFIED",
              "D1-7: role serializes as the full SCREAMING_SNAKE name, not a bare suffix");
        auto parsed = a2a::a2a_role_from_wire_string("ROLE_AGENT");
        check(parsed.has_value() && *parsed == a2a::a2a_role::agent, "D1-7: role round-trips");
    }

    // --- D1-9: Message round-trips every field, camelCase on the wire --------------------------------
    {
        a2a::Message m;
        m.message_id = "msg-1";
        m.context_id = "ctx-1";
        m.task_id    = "task-1";
        m.role       = a2a::a2a_role::user;
        a2a::Part p;
        p.value = a2a::TextPart{"hi"};
        m.parts.push_back(p);
        m.metadata = json::Value::make_object({{"k", json::Value::make_string("v")}});
        m.extensions = {"ext-a", "ext-b"};
        m.reference_task_ids = {"task-0"};

        json::Value wire = a2a::to_json(m);
        std::string dumped = json::dump(wire);
        check(dumped.find("\"messageId\"") != std::string::npos &&
                  dumped.find("\"contextId\"") != std::string::npos &&
                  dumped.find("\"taskId\"") != std::string::npos &&
                  dumped.find("\"referenceTaskIds\"") != std::string::npos,
              "D1-9: Message fields are camelCase on the wire");

        auto parsed = a2a::message_from_json(wire);
        check(parsed.has_value(), "D1-9: Message round-trips through from_json");
        if (parsed.has_value()) {
            check(parsed->message_id == "msg-1" && parsed->context_id == "ctx-1" &&
                      parsed->task_id == "task-1" && parsed->role == a2a::a2a_role::user &&
                      parsed->parts.size() == 1 && parsed->extensions.size() == 2 &&
                      parsed->reference_task_ids.size() == 1,
                  "D1-9: every Message field survives the round trip exactly");
        }
    }

    // --- D1-10: optional/empty fields are OMITTED on the wire, not emitted as empty arrays -----------
    {
        a2a::Message m;
        m.message_id = "msg-2";
        m.role       = a2a::a2a_role::agent;
        a2a::Part p;
        p.value = a2a::TextPart{"minimal"};
        m.parts.push_back(p);
        json::Value wire = a2a::to_json(m);
        std::string dumped = json::dump(wire);
        check(dumped.find("\"contextId\"") == std::string::npos &&
                  dumped.find("\"extensions\"") == std::string::npos &&
                  dumped.find("\"referenceTaskIds\"") == std::string::npos,
              "D1-10: unset optionals and empty repeated fields are omitted, not emitted empty");
        auto parsed = a2a::message_from_json(wire);
        check(parsed.has_value() && !parsed->context_id.has_value() && parsed->extensions.empty() &&
                  parsed->reference_task_ids.empty(),
              "D1-10: parsing a minimal Message back yields the same empty/unset defaults");
    }

    // --- D1-11: Artifact round-trips --------------------------------------------------------------------
    {
        a2a::Artifact art;
        art.artifact_id = "art-1";
        art.name        = "result.txt";
        a2a::Part p;
        p.value = a2a::TextPart{"artifact body"};
        art.parts.push_back(p);
        art.extensions = {"ext-x"};
        auto parsed = a2a::artifact_from_json(a2a::to_json(art));
        check(parsed.has_value() && parsed->artifact_id == "art-1" && parsed->name == "result.txt" &&
                  parsed->parts.size() == 1 && parsed->extensions.size() == 1,
              "D1-11: Artifact round-trips every field");
    }

    // --- D1-12: TaskStatus round-trips with and without an embedded message --------------------------
    {
        a2a::TaskStatus s;
        s.state = a2a::task_state::input_required;
        a2a::Message msg;
        msg.message_id = "msg-3";
        msg.role        = a2a::a2a_role::agent;
        a2a::Part p;
        p.value = a2a::TextPart{"need input"};
        msg.parts.push_back(p);
        s.message   = msg;
        s.timestamp = "2026-08-08T00:00:00Z";
        auto parsed = a2a::task_status_from_json(a2a::to_json(s));
        check(parsed.has_value() && parsed->state == a2a::task_state::input_required &&
                  parsed->message.has_value() && parsed->message->message_id == "msg-3" &&
                  parsed->timestamp == "2026-08-08T00:00:00Z",
              "D1-12: TaskStatus round-trips its embedded Message and timestamp");

        a2a::TaskStatus minimal;
        minimal.state = a2a::task_state::working;
        auto parsed_min = a2a::task_status_from_json(a2a::to_json(minimal));
        check(parsed_min.has_value() && !parsed_min->message.has_value() &&
                  !parsed_min->timestamp.has_value(),
              "D1-12: a TaskStatus with no message/timestamp round-trips to the same unset state");
    }

    // --- D1-13: Task round-trips, both fully populated and minimal -----------------------------------
    {
        a2a::Task t;
        t.id         = "task-9";
        t.context_id = "ctx-9";
        t.status.state = a2a::task_state::completed;
        a2a::Artifact art;
        art.artifact_id = "art-9";
        a2a::Part ap;
        ap.value = a2a::TextPart{"done"};
        art.parts.push_back(ap);
        t.artifacts.push_back(art);
        a2a::Message hist_msg;
        hist_msg.message_id = "msg-9";
        hist_msg.role         = a2a::a2a_role::user;
        a2a::Part hp;
        hp.value = a2a::TextPart{"go"};
        hist_msg.parts.push_back(hp);
        t.history.push_back(hist_msg);
        t.metadata = json::Value::make_object({{"tag", json::Value::make_string("v1")}});

        auto parsed = a2a::task_from_json(a2a::to_json(t));
        check(parsed.has_value() && parsed->id == "task-9" && parsed->context_id == "ctx-9" &&
                  parsed->status.state == a2a::task_state::completed && parsed->artifacts.size() == 1 &&
                  parsed->history.size() == 1 && parsed->metadata.has_value(),
              "D1-13: a fully populated Task round-trips every field");

        a2a::Task minimal;
        minimal.id            = "task-min";
        minimal.status.state  = a2a::task_state::submitted;
        auto parsed_min = a2a::task_from_json(a2a::to_json(minimal));
        check(parsed_min.has_value() && parsed_min->artifacts.empty() && parsed_min->history.empty() &&
                  !parsed_min->metadata.has_value(),
              "D1-13: a minimal Task (no artifacts/history/metadata) round-trips to the same defaults");
    }

    // --- D1-14: a Part with TWO oneof members is rejected, never silently picks one ------------------
    {
        json::Value bad = json::Value::make_object(
            {{"text", json::Value::make_string("a")}, {"url", json::Value::make_string("b")}});
        auto parsed = a2a::part_from_json(bad);
        check(!parsed.has_value(), "D1-14: a Part with two oneof members is rejected");
    }

    // --- D1-15: a Part with ZERO oneof members is rejected --------------------------------------------
    {
        json::Value bad = json::Value::make_object({{"mediaType", json::Value::make_string("text/plain")}});
        auto parsed = a2a::part_from_json(bad);
        check(!parsed.has_value(), "D1-15: a Part with no oneof member at all is rejected");
    }

    // --- D1-16: a Message with an empty parts array is rejected (spec: parts[] >= 1) -----------------
    {
        json::Value bad = json::Value::make_object(
            {{"messageId", json::Value::make_string("m")}, {"role", json::Value::make_string("ROLE_USER")},
             {"parts", json::Value::make_array({})}});
        auto parsed = a2a::message_from_json(bad);
        check(!parsed.has_value(), "D1-16: a Message with zero parts is rejected");
    }

    // --- D1-17: an unrecognized task_state wire string is rejected, never silently coerced -----------
    {
        auto parsed = a2a::task_state_from_wire_string("TASK_STATE_MADE_UP");
        check(!parsed.has_value(), "D1-17: an unrecognized task_state string is rejected");
    }

    if (g_failures == 0) {
        std::printf("test_a2a_types: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_a2a_types: %d failure(s)\n", g_failures);
    return 1;
}
