// Positive control for bound_omitted_member.cpp (decisions/ADR-244-required-choices-step-1b.md §2.1,
// G-C1): an aggregate whose `Bound` members are all stated compiles, with either spelling.

#include <cstdint>

#include "agentengine/core/bound.hpp"

namespace {
struct Limits {
    agentengine::Bound<std::uint64_t> memory_bytes;
    agentengine::Bound<std::uint32_t> pids;
};
}  // namespace

int main() {
    Limits limits{.memory_bytes = agentengine::Bound<std::uint64_t>::at_most(1u << 20),
                  .pids         = agentengine::Bound<std::uint32_t>::unbounded()};
    return limits.pids.is_unbounded() ? 0 : 1;
}
