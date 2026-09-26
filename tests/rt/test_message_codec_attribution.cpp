// Closes decisions/ADR-066-context-provider-attribution-provenance.md §7's named residual:
// "attribution is stamped only inside assemble_context()'s own loop ... wiring attribution through
// rt/message_codec.hpp's JSON codec -- checked, confirmed absent; the field does not currently
// survive a JSON round-trip through that codec." Proves message_to_json()/message_from_json() now
// round-trip Message::attribution byte-identically (Message already has a default operator==), and
// that a genuinely-absent attribution (nullopt) round-trips back to nullopt, never a
// zeroed/default-constructed ContributorProvenance.

#include <iostream>
#include <string>

#include "agentengine/core/content.hpp"
#include "agentengine/rt/message_codec.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    // -- attribution present: round-trips byte-identically --------------------------------------
    Message with_attribution;
    with_attribution.role = role::system;
    ContentItem item{};
    item.value = Text{"a summary"};
    item.origin = content_origin::system;
    with_attribution.content.push_back(std::move(item));
    with_attribution.message_id = "m-1";
    with_attribution.attribution = ContributorProvenance{2, "history"};

    json::Value encoded = rt::message_to_json(with_attribution);
    result<Message> decoded = rt::message_from_json(encoded);
    AE_CHECK(decoded.has_value(), "R1: a Message with attribution round-trips without error");
    AE_CHECK(decoded.has_value() && decoded->attribution.has_value(),
             "R2: attribution survives the round-trip as a present value, not nullopt");
    AE_CHECK(decoded.has_value() && decoded->attribution.has_value() &&
                 decoded->attribution->contributor_index == 2 &&
                 decoded->attribution->contributor_type == "history",
             "R3: contributor_index/contributor_type are byte-identical after the round-trip");
    AE_CHECK(decoded.has_value() && *decoded == with_attribution,
             "R4: the whole Message (via its own operator==) is byte-identical after the round-trip");

    // -- attribution absent: round-trips to nullopt, never a zeroed ContributorProvenance -------
    Message without_attribution;
    without_attribution.role = role::user;
    ContentItem plain_item{};
    plain_item.value = Text{"hello"};
    plain_item.origin = content_origin::user;
    without_attribution.content.push_back(std::move(plain_item));
    without_attribution.message_id = "m-2";
    // without_attribution.attribution left at its default: std::nullopt

    json::Value encoded_no_attr = rt::message_to_json(without_attribution);
    AE_CHECK(encoded_no_attr.find("attribution") == nullptr,
             "R5: an absent attribution is omitted from the wire JSON entirely, not encoded as null");

    result<Message> decoded_no_attr = rt::message_from_json(encoded_no_attr);
    AE_CHECK(decoded_no_attr.has_value() && !decoded_no_attr->attribution.has_value(),
             "R6: decoding a Message with no 'attribution' key produces nullopt, not a "
             "zeroed/default-constructed ContributorProvenance");
    AE_CHECK(decoded_no_attr.has_value() && *decoded_no_attr == without_attribution,
             "R7: the whole Message round-trips byte-identically when attribution was never set");

    std::cout << (g_failures == 0 ? "test_message_codec_attribution: OK\n"
                                   : "test_message_codec_attribution: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
