// #120 S3 (decisions/ADR-202-one-message-json-codec.md): proves the merged Message/ContentItem <-> JSON codec
// (core/message_json.hpp) behaves exactly like the two copies it replaced, per profile, and that the state profile
// keeps I3.
//
// 1. Differential. Verbatim copies of the OLD codecs live in tests/support/oracle_chat_recording_codec.hpp
//    (core/chat_recording.hpp's) and tests/support/oracle_message_codec.hpp (rt/message_codec.hpp's). For every
//    input, the new public wrappers -- `agentengine::` (recording profile) and `agentengine::rt::` (state profile)
//    -- must give what the matching oracle gives: byte-identical `json::dump` output for encode; for decode, equal
//    values on success, or equal failure_class, code AND message on error, or the same exception type (the old
//    recording decoder throws std::bad_variant_access on a non-string "kind", ADR-202 §4 D3, and still does).
//    Inputs: a hand-built corpus of every variant, origin, role and field state, a malformed-JSON corpus, and a
//    seeded, bounded fuzz loop that mutates valid encodings.
//    ADR-204 changed exactly one recording behaviour on purpose: `Message::attribution` is now written and read,
//    as the state profile does. The recording comparison therefore expects the oracle unmodified for every item,
//    every message without attribution and every error/throw, and for an attributed message the oracle plus the
//    `attribution` member exactly where and as the old state codec wrote/read it (`expected_recording_*`).
// 2. I3. The state profile neither writes nor reads `approval` / `deliver_as_instructions`; the recording profile
//    does both (the control). Both profiles round-trip `Message::attribution` (ADR-204).
// 3. Self-control. The harness reports a difference when handed two codecs that really differ (the two oracles).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/chat_recording.hpp"
#include "agentengine/core/message_json.hpp"
#include "agentengine/rt/message_codec.hpp"

#include "../../support/oracle_chat_recording_codec.hpp"
#include "../../support/oracle_message_codec.hpp"

namespace {

namespace ae = agentengine;
namespace oldrec = agentengine::oracle_recording;
namespace oldst = agentengine::oracle_state;
using ae::json::Value;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        if (g_failures <= 50) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// ---- outcome capture ------------------------------------------------------------------------------

template <class T>
struct Outcome {
    enum class state { ok, err, threw } st = state::ok;
    std::optional<T> value;
    ae::failure_class klass{};
    std::string code;
    std::string message;
    std::string exception;
};

template <class T>
bool same(Outcome<T> const& a, Outcome<T> const& b) {
    if (a.st != b.st) return false;
    switch (a.st) {
        case Outcome<T>::state::ok: return *a.value == *b.value;
        case Outcome<T>::state::err: return a.klass == b.klass && a.code == b.code && a.message == b.message;
        case Outcome<T>::state::threw: return a.exception == b.exception;
    }
    return false;
}

template <class T>
std::string describe(Outcome<T> const& o) {
    switch (o.st) {
        case Outcome<T>::state::ok: return "ok";
        case Outcome<T>::state::err: return "err{" + o.code + ", " + o.message + "}";
        case Outcome<T>::state::threw: return "threw " + o.exception;
    }
    return "?";
}

template <class T>
Outcome<T> capture(std::function<ae::result<T>()> const& f) {
    Outcome<T> o;
    try {
        auto r = f();
        if (r) {
            o.value = std::move(*r);
        } else {
            o.st = Outcome<T>::state::err;
            o.klass = r.error().klass;
            o.code = r.error().code;
            o.message = r.error().message;
        }
    } catch (std::bad_variant_access const&) {
        o.st = Outcome<T>::state::threw;
        o.exception = "std::bad_variant_access";
    } catch (std::exception const& e) {
        o.st = Outcome<T>::state::threw;
        o.exception = std::string("std::exception: ") + e.what();
    } catch (...) {
        o.st = Outcome<T>::state::threw;
        o.exception = "unknown";
    }
    return o;
}

struct Counts {
    std::size_t encode = 0, decode = 0, decode_ok = 0, decode_err = 0, decode_threw = 0;
} g_counts;

template <class T>
void tally(Outcome<T> const& o) {
    ++g_counts.decode;
    if (o.st == Outcome<T>::state::ok) ++g_counts.decode_ok;
    if (o.st == Outcome<T>::state::err) ++g_counts.decode_err;
    if (o.st == Outcome<T>::state::threw) ++g_counts.decode_threw;
}

// ---- the four comparisons -------------------------------------------------------------------------

void diff_encode_item(ae::ContentItem const& x, std::string const& label) {
    g_counts.encode += 2;
    std::string const new_rec = ae::json::dump(ae::content_item_to_json(x));
    std::string const old_rec = ae::json::dump(oldrec::content_item_to_json(x));
    check(new_rec == old_rec, "encode item recording: " + label + "\n  new " + new_rec + "\n  old " + old_rec);
    std::string const new_st = ae::json::dump(ae::rt::content_item_to_json(x));
    std::string const old_st = ae::json::dump(oldst::content_item_to_json(x));
    check(new_st == old_st, "encode item state: " + label + "\n  new " + new_st + "\n  old " + old_st);
    // The direct API is what the wrappers call.
    check(ae::json::dump(ae::message_json::content_item_to_json(x, ae::message_json::Profile::recording())) == new_rec,
          "direct recording == wrapper: " + label);
    check(ae::json::dump(ae::message_json::content_item_to_json(x, ae::message_json::Profile::state())) == new_st,
          "direct state == wrapper: " + label);
}

// ADR-204: the recording profile now writes and reads `Message::attribution` exactly as the state profile does.
// The recording oracle predates that, so for a message that carries attribution the expected recording encoding is
// the old recording encoding plus the `attribution` member in the position -- and with the value -- the old state
// codec gives it (after "content", the last member). A message without attribution is compared with the oracle
// unmodified: byte-identical, as before ADR-204.
Value expected_recording_encoding(ae::Message const& m) {
    Value old_rec = oldrec::message_to_json(m);
    if (!m.attribution.has_value()) return old_rec;
    Value const old_st = oldst::message_to_json(m);
    std::vector<std::pair<std::string, Value>> o = old_rec.as_object();
    Value const* attribution = old_st.find("attribution");
    check(attribution != nullptr && old_st.as_object().back().first == "attribution",
          "oracle: the old state codec writes attribution last");
    if (attribution != nullptr) o.emplace_back("attribution", *attribution);
    return Value::make_object(std::move(o));
}

// Likewise on decode: a message JSON with a top-level "attribution" member decodes through the recording profile to
// what the recording oracle gives, with `attribution` set exactly as the old state decoder reads it. Every other
// outcome -- errors (class, code, message), throws (D3), and values from JSON without "attribution" -- must equal the
// oracle unmodified.
Outcome<ae::Message> expected_recording_decode(Value const& j, Outcome<ae::Message> oracle) {
    if (oracle.st != Outcome<ae::Message>::state::ok || !j.is_object() || j.find("attribution") == nullptr) return oracle;
    auto const st = capture<ae::Message>([&] { return oldst::message_from_json(j); });
    check(st.st == Outcome<ae::Message>::state::ok, "oracle: old state decoder succeeds where old recording does");
    if (st.st == Outcome<ae::Message>::state::ok) oracle.value->attribution = st.value->attribution;
    return oracle;
}

struct AttributionCounts {
    std::size_t encode_plain = 0, encode_attributed = 0, decode_plain = 0, decode_attributed = 0;
} g_attr_counts;

void diff_encode_message(ae::Message const& m, std::string const& label) {
    g_counts.encode += 2;
    std::string const new_rec = ae::json::dump(ae::message_to_json(m));
    std::string const old_rec = ae::json::dump(expected_recording_encoding(m));
    (m.attribution.has_value() ? g_attr_counts.encode_attributed : g_attr_counts.encode_plain) += 1;
    if (m.attribution.has_value()) {
        check(new_rec != ae::json::dump(oldrec::message_to_json(m)),
              "encode message recording (ADR-204): attribution written: " + label);
    }
    check(new_rec == old_rec, "encode message recording: " + label + "\n  new " + new_rec + "\n  old " + old_rec);
    std::string const new_st = ae::json::dump(ae::rt::message_to_json(m));
    std::string const old_st = ae::json::dump(oldst::message_to_json(m));
    check(new_st == old_st, "encode message state: " + label + "\n  new " + new_st + "\n  old " + old_st);
}

void diff_decode_item(Value const& j, std::string const& label) {
    auto const nr = capture<ae::ContentItem>([&] { return ae::content_item_from_json(j); });
    auto const orr = capture<ae::ContentItem>([&] { return oldrec::content_item_from_json(j); });
    tally(nr);
    check(same(nr, orr), "decode item recording: " + label + " new=" + describe(nr) + " old=" + describe(orr) +
                             " json=" + ae::json::dump(j));
    auto const ns = capture<ae::ContentItem>([&] { return ae::rt::content_item_from_json(j); });
    auto const os = capture<ae::ContentItem>([&] { return oldst::content_item_from_json(j); });
    tally(ns);
    check(same(ns, os), "decode item state: " + label + " new=" + describe(ns) + " old=" + describe(os) +
                            " json=" + ae::json::dump(j));
}

void diff_decode_message(Value const& j, std::string const& label) {
    auto const nr = capture<ae::Message>([&] { return ae::message_from_json(j); });
    bool const has_attribution = j.is_object() && j.find("attribution") != nullptr;
    (has_attribution ? g_attr_counts.decode_attributed : g_attr_counts.decode_plain) += 1;
    auto const orr = expected_recording_decode(j, capture<ae::Message>([&] { return oldrec::message_from_json(j); }));
    tally(nr);
    check(same(nr, orr), "decode message recording: " + label + " new=" + describe(nr) + " old=" + describe(orr) +
                             " json=" + ae::json::dump(j));
    auto const ns = capture<ae::Message>([&] { return ae::rt::message_from_json(j); });
    auto const os = capture<ae::Message>([&] { return oldst::message_from_json(j); });
    tally(ns);
    check(same(ns, os), "decode message state: " + label + " new=" + describe(ns) + " old=" + describe(os) +
                            " json=" + ae::json::dump(j));
}

// Encode with every codec, then decode every encoding with every decoder: covers "old writes, new reads" and back.
void diff_item(ae::ContentItem const& x, std::string const& label) {
    diff_encode_item(x, label);
    diff_decode_item(oldrec::content_item_to_json(x), label + " [from recording encoding]");
    diff_decode_item(oldst::content_item_to_json(x), label + " [from state encoding]");
}

void diff_message(ae::Message const& m, std::string const& label) {
    diff_encode_message(m, label);
    diff_decode_message(oldrec::message_to_json(m), label + " [from recording encoding]");
    diff_decode_message(oldst::message_to_json(m), label + " [from state encoding]");
}

// ---- JSON building helpers ------------------------------------------------------------------------

using Obj = std::vector<std::pair<std::string, Value>>;
Value S(std::string s) { return Value::make_string(std::move(s)); }
Value N(double d) { return Value::make_number(d); }
Value B(bool b) { return Value::make_bool(b); }
Value O(Obj o) { return Value::make_object(std::move(o)); }
Value A(std::vector<Value> a) { return Value::make_array(std::move(a)); }

// ---- corpus -----------------------------------------------------------------------------------------

std::vector<std::byte> bytes_of(std::size_t n, unsigned seed) {
    std::vector<std::byte> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::byte>((i * 131 + seed * 7 + 3) & 0xFF);
    return out;
}

std::string const kUnicode = "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x99\x82 \x01\x1F\n\t\"\\/";

std::vector<ae::ContentItem> base_items() {
    std::vector<ae::ContentItem> v;
    auto push = [&](auto value) {
        ae::ContentItem c{};
        c.value = std::move(value);
        v.push_back(std::move(c));
    };
    push(ae::Text{"hello"});
    push(ae::Text{""});
    push(ae::Text{kUnicode});
    push(ae::Text{std::string(20000, 'x')});
    push(ae::Reasoning{"thinking", false});
    push(ae::Reasoning{"opaque", true, "vendor:model"});  // producer id is not encoded by either copy
    for (std::size_t n : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 64u, 1000u}) {
        ae::Media m;
        m.media_type = "image/png";
        m.payload = bytes_of(n, static_cast<unsigned>(n));
        push(m);
    }
    {
        ae::Media m;
        m.media_type = "";
        m.payload = std::string("https://example.test/a.png?x=" + kUnicode);
        push(m);
    }
    {
        ae::Media m;
        m.media_type = "application/pdf";
        m.payload = std::string{};
        push(m);
    }
    for (std::size_t sz : {std::size_t{0}, std::size_t{42}, std::size_t{1} << 40, std::size_t{1} << 53}) {
        ae::Media m;
        m.media_type = "video/mp4";
        m.payload = ae::BlobRef{"sha256:abc", "video/mp4", sz, "blobs"};
        push(m);
    }
    {
        ae::Media m;
        m.payload = ae::BlobRef{};
        push(m);
    }
    push(ae::Data{"{\"a\":1}", std::nullopt});
    push(ae::Data{"", std::string("schema://x")});
    push(ae::Data{kUnicode, std::string("")});
    push(ae::ToolCall{"call-1", "search", "{\"q\":\"x\"}"});
    {
        ae::ToolCall tc{"", "", ""};
        tc.origin = ae::content_origin::tool;  // not encoded by either copy
        tc.provenance = ae::call_provenance::text_derived;
        push(tc);
    }
    push(ae::ToolResult{"call-1", {}, false});
    push(ae::Citation{"doc.md", 0, 0});
    push(ae::Citation{kUnicode, 12, 34});
    push(ae::Citation{"big", std::size_t{1} << 40, std::size_t{1} << 53});
    push(ae::Error{"boom"});
    push(ae::Error{""});
    push(ae::Custom{"urn:x:y", "{\"k\":[1,2]}"});
    push(ae::Custom{"", ""});
    return v;
}

ae::ContentItem nested_tool_result(int depth, bool marks) {
    ae::ToolResult tr{"call-" + std::to_string(depth), {}, depth % 2 == 1};
    ae::ContentItem leaf{};
    leaf.value = ae::Text{"child at " + std::to_string(depth)};
    leaf.origin = ae::content_origin::tool;
    leaf.tainted = true;
    if (marks) {
        leaf.approval = "ack:child";
        leaf.deliver_as_instructions = true;
    }
    tr.content.push_back(leaf);
    ae::ContentItem media{};
    ae::Media m;
    m.media_type = "image/gif";
    m.payload = bytes_of(5, 9);
    media.value = m;
    tr.content.push_back(media);
    if (depth > 0) tr.content.push_back(nested_tool_result(depth - 1, marks));
    ae::ContentItem c{};
    c.value = std::move(tr);
    c.origin = ae::content_origin::external;
    c.tainted = true;
    if (marks) c.approval = "ack:outer";
    return c;
}

std::vector<ae::content_origin> const kOrigins = {ae::content_origin::user, ae::content_origin::assistant,
                                                  ae::content_origin::tool, ae::content_origin::system,
                                                  ae::content_origin::external};
std::vector<ae::role> const kRoles = {ae::role::system, ae::role::user, ae::role::assistant, ae::role::tool};

void run_item_corpus() {
    int n = 0;
    for (auto const& base : base_items()) {
        for (auto origin : kOrigins) {
            for (bool tainted : {false, true}) {
                for (std::string approval : {std::string{}, std::string("ack:sha256:0123"), kUnicode}) {
                    for (bool deliver : {false, true}) {
                        ae::ContentItem x = base;
                        x.origin = origin;
                        x.tainted = tainted;
                        x.approval = approval;
                        x.deliver_as_instructions = deliver;
                        diff_item(x, "corpus item #" + std::to_string(n++));
                    }
                }
            }
        }
    }
    for (int depth = 0; depth < 4; ++depth) {
        for (bool marks : {false, true}) diff_item(nested_tool_result(depth, marks), "nested tool_result");
    }
}

void run_message_corpus() {
    auto items = base_items();
    int n = 0;
    for (auto r : kRoles) {
        for (int attr = 0; attr < 3; ++attr) {
            for (std::string id : {std::string{}, std::string("msg-1"), kUnicode}) {
                ae::Message m;
                m.role = r;
                m.message_id = id;
                if (attr == 1) m.attribution = ae::ContributorProvenance{3, "memory"};
                if (attr == 2) m.attribution = ae::ContributorProvenance{0, ""};
                if (n % 2 == 0) {
                    for (std::size_t i = 0; i < items.size(); i += 3) {
                        ae::ContentItem c = items[i];
                        c.approval = (i % 2) ? "ack:x" : "";
                        c.deliver_as_instructions = (i % 4) == 0;
                        c.tainted = (i % 3) == 0;
                        m.content.push_back(c);
                    }
                    m.content.push_back(nested_tool_result(2, true));
                }
                diff_message(m, "corpus message #" + std::to_string(n++));
            }
        }
    }
    {
        ae::Message m;
        m.role = ae::role::assistant;
        m.attribution = ae::ContributorProvenance{std::size_t{1} << 40, kUnicode};
        diff_message(m, "large attribution index");
    }
}

// Malformed and wrong-typed JSON, item level.
void run_malformed_items() {
    std::vector<std::pair<std::string, Value>> cases;
    auto add = [&](std::string label, Value v) { cases.emplace_back(std::move(label), std::move(v)); };

    // Not an object at all.
    add("null", Value::make_null());
    add("number", N(3));
    add("string", S("text"));
    add("bool", B(true));
    add("array", A({S("kind"), S("text")}));
    add("empty object", O({}));
    // "kind" missing, wrong-typed, unknown.
    add("missing kind", O({{"text", S("x")}}));
    add("kind null", O({{"kind", Value::make_null()}}));
    add("kind number", O({{"kind", N(1)}}));
    add("kind bool", O({{"kind", B(false)}}));
    add("kind array", O({{"kind", A({S("text")})}}));
    add("kind object", O({{"kind", O({{"x", S("text")}})}}));
    add("kind unknown", O({{"kind", S("bogus")}}));
    add("kind empty", O({{"kind", S("")}}));
    add("kind case", O({{"kind", S("Text")}}));
    add("kind unicode", O({{"kind", S(kUnicode)}}));
    // Media payload_kind.
    add("media no payload_kind", O({{"kind", S("media")}}));
    add("media bad payload_kind", O({{"kind", S("media")}, {"payload_kind", S("file")}}));
    add("media payload_kind number", O({{"kind", S("media")}, {"payload_kind", N(1)}}));
    add("media blob_ref missing", O({{"kind", S("media")}, {"payload_kind", S("blob_ref")}}));
    add("media blob_ref null", O({{"kind", S("media")}, {"payload_kind", S("blob_ref")}, {"blob_ref", Value::make_null()}}));
    add("media blob_ref string", O({{"kind", S("media")}, {"payload_kind", S("blob_ref")}, {"blob_ref", S("x")}}));
    add("media blob_ref wrong types",
        O({{"kind", S("media")}, {"payload_kind", S("blob_ref")},
           {"blob_ref", O({{"digest", N(1)}, {"media_type", B(true)}, {"size", S("12")}, {"store", A({})}})}}));
    add("media blob_ref fractional size",
        O({{"kind", S("media")}, {"payload_kind", S("blob_ref")}, {"blob_ref", O({{"size", N(12.75)}})}}));
    add("media uri missing", O({{"kind", S("media")}, {"payload_kind", S("uri")}}));
    add("media uri number", O({{"kind", S("media")}, {"payload_kind", S("uri")}, {"uri", N(5)}}));
    // base64.
    std::vector<std::string> const b64_cases = {"", "QQ==", "QQ", "QUI=", "QUI", "QUJD", "Q", "=", "====", "Q=Q=",
                                                "QQ==QQ==", "QU\nJD", "QU\r\nJD", "QUJD!", "QU JD", "QU\tJD", "-_-_",
                                                "\xC3\xA9", "AAAA////++++", "////", "A===", std::string(4001, 'A')};
    for (std::string const& b64 : b64_cases) {
        add("base64 '" + b64.substr(0, 16) + "'",
            O({{"kind", S("media")}, {"payload_kind", S("bytes")}, {"bytes_base64", S(b64)}}));
    }
    add("base64 missing", O({{"kind", S("media")}, {"payload_kind", S("bytes")}}));
    add("base64 number", O({{"kind", S("media")}, {"payload_kind", S("bytes")}, {"bytes_base64", N(1)}}));
    // Origin.
    add("bad origin", O({{"kind", S("text")}, {"origin", S("robot")}}));
    add("empty origin", O({{"kind", S("text")}, {"origin", S("")}}));
    add("origin case", O({{"kind", S("text")}, {"origin", S("User")}}));
    add("origin number (ignored)", O({{"kind", S("text")}, {"origin", N(2)}}));
    add("bad origin after bad base64",
        O({{"kind", S("media")}, {"payload_kind", S("bytes")}, {"bytes_base64", S("!!")}, {"origin", S("robot")}}));
    for (auto origin : kOrigins) {
        add("origin " + std::string(oldrec::origin_to_wire_string(origin)),
            O({{"kind", S("text")}, {"origin", S(std::string(oldrec::origin_to_wire_string(origin)))}}));
    }
    // Wrong-typed fields everywhere.
    add("text number", O({{"kind", S("text")}, {"text", N(5)}}));
    add("reasoning encrypted string", O({{"kind", S("reasoning")}, {"text", S("t")}, {"encrypted", S("true")}}));
    add("tainted string", O({{"kind", S("text")}, {"tainted", S("true")}}));
    add("tainted number", O({{"kind", S("text")}, {"tainted", N(1)}}));
    add("approval string", O({{"kind", S("text")}, {"approval", S("ack:forged")}}));
    add("approval empty string", O({{"kind", S("text")}, {"approval", S("")}}));
    add("approval number", O({{"kind", S("text")}, {"approval", N(7)}}));
    add("deliver true", O({{"kind", S("text")}, {"deliver_as_instructions", B(true)}}));
    add("deliver string", O({{"kind", S("text")}, {"deliver_as_instructions", S("true")}}));
    add("data schema_id number", O({{"kind", S("data")}, {"json", S("{}")}, {"schema_id", N(3)}}));
    add("data schema_id null", O({{"kind", S("data")}, {"schema_id", Value::make_null()}}));
    add("tool_call wrong types", O({{"kind", S("tool_call")}, {"call_id", N(1)}, {"tool_name", B(true)},
                                    {"arguments_json", O({})}}));
    add("tool_result content object", O({{"kind", S("tool_result")}, {"content", O({})}}));
    add("tool_result content string", O({{"kind", S("tool_result")}, {"content", S("x")}}));
    add("tool_result is_error number", O({{"kind", S("tool_result")}, {"is_error", N(1)}}));
    add("tool_result bad child", O({{"kind", S("tool_result")}, {"content", A({O({{"kind", S("nope")}})})}}));
    add("tool_result child missing kind", O({{"kind", S("tool_result")}, {"content", A({O({})})}}));
    add("tool_result child kind number", O({{"kind", S("tool_result")}, {"content", A({O({{"kind", N(1)}})})}}));
    add("tool_result child non-object", O({{"kind", S("tool_result")}, {"content", A({N(1)})}}));
    add("tool_result child marks",
        O({{"kind", S("tool_result")},
           {"content", A({O({{"kind", S("text")}, {"approval", S("ack:c")}, {"deliver_as_instructions", B(true)}})})}}));
    add("tool_result deep bad origin",
        O({{"kind", S("tool_result")},
           {"content", A({O({{"kind", S("tool_result")}, {"content", A({O({{"kind", S("text")}, {"origin", S("?")}})})}})})}}));
    add("citation spans strings", O({{"kind", S("citation")}, {"span_start", S("3")}, {"span_end", B(true)}}));
    add("citation spans fractional", O({{"kind", S("citation")}, {"span_start", N(3.9)}, {"span_end", N(1e15)}}));
    add("error message number", O({{"kind", S("error")}, {"message", N(1)}}));
    add("custom wrong types", O({{"kind", S("custom")}, {"type_id", A({})}, {"payload_json", N(0)}}));
    add("duplicate kind keys", O({{"kind", S("text")}, {"kind", S("bogus")}}));
    add("extra unknown keys", O({{"kind", S("text")}, {"zzz", S("y")}, {"attribution", O({})}}));

    for (auto const& [label, v] : cases) diff_decode_item(v, "malformed item: " + label);
}

void run_malformed_messages() {
    std::vector<std::pair<std::string, Value>> cases;
    auto add = [&](std::string label, Value v) { cases.emplace_back(std::move(label), std::move(v)); };
    add("null", Value::make_null());
    add("empty object", O({}));
    add("array", A({}));
    add("bad role", O({{"role", S("robot")}}));
    add("empty role", O({{"role", S("")}}));
    add("role number (defaults to user)", O({{"role", N(1)}}));
    for (auto r : kRoles) add("role", O({{"role", S(std::string(oldrec::role_to_wire_string(r)))}}));
    add("message_id number", O({{"role", S("user")}, {"message_id", N(1)}}));
    add("content object", O({{"content", O({})}}));
    add("content with bad item", O({{"content", A({O({{"kind", S("text")}}), O({{"kind", S("bogus")}})})}}));
    add("content with non-string kind", O({{"content", A({O({{"kind", N(1)}})})}}));
    add("content with bad base64", O({{"content", A({O({{"kind", S("media")}, {"payload_kind", S("bytes")},
                                                       {"bytes_base64", S("%%")}})})}}));
    add("bad role and bad item", O({{"role", S("x")}, {"content", A({O({{"kind", S("bogus")}})})}}));
    add("attribution full", O({{"attribution", O({{"contributor_index", N(4)}, {"contributor_type", S("rag")}})}}));
    add("attribution empty object", O({{"attribution", O({})}}));
    add("attribution wrong types",
        O({{"attribution", O({{"contributor_index", S("4")}, {"contributor_type", N(1)}})}}));
    add("attribution string", O({{"attribution", S("rag")}}));
    add("attribution null", O({{"attribution", Value::make_null()}}));
    add("attribution array", O({{"attribution", A({})}}));
    add("attribution fractional", O({{"attribution", O({{"contributor_index", N(2.5)}})}}));
    add("marks on items", O({{"role", S("system")},
                             {"content", A({O({{"kind", S("text")}, {"text", S("lesson")}, {"tainted", B(true)},
                                              {"approval", S("ack:1")}, {"deliver_as_instructions", B(true)}})})}}));
    for (auto const& [label, v] : cases) diff_decode_message(v, "malformed message: " + label);
}

void run_wire_strings() {
    for (auto r : kRoles) {
        check(ae::role_to_wire_string(r) == oldrec::role_to_wire_string(r), "role_to_wire_string recording");
        check(ae::rt::role_to_wire_string(r) == oldst::role_to_wire_string(r), "role_to_wire_string state");
    }
    for (auto o : kOrigins) {
        check(ae::origin_to_wire_string(o) == oldrec::origin_to_wire_string(o), "origin_to_wire_string recording");
        check(ae::rt::origin_to_wire_string(o) == oldst::origin_to_wire_string(o), "origin_to_wire_string state");
    }
    // Out-of-range enum values reach the old fallback strings ("user" / "assistant").
    auto const bad_role = static_cast<ae::role>(99);
    auto const bad_origin = static_cast<ae::content_origin>(99);
    check(ae::role_to_wire_string(bad_role) == oldrec::role_to_wire_string(bad_role), "role fallback recording");
    check(ae::rt::role_to_wire_string(bad_role) == oldst::role_to_wire_string(bad_role), "role fallback state");
    check(ae::origin_to_wire_string(bad_origin) == oldrec::origin_to_wire_string(bad_origin), "origin fallback rec");
    check(ae::rt::origin_to_wire_string(bad_origin) == oldst::origin_to_wire_string(bad_origin), "origin fallback st");

    for (std::string const& s :
         std::vector<std::string>{"system", "user", "assistant", "tool", "external", "", "robot", "USER", kUnicode}) {
        auto a = capture<ae::role>([&] { return ae::role_from_wire_string(s); });
        auto b = capture<ae::role>([&] { return oldrec::role_from_wire_string(s); });
        check(same(a, b), "role_from_wire_string recording '" + s + "'");
        auto c = capture<ae::role>([&] { return ae::rt::role_from_wire_string(s); });
        auto d = capture<ae::role>([&] { return oldst::role_from_wire_string(s); });
        check(same(c, d), "role_from_wire_string state '" + s + "'");
        auto e = capture<ae::content_origin>([&] { return ae::origin_from_wire_string(s); });
        auto f = capture<ae::content_origin>([&] { return oldrec::origin_from_wire_string(s); });
        check(same(e, f), "origin_from_wire_string recording '" + s + "'");
        auto g = capture<ae::content_origin>([&] { return ae::rt::origin_from_wire_string(s); });
        auto h = capture<ae::content_origin>([&] { return oldst::origin_from_wire_string(s); });
        check(same(g, h), "origin_from_wire_string state '" + s + "'");
    }
}

// ---- seeded fuzz --------------------------------------------------------------------------------------

class Fuzz {
public:
    explicit Fuzz(std::uint32_t seed) : rng_(seed) {}

    std::size_t below(std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng_); }
    bool coin() { return below(2) == 0; }

    std::string random_string() {
        static std::vector<std::string> const tokens = {
            "", "text", "reasoning", "media", "data", "tool_call", "tool_result", "citation", "error", "custom",
            "bytes", "uri", "blob_ref", "user", "assistant", "tool", "system", "external", "robot", "QQ==", "QUJD",
            "Q", "!!", "=", "ack:1", kUnicode};
        if (below(3) != 0) return tokens[below(tokens.size())];
        std::string s;
        std::size_t const n = below(12);
        for (std::size_t i = 0; i < n; ++i) s += static_cast<char>(below(95) + 32);
        return s;
    }

    // Non-negative and below 2^53: both old copies cast numbers to uint64 without a range check, which is
    // undefined for negatives and values >= 2^64 -- a shared property, not a difference (ADR-202 §4).
    double random_number() {
        switch (below(4)) {
            case 0: return 0;
            case 1: return static_cast<double>(below(10));
            case 2: return static_cast<double>(below(1000)) + 0.5;
            default: return static_cast<double>(std::size_t{1} << below(53));
        }
    }

    Value random_value(int depth) {
        switch (below(depth > 0 ? 7 : 5)) {
            case 0: return Value::make_null();
            case 1: return B(coin());
            case 2: return N(random_number());
            case 3:
            case 4: return S(random_string());
            case 5: {
                std::vector<Value> a;
                for (std::size_t i = below(3); i > 0; --i) a.push_back(random_value(depth - 1));
                return A(std::move(a));
            }
            default: {
                Obj o;
                for (std::size_t i = below(3); i > 0; --i) o.emplace_back(random_key(), random_value(depth - 1));
                return O(std::move(o));
            }
        }
    }

    std::string random_key() {
        static std::vector<std::string> const keys = {
            "kind", "text", "encrypted", "media_type", "payload_kind", "bytes_base64", "uri", "blob_ref", "digest",
            "size", "store", "json", "schema_id", "call_id", "tool_name", "arguments_json", "content", "is_error",
            "source", "span_start", "span_end", "message", "type_id", "payload_json", "origin", "tainted",
            "approval", "deliver_as_instructions", "role", "message_id", "attribution", "contributor_index",
            "contributor_type", "zzz"};
        return keys[below(keys.size())];
    }

    ae::ContentItem random_item(int depth) {
        ae::ContentItem c{};
        switch (below(depth > 0 ? 9 : 8)) {
            case 0: c.value = ae::Text{random_string()}; break;
            case 1: c.value = ae::Reasoning{random_string(), coin()}; break;
            case 2: {
                ae::Media m;
                m.media_type = random_string();
                switch (below(3)) {
                    case 0: m.payload = bytes_of(below(9), static_cast<unsigned>(below(100))); break;
                    case 1: m.payload = random_string(); break;
                    default:
                        m.payload = ae::BlobRef{random_string(), random_string(),
                                                static_cast<std::size_t>(random_number()), random_string()};
                }
                c.value = m;
                break;
            }
            case 3: c.value = ae::Data{random_string(), coin() ? std::optional<std::string>(random_string()) : std::nullopt}; break;
            case 4: c.value = ae::ToolCall{random_string(), random_string(), random_string()}; break;
            case 5:
                c.value = ae::Citation{random_string(), static_cast<std::size_t>(random_number()),
                                       static_cast<std::size_t>(random_number())};
                break;
            case 6: c.value = ae::Error{random_string()}; break;
            case 7: c.value = ae::Custom{random_string(), random_string()}; break;
            default: {
                ae::ToolResult tr{random_string(), {}, coin()};
                for (std::size_t i = below(3); i > 0; --i) tr.content.push_back(random_item(depth - 1));
                c.value = std::move(tr);
            }
        }
        c.origin = kOrigins[below(kOrigins.size())];
        c.tainted = coin();
        if (coin()) c.approval = random_string();
        c.deliver_as_instructions = coin();
        return c;
    }

    ae::Message random_message() {
        ae::Message m;
        m.role = kRoles[below(kRoles.size())];
        m.message_id = random_string();
        for (std::size_t i = below(4); i > 0; --i) m.content.push_back(random_item(2));
        if (coin()) m.attribution = ae::ContributorProvenance{static_cast<std::size_t>(random_number()), random_string()};
        return m;
    }

    // One mutation somewhere in the tree: drop a key, retype a value, set a known key, or replace an element.
    Value mutate(Value const& v, int depth) {
        if (v.is_object()) {
            Obj o = v.as_object();
            std::size_t const pick = below(o.size() + 1);
            if (pick < o.size() && depth < 6 && (o[pick].second.is_object() || o[pick].second.is_array()) && coin()) {
                o[pick].second = mutate(o[pick].second, depth + 1);
                return O(std::move(o));
            }
            switch (below(4)) {
                case 0:
                    if (!o.empty()) o.erase(o.begin() + static_cast<std::ptrdiff_t>(below(o.size())));
                    break;
                case 1:
                    if (!o.empty()) o[below(o.size())].second = random_value(2);
                    break;
                case 2: {
                    std::string const key = random_key();
                    bool replaced = false;
                    for (auto& [k, val] : o) {
                        if (k == key) {
                            val = random_value(2);
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) o.emplace_back(key, random_value(2));
                    break;
                }
                default:
                    if (!o.empty()) o[below(o.size())].second = S(random_string());
            }
            return O(std::move(o));
        }
        if (v.is_array()) {
            std::vector<Value> a = v.as_array();
            if (a.empty() || below(4) == 0) {
                a.push_back(random_value(2));
            } else {
                std::size_t const i = below(a.size());
                a[i] = (depth < 6 && coin()) ? mutate(a[i], depth + 1) : random_value(2);
            }
            return A(std::move(a));
        }
        return random_value(2);
    }

private:
    std::mt19937 rng_;
};

std::size_t run_fuzz(std::uint32_t seed, std::size_t iterations) {
    Fuzz f(seed);
    std::size_t cases = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        std::string const label = "fuzz seed=" + std::to_string(seed) + " i=" + std::to_string(i);
        if (f.coin()) {
            ae::ContentItem const x = f.random_item(2);
            diff_encode_item(x, label);
            Value j = f.coin() ? oldrec::content_item_to_json(x) : oldst::content_item_to_json(x);
            for (std::size_t k = f.below(3) + 1; k > 0; --k) j = f.mutate(j, 0);
            diff_decode_item(j, label);
        } else {
            ae::Message const m = f.random_message();
            diff_encode_message(m, label);
            Value j = f.coin() ? oldrec::message_to_json(m) : oldst::message_to_json(m);
            for (std::size_t k = f.below(3) + 1; k > 0; --k) j = f.mutate(j, 0);
            diff_decode_message(j, label);
        }
        ++cases;
    }
    return cases;
}

// ---- I3 ---------------------------------------------------------------------------------------------

ae::ContentItem approved_lesson() {
    ae::ContentItem c{};
    c.value = ae::Text{"always run the tests"};
    c.origin = ae::content_origin::system;
    c.tainted = true;
    c.approval = "ack:sha256:feed";
    c.deliver_as_instructions = true;
    return c;
}

bool mentions_marks(std::string const& s) {
    return s.find("approval") != std::string::npos || s.find("deliver_as_instructions") != std::string::npos;
}

void run_i3() {
    ae::ContentItem const lesson = approved_lesson();
    ae::ToolResult tr{"c", {lesson}, false};
    ae::ContentItem outer{};
    outer.value = tr;
    outer.approval = "ack:outer";
    outer.deliver_as_instructions = true;
    ae::Message msg{ae::role::system, {lesson, outer}, "m-1", std::nullopt};

    // Encode: the state profile never writes the marks, even when every item carries them.
    check(!mentions_marks(ae::json::dump(ae::rt::content_item_to_json(lesson))), "I3: state encode omits marks");
    check(!mentions_marks(ae::json::dump(ae::rt::content_item_to_json(outer))), "I3: state encode omits nested marks");
    check(!mentions_marks(ae::json::dump(ae::rt::message_to_json(msg))), "I3: state message encode omits marks");
    // Control: the recording profile writes them.
    std::string const rec = ae::json::dump(ae::content_item_to_json(lesson));
    check(rec.find("\"approval\":\"ack:sha256:feed\"") != std::string::npos, "control: recording writes approval");
    check(rec.find("\"deliver_as_instructions\":true") != std::string::npos, "control: recording writes deliver");

    // Decode: JSON that carries the marks (a crafted checkpoint) restores nothing through the state profile.
    Value const crafted = ae::content_item_to_json(outer);  // recording encoding: marks present, nested too
    auto st = ae::rt::content_item_from_json(crafted);
    check(st.has_value(), "I3: state decode succeeds");
    if (st) {
        check(st->approval.empty() && !st->deliver_as_instructions, "I3: state decode drops outer marks");
        auto const* inner = std::get_if<ae::ToolResult>(&st->value);
        check(inner != nullptr && inner->content.size() == 1 && inner->content[0].approval.empty() &&
                  !inner->content[0].deliver_as_instructions,
              "I3: state decode drops nested marks");
    }
    auto st_msg = ae::rt::message_from_json(ae::message_to_json(msg));
    check(st_msg.has_value(), "I3: state message decode succeeds");
    if (st_msg) {
        for (auto const& c : st_msg->content) {
            check(c.approval.empty() && !c.deliver_as_instructions, "I3: state message decode drops marks");
        }
    }
    // Control: the recording profile restores them.
    auto rc = ae::content_item_from_json(crafted);
    check(rc.has_value() && *rc == outer, "control: recording decode restores marks (outer and nested)");
    auto rc_msg = ae::message_from_json(ae::message_to_json(msg));
    check(rc_msg.has_value() && rc_msg->content == msg.content, "control: recording message decode restores marks");

    // Attribution (ADR-204): both profiles carry it, identically -- it is data (I4), never a mark.
    ae::Message attributed{ae::role::user, {lesson}, "m-2", ae::ContributorProvenance{2, "memory"}};
    auto st_attr = ae::rt::message_from_json(ae::rt::message_to_json(attributed));
    check(st_attr.has_value() && st_attr->attribution == attributed.attribution, "state round-trips attribution");
    auto rc_attr = ae::message_from_json(ae::message_to_json(attributed));
    check(rc_attr.has_value() && rc_attr->attribution == attributed.attribution,
          "ADR-204: recording round-trips attribution");
    check(rc_attr.has_value() && *rc_attr == attributed, "ADR-204: recording round-trips the whole message");
    auto rc_from_st = ae::message_from_json(ae::rt::message_to_json(attributed));
    check(rc_from_st.has_value() && rc_from_st->attribution == attributed.attribution,
          "ADR-204: recording decode reads the state encoding's attribution");
    auto st_from_rc = ae::rt::message_from_json(ae::message_to_json(attributed));
    check(st_from_rc.has_value() && st_from_rc->attribution == attributed.attribution &&
              st_from_rc->content.size() == 1 && st_from_rc->content[0].approval.empty() &&
              !st_from_rc->content[0].deliver_as_instructions,
          "ADR-204 + I3: state decode of a recording keeps attribution and still drops the marks");
    // Byte-level: the recording encoding's attribution member is the state encoding's, in the same (last) position.
    std::string const rec_attr = ae::json::dump(ae::message_to_json(attributed));
    std::string const st_attr_text = ae::json::dump(ae::rt::message_to_json(attributed));
    std::string const member = R"(,"attribution":{"contributor_index":2,"contributor_type":"memory"}})";
    check(rec_attr.ends_with(member) && st_attr_text.ends_with(member),
          "ADR-204: both profiles write attribution last, byte-identical\n  rec " + rec_attr + "\n  st  " + st_attr_text);
    // Absent attribution stays absent (omit-when-absent), and an old recording without it reads back with none.
    ae::Message plain{ae::role::user, {lesson}, "m-3", std::nullopt};
    std::string const rec_plain = ae::json::dump(ae::message_to_json(plain));
    check(rec_plain.find("attribution") == std::string::npos, "ADR-204: recording omits absent attribution");
    check(rec_plain == ae::json::dump(oldrec::message_to_json(plain)),
          "ADR-204: an unattributed message encodes byte-identically to the pre-ADR-204 recording codec");
    auto old_file = ae::message_from_json(oldrec::message_to_json(attributed));  // old writer dropped it
    check(old_file.has_value() && !old_file->attribution.has_value(),
          "ADR-204: an old recording (no attribution member) reads back with no attribution");

    // The two profiles are the only two, and differ where documented.
    using ae::message_json::Profile;
    static_assert(Profile::recording().carries_delivery_marks());
    static_assert(!Profile::state().carries_delivery_marks());
    static_assert(Profile::recording() != Profile::state());
    static_assert(Profile::recording().error_code_prefix() == "recording.");
    static_assert(Profile::state().error_code_prefix() == "rt.message_codec.");
}

// ---- harness self-control ----------------------------------------------------------------------------

// The comparison must be able to fail: the two OLD codecs genuinely differ on marks, attribution and codes, and
// `same`/dump comparison must say so. If this ever passes as "equal", the harness proves nothing.
void run_self_control() {
    ae::ContentItem const lesson = approved_lesson();
    check(ae::json::dump(oldrec::content_item_to_json(lesson)) != ae::json::dump(oldst::content_item_to_json(lesson)),
          "self-control: the two old encoders differ on marks");
    Value const j = O({{"kind", S("bogus")}});
    auto a = capture<ae::ContentItem>([&] { return oldrec::content_item_from_json(j); });
    auto b = capture<ae::ContentItem>([&] { return oldst::content_item_from_json(j); });
    check(!same(a, b), "self-control: error codes differ between the old decoders");
    auto c = capture<ae::ContentItem>([&] { return oldrec::content_item_from_json(ae::content_item_to_json(lesson)); });
    auto d = capture<ae::ContentItem>([&] { return oldst::content_item_from_json(ae::content_item_to_json(lesson)); });
    check(!same(c, d), "self-control: decoded values differ on marks");
    Value const k = O({{"kind", N(1)}});
    auto e = capture<ae::ContentItem>([&] { return oldrec::content_item_from_json(k); });
    auto f = capture<ae::ContentItem>([&] { return oldst::content_item_from_json(k); });
    check(e.st == Outcome<ae::ContentItem>::state::threw && f.st == Outcome<ae::ContentItem>::state::err,
          "self-control: old recording throws on non-string kind, old state returns an error");
    // ADR-204's adjusted expectation is not the oracle itself: for an attributed message it differs from the old
    // recording encoding and decoding, so the attributed comparisons would fail if the new codec still dropped it.
    ae::Message const attributed{ae::role::user, {}, "m", ae::ContributorProvenance{7, "rag"}};
    check(ae::json::dump(expected_recording_encoding(attributed)) != ae::json::dump(oldrec::message_to_json(attributed)),
          "self-control: the ADR-204 expected encoding differs from the old recording encoding when attributed");
    Value const attributed_json = oldst::message_to_json(attributed);
    auto const old_decode = capture<ae::Message>([&] { return oldrec::message_from_json(attributed_json); });
    check(!same(expected_recording_decode(attributed_json, old_decode), old_decode),
          "self-control: the ADR-204 expected decode differs from the old recording decode when attributed");
}

}  // namespace

int main() {
    run_self_control();
    run_wire_strings();
    run_item_corpus();
    run_message_corpus();
    run_malformed_items();
    run_malformed_messages();
    run_i3();
    std::size_t fuzz_cases = 0;
    for (std::uint32_t seed : {0xAE202u, 1u, 20260927u}) fuzz_cases += run_fuzz(seed, 1500);

    std::printf("encodings compared: %zu; decodings compared: %zu (ok %zu, error %zu, threw %zu); fuzz cases: %zu; "
                "checks: %d\n",
                g_counts.encode, g_counts.decode, g_counts.decode_ok, g_counts.decode_err, g_counts.decode_threw,
                fuzz_cases, g_checks);
    std::printf("recording messages (ADR-204): encoded %zu without / %zu with attribution; decoded %zu without / %zu "
                "with an \"attribution\" member\n",
                g_attr_counts.encode_plain, g_attr_counts.encode_attributed, g_attr_counts.decode_plain,
                g_attr_counts.decode_attributed);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_message_json_equivalence: all passed\n");
    return 0;
}
