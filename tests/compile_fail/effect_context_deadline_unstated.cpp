// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 5): an `EffectContext` with no
// stated deadline. `deadline` used to be a `steady_clock::time_point` whose zero value silently meant "no time limit";
// `Deadline` has no default constructor, so a context states `Deadline::at(t)` or the named `Deadline::none()`.

#include "agentengine/core/effect_context.hpp"

int main() {
    agentengine::EffectContext ctx{};  // no deadline stated
    (void)ctx;
    return 0;
}
