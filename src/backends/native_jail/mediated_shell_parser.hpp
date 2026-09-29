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

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"
#include "backends/native_jail/mediated_shell_grammar.hpp"

namespace agentengine::native_jail::mediated_shell {

// The parser's bounded arena (ADR-001 §2.5.6: one fixed block, everything freed at once). GitHub issue #147.
//
// It used to be a `monotonic_buffer_resource` over `null_memory_resource()`, so an allocation past the block
// THREW. Harmless when the allocating call may throw (the parser catches `bad_alloc`), and fatal when it may
// not: under MSVC's iterator debugging (`_ITERATOR_DEBUG_LEVEL` 1 or 2, every Debug build) every container
// move constructor allocates a `_Container_proxy` through its allocator, and those constructors are
// `noexcept`. Measured (`std::vector<PipelineNode, pmr>::vector` <- `polymorphic_allocator<_Container_proxy>::
// allocate` <- `monotonic_buffer_resource::_Increase_capacity` <- `bad_alloc`): a valid 13,000-statement
// script called `std::terminate`, so a model could abort a Debug-built host, and every test run is one.
//
// So exhaustion is NOT an exception here. Past the block, an allocation is served from the heap in an exact-
// size chunk, `exhausted()` latches, and the lexer and parser poll it at every loop step and fail the parse
// with `shell.arena_exhausted`. Nothing aborts in any build type, and the overshoot is bounded: at most
// `kArenaSpillBytes` of heap, after which `do_allocate` throws (fail closed, and unreachable from a script
// within `kMaxSourceBytes`/`kMaxTokens`, since a step between two polls allocates far less than that).
// A chunk is never freed individually, matching the monotonic contract; all are freed with the arena.
class ParseArena final : public std::pmr::memory_resource {
public:
    ParseArena(std::byte* block, std::size_t size, std::size_t spill_cap) noexcept
        : block_(block), size_(size), spill_cap_(spill_cap) {}
    ParseArena(ParseArena const&) = delete;
    ParseArena& operator=(ParseArena const&) = delete;
    ~ParseArena() override {
        for (Spill const& s : spill_) ::operator delete(s.ptr, std::align_val_t{s.align});
    }

    // True once any allocation did not fit in the block. Latches; cheap enough to poll per token.
    [[nodiscard]] bool exhausted() const noexcept { return exhausted_; }
    [[nodiscard]] std::size_t spilled_bytes() const noexcept { return spilled_; }

private:
    struct Spill {
        void* ptr;
        std::size_t align;
    };

    void* do_allocate(std::size_t bytes, std::size_t align) override {
        void* cursor = block_ + used_;
        std::size_t space = size_ - used_;
        if (void* p = std::align(align, bytes, cursor, space)) {
            used_ = static_cast<std::size_t>(static_cast<std::byte*>(p) - block_) + bytes;
            return p;
        }
        exhausted_ = true;
        if (bytes > spill_cap_ - spilled_) throw std::bad_alloc();
        // Room for the record BEFORE allocating, so a throw here leaks nothing. Geometric, never
        // `reserve(size() + 1)`: that reallocates the whole list on every spill, and the first version of
        // this did -- 24,000 spills requested 4.6 GB of heap for a 96 KB script.
        if (spill_.size() == spill_.capacity()) spill_.reserve(spill_.capacity() == 0 ? 16 : spill_.capacity() * 2);
        void* p = ::operator new(bytes, std::align_val_t{align});
        spill_.push_back(Spill{p, align});  // capacity reserved above
        spilled_ += bytes;
        return p;
    }
    void do_deallocate(void*, std::size_t, std::size_t) override {}  // monotonic: freed with the arena
    bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override { return this == &other; }

    std::byte* block_;
    std::size_t size_;
    std::size_t used_ = 0;
    std::size_t spill_cap_;
    std::size_t spilled_ = 0;
    bool exhausted_ = false;
    std::vector<Spill> spill_;
};

// Owns the arena a successfully-parsed AST's `pmr::` members point into. The arena must outlive
// the `ScriptNode` (a plain `monotonic_buffer_resource` local to `parse()` would dangle the moment
// the function returned), so this struct bundles both.
struct ParsedScript {
    std::unique_ptr<std::byte[]> arena_storage;
    std::unique_ptr<ParseArena> resource;
    std::optional<ScriptNode> script;
};

[[nodiscard]] result<ParsedScript> parse(std::string_view source);

}  // namespace agentengine::native_jail::mediated_shell
