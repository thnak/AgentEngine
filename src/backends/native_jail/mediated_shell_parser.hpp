#pragma once
// Implements 010-Python-Code-Interpreter.md §1a/§2 -- Milestone 3 Phase E3. A genuinely new parser
// over mediated_shell_grammar.hpp's AST, carrying forward decisions/ADR-001's Judged Design A
// finding (whole-script-parses-before-anything-executes, bounded arena, shared recursion-depth
// counter) as a design, not copied code -- shell_parser.{hpp,cpp} stay untouched.
//
// `parse` is a pure `bytes -> result<ScriptNode>` function (ADR-001's own load-bearing property,
// carried forward): it has no dependency on FileSystemAdapter, CommandRegistry, ExecState, or
// EffectContext -- the entire authorization/dispatch/filesystem surface is unreachable from inside
// the parser BY TYPE, not by discipline. Nothing executes until the whole script has parsed
// successfully.

#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "agentengine/core/error.hpp"
#include "backends/native_jail/mediated_shell_grammar.hpp"

namespace agentengine::native_jail::mediated_shell {

// Owns the arena a successfully-parsed AST's `pmr::` members point into. The arena must outlive
// the `ScriptNode` (a plain `monotonic_buffer_resource` local to `parse()` would dangle the moment
// the function returned), so this struct bundles both.
struct ParsedScript {
    std::unique_ptr<std::byte[]> arena_storage;
    std::unique_ptr<std::pmr::monotonic_buffer_resource> resource;
    std::optional<ScriptNode> script;
};

[[nodiscard]] result<ParsedScript> parse(std::string_view source);

}  // namespace agentengine::native_jail::mediated_shell
