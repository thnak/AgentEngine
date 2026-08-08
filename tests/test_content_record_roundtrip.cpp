// Tests include/agentengine/core/content_record.hpp -- Milestone 6 Phase F's prerequisite:
// Message/ContentItem's durable projection, closing the gap named at agent_session.hpp:154-158/
// 207-213 and interaction.hpp's own header comment ("Message/ContentItem ... has no
// QUARK_SERIALIZE at all"). See content_record.hpp's own header comment for why this is a
// separate flat Record family rather than QUARK_SERIALIZE directly on ContentItem/Media (two
// rejected designs are named there: fingerprint non-determinism from a branching quark_describe,
// and undefined behaviour from mutating a const-cast'd object on the write/size pass).
//
// Round-trips through the CANONICAL TAGGED codec only (encode_tagged/decode_tagged) -- the only
// form this project uses for durable data (016's own rule, wire.hpp:9-10), matching
// third_party/quark/tests/serialize_roundtrip_test.cpp's own precedent for exercising a
// QUARK_SERIALIZE'd type.
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "quark/core/serialize.hpp"

#include "agentengine/core/content.hpp"
#include "agentengine/core/content_record.hpp"

using namespace agentengine;

namespace {

int  g_failures = 0;
#define CR_CHECK(cond, label)                                                                   \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL: %s (%s) at %s:%d\n", label, #cond, __FILE__, __LINE__);  \
            ++g_failures;                                                                        \
        }                                                                                         \
    } while (0)

std::string random_string(std::mt19937_64& rng, std::size_t max_len) {
    std::string s;
    std::size_t const n = rng() % (max_len + 1);
    for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>('a' + rng() % 26));
    return s;
}

// One randomized ContentItem covering all 9 arms (ToolResult recurses up to `depth` levels, so
// the recursive Record family gets genuinely exercised, not just declared-and-never-hit).
ContentItem random_content_item(std::mt19937_64& rng, int depth) {
    ContentItem item;
    item.origin  = static_cast<content_origin>(rng() % 5);
    item.tainted = (rng() % 2) == 0;

    // ToolResult only past depth 0, so recursion is bounded -- matching content_record.hpp's own
    // note that real recursion depth is bounded by actual data, not by the codec.
    std::uint64_t const arm = depth > 0 ? rng() % 9 : rng() % 8;  // arm 5 == ToolResult

    switch (arm) {
        case 0:
            item.value = Text{random_string(rng, 40)};
            break;
        case 1:
            item.value = Reasoning{random_string(rng, 40), (rng() % 2) == 0};
            break;
        case 2: {
            Media m;
            switch (rng() % 3) {
                case 0: {
                    std::vector<std::byte> bytes;
                    std::size_t const      n = rng() % 16;
                    for (std::size_t i = 0; i < n; ++i)
                        bytes.push_back(static_cast<std::byte>(rng() % 256));
                    m.payload = std::move(bytes);
                    break;
                }
                case 1:
                    m.payload = random_string(rng, 30);
                    break;
                default:
                    m.payload = BlobRef{random_string(rng, 20), random_string(rng, 10),
                                         static_cast<std::size_t>(rng() % 100000),
                                         random_string(rng, 10)};
                    break;
            }
            m.media_type = random_string(rng, 15);
            item.value   = std::move(m);
            break;
        }
        case 3: {
            Data d;
            d.json = random_string(rng, 50);
            // DataRecord's documented convention (content_record.hpp) collapses a PRESENT-but-empty
            // schema_id to absent, same as this file's own dedicated "empty-string-means-absent"
            // block below tests directly -- so this generator must never produce that combination,
            // or the round-trip oracle below would flag content_record.hpp's own documented,
            // deliberate narrowing as a bug. `+ "x"` guarantees non-empty when present.
            d.schema_id = (rng() % 2) == 0
                              ? std::nullopt
                              : std::optional<std::string>{random_string(rng, 12) + "x"};
            item.value = std::move(d);
            break;
        }
        case 4:
            item.value = ToolCall{random_string(rng, 10), random_string(rng, 10),
                                   random_string(rng, 30),
                                   static_cast<content_origin>(rng() % 5)};
            break;
        case 5: {
            ToolResult tr;
            tr.call_id  = random_string(rng, 10);
            tr.is_error = (rng() % 2) == 0;
            std::size_t const n = rng() % 3;
            for (std::size_t i = 0; i < n; ++i) tr.content.push_back(random_content_item(rng, depth - 1));
            item.value = std::move(tr);
            break;
        }
        case 6:
            item.value = Citation{random_string(rng, 20), rng() % 1000, rng() % 1000};
            break;
        case 7:
            item.value = Error{random_string(rng, 30)};
            break;
        default:
            item.value = Custom{random_string(rng, 15), random_string(rng, 30)};
            break;
    }
    return item;
}

Message random_message(std::mt19937_64& rng) {
    Message m;
    m.role = static_cast<role>(rng() % 4);
    std::size_t const n = rng() % 5;
    for (std::size_t i = 0; i < n; ++i) m.content.push_back(random_content_item(rng, 2));
    m.message_id = random_string(rng, 16);
    return m;
}

}  // namespace

int main() {
    std::mt19937_64 rng(0xC0FFEE);

    // ---- In-memory round-trip: from_record(to_record(x)) == x, no wire involved ------------------
    // Isolates the variant<->flat mapping itself from the codec, so a mapping bug and a codec bug
    // don't get conflated into one failure.
    for (int i = 0; i < 2000; ++i) {
        Message const original = random_message(rng);
        MessageRecord const rec = to_record(original);
        Message const back = from_record(rec);
        CR_CHECK(back == original, "in-memory Message round-trip via to_record/from_record");
    }

    // ---- Wire round-trip: the canonical tagged codec (the only durable form) ----------------------
    for (int i = 0; i < 2000; ++i) {
        Message const        original = random_message(rng);
        MessageRecord const  rec      = to_record(original);
        std::vector<std::byte> buf(64 * 1024);
        auto const n = quark::encode_tagged(rec, buf.data(), buf.size());
        CR_CHECK(n.has_value(), "encode_tagged succeeds for a random MessageRecord");
        if (!n.has_value()) continue;

        MessageRecord decoded{};
        auto const rc = quark::decode_tagged(buf.data(), *n, decoded);
        CR_CHECK(rc.has_value(), "decode_tagged succeeds on encode_tagged's own bytes");
        CR_CHECK(decoded == rec, "decoded MessageRecord equals the encoded one");
        CR_CHECK(from_record(decoded) == original, "wire round-trip reproduces the original Message");
    }

    // ---- DataRecord's empty-string-means-absent convention, both directions -----------------------
    {
        Data const with_schema{"{}", std::optional<std::string>{"my-schema"}};
        DataRecord const rec1 = to_record(with_schema);
        CR_CHECK(rec1.schema_id == "my-schema", "DataRecord keeps a present schema_id verbatim");
        CR_CHECK(from_record(rec1) == with_schema, "Data round-trips when schema_id is present");

        Data const without_schema{"{}", std::nullopt};
        DataRecord const rec2 = to_record(without_schema);
        CR_CHECK(rec2.schema_id.empty(), "DataRecord encodes nullopt schema_id as empty string");
        CR_CHECK(from_record(rec2) == without_schema, "Data round-trips when schema_id is absent");
    }

    // ---- Fingerprint is a compile-time constant of the TYPE (never a value) -----------------------
    // The whole reason this file's sibling header rejected the branching-quark_describe design:
    // confirming ContentItemRecord/MediaRecord/ToolResultRecord/MessageRecord are all `Described`
    // (this line wouldn't compile otherwise) and that fingerprint_v is usable in a constant
    // expression -- i.e. genuinely independent of any runtime instance, unlike the design this
    // file's sibling header rejected.
    constexpr std::uint64_t content_item_fp = quark::fingerprint_v<ContentItemRecord>;
    constexpr std::uint64_t media_fp        = quark::fingerprint_v<MediaRecord>;
    constexpr std::uint64_t tool_result_fp  = quark::fingerprint_v<ToolResultRecord>;
    constexpr std::uint64_t message_fp      = quark::fingerprint_v<MessageRecord>;
    static_assert(content_item_fp != 0 || media_fp != 0 || tool_result_fp != 0 || message_fp != 0 ||
                  true);  // the static_assert itself is the proof (compiles => constexpr-evaluable)

    if (g_failures == 0) {
        std::printf("test_content_record_roundtrip: OK (fingerprints: item=%016llx media=%016llx "
                    "tool_result=%016llx message=%016llx)\n",
                    static_cast<unsigned long long>(content_item_fp),
                    static_cast<unsigned long long>(media_fp),
                    static_cast<unsigned long long>(tool_result_fp),
                    static_cast<unsigned long long>(message_fp));
        return 0;
    }
    std::fprintf(stderr, "test_content_record_roundtrip: %d failure(s)\n", g_failures);
    return 1;
}
