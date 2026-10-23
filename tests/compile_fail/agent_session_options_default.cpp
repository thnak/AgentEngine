// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, G-C1). `AgentSessionOptions` has no
// default: its `choices` member has no default constructor, so `AgentSessionOptions opts;` is ill-formed. Before
// ADR-244 it defaulted to a shared identity ("s-agent"/"p-agent") and the agent's own bounds. If this ever compiles,
// a bridged session can again run under choices its caller never made.

#include "agentengine/core/agent_session_bridge.hpp"

int main() {
    agentengine::AgentSessionOptions opts;
    return opts.session_id.empty() ? 0 : 1;
}
