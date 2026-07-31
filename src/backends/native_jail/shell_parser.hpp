#pragma once
// Implements ADR-001-shellrunner-grammar-and-dispatch.md §3 — the recursive-descent parser.
// `parse()` is a pure `bytes -> result<ParsedScript>` function with NO dependency on
// FileSystemAdapter, CommandRegistry, ExecState, or EffectContext: the entire
// authorization/dispatch/filesystem surface is unreachable from inside the parser BY TYPE (the
// function's own signature has no parameter through which such a call could be reached), not by
// discipline. This is the property Sh-S1 and A-S1 lean on and the one that makes this parser
// fuzzable with a five-line harness (§3 steelman #1).
//
// `ParsedScript` bundles the produced `ScriptNode` together with the bounded arena backing its
// `std::pmr` containers. This is a necessary concretization of §3's literal `result<ScriptNode>
// parse(...)` signature: `ScriptNode`'s containers hold a `std::pmr::polymorphic_allocator<>`
// that only stores a `memory_resource*` pointer, so a `std::pmr::monotonic_buffer_resource` local
// to `parse()`'s stack frame would be destroyed on return while the returned tree still pointed
// at it — a dangling-allocator bug the ADR's shown signature doesn't surface because it never
// discusses arena ownership across the function boundary. `ParsedScript` closes that gap by
// heap-allocating the arena's backing storage and the resource object itself (each via
// unique_ptr, since std::pmr::monotonic_buffer_resource is neither copyable nor movable) so the
// owning struct as a whole IS movable — the tree's allocator pointer stays valid across a move
// because only the unique_ptrs move, not the pointee addresses.

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "agentengine/core/error.hpp"
#include "backends/native_jail/shell_grammar.hpp"

namespace agentengine::shell {

struct ParsedScript {
    ParsedScript()                                = default;
    ParsedScript(ParsedScript&&) noexcept          = default;
    ParsedScript& operator=(ParsedScript&&) noexcept = default;
    ParsedScript(ParsedScript const&)              = delete;
    ParsedScript& operator=(ParsedScript const&)   = delete;

    std::unique_ptr<std::byte[]>                          arena_storage;
    std::unique_ptr<std::pmr::monotonic_buffer_resource>  resource;
    std::optional<ScriptNode>                             script;
};

// Rejects (before any tokenizing begins) any source over kMaxSourceBytes. Otherwise: tokenizes
// and parses in one recursive-descent pass, bounded by a single shared depth counter (incremented
// on entry / decremented on exit of every recursive grammar production — currently only
// `parse_if`/`parse_for`, since word/atom scanning is implemented iteratively in this codebase,
// eliminating that recursion vector entirely rather than merely bounding it — see
// shell_parser.cpp's top comment) and a total-token counter (kMaxTokens), with all AST nodes
// allocated from a bounded `std::pmr::monotonic_buffer_resource` backed by a
// `std::pmr::null_memory_resource()` upstream, so exhausting the arena raises `std::bad_alloc`
// which `parse()` catches at this single boundary and reports as a `resource`-classed error
// (`shell.arena_exhausted`) — never an OOM of the host process, per §3's own requirement.
[[nodiscard]] result<ParsedScript> parse(std::string_view source);

} // namespace agentengine::shell
