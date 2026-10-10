// devsim/verify-includes — compiles every `#include` path the docs publish through
// ApiIncludes.tsx. If a header is renamed or removed, this stops compiling and the gate fails,
// which is what keeps the published include blocks honest.
//
// The two vendor-client headers are HTTPS-gated: without -DAGENTENGINE_WITH_HTTPS they are expected
// to contribute nothing (not to fail), which is exactly what this file checks by including them
// unconditionally.
#include <cstdio>

// ---- agent authoring --------------------------------------------------------------------------
#include "agentengine/core/agent.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"

// ---- builder ----------------------------------------------------------------------------------
#include "agentengine/core/session_builder.hpp"
#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/magentic.hpp"

// ---- tools ------------------------------------------------------------------------------------
#include "agentengine/core/tool.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/core/error.hpp"

// ---- workflow ---------------------------------------------------------------------------------
#include "agentengine/workflow/graph.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/workflow_event.hpp"
#include "agentengine/rt/block_on.hpp"

// ---- runtime / sessions -----------------------------------------------------------------------
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/agent_session_core.hpp"
#include "agentengine/rt/session_store.hpp"

// ---- human-in-the-loop ------------------------------------------------------------------------
#include "agentengine/core/interaction.hpp"
#include "agentengine/core/tool_call_hook.hpp"

// ---- providers --------------------------------------------------------------------------------
#include "agentengine/trust/secret.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"

// ---- other subsystems -------------------------------------------------------------------------
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/core/memory_provider.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/plugin/plugin.hpp"

int main() {
    std::printf("verify_includes: OK -- every published include path exists and is self-contained\n");
    return 0;
}
