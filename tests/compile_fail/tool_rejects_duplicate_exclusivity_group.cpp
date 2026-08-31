// This file MUST NOT compile (decisions/ADR-158-tool-concurrency-exclusivity-policy.md §5) — see
// tests/CMakeLists.txt's try_compile() gate. Declaring more than one `ExclusivityGroup<Name>` on
// the same tool must fail to compile, not silently keep only the LAST-declared group the way the
// ordinary policy-tag fold (e.g. `declared_approval()`) does for other tags — silently narrowing a
// concurrency-safety guarantee is a materially worse failure mode than narrowing an approval mode.

#include "agentengine/core/tool.hpp"

using namespace agentengine;

// must not compile: BadTool declares two exclusivity groups; which one would even apply is
// ambiguous, and this project's convention is to reject outright rather than guess.
struct BadTool : Tool<BadTool, ExclusivityGroup<"db-write">, ExclusivityGroup<"cache-flush">> {};

int main() { return 0; }
