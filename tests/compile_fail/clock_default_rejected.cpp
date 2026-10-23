// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 5): a `cap::Clock` with no
// stated resolution. A resolution of `0` used to mean "exact time" without anyone saying so; `ClockResolution` has no
// default constructor and `finest()` is the named spelling.

#include "agentengine/trust/capability.hpp"

int main() {
    agentengine::cap::Clock clock{};  // no resolution stated
    (void)clock;
    return 0;
}
