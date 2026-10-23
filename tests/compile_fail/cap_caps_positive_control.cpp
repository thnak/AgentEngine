// Positive control for the cap_rejects_* proofs (decisions/ADR-244-required-choices-step-1b.md §2.2, slice 5,
// G-C1/G-C2): every capability cap is stated -- a `cap::Limit` at runtime, a `BoundValue` in a CRTP tag -- and this
// file compiles. Each negative differs from it in exactly one place, so a rejection cannot be for another reason.

#include "agentengine/core/bound_value.hpp"
#include "agentengine/trust/capability.hpp"

using namespace agentengine;

int main() {
    using L = cap::Limit;
    Capability const caps[] = {
        cap::FsRead{"work", "", L::at_most(1024)},
        cap::FsWrite{"work", "", L::at_most(4096), L::unbounded()},
        cap::NetOut{{"api.example:443:https"}, L::at_most(1 << 20), {}},
        cap::Exec{"native_jail", L::at_most(1000), L::at_most(2000), L::unbounded()},
        cap::NativeExec{"node", "work", L::unbounded(), L::at_most(30000), L::at_most(1 << 28), false, L::unbounded(),
                        L::unbounded()},
    };
    Capability const tagged[] = {
        to_capability(cap::decl::FsRead<"work", at_most(1024)>{}),
        to_capability(cap::decl::FsWrite<"work", at_most(4096), unbounded()>{}),
        to_capability(cap::decl::NetOut<"api.example", unbounded()>{}),
        to_capability(cap::decl::Exec<"native_jail", at_most(1000), at_most(2000), unbounded()>{}),
        to_capability(cap::decl::NativeExec<"node", "work", unbounded(), at_most(30000), at_most(1 << 28)>{}),
    };
    return (sizeof(caps) + sizeof(tagged)) == 0 ? 1 : 0;
}
