#pragma once
// Implements decisions/ADR-244-required-choices-step-1b.md §2.1 (header-split plan, 2026-10-23): the one error
// shape for a choice that was never made, split out of core/bound.hpp so that header, which nearly every
// translation unit includes, does not change when this one does.
//
// `choice_missing()` builds class `contract`, code `choice.missing.<resource>`, and a message that lists the
// options. Callers pass the same option table their parser reads, so an option added there is in the message too.

#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "agentengine/core/error.hpp"

namespace agentengine {

// The error for a choice nobody made. `options` is the table the resource's parser reads.
[[nodiscard]] inline error choice_missing(std::string_view resource, std::span<std::string_view const> options) {
    std::string message = "no choice was made for " + std::string(resource) + "; choose one of: ";
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (i != 0) message += ", ";
        message += options[i];
    }
    return error{failure_class::contract, std::move(message), "choice.missing." + std::string(resource)};
}

}  // namespace agentengine
