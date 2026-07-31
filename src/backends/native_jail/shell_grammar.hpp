#pragma once
// Implements ADR-001-shellrunner-grammar-and-dispatch.md §3 (Design A's grammar and AST), as
// revised by §2.5.6 (finding 14: every container is std::pmr::vector, not plain std::vector, so
// A-F1's allocation-count claim is actually testable against the shown types).
//
// One deviation from §3's literal spelling, necessary to make the shown types compile at all
// (parallel in spirit to §2.5.6's own fix for findings 14/18 — a shown-types-don't-compile issue,
// not a behavior or security change): §3 writes `using StatementNode = std::variant<AndOrNode,
// IfNode, ForNode>;` with IfNode/ForNode each holding `std::pmr::vector<StatementNode>`. A type
// alias cannot be forward-declared, and std::variant requires every alternative to be a complete
// type at the point the variant is instantiated — so IfNode and ForNode would need to be complete
// before StatementNode's `using` can be written, while StatementNode itself would need to already
// exist for IfNode/ForNode's bodies to name it. This file breaks the cycle the standard way:
// StatementNode is a one-member wrapper struct (forward-declarable) around the same
// std::variant<AndOrNode, IfNode, ForNode> the ADR names, so IfNode/ForNode can hold
// `std::pmr::vector<StatementNode>` of a merely-forward-declared type (std::vector has supported
// incomplete element types since C++17) and StatementNode's own definition is written last, once
// IfNode/ForNode are complete. Semantically this is exactly the ADR's variant — `.value` is the
// only added spelling.

#include <cstddef>
#include <memory_resource>
#include <string>
#include <variant>
#include <vector>

namespace agentengine::shell {

// ---- Safety knobs (§3 "Safety knobs, each load-bearing") ----------------------------------
// Concrete numbers chosen for this implementation; the ADR gives these as illustrative ("e.g. 32",
// "e.g. 1 MiB") rather than load-bearing exact figures, so the actual constants are recorded here
// and in ADR-001 §8 (Executed evidence) rather than re-litigated per callsite.
inline constexpr std::size_t kMaxSourceBytes     = 1u * 1024u * 1024u;      // 1 MiB (§3)
// Chosen so that A-S1's own named "100k-stage pipeline" adversarial probe (~200k word+pipe
// tokens) is cleanly REJECTED by the token cap well before it could stress anything else — the
// claim's wording ("rejected in <100 ms") is satisfied by a fast, clean `resource`-classed parse
// error, not by successfully building a 100k-node tree.
inline constexpr std::size_t kMaxTokens          = 50'000;                  // §3 / finding 13
inline constexpr std::size_t kMaxNestingDepth    = 32;                      // §3 / finding 12
// Measured (prove phase, ADR-001 §8) worst-case per-node arena overhead: sizeof/alignment/control
// block cost of the smallest AST node shape (a one-atom Word inside a SimpleCommandNode), with
// margin for std::pmr::vector's internal bookkeeping. See real numbers recorded in the ADR.
inline constexpr std::size_t kBytesPerNodeUpperBound = 256;
inline constexpr std::size_t kArenaBytes         = kMaxTokens * kBytesPerNodeUpperBound; // finding 13

// ---- AST (§3, §2.5.6) ----------------------------------------------------------------------

enum class word_atom_kind { literal, var_ref, quoted };

// One atom of a `word` production. `var_ref`'s `text` is the referenced NAME (not yet expanded —
// expansion happens in the evaluator, §2.5.2: an opaque splice at evaluation time, never
// re-tokenized). `quoted`'s `text` is the literal contents of a quoted span verbatim; per finding
// 11, no `$NAME` substitution is ever attempted inside a `quoted` atom — the parser does not even
// look for `$` while scanning one.
struct WordAtom {
    word_atom_kind    kind;
    std::pmr::string  text;
};

struct Word {
    explicit Word(std::pmr::polymorphic_allocator<> alloc) : atoms(alloc) {}
    std::pmr::vector<WordAtom> atoms;
};

struct Assignment {
    std::pmr::string name;
    Word              value;
};

enum class redirect_kind { input, output, append };

struct Redirect {
    redirect_kind kind;
    Word           target;
};

struct SimpleCommandNode {
    explicit SimpleCommandNode(std::pmr::polymorphic_allocator<> alloc)
        : assigns(alloc), words(alloc), redirects(alloc) {}
    std::pmr::vector<Assignment> assigns;
    std::pmr::vector<Word>       words;
    std::pmr::vector<Redirect>   redirects;
};

struct PipelineNode {
    explicit PipelineNode(std::pmr::polymorphic_allocator<> alloc) : commands(alloc) {}
    std::pmr::vector<SimpleCommandNode> commands;
};

struct AndOrNode {
    explicit AndOrNode(std::pmr::polymorphic_allocator<> alloc) : pipelines(alloc), is_and(alloc) {}
    std::pmr::vector<PipelineNode> pipelines;
    std::pmr::vector<bool>         is_and; // is_and[i] joins pipelines[i] and pipelines[i+1]
};

struct StatementNode; // forward declaration — see the file-header note on why this can't be a
                       // bare `using` alias to the variant the way §3 writes it.

// A SECOND, toolchain-forced deviation beyond the file-header note: IfNode/ForNode hold
// `StatementNode*` (arena-allocated via `polymorphic_allocator<>::new_object`, never individually
// `delete_object`-ed), not `std::pmr::vector<StatementNode>` by value as the header note's first
// fix originally attempted. MSVC STL's <vector> (used here even under clang, since this target
// builds in MSVC-ABI mode against MSVC's own standard library headers) computes
// `is_trivially_destructible<T>` — which requires T complete — directly inside
// `vector<T,PolymorphicAllocator<T>>`'s destructor body, and that destructor is implicitly
// instantiated as soon as `IfNode`'s own (also implicit) destructor is needed for constructor
// exception-safety codegen, i.e. right here, before `StatementNode` is complete — unlike
// libstdc++/libc++, which defer this far enough that the incomplete-type support C++17 promises
// for `std::vector` actually holds in practice. `StatementNode*` sidesteps this: a pointer is
// always a complete type regardless of what it points to. This does not change the arena-lifetime
// story A-F1 depends on — `new_object`'s allocation comes from the same bounded arena as
// everything else, and never explicitly destroying a `StatementNode` is fine precisely because
// nothing in this AST owns a non-arena resource; the whole tree is reclaimed at once when
// `ParsedScript`'s `monotonic_buffer_resource` (shell_parser.hpp) is destroyed, matching the
// "bulk-reclaim an arena, never destroy individual nodes" idiom the ADR's own arena bullet (§3)
// already calls for, just made structurally necessary here rather than optional.
struct IfNode {
    explicit IfNode(std::pmr::polymorphic_allocator<> alloc)
        : cond(alloc), then_body(alloc), else_body(alloc) {}
    PipelineNode                      cond;
    std::pmr::vector<StatementNode*>  then_body;
    std::pmr::vector<StatementNode*>  else_body;
};

struct ForNode {
    explicit ForNode(std::pmr::polymorphic_allocator<> alloc) : var(alloc), items(alloc), body(alloc) {}
    std::pmr::string                  var;
    std::pmr::vector<Word>            items;
    std::pmr::vector<StatementNode*>  body;
};

struct StatementNode {
    explicit StatementNode(std::variant<AndOrNode, IfNode, ForNode> v) : value(std::move(v)) {}
    std::variant<AndOrNode, IfNode, ForNode> value;
};

struct ScriptNode {
    explicit ScriptNode(std::pmr::polymorphic_allocator<> alloc) : statements(alloc) {}
    std::pmr::vector<StatementNode*> statements;
};

} // namespace agentengine::shell
