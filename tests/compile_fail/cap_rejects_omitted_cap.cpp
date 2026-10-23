// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 5, G-C1): a capability that
// leaves a cap out. `cap::Limit` has no default constructor, so an omitted `size_cap_bytes` is ill-formed instead of
// reading as "uncapped". If this ever compiles, a grant can again carry no limit by omission (ADR-242 §2 rule 5).

#include "agentengine/trust/capability.hpp"

int main() {
    agentengine::cap::FsRead const read{"work", ""};  // size_cap_bytes omitted
    return read.mount_id.empty() ? 1 : 0;
}
