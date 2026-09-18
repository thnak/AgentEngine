// ADR-176 §14/§15 compile-fail proof (POSITIVE control).
//
// The counterpart to image_provenance_unstampable_reply.cpp. A fail-only suite cannot distinguish
// "correctly rejected" from "nothing in this directory compiles at all" -- this file is the same shape
// with the three fields spelled as plain `std::string`s, which is what every real tool reply in
// `mandatory_sandbox_provider.hpp` uses, and it MUST build.

#include <string>

#include "agentengine/sandbox/execution_surface.hpp"

namespace {

struct StampableReply {
    int exit_code = 0;
    std::string image;
    std::string image_digest;
    std::string image_digest_kind;
};

template <class R>
void requires_stampable_provenance() {
    static_assert(agentengine::ImageProvenanceReply<R>,
                  "a reply declaring `image` must carry both companions as plain std::string");
}

}  // namespace

int main() {
    requires_stampable_provenance<StampableReply>();
    return 0;
}
