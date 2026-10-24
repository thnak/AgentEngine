#pragma once
// decisions/ADR-244-required-choices-step-1b.md §2.5 (G-C9): an image a sandbox surface boots from is named by digest.
//
// A tag names different bytes on two hosts, or on one host a week apart; the surface would then run code nobody chose.
// `check_image_pinned` accepts only `<repository>@sha256:<64 lowercase hex>` and refuses everything else with
// `sandbox.image_not_pinned`. The Docker, containerd and Kata backends call it at `create()`, before any CLI call, so
// the refusal is host-free and the constructors stay infallible.

#include <algorithm>
#include <expected>
#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"

namespace agentengine {

[[nodiscard]] inline agentengine::result<void> check_image_pinned(std::string_view image) {
    constexpr std::string_view kMarker = "@sha256:";
    auto const at = image.rfind(kMarker);
    bool pinned   = at != std::string_view::npos && at > 0;
    if (pinned) {
        std::string_view const hex = image.substr(at + kMarker.size());
        pinned = hex.size() == 64 && std::all_of(hex.begin(), hex.end(), [](char c) {
                     return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                 });
    }
    if (pinned) return {};
    return std::unexpected(agentengine::error{
        agentengine::failure_class::policy,
        "image '" + std::string(image) +
            "' is not digest-pinned: state it as <repository>@sha256:<64 hex>. A tag names different bytes on two hosts "
            "(ADR-244 section 2.5)",
        "sandbox.image_not_pinned"});
}

}  // namespace agentengine
