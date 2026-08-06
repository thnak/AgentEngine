#pragma once
// Milestone 3 Phase H1 (026-Agent-Facing-Runtime-Surface.md §7's prompt budget table -- "Enforced by
// a test that measures the assembled system prompt"). A real BPE tokenizer (the exact count any
// given vendor's API would bill) is a heavy dependency this project has no other reason to take on
// for a budget GATE, whose job is catching bloat regressions, not billing accuracy. This is a
// documented, deliberately CONSERVATIVE approximation -- ~4 characters per token (a widely used
// English-text/code heuristic, consistent across major tokenizers to within a small margin), rounded
// UP -- so the gate fails toward flagging bloat it might not actually have, never toward silently
// missing bloat a real tokenizer would catch. Never used for anything except this budget gate; never
// presented as an exact, model-billed count (a real ChatResponse's `Usage::input_tokens`/
// `output_tokens`, core/content.hpp, is the real number when one exists).

#include <cstdint>
#include <string_view>

namespace agentengine {

[[nodiscard]] inline std::uint64_t estimate_tokens(std::string_view text) {
    if (text.empty()) return 0;
    return (text.size() + 3) / 4;  // ceiling division, ~4 chars/token
}

}  // namespace agentengine
