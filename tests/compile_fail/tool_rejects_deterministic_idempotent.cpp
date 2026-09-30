// This file MUST NOT compile (decisions/ADR-212-deterministic-tool-output-declaration.md §3.3) -- see
// tests/compile_fail/CMakeLists.txt. An `idempotent` re-run is safe only under the original
// idempotency key, which a verifying node does not get; with it, a correct backend would replay the
// stored first reply and the comparison would prove nothing. Only `pure` qualifies.

#include "agentengine/core/tool.hpp"

using namespace agentengine;

struct BadTool : Tool<BadTool, Deterministic, EffectClass<effect_class::idempotent>> {};

int main() { return 0; }
