// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.3, G-C1). `RequestAuthority::expiry`
// is an `Expiry` -- `Expiry::at(t)` or `Expiry::never()` -- with no default; the old default-constructed
// `time_point` meant "already expired" by accident and "never" was spelled `time_point::max()`. If this ever
// compiles, an authority's lifetime is an omission again.

#include "agentengine/rt/agent_session.hpp"

int main() {
    agentengine::rt::RequestAuthority const authority{.principal = agentengine::Principal{"p", ""},
                                                      .capabilities = nullptr};  // expiry omitted
    return authority.principal.id.empty() ? 1 : 0;
}
