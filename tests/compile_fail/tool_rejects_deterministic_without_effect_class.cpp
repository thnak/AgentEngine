// This file MUST NOT compile (decisions/ADR-212-deterministic-tool-output-declaration.md §3.3) -- see
// tests/compile_fail/CMakeLists.txt. `Deterministic` is only ever checked by running the call again;
// with no `EffectClass` the tool defaults to `at_most_once`, which forbids that. The author must state
// `pure` or `idempotent` explicitly rather than have the claim accepted by omission.

#include "agentengine/core/tool.hpp"

using namespace agentengine;

struct BadTool : Tool<BadTool, Deterministic> {};

int main() { return 0; }
