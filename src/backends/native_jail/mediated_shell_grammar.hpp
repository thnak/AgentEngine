#pragma once
// Implements 010-Python-Code-Interpreter.md §1a/§2/§9 G2/G3/G4/G6 -- Milestone 3 Phase E3
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, decision 4). A genuinely
// new AST/grammar, carrying forward decisions/ADR-001-shellrunner-grammar-and-dispatch.md's Judged
// Design A finding (the exact EBNF below, §3) as a DESIGN, not as copied code -- `shell_grammar.hpp`
// and its siblings stay in place, completely untouched, as that ADR's cited prove-phase evidence.
//
// Grammar (ADR-001 §3, reproduced here as the design this file's types implement, not as a literal
// quotation of the ADR's own C++):
//   command_line   := and_or (';' and_or)*
//   and_or         := pipeline (('&&' | '||') pipeline)*
//   pipeline       := simple_command ('|' simple_command)*
//   simple_command := assignment* word+ redirect*
//   assignment     := NAME '=' word
//   redirect       := ('<' | '>' | '>>') word
//   word           := (literal | '$' NAME | '${' NAME '}' | quoted)+   -- expansion never
//                     re-tokenizes: a spliced value is inserted as one opaque unit into the exact
//                     slot it came from, never re-scanned for operators/whitespace/new grammar text
//                     (ADR-001 finding 10/11's closed resolution -- ambient-authority-shaped bug
//                     class, "$(cat evil.txt)" as a variable value must never re-parse as a new
//                     command). `quoted` atoms never expand at all.
//   if_stmt        := 'if' pipeline 'then' command_line ('else' command_line)? 'fi'
//   for_stmt       := 'for' NAME 'in' word+ 'do' command_line 'done'
//   statement      := and_or | if_stmt | for_stmt
//   script         := statement*
// Deliberately excluded from v1, matching ADR-001 §3's own scope: subshells, functions, here-docs,
// command substitution `$(...)` (a natural re-entry point for the parser on attacker-controlled
// text -- 010 §10 Q7's own resolution names continuous fuzzing, not a bigger grammar, as this
// project's answer to parser-safety risk). Background processes (`&`) route through 006 §6b's
// `Backgroundable` capability mechanism instead of the grammar (010 §10 Q6, Resolved).

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace agentengine::native_jail::mediated_shell {

// Safety knobs (ADR-001 §2.5.6/finding 12/13's closed findings, carried forward as measured
// design constants -- not copied code, this project's own bounded-arena discipline applied fresh):
// a script is rejected outright, before any AST node is built, once it would exceed these.
inline constexpr std::size_t kMaxSourceBytes = 1u * 1024 * 1024;       // 1 MiB
inline constexpr std::size_t kMaxTokens = 50'000;
inline constexpr std::size_t kMaxNestingDepth = 32;                    // shared across if/for, sum not max
inline constexpr std::size_t kBytesPerNodeUpperBound = 256;
inline constexpr std::size_t kArenaBytes = kMaxTokens * kBytesPerNodeUpperBound;

enum class word_atom_kind { literal, var_ref, quoted };

struct WordAtom {
    word_atom_kind kind;
    std::pmr::string text;  // literal text, or the variable NAME (var_ref), or the quoted content
};

struct Word {
    std::pmr::vector<WordAtom> atoms;
};

struct Assignment {
    std::pmr::string name;
    Word value;
};

enum class redirect_kind { input, output, append };

struct Redirect {
    redirect_kind kind;
    Word target;
};

struct SimpleCommandNode {
    std::pmr::vector<Assignment> assigns;
    std::pmr::vector<Word> words;
    std::pmr::vector<Redirect> redirects;
};

struct PipelineNode {
    std::pmr::vector<SimpleCommandNode> commands;
};

struct AndOrNode {
    std::pmr::vector<PipelineNode> pipelines;
    std::pmr::vector<bool> is_and;  // one fewer entry than pipelines; is_and[i] joins pipelines[i]/[i+1]
};

// Forward-declared: `if`/`for` bodies hold pointers to arena-allocated StatementNode instances
// (never individually freed -- the whole arena is torn down at once when the owning ParsedScript
// destructs), matching the toolchain-forced shape ADR-001's own implementation needed (a
// `std::pmr::vector<StatementNode>` BY VALUE breaks under MSVC STL's `<vector>`, which instantiates
// `is_trivially_destructible<T>` requiring `T` complete earlier than libstdc++/libc++ do, for this
// specific mutually-recursive shape) -- carried forward as a design finding, reproduced fresh here
// since it is a real, toolchain-dependent fact this file's own build would hit identically, not
// something specific to the spike's code.
struct StatementNode;

struct IfNode {
    PipelineNode cond;
    std::pmr::vector<StatementNode*> then_body;
    std::pmr::vector<StatementNode*> else_body;  // empty means no else clause
};

struct ForNode {
    std::pmr::string var;
    std::pmr::vector<Word> items;
    std::pmr::vector<StatementNode*> body;
};

struct StatementNode {
    std::variant<AndOrNode, IfNode, ForNode> value;
};

struct ScriptNode {
    std::pmr::vector<StatementNode*> statements;
};

}  // namespace agentengine::native_jail::mediated_shell
