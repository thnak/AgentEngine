// This file MUST NOT compile (decisions/ADR-212-deterministic-tool-output-declaration.md §3.3) -- see
// tests/compile_fail/CMakeLists.txt. An `at_most_once` tool may not be re-run without operator
// acknowledgement (019 §6), so a claim that is only checkable by re-running it is contradictory.

#include "agentengine/core/tool.hpp"

using namespace agentengine;

struct BadTool : Tool<BadTool, Deterministic, EffectClass<effect_class::at_most_once>> {};

int main() { return 0; }
