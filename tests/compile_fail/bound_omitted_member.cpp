// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.1, G-C1). `Bound<T>` has
// no default constructor, so an aggregate that leaves a `Bound` member out of its initializer is
// ill-formed: the member is copy-initialized from `{}`, which needs the deleted constructor. If this
// ever compiles, an omitted limit can again mean "no limit" -- the implicit choice ADR-242 §2 forbids.

#include <cstdint>

#include "agentengine/core/bound.hpp"

namespace {
struct Limits {
    agentengine::Bound<std::uint64_t> memory_bytes;
    agentengine::Bound<std::uint32_t> pids;
};
}  // namespace

int main() {
    Limits limits{.memory_bytes = agentengine::Bound<std::uint64_t>::at_most(1u << 20)};  // pids omitted
    return limits.memory_bytes.is_unbounded() ? 1 : 0;
}
