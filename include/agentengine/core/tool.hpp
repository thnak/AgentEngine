#pragma once
// Implements 006-Tool-and-Function-Plane.md — one declaration, one invocation path, one approval
// model for tools regardless of source (native, WASM plugin, MCP server, remote agent, sandboxed
// script, composite workflow).

#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"

namespace agentengine {

enum class approval_mode { never_require, always_require, policy_driven };

enum class tool_source { native, wasm_plugin, mcp_server, remote_agent, sandboxed_script, composite };

// The ten-step invocation pipeline (006 §3) is host machinery, not part of a tool's own shape, and
// is not modeled here. `Tool` fixes the declaration surface only: a schema-typed name plus an
// `invoke` reachable exclusively through that pipeline.
template <class Derived>
struct Tool {
    // Derived provides: static name, static description, nested Args/Reply types (JSON Schema
    // 2020-12 derived from them, 006 §1), and `static ae::task<result<Reply>> invoke(Args,
    // EffectContext&)`. Policies (Capabilities<...>, Approval<...>, Parallelizable, Timeout<...>)
    // are template parameters on Derived's declaration (002), not on this base.
};

} // namespace agentengine
