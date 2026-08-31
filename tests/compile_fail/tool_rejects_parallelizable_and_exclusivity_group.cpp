// This file MUST NOT compile (decisions/ADR-158-tool-concurrency-exclusivity-policy.md §4, MUST-FIX
// 1) — see tests/CMakeLists.txt's try_compile() gate. `Parallelizable` (unconditional: safe
// alongside every other tool) and `ExclusivityGroup<Name>` (narrower: safe alongside everyone
// EXCEPT its own group) are two different, contradictory concurrency claims about the same tool —
// declaring both must fail to compile, not silently accept one or the other.

#include "agentengine/core/tool.hpp"

using namespace agentengine;

// must not compile: BadTool claims both "unconditionally concurrency-safe" and "concurrency-safe
// except within group db-write" about itself at once.
struct BadTool : Tool<BadTool, Parallelizable, ExclusivityGroup<"db-write">> {};

int main() { return 0; }
