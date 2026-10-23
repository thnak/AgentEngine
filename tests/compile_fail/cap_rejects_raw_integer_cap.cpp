// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 5, G-C2): a bare integer as a
// capability cap. A number nobody wrapped in `Limit::at_most` (and so nobody checked against `0`) has no conversion.

#include "agentengine/trust/capability.hpp"

int main() {
    agentengine::cap::FsRead const read{"work", "", 1024};  // not a Limit
    return read.mount_id.empty() ? 1 : 0;
}
