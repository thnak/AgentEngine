// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 5, G-C2): a CRTP capability
// tag whose cap is `at_most(0)`. `0` is not a limit (use `unbounded()` for none); the consteval factory refuses it at
// the declaration.

#include "agentengine/core/bound_value.hpp"
#include "agentengine/trust/capability.hpp"

int main() {
    using namespace agentengine;
    auto const capability = to_capability(cap::decl::FsRead<"work", at_most(0)>{});
    return capability_kind_of(capability) == capability_kind::fs_read ? 1 : 0;
}
