// Positive control for clock_default_rejected: the same program with the resolution stated, both spellings.

#include <chrono>

#include "agentengine/trust/capability.hpp"

int main() {
    using namespace agentengine;
    cap::Clock exact{cap::ClockResolution::finest()};
    cap::Clock coarse{cap::ClockResolution::at_resolution(std::chrono::milliseconds{100})};
    return exact.resolution.is_finest() && !coarse.resolution.is_finest() ? 0 : 1;
}
