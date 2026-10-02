#pragma once

// Precompiled header shared by the plain test executables when AGENTENGINE_TEST_PCH is ON
// (cmake/AgentEngineTestPch.cmake). Build-speed only: nothing may depend on it. Every test still
// includes what it uses, and the CI legs that leave the option OFF prove that.
//
// Contents were chosen from a clang -ftime-trace of the dev build (2026-10-02): headers that at least
// half of the 303 plain test TUs already include, ranked by summed parse cost. A header only a few
// tests use does not belong here -- it would be parsed into every test's PCH load for no gain.

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/capability.hpp"
