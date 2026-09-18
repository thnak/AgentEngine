// ADR-176 §14/§15 compile-fail proof (NEGATIVE half).
//
// A tool reply that declares `image` but spells its provenance companions as anything other than a plain
// `std::string` must NOT build. `std::optional<std::string>` is the spelling that motivated this: it
// publishes exactly `{"type":"string"}` in the reply schema, so it is invisible on the wire, while failing
// `ImageProvenanceReply`. The first version of `stamp_image_provenance()` used a bare `requires` and
// SKIPPED such a reply silently -- including its `image` field -- so the tool shipped
// `{"image":"alpine:latest"}` with no digest and no kind beside it: a provenance claim with its
// falsifiable half missing, which ADR-176 §2 calls worse than a visible hole. A red-team round found it by
// compiling it.
//
// What this file proves is that the REQUIREMENT rejects the shape. That the provider enforces the
// requirement is one line -- `static_assert(agentengine::ImageProvenanceReply<ReplyT>, ...)` in
// `stamp_image_provenance()` -- and is not re-proven here, because reaching that private member from a
// compile test would fail on ACCESS and a fail-only proof cannot tell one compile error from another.
//
// Positive control: image_provenance_stampable_reply.cpp.

#include <optional>
#include <string>

#include "agentengine/sandbox/execution_surface.hpp"

namespace {

struct UnstampableReply {
    int exit_code = 0;
    std::string image;
    std::optional<std::string> image_digest;       // reads as a plain string on the wire
    std::optional<std::string> image_digest_kind;  // ...and so does this one
};

template <class R>
void requires_stampable_provenance() {
    static_assert(agentengine::ImageProvenanceReply<R>,
                  "a reply declaring `image` must carry both companions as plain std::string");
}

}  // namespace

int main() {
    requires_stampable_provenance<UnstampableReply>();
    return 0;
}
