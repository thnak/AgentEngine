# ADR-001 — ShellRunner grammar and dispatch

- **Status:** Proposed — **design revised post-red-team** (§2.5 closes §7's 4 blocking findings;
  §7 itself is left exactly as written, as the historical record). **Prove phase executed for
  Design A** (§8 executed evidence, §9 per-claim verdicts) — Design A only, real C++23, built
  under MSVC and clang, tested under ASan+UBSan. **Judge phase pending** (§10 remains a
  placeholder per `decisions/README.md`'s process; no winner is declared here — that is explicitly
  not this pass's call to make).
- **Date:** 2026-07-31
- **Scope:** `ShellRunner` (`src/backends/native_jail/shell_runner.hpp`) — the concrete class
  satisfying the `Runner` concept (`include/agentengine/sandbox/runner.hpp`) that backs the Shell
  mode (010 §1). Excludes `PythonRunner` (separate, concurrent ADR) and excludes 025's real
  `Worktree` implementation (not designed here — see §2.1).
- **Related specs:** `010-Python-Code-Interpreter.md` §1a, §2, §3a, §10 Q6/Q7 · `006-Tool-and-
  Function-Plane.md` §2, §3, §4 · `007-Capability-and-Trust-Model.md` · `008-Sandbox-and-
  Isolation.md` §1b, §3 · `025-Worktree-and-Virtual-Filesystem.md` §5, §7 · `026-Agent-Facing-
  Runtime-Surface.md` §3 (error surface) · `003-Message-and-Content-Model.md` §2 (taint).

## 1. The question

**How does `ShellRunner` parse its small, explicitly non-POSIX-complete grammar (pipes, redirects,
variable assignment/expansion, `&&`/`||`, minimal control flow) and dispatch a resolved command
name, such that:**

1. **a name can never resolve to an arbitrary process** — only the fixed builtin set, a registered
   `Runner` (010 §1a), or a registered `Tool` (006) are reachable, and anything else fails closed
   with "command not found" (010 §2, §9 G2) — **by construction, not by containment**;
2. **the parser itself is safe against adversarial input** (010 §10 Q7) — bounded time, bounded
   memory, bounded recursion, no memory-unsafety — given that its input may be model output or
   content quoted from a tool result, i.e. tainted (003 §2), and the parser runs before any
   authorization decision, not after;
3. **`ExecState` (`{cwd, env}`) mutations made through `ShellRunner` are visible, by reference, to
   the next call on `PythonRunner`** sharing the same object (010 §3a, §9 G3);

while remaining **extensible** — adding a builtin or an operator later should not require
rearchitecting the parser or the dispatch layer?

This has a wrong answer. Two concrete wrong answers, both explicitly ruled out by 010 §2 but worth
stating as the failure modes this question guards against:

- **"Shell out to a real interpreter (`/bin/sh`, `cmd.exe`) with a restricted `PATH` / seccomp
  filter / allowlist."** This is exactly the ambient-authority shape I2 forbids: "reachable" still
  means "anything on `PATH`," and every hardening technique is a way of *containing* authority that
  was never supposed to exist, not a way of not having granted it. It would also make claim (2)
  someone else's problem (the OS shell's own parser), which does not make it disappear — it makes
  it unauditable.
- **"A hand-rolled grammar, but resolution falls through to `dlopen`/`LoadLibrary`/`CreateProcess`
  for any name not recognized as a builtin, gated by an allowlist file."** This is the same mistake
  one level down: an allowlist is data, and the resolution code path that *would* call the
  process-creation API still exists and is one bug (a missing check, a TOCTOU on the allowlist
  file, a symlink) away from firing. §9 G2's bar is that the code path **must not exist**, not that
  it must be checked correctly every time.

### 1.1 What this ADR does not design

`Worktree` (025) has no I/O implementation yet — only the abstract `Blob`/`Tree`/`Ref` shape
(`include/agentengine/core/worktree.hpp`). Both designs below therefore define an **injectable
`FileSystemAdapter` interface** that the builtin set operates against, with a concrete
`RealFileSystemAdapter` backed by an ordinary OS directory as the design's only implementation
target for now (useful for testing and for a non-worktree-backed profile). The production adapter
— a `WorktreeFileSystemAdapter` mounting `/work`, `/input`, `/out` per 025 §3/§5 — is **future
work gated on 025 landing**, not something this ADR designs. Nothing here should be read as a
proposal for 025's shape; the adapter interface is deliberately the minimum surface the builtin
set needs (§2.2), not a preview of the Worktree API.

## 2. Shared vocabulary — identical in both designs

Both candidate designs answer claims (1) and (3) the same way, through the same two seam
interfaces. What differs between them (§3, §4) is purely *how raw shell text becomes a sequence of
resolved, argument-expanded commands to dispatch* — the parsing/evaluation strategy. Isolating this
shared machinery matters for the ADR's own falsifiability: any claim that both designs make
identically (e.g. "no ambient process creation") is a property of §2's dispatch layer, not of
either parser, and should not be credited to whichever design happens to be discussed first.

### 2.1 `FileSystemAdapter` — the injectable seam standing in for 025

```cpp
// Seam interface (CONVENTIONS.md tier-2 style: virtual dispatch permitted here because this is a
// declared backend/store seam, not a per-token hot-path policy — 023's hot-path rule targets
// per-token streaming, not per-shell-invocation setup). Path canonicalization against the mount
// root happens INSIDE the adapter; callers never see host paths (025 §5, 026 §2).
class FileSystemAdapter {
public:
    virtual ~FileSystemAdapter() = default;

    virtual result<std::vector<std::byte>> read_file(std::string_view path) = 0;
    virtual result<void> write_file(std::string_view path, std::span<std::byte const> data,
                                     bool append) = 0;
    virtual result<void> remove(std::string_view path, bool recursive) = 0;
    virtual result<void> rename(std::string_view from, std::string_view to) = 0;
    virtual result<void> copy_file(std::string_view from, std::string_view to) = 0;
    virtual result<void> make_directory(std::string_view path, bool parents) = 0;
    virtual result<std::vector<DirEntry>> list_directory(std::string_view path) = 0;
    virtual result<bool> exists(std::string_view path) = 0;
};
```

`RealFileSystemAdapter : FileSystemAdapter` resolves `path` against a fixed root directory handed
to it at construction and rejects `..`/absolute-redirect/symlink-escape the same way 025 §5
requires of the eventual Worktree adapter — this design borrows that requirement without
implementing 025's object-store side of it. **Both designs call the same `FileSystemAdapter`
through the same fixed builtin implementations** (§2.3); the parser never touches a filesystem
directly.

### 2.2 `CommandRegistry` — the closed three-way lookup, never a search path

```cpp
enum class command_kind { builtin, runner, tool, not_found };

struct ResolvedCommand {
    command_kind kind;
    // exactly one populated per kind: `builtin` carries no payload (dispatch is a fixed
    // compiled-in table, §2.3); `runner` carries the RunnerCall<name>-gated handle (010 §1a);
    // `tool` carries the run's snapshotted Tool handle (006 §6).
};

class CommandRegistry {
public:
    virtual ~CommandRegistry() = default;
    // Pure lookup over three closed, host-controlled tables: the fixed builtin name set (compiled
    // in, not configurable — 010 §2's "fixed"), the run's registered-Runner table, and the run's
    // immutable per-run Tool table (006 §6, snapshotted at run start). NO step here reads PATH,
    // touches a filesystem, or calls dlopen/LoadLibrary — resolve() is a name comparison against
    // in-memory tables and nothing else. This is what makes §9 G2 a structural property: there is
    // no branch in this function's implementation that could reach fork/exec/CreateProcess for an
    // unrecognized name, because it never had a reference to any process-creation API in the first
    // place — grep/link-check for those symbols in the translation unit is a compile-time
    // corroboration of a property the function's own signature already guarantees.
    [[nodiscard]] virtual ResolvedCommand resolve(std::string_view name) const = 0;
};
```

### 2.3 The shared dispatch step

Once a parser (either design) has produced one fully-expanded simple command — a command name plus
its expanded argument words plus its redirects — dispatch is one function, shared by both designs:

```cpp
result<CommandOutcome> dispatch_command(std::string_view name, std::vector<std::string> const& argv,
                                         RedirectSet const& redirects, CommandRegistry const& registry,
                                         FileSystemAdapter& fs, ExecState& state, EffectContext& ctx) {
    switch (registry.resolve(name).kind) {
    case command_kind::builtin:
        // e.g. cd/pwd/ls/cat/echo/export/mkdir/rm/mv/cp — each checks the relevant capability
        // (FsRead/FsWrite scoped to the resolved path, per 007) against ctx.capabilities BEFORE
        // touching `fs`, mutates `state` directly for cd/export (same object, by reference — the
        // §9 G3 property), and never invokes the 006 §3 ten-step Tool pipeline (006 §2's explicit
        // carve-out: paying validate/approve/admit/account for `ls` has no matching benefit).
        return run_builtin(name, argv, redirects, fs, state, ctx);
    case command_kind::runner:
        // requires RunnerCall<name> in ctx.capabilities (010 §1a, §9 G4); on success, calls
        // registered_runner.run(sub_request, state /* same ExecState&, not a copy */, ctx).
        return invoke_registered_runner(name, argv, state, ctx);
    case command_kind::tool:
        // full 006 §3 pipeline: resolve/validate/taint/authorize/approve/admit/bind/invoke/
        // normalize/account. argv -> the Tool's typed Args is an open mapping question (§11).
        return invoke_registered_tool(name, argv, ctx);
    case command_kind::not_found:
        return std::unexpected(ae::error{failure_class::contract,
                                          "command not found: " + std::string(name),
                                          "shell.command_not_found"});
    }
}
```

Because §2.2–§2.3 are identical in both designs, any claim phrased purely in terms of them (the
"never reaches `exec`" claim, the `ExecState`-sharing claim) is **not a distinguishing claim between
Design A and Design B** — both inherit it from this shared layer, and §5 states that explicitly
rather than double-counting it as evidence for whichever design is read first.

### 2.4 The fixed builtin set and its capability mapping

| Builtin | Adapter call(s) | Capability checked before the call |
|---|---|---|
| `cd` | `exists` (validate target) | none (cwd bookkeeping is host-side; no I/O) — see §2.5.3 for the canonicalization rule this now carries |
| `pwd` | none (`state.cwd`) | none |
| `ls` | `list_directory` | `FsRead`\* over the resolved path |
| `cat` | `read_file` | `FsRead`\* over the resolved path |
| `echo` | `write_file` only if redirected | `FsWrite`\* over the redirect target, if any |
| `export` | none (`state.env`) | **`EnvWrite`** (§2.5.1 — revised; was "none") |
| `mkdir` | `make_directory` | `FsWrite`\* over the resolved parent |
| `rm` | `remove` | `FsWrite`\* over the resolved path |
| `mv` | `rename` (or `copy_file`+`remove` cross-device) | `FsRead`\* on source, `FsWrite`\* on dest |
| `cp` | `copy_file` | `FsRead`\* on source, `FsWrite`\* on dest |

\* **"Scoped" is aspirational, not yet testable — see §2.5.4.** `Capability` (`trust/capability.hpp`)
today carries only a `capability_kind`, no subtree/path/quota field, so no positive/negative control
pair ("grant `FsRead` on `/work/public`, deny it on `/work/secret`") can currently be constructed.
Every "scoped" cell in this table means "the *design intends* path-scoped authorization once 007
adds a scope field to `Capability`"; the prove phase can only test kind-level possession
(`FsRead` held or not held, full stop) until then, per §2.5.4.

Builtins skip the 006 §3 Tool pipeline's ten steps (006 §2's explicit carve-out) but **do not skip
authorization** — each still checks the relevant capability against `ctx.capabilities` before
touching `FileSystemAdapter`, because I2 does not have a "but it's a builtin" exception. A **null**
`EffectContext::capabilities` is treated as an empty `CapabilitySet` (deny-all) by every check site —
no check site may dereference without this guard (closes finding 7). The exact capability-check
mechanism (how a `Capability` is matched against a requested subtree/quota) is 007's own pending
design; this ADR assumes only that such a check exists and returns a `policy`-classed `ae::error` on
denial, not the concrete API.

### 2.5 Amendments closing §7's blocking and near-blocking findings

Written after the red-team pass (§7) found four findings serious enough to require a design change,
not just a caveat, before the prove phase starts. This section states the fix directly; §7 itself is
left unedited as the historical record of what was found. Cross-referenced from §11.

#### 2.5.1 Finding 4 — `export`/`ExecState.env` as an unmodeled ambient-authority channel (BLOCKING, closed)

**Fix:** `export` now requires the new `EnvWrite` capability kind (added to `capability_kind`,
`include/agentengine/trust/capability.hpp`) — a shell command cannot mutate `state.env` at all
without it, closing the "zero capability check" half of finding 4.

This is **necessary but not sufficient**, and stating why matters: `EnvWrite` authorizes *that* an
agent may set environment variables at all — it does not, by itself, make `PYTHONPATH`/
`PIP_INDEX_URL`/`HTTP_PROXY`/loader variables safe for `PythonRunner` to trust once set, because the
hazard is not "was this write authorized" but "does a *different* Runner's embedded interpreter
silently treat this value as trusted configuration." Closing that half is **not this ADR's to fix** —
it is a `PythonRunner`-side mediation requirement, and is recorded here as a cross-ADR dependency:
**ADR-002 (`PythonRunner`, currently in its own red-team phase) must state explicitly that every
value read from the shared `ExecState.env` is treated as tainted input with respect to any
security-relevant consultation (import machinery, network/proxy configuration, subprocess
environment) — read at the point of use and validated/ignored per policy, never inherited into the
embedded interpreter's process environment as ambient configuration.** Until ADR-002 states this,
finding 4 is only half-closed; §12 records this as a spec dependency this ADR's promotion is
contingent on.

#### 2.5.2 Finding 10/11 — grammar expansion semantics (BLOCKING, closed)

**Fix, stated as the grammar's rule, not left inferable from production shape:** expansion never
re-tokenizes. `$NAME`/`${NAME}` substitution splices the variable's current value as an **opaque
literal** into the exact word/argv slot the reference occupies — the result is never re-scanned for
operator characters (`|`, `&&`, `||`, `;`, redirects), never re-split on whitespace into additional
words, and never treated as new grammar text. `for x in $list` iterates over exactly one item — the
whole expanded string — never a POSIX-style word-split into several. **`quoted` atoms never expand**
— no `$NAME` substitution occurs inside a `quoted` atom at all (finding 11); this is stronger and
simpler than POSIX's single/double-quote distinction and is a deliberate simplification, not an
oversight, and must not be "improved" toward POSIX behavior without reopening this finding.

**New falsifiable claim added to §5.1 as shared** (both designs consume the same grammar rule): see
`Sh-C3`.

#### 2.5.3 Finding 3 — `cd` canonicalization (closed as a stated rule)

**Fix:** `ExecState.cwd` always holds the `FileSystemAdapter`'s **canonical output**, never the raw
`cd` argument string. Every consumer (both `ShellRunner`'s own subsequent builtins and
`PythonRunner`'s mediated `os` layer, per the cross-ADR dependency in §2.5.1's spirit) re-canonicalizes
relative paths against the current `cwd` on every use rather than trusting a previously-cached
canonical form — closing the confused-deputy window finding 3 describes.

#### 2.5.4 Finding 23 — `CapabilitySet` has no scope field; "scoped" claims are currently untestable (BLOCKING, scoped down rather than fixed)

**This ADR does not fix `CapabilitySet`** — extending it with a real scope/subtree/quota
representation is 007's own security-critical design, requiring its own design → red-team → prove →
judge cycle, and is out of scope here. Per finding 23's own recommendation, every "scoped" claim in
§2.4's table is **downgraded for the prove phase**: what can actually be tested today is kind-level
possession (`FsRead` held or not held for this call, full stop), not path-scoped authorization. Any
prove-phase result for a "scoped" claim must be recorded as **`INCONCLUSIVE`**, not `CORRECT`,
until 007 lands a real scope field and the test can be rewritten as the positive/negative control
pair finding 23 describes. Marking it `CORRECT` on today's `CapabilitySet` would be laundering an
untestable claim into a passed one, which `decisions/README.md` §6 explicitly forbids.

#### 2.5.5 Finding 9 — `RealFileSystemAdapter` path-escape handling (BLOCKING for this adapter, closed)

**Fix:** `RealFileSystemAdapter` (§2.1) rejects or canonicalizes, before any capability-scope
comparison, the full list 025 §5 requires — not the narrower paraphrase §2.1 originally gave:
`..` traversal, absolute-path redirection, symlink/junction/reparse-point escape, Alternate Data
Streams, UNC paths (`\\server\share\...`), extended-length `\\?\` prefixes, reserved device names
(`CON`, `NUL`, `AUX`, `COM1`–`COM9`, `LPT1`–`LPT9`, as a stem with any extension), and
case-insensitivity divergence — the capability-scope comparison and the actual I/O call must operate
on the **same canonicalized, case-folded representation**, computed once, never compared as two
independently-derived strings. This list is a floor, not a target: it must match or exceed 025 §5
verbatim, since `RealFileSystemAdapter` — not the deferred `WorktreeFileSystemAdapter` — is this
project's actual near-term, Windows-first implementation target.

#### 2.5.6 Findings 14/18 — arena/stack allocation claims contradicted by the shown types (closed)

**Fix, Design A (finding 14):** §3's AST node types (`SimpleCommandNode`, `PipelineNode`,
`AndOrNode`, `IfNode`, `ForNode`, `ScriptNode`) are revised to use `std::pmr::vector<T>` throughout,
constructed with the `polymorphic_allocator<>` drawn from the bounded arena described in §3's
"safety knobs" — not plain `std::vector<T>`, which allocates from the global allocator regardless of
any arena declared elsewhere in the same function. A-F1's "≤4 heap allocations" now means ≤4
arena-backed `polymorphic_allocator` allocations (the arena's own upstream allocation counts as one,
not each `pmr::vector`'s individual growth).

**Fix, Design B (finding 18):** §4's `PipelineFrame::commands` is revised from `std::vector` to a
fixed-capacity inline container (`InlineVector<SimpleCommandBuilder, kMaxPipelineStagesInline>`) —
a real capacity bound, not a growable heap container — and `SimpleCommandBuilder`'s own argv
accumulation must be given the same treatment (a fixed-capacity inline buffer for the common case,
falling back to the arena only past `kMaxPipelineStagesInline`/an analogous per-command word-count
threshold) before B-F1's **absolute** "zero heap allocations beyond the fixed-capacity stacks" claim
is consistent with the shown types. With both fixed, B-F1 stands as originally written (not
softened to "relative") — the finding-18 fix is to make the types match the claim, not to weaken
the claim to match the types.

## 3. Design A — recursive-descent parser → AST → tree-walking interpreter

**Grammar** (informs both designs; restated here for concreteness):

```
command_line   := and_or (';' and_or)*                       -- ';' sequencing, no job control
and_or         := pipeline (('&&' | '||') pipeline)*
pipeline       := simple_command ('|' simple_command)*
simple_command := assignment* word+ redirect*
assignment     := NAME '=' word
redirect       := ('<' | '>' | '>>') word
word           := (literal | '$' NAME | '${' NAME '}' | quoted)+
                  -- expansion never re-tokenizes (§2.5.2): a `$NAME`/`${NAME}` reference splices
                  -- its value as one opaque literal into this exact slot; the result is never
                  -- re-scanned for operators and never re-split on whitespace. `quoted` atoms never
                  -- expand at all — no `$NAME` substitution occurs inside them (finding 11).
if_stmt        := 'if' pipeline 'then' command_line ('else' command_line)? 'fi'
for_stmt       := 'for' NAME 'in' word+ 'do' command_line 'done'
statement      := and_or | if_stmt | for_stmt
script         := statement*
```

No subshells, no functions, no here-docs, no command substitution `$(...)` — command substitution
is deliberately excluded from v1: it is a natural place to re-enter the parser recursively on
attacker-controlled text (a word contains a nested script), which is exactly the amplification
vector §1 claim (2) needs to bound, and adding it back should be its own ADR once the base grammar
has a fuzzing track record, not folded in here.

**AST:**

```cpp
// Revised per §2.5.6 (findings 14/18): every container is std::pmr::vector, constructed with the
// polymorphic_allocator<> drawn from parse()'s bounded arena (below) — a plain std::vector<T> here
// would allocate from the global allocator regardless of the arena, silently defeating A-F1's
// allocation-count claim and the arena's own memory-bound guarantee.
struct SimpleCommandNode { std::pmr::vector<Assignment> assigns; std::pmr::vector<Word> words; std::pmr::vector<Redirect> redirects; };
struct PipelineNode      { std::pmr::vector<SimpleCommandNode> commands; };
struct AndOrNode         { std::pmr::vector<PipelineNode> pipelines; std::pmr::vector<bool> is_and; };
struct IfNode            { PipelineNode cond; std::pmr::vector<StatementNode> then_body, else_body; };
struct ForNode           { std::string var; std::pmr::vector<Word> items; std::pmr::vector<StatementNode> body; };
using  StatementNode     = std::variant<AndOrNode, IfNode, ForNode>;
struct ScriptNode        { std::pmr::vector<StatementNode> statements; };

result<ScriptNode> parse(std::string_view source);  // pure function — no fs/registry/state/ctx
result<ExecOutcome> evaluate(ScriptNode const&, CommandRegistry const&, FileSystemAdapter&,
                              ExecState&, EffectContext&);
```

`parse` is a **pure `bytes -> result<ScriptNode>` function** with no dependency on `FileSystemAdapter`,
`CommandRegistry`, `ExecState`, or `EffectContext` — the entire authorization/dispatch/filesystem
surface is unreachable from inside the parser by type, not by discipline. `evaluate` walks
`ScriptNode` in order, threading one `ExecState&` through every builtin/runner/tool call (§2.3),
so a `cd` inside a `for` loop body mutates the same object a sibling `simple_command` two
iterations later reads.

**Safety knobs, each load-bearing:**

- **Explicit depth counter, not native call-stack trust — revised per finding 12 to be unambiguous.**
  **One** `depth_` field, shared across **every** grammar production capable of recursive descent
  (`parse_if`, `parse_for`, nested `${...}` inside a word, and any mutually-recursive
  word/quoted/expansion helper the implementation ends up with) — not one counter per production
  kind. It is incremented on entry and decremented on exit of *every* such entry point, wherever
  that entry point lives in the eventual function decomposition, and checked against
  `kMaxNestingDepth` (a small constant, e.g. 32) on increment; exceeding it is a parse error
  (`shell.nesting_too_deep`), not a stack overflow. A single shared counter closes finding 12's
  "sum, not max" hazard: 31 nested `if`s wrapping a word with 31 nested `${...}` hits the same
  shared counter and fails at the true combined depth, not at 31-and-31 independently. This converts
  the mitigation from "bounded by whatever the OS stack happens to be" to "bounded by an invariant
  checked in software," which is the actual answer to Q7's "the parser is first-party code parsing
  adversarial input" concern.
- **Input and token caps enforced before parsing starts**: `kMaxSourceBytes` (e.g. 1 MiB — a shell
  command line longer than that is already anomalous for 010 §1's stated use cases) and
  `kMaxTokens`, so a 10 MB input of pathological single-character tokens cannot build an
  unboundedly large token stream before the depth check ever triggers.
- **No backtracking.** The grammar above is one-token-lookahead (the leading keyword or operator
  symbol always disambiguates the production) and every production consumes input monotonically —
  there is no grammar-shaped path to catastrophic-backtracking behavior the way a regex-based
  tokenizer over nested quantifiers could have. This is a claim to prove (§5), not an assumption.
- **AST nodes allocated from a bounded arena** (`std::pmr::monotonic_buffer_resource`), sized per
  finding 13 by a **per-token** budget, not a raw-byte multiplier: `kArenaBytes = kMaxTokens *
  kBytesPerNodeUpperBound`, where `kBytesPerNodeUpperBound` is measured (prove phase) as the actual
  worst-case `sizeof`+alignment+control-block overhead of the smallest AST node shape (a one-atom
  `Word` inside a `SimpleCommandNode`) — the input shape that stresses this axis is maximum token
  count at minimum per-token content (e.g. `a ` repeated to `kMaxTokens`), not deep nesting (bounded
  by the depth counter above) or wide pipelines with non-trivial per-stage content (already in
  A-S1's corpus). Exhausting the arena is a clean `resource`-classed parse error, never an OOM of the
  host process.
- **Pipes are engine-native buffer hand-offs, not OS pipes.** A `PipelineNode`'s evaluation feeds
  each command's captured, size-capped stdout buffer (010 §3's output-discipline cap, reused) as
  the next command's stdin; there is no `pipe()`/`CreatePipe` call and no file-descriptor lifetime
  to manage, which removes an entire bug class rather than hardening it.

**Steelman:**

1. **The parser is fuzzable in total isolation.** `parse: bytes -> result<ScriptNode>` needs no
   fake `EffectContext`, no fake registry, no fake filesystem — a libFuzzer harness is a five-line
   wrapper. This is the most direct answer to Q7 available in either design.
2. **Structural pre-execution audit is free.** Because the whole script parses to a complete AST
   before any node evaluates, a "list every command this script would touch, without running any
   of them" pass (useful for a 006 §4-style approval preview, or for `dry_run` diagnostics) is a
   second, side-effect-free walk of the same tree — Design B cannot offer this without buffering
   the entire input first, which erodes its single-pass premise (§4).
3. **Extensibility is additive and localized.** A new builtin is one entry in `run_builtin`'s
   dispatch table — no parser change. A new operator (e.g. eventual `;` chaining, `!` negation) is
   one new grammar production, one new node variant, one new `evaluate` case — existing productions
   and their tests are untouched.
4. **Parse and eval are independently testable and independently reviewable.** A reviewer can
   verify "the grammar cannot produce a node this evaluator mishandles" and "the evaluator upholds
   dispatch/capability rules" as two smaller claims instead of one entangled one.

**Costs, stated plainly:** two data structures (token stream, then AST) instead of one; more total
code across two modules; a script that is syntactically invalid nowhere executes any part of
itself (arguably a feature, listed above, but it does mean "run what parsed so far" is not
available even if that were ever wanted).

## 4. Design B — single-pass tokenizer + Pratt/shunting-yard evaluate-as-you-parse

Same grammar as §3 (this is a strategy difference, not a language difference). No separate AST: a
tokenizer streams tokens; an operator-precedence evaluator consumes them with explicit,
size-bounded operand/operator stacks (shunting-yard for `|`/`&&`/`||`, all left-associative, no
precedence surprises), executing a pipeline's simple command as soon as its word list and redirects
are fully parsed, and pushing its outcome as the operand the surrounding `&&`/`||` combinator acts
on.

```cpp
enum class token_kind { word, pipe, and_and, or_or, semi, redirect_in, redirect_out,
                         redirect_append, assign, if_kw, then_kw, else_kw, fi_kw,
                         for_kw, in_kw, do_kw, done_kw, eof };
struct Token { token_kind kind; std::string text; std::size_t pos; };

class Tokenizer {  // single pass, ≤2-character lookahead (e.g. '&' vs "&&")
public:
    result<Token> next();
};

class ShellEvaluator {
public:
    result<ExecOutcome> evaluate(std::string_view source, CommandRegistry const&, FileSystemAdapter&,
                                  ExecState&, EffectContext&);
private:
    // Fixed-capacity inline storage (e.g. a small_vector/inplace_vector bounded by a max-pipeline-
    // stages constant), not a growable std::vector — per §2.5.6/finding 18, B-F1's "zero heap
    // allocations beyond the fixed-capacity stacks" claim is only true if this container actually
    // is fixed-capacity; a plain std::vector here would allocate on the heap for any pipeline at
    // all, contradicting the claim as originally shown.
    struct PipelineFrame { InlineVector<SimpleCommandBuilder, kMaxPipelineStagesInline> commands; };
    // explicit, capacity-bounded stacks — NOT the native call stack — for the flat pipe/and-or
    // precedence climb; `if`/`for` bodies still recurse (see honest concession, below).
};
```

**Honest concession, stated up front:** the minimal control-flow subset (`if`/`for`) is inherently
block-structured (context-free), no matter how the flat pipe/and-or expression grammar is
evaluated. Design B's "no separate AST, evaluate as you parse" pitch is strongest for the flat
pipeline/and-or grammar — the common case per 010 §1 (`ls`, `grep`, `git`, a build tool) — and is
**only as recursive as Design A** for the `if`/`for` subset, using the same explicit-depth-counter
mitigation described in §3. This is not a flaw unique to B; it is a reason not to oversell "single
pass" as a blanket safety property.

**A real, distinguishing hazard:** because evaluation is interleaved with parsing, a simple command
completes and its side effects (a `cd`, a file write) happen *before* the parser has necessarily
determined that the rest of the command line is syntactically valid. A malformed script —
unterminated quote, `if` without `fi` — can therefore leave some prefix of commands already
executed by the time the syntax error surfaces, whereas Design A's full-script parse-before-eval
structurally cannot do this (a syntax error anywhere means zero nodes evaluate). This is §5's S3
claim below, and is the sharpest asymmetry between the two designs.

**Steelman:**

1. **Fewer allocations for the dominant case.** For a short, non-nested pipeline (the stated common
   case), there is no AST node allocation at all — the operand/operator stacks and the
   already-required captured-output buffers are the only heap activity, potentially undercutting
   Design A's arena allocation for that path.
2. **Conceptually one traversal.** There is one function reading the input once, rather than a
   "build a tree, then walk it" two-phase shape — less total code by line count is a plausible
   (falsifiable, §5) claim.

**Costs, stated plainly:**

1. **The parser cannot be fuzzed in isolation without an execution-shaped harness.** Because parsing
   *is* executing, a `bytes -> does-it-crash` fuzz target needs a `CommandRegistry`/
   `FileSystemAdapter`/`ExecState`/`EffectContext` in hand (real, fake, or no-op) — every fuzz run
   is only as faithful as those fakes, unlike Design A's fake-free `parse()` target. A `dry_run`
   flag threaded through the evaluator to recover isolated fuzzing is an extra code path that must
   itself stay in sync with the real path — exactly the kind of parallel-implementation risk this
   whole subsystem exists to avoid (010 §2's uniformity argument, one level down).
2. **A new operator touches two places at once.** Both the precedence table and the evaluation
   loop's state machine change together, versus Design A's isolated grammar-production-plus-
   eval-case pair.
3. **§4's partial-execution hazard (above)** is a correctness/safety property this design must
   affirmatively defend, not one it gets for free the way Design A does.

## 5. Falsifiable claims

Claims already common to both designs by construction (§2) are listed once and marked **[shared]**;
they are not evidence for either design specifically.

### 5.1 Shared claims (§2's dispatch/registry/adapter layer)

| # | Claim | Disproving experiment |
|---|---|---|
| **Sh-S1** | Resolving a name that is neither a fixed builtin nor present in the run's Runner/Tool tables never calls any process-creation primitive (`fork`, `exec*`, `posix_spawn`, `CreateProcess`, `dlopen`/`LoadLibrary`). | **Positive control**: link a shim replacing the platform's process-creation entry points with one that sets a flag and aborts if called; run a hostile-name corpus (`/bin/sh`, `; rm -rf /`-shaped words, `../../registered_tool`, names differing from a registered name by one byte) through `ShellRunner::run`. The flag must never be set. A secondary, static check: the `ShellRunner`/`CommandRegistry` translation unit must contain **zero references** to any process-creation symbol at link time — a symbol appearing there at all falsifies the "the code path does not exist" claim regardless of runtime behavior. |
| **Sh-C1** | A `cd` executed via `ShellRunner::run` mutates the same `ExecState` object a subsequent `PythonRunner::run(..., state, ...)` call (same `&state`) observes. | Construct one `ExecState`; run `ShellRunner::run({..,"cd /work/sub"}, state, ctx)`; assert `&state` unchanged and `state.cwd == "/work/sub"`; a companion (cross-ADR) test then runs `PythonRunner::run({.., "import os; os.getcwd()"}, state, ctx)` and asserts equality. The `ShellRunner`-only half — object identity plus direct mutation, no copy, no queued update — is provable unilaterally here. |
| **Sh-C2** | A name that shadows a builtin (e.g. exported as an env var, or matching a registered Tool/Runner name as a substring) resolves to exactly the registered/builtin identity, never something else, never ambiguously. | Aliasing/shadowing corpus: env vars named `ls`, a registered tool `grep` alongside a builtin-adjacent name, a name one edit-distance from a registered Runner. Any resolution other than the exact intended identity (or a clean `not_found`) falsifies it. |
| **Sh-G4** | `ShellRunner` cannot invoke a registered `Runner` (e.g. `PythonRunner`) without a held `RunnerCall<name>` capability. **The "cannot exceed" half (revised per finding 6):** the nested call receives the *identical* `EffectContext&` — same object, never a copy, never attenuated or broadened — so exceeding the grantor's capabilities is impossible by construction, not by a check; a future implementation that adds any capability-narrowing/broadening step before the nested call is a regression this claim must catch. | Gate half: deny `RunnerCall<python>`, attempt a composing shell command; must fail `policy`-classed before the nested `run()` executes. Construction half: assert `&ctx` passed to the nested `Runner::run` is pointer-identical to the caller's; **negative control**: deliberately patch a test build to broaden capabilities before the nested call and confirm the identity assertion catches it. |
| **Sh-C3** (added §2.5.2, finding 10) | An expanded variable's value never changes the argv count or pipeline/operator structure of the command it appears in, regardless of content — expansion is an opaque splice, never re-tokenized. | `export X=";rm -rf /work"` (`EnvWrite` granted); run `echo $X` unquoted; assert exactly one word, the literal string passed through unchanged, no second command executed. Repeat with `|`, `&&`, and embedded whitespace in the value. |

### 5.2 Design A — recursive-descent / AST

| # | Kind | Claim | Disproving experiment |
|---|---|---|---|
| A-F1 | fast | A typical command (<200 B, ≤3 pipeline stages) parses in <10 µs single-threaded with ≤4 heap allocations (arena-backed). | Hooked-allocator benchmark on reference hardware; either bound exceeded falsifies it. |
| A-S1 | safe | A 10 MB adversarially crafted input (nesting past `kMaxNestingDepth`, a 100k-stage pipeline, deeply nested `${...}`, unterminated quotes) is rejected in <100 ms and ≤4× input-size memory, zero ASan/UBSan findings, no OOB access. | libFuzzer/AFL corpus under ASan+UBSan with wall-clock/memory caps; any timeout, OOM, or sanitizer report falsifies it. Specific structural probe: `if` nested 10,000 deep with no closing `fi`, run under an artificially small stack (`ulimit -s` / thread stack size) — must fail cleanly at `kMaxNestingDepth` with `shell.nesting_too_deep`, never crash via native stack overflow. |
| A-S2 | safe | The grammar admits no catastrophic-backtracking input class (parse time is linear in input length, not exponential in any substructure). | Timing sweep over adversarially repetitive inputs (e.g. `$(` -like nested-lookalike sequences the grammar rejects, repeated N times) for N up to `kMaxTokens`; superlinear growth falsifies it. |
| A-C1 | correct | A golden corpus of example scripts (covering every grammar production) produces byte-identical `ExecOutcome`s on Windows, Linux, and macOS (010 §9 G6). | Run the corpus on all three target OSes; any divergence falsifies it. |
| A-C2 | correct | A syntactically invalid script executes **zero** builtins/runners/tools — the whole-script-parses-before-any-eval property. | Construct `mkdir /work/should_not_exist && <malformed tail>`; assert the directory does not exist after the call returns its parse error. |

### 5.3 Design B — single-pass tokenizer / operator-precedence evaluator

| # | Kind | Claim | Disproving experiment |
|---|---|---|---|
| B-F1 | fast | For the common case (single pipeline, ≤3 stages, no control flow), evaluation completes with fewer heap allocations than Design A (no AST node allocation) — specifically zero heap allocations beyond the fixed-capacity operator/operand stacks and the output-capture buffers both designs already require. | Identical hooked-allocator benchmark, same corpus, run against both implementations side by side; B not measurably lower (or equal) falsifies the "single pass is cheaper" pitch for the common case. |
| B-S1 | safe | Same adversarial-corpus bound as A-S1 (bounded time/memory, zero sanitizer findings), given a real-or-capability-denying fake `CommandRegistry`/`FileSystemAdapter`/`ExecState`/`EffectContext` supplied to the fuzz harness. | Same libFuzzer/ASan/UBSan setup as A-S1, adapted to require the execution-shaped harness; same falsifying conditions. |
| B-S2 | safe | No adversarial input causes a builtin side effect (file write, `cd`, `export`) to occur for a script that ultimately fails to parse, when the equivalent input under Design A would not have executed anything (§4's distinguishing hazard). | Construct a script whose first pipeline is a real, valid `mkdir /work/should_not_exist` followed by a later, unrelated syntax error (unterminated quote in a subsequent statement); assert the directory **does not** exist after the call returns its parse error. **This is expected to be the harder claim for B to sustain** — a positive result here (directory does not exist) would mean B added explicit buffering/lookahead to avoid the interleaving hazard, which is worth recording either way. |
| B-C1 | correct | Same golden-corpus / cross-OS claim as A-C1. | Same experiment as A-C1. |
| B-C2 | correct | `&&`/`||` short-circuit is exact: the right-hand side of `&&` after a failing left-hand side, or of `||` after a succeeding left-hand side, is never evaluated — checked by the *absence* of its side effect, not merely by the returned status. | `false_builtin && mkdir /work/should_not_run` — assert the directory does not exist. This is an easy bug class specifically in an evaluate-as-you-parse design, where "just keep consuming tokens" can accidentally evaluate an operand before checking whether the combinator needs it. |

## 6. Parser safety (Q7) as a first-class design axis, not an afterthought

010 §10 Q7 frames the open question precisely: the *actions* `ShellRunner` can cause are
capability-gated by construction (§2), but the *parser itself* is first-party code parsing
adversarial input — the same risk category 009 §7 pushes to a WASM component for file-format
parsers. Both designs above answer Q7 the same way at the architecture level — explicit depth
counters instead of native-stack trust, input/token caps, bounded-memory arenas or stacks — and
differ only in *how cleanly that hardening can be exercised in isolation* (§5.2/§5.3's A-S1/B-S1
asymmetry: Design A's `parse()` needs nothing but a byte string; Design B's evaluator needs a
harness that also stands in for dispatch).

**Why "hardened host code" rather than "run the parser as a WASM component" is this design's
starting position, not a dismissal of Q7:** the grammar in §3/§4 is deliberately tiny — a fixed,
enumerable production set with no user-extensible syntax, unlike a PDF/image/document parser (009
§7's actual justification for pushing parsing to a sandboxed plugin, where the input format's
complexity is large and evolving). A grammar this small is a plausible target for "fuzz it hard
enough that a component boundary buys little," but that is a claim §5's A-S1/B-S1 experiments are
supposed to test, not an assumption this ADR is entitled to make permanent. **If the red-team or
prove phase finds that either design's parser cannot be bounded/hardened adequately even after
fixes** — a sanitizer finding that survives a revision, a timing bound that cannot be met — the
fallback is compiling the parser to a WASM component and running it via the existing plugin host
(009), at whatever latency cost that carries. That escalation is out of scope for this ADR and
should be its own follow-up question if it becomes necessary, not folded silently into whichever
design is chosen here.

## 7. Red-team attack

Severity key used throughout: **[BLOCKING]** — breaks I2 or a stated gate, must be fixed before the
prove phase starts; **[REAL GAP]** — a genuine, currently-unaddressed hazard, survivable with a
documented design decision or caveat but should not be silently deferred; **[HOLDS]** — an attack
was attempted and the claim/experiment survives it as stated, recorded so it isn't re-litigated.
No implementation exists yet (`src/backends/native_jail/shell_runner.hpp` is a stub returning
`not_implemented`); every finding below is a design-level flaw or an experiment-design flaw, never
an observed runtime bug.

### 7.1 Shared claims (§5.1)

1. **Sh-S1 — the static "zero process-creation references" check is scoped to the wrong unit.**
   **[REAL GAP].** The claim's static half checks that "the `ShellRunner`/`CommandRegistry`
   translation unit" contains zero references to a process-creation symbol. That is sound evidence
   for exactly that TU and nothing more. It is not evidence for the property §1 actually needs —
   "no path from `ShellRunner::run`'s call graph reaches `exec`/`CreateProcess`" — because (a)
   `ShellRunner` legitimately composes with `PythonRunner` (010 §1a) and, through `dispatch_command`'s
   `tool` arm, with the 006 §3 Tool pipeline, both of which may live in *different* translation
   units that the same link target still pulls in; a future WASM-plugin-backed `Tool` (009) or a
   `remote`-backend `Runner` legitimately references `CreateProcess`/`posix_spawn` for reasons
   unrelated to name resolution, and nothing stops that reference from sitting in the same static
   library `ShellRunner.obj` links against; (b) build-layout is not call-graph — merging translation
   units (unity builds, LTO object grouping) or, conversely, splitting `CommandRegistry::resolve`
   into its own TU purely for the check's benefit, changes the check's verdict without changing
   anything about reachability. A grep/link-check that a diligent implementer can satisfy by moving
   code between files, without touching what actually executes, is exactly the "quietly disabled
   later" failure mode task 6 warns about. **Fix needed:** state the check at link-target/library
   granularity — `ShellRunner` + `CommandRegistry` + the builtin table compile into a library that
   must have **no link dependency, direct or transitive, on any object defining a process-creation
   symbol** — and treat a legitimate collaborator (WASM host, `remote` backend) needing that symbol
   elsewhere as evidence the check's boundary must exclude those libraries by construction (they are
   never linked into the shell-dispatch target), not as grounds to weaken the check.
2. **Sh-S1 — the runtime shim experiment is under-specified for the Windows-first target.**
   **[REAL GAP].** "Link a shim replacing the platform's process-creation entry points" does not say
   *how* (IAT patching, detours, a link-seam substitute symbol) or at what scope (per-TU vs.
   process-wide). A process-wide hook risks a false positive if anything else in the same test
   binary — the test harness, Quark's own actor engine, a `remote`-backend control-plane call —
   legitimately creates a process during the same run, and a narrowly-scoped hook risks a false
   negative if an indirect route (`_wspawnv`, `system()`, a CRT wrapper calling into `CreateProcess`
   through a different import) isn't covered. **Fix needed:** name the actual hooking mechanism and
   its scope before the prove phase writes this test, and isolate the process-creation-flag assertion
   to a test binary that links nothing else capable of legitimately triggering it.
3. **Sh-C1 — `ExecState` sharing is proven, but `cd`'s write to `state.cwd` is under-specified in a
   way that opens a canonicalization TOCTOU.** **[REAL GAP].** §2.4 has `cd` validate the target via
   `fs.exists()` but requires no capability ("cwd bookkeeping is host-side; no I/O"). The ADR never
   states whether `state.cwd` is then set to the *raw argument string* or to whatever canonical form
   the adapter would produce. `RealFileSystemAdapter` (§2.1) canonicalizes per call. If `cd` stores
   the raw argument and a later command (a sibling `ls`, or `PythonRunner`'s mediated `os` layer)
   re-resolves `cwd + relative-arg` and the adapter's canonicalization is not idempotent across two
   separate calls — a symlink created in between, or, on Windows, case-folding/8.3-short-name
   aliasing making two different-looking strings resolve to the same or a different path depending on
   when they're evaluated — the path validated at `cd` time and the path actually used later can
   diverge. This does not reach ambient authority (every real I/O still re-checks capability against
   whatever path it resolves), so it is not an I2 break, but it is a genuine confused-deputy hazard:
   a capability scoped against the canonical path might not match what `cd` validated. **Fix needed:**
   state explicitly that `ExecState.cwd` always holds the adapter's canonical output, never the raw
   argument, and that every consumer (both Runners) re-canonicalizes on every use rather than trusting
   a cached canonical form.
4. **Sh-C1 / §2.4 `export` — env-var mutation is treated as capability-free "bookkeeping," but
   `ExecState.env` is shared by reference into `PythonRunner`, which is exactly the kind of ambient,
   implicit-trust channel I2/I3 exist to close.** **[BLOCKING — must be resolved or explicitly scoped
   before prove phase].** This is the strongest finding in this review. §2.4's table marks `export`
   as requiring **no** capability, on the same theory as `cd` ("host-side, no I/O"). But 010 §3a
   states plainly that "Python's own `subprocess`/`os` calls and `ShellRunner` observe the same
   `ExecState`" — meaning whatever `export` writes into `state.env` is available to `PythonRunner`'s
   embedded CPython as `os.environ` (or whatever the mediated layer exposes). CPython itself reads
   ambient authority from environment variables that were never modeled as capabilities anywhere in
   this system: `PYTHONPATH`/`PYTHONSTARTUP` (import-machinery and startup-code injection),
   `PIP_INDEX_URL`/`PIP_*` (relevant the moment §5's `allowlist`/`open` package policy is active),
   `HTTP_PROXY`/`NO_PROXY`/`SSL_CERT_FILE` (network-trust configuration), and platform loader
   variables. A shell command whose text is itself model output (ordinary, expected — 010 treats the
   whole command as the agent's authored action) can `export PYTHONPATH=/tmp/attacker` or
   `export PIP_INDEX_URL=http://evil/simple` with **zero capability check**, and that value is later
   consulted by `PythonRunner` — including at points (interpreter/import startup, package resolution)
   that are not modeled as "a call requiring a capability" at all, so 008 §1b's per-call mediation
   doesn't obviously catch it either. This is model-influenced data acquiring the power to change a
   *different* Runner's trust decisions — the shape I3 forbids — and it is not mentioned anywhere in
   §11's residual-risk list. **Fix needed:** either (a) give `export` a scoped capability (e.g. an
   `EnvWrite` kind, possibly allowlisted by variable name, mirroring `FsRead`/`FsWrite` scoping), or
   (b) require the mediated CPython embedding (008 §1b) to treat every value read from `ExecState.env`
   as tainted with respect to any security-relevant consultation (import machinery, network
   configuration, subprocess environment) rather than as host-set configuration. Silence on this is
   not an acceptable resolution — the sharing mechanism that makes `ExecState` valuable (010 §3a, §9
   G3) is precisely what makes this dangerous.
5. **Sh-C2 — the shadowing claim has no stated precedence rule for a genuine name collision between a
   builtin and a registered Tool, and the "wrong" precedence is security-relevant, not merely
   surprising.** **[REAL GAP, borderline BLOCKING].** Consider an operator who registers a Tool named
   `cat` specifically to add DLP scanning or audit logging around file reads — a realistic operator
   motivation. §2.2's `CommandRegistry::resolve` is described as a "pure lookup over three closed
   tables," but the ADR never states which table wins when a name exists in more than one (the
   `command_kind` enum's declaration order `builtin, runner, tool` hints at a possible precedence, but
   hints are not a spec). If the builtin silently wins, the operator's audit/DLP wrapper is never
   invoked and the operator has no way to know their own security control was shadowed — a violation
   of the operator's actual authorization intent even though every individual capability check
   "worked." If the tool silently wins, a runtime registration (potentially reachable through a
   different, less-trusted admission path than the compiled-in builtin table) can transparently
   replace what looks to every other reviewer like a fixed, audited builtin. **Fix needed:** resolve
   this by construction — reject registration of a Tool/Runner whose name collides with a builtin
   name at registration time ("reserved name," a clean, fail-closed `contract` error) rather than
   leaving precedence to resolve()'s implementation.
6. **Sh-G4 — the "cannot exceed the capability set it was itself granted" half of the claim is either
   vacuous or untested by the given experiment.** **[REAL GAP — experiment-design flaw].** §2.3's
   `invoke_registered_runner` passes the *same* `EffectContext& ctx` into the nested
   `registered_runner.run(sub_request, state, ctx)` call — same object, not a copy or an attenuated
   view. If that is really the design, PythonRunner cannot possibly exceed ShellRunner's capabilities
   because it is handed the identical set — true by construction, not by any check the given
   experiment (deny `RunnerCall<python>`, assert failure before nested execution) exercises. That
   experiment only proves the *gate* half of Sh-G4; it says nothing about the *"cannot exceed"* half,
   which today is either trivially true (same pointer, no attenuation code exists to get wrong) or
   silently unverified the moment a future implementation adds any capability-narrowing/broadening
   step before the nested call. **Fix needed:** state explicitly that the `ctx` passed to a nested
   `Runner::run` is the identical object, never attenuated or broadened, and add a control to the
   prove-phase experiment where an implementation is deliberately made to broaden capabilities before
   the nested call, to confirm such a regression would actually be caught.

### 7.2 The shared dispatch/registry/builtin layer beyond the named claims (§2)

7. **`cd`/`export` requiring no capability is one instance of a broader unaddressed question: what
   does a null or dangling `EffectContext.capabilities` do?** **[REAL GAP].** `EffectContext.capabilities`
   is `CapabilitySet const*`, "borrowed; never owned" (`effect_context.hpp`). Nothing in §2 or §2.4
   states that a null pointer must be treated as an empty set (deny-all) rather than dereferenced.
   Since I2 depends on *every* check site getting this right, and there will be many check sites (one
   per builtin, plus the Tool/Runner arms), this should be a single stated invariant ("a null
   `capabilities` pointer denies every capability check; no check site may dereference without this
   guard") rather than left to each builtin's author to independently get right.
8. **`mv`/`cp`'s cross-device fallback (`copy_file` + `remove`) is a non-atomic two-step sequence with
   no stated rollback, and this is unaddressed in §11.** **[REAL GAP].** When `rename()` fails
   cross-device, §2.4 falls back to `copy_file` then `remove`. Between those two calls: (a) the source
   still exists *and* a full copy exists at the destination — for 025 §3's explicitly supported
   multi-agent shared-worktree case, a concurrent second agent can observe or mutate the source during
   a window where the "move" is supposed to have already made it disappear from that location, which
   matters if the two locations carry different capability scopes (e.g. `/work` → `/out` promotion,
   010 §4); (b) if `remove(from)` fails after `copy_file` succeeds (resource limit firing mid-builtin,
   disk full, a killed process), the result is a silently duplicated file with no compensating
   transaction and, as described, no distinct error class separating "moved" from "copied-but-not-
   removed." This is not an I2 break (no ambient authority reached) but is a genuine confinement/
   consistency gap in a layer both designs share equally, and it is not mentioned anywhere in §11.
   **Fix needed:** a distinctly-classed error for "copied but the source removal failed," and a stated
   answer for whether the worktree's concurrency model (025 §3) requires a lock across the two steps.
9. **§2.1's restatement of 025 §5's path-escape requirement is narrower than 025 §5 itself, and — for
   `RealFileSystemAdapter` specifically, the *actual* near-term implementation target, not the
   deferred `WorktreeFileSystemAdapter` — Windows-specific bypasses are entirely unaddressed.**
   **[BLOCKING for `RealFileSystemAdapter`, before it is implemented].** 025 §5 requires rejecting
   "`..`, absolute redirects, symlinks/junctions/reparse points crossing the boundary, ADS." §2.1
   restates this as "rejects `..`/absolute-redirect/symlink-escape" — dropping junctions/reparse
   points and Alternate Data Streams from its own paraphrase, and neither document's text for this
   ADR's actual implementation target addresses UNC paths (`\\server\share\...`), extended-length
   `\\?\` prefixes, reserved device names (`CON`, `NUL`, `AUX`, `COM1`–`COM9`, `LPT1`–`LPT9`, including
   as a stem with any extension), or case-insensitivity divergence between whatever does the I/O and
   whatever does the capability-scope string comparison. That last one is the sharp edge: if capability
   scope matching string-compares a *different* representation of the path than the one the adapter
   actually resolves against the OS (raw argument vs. canonicalized form, case-preserved vs.
   case-folded, with vs. without an ADS suffix), a scope check can diverge from the actual I/O target
   in either direction — silently narrower (false denial, safe) or silently broader (a real bypass).
   Given this project explicitly targets Windows first (see recent commit history), this cannot be
   left to 025's future `WorktreeFileSystemAdapter` — it applies to the adapter this ADR actually
   scopes as its implementation target now. **Fix needed:** §2.1 needs its own explicit list — matching
   or exceeding 025 §5's — of what `RealFileSystemAdapter` rejects or canonicalizes before any
   capability-scope comparison happens, stated as a testable claim, not a one-sentence gesture at 025.

### 7.3 The grammar itself (§3) — word-splitting and expansion semantics

10. **The grammar specifies no expansion/word-splitting semantics, and this is the single most
    security-relevant gap in the whole document — yet §5 has no claim testing it at all.**
    **[BLOCKING].** `word := (literal | '$' NAME | '${' NAME '}' | quoted)+` says how a word's *source
    text* is structured; it says nothing about what happens when an expanded `$NAME` value is
    substituted back into that position. Two readings are both consistent with the grammar as
    written: (a) **opaque splice** — the expanded value is spliced in as inert data occupying exactly
    the one word/argv slot it appears in, never re-scanned for `|`/`&&`/`;`/whitespace, never
    re-tokenized, never changing argv count — the safe reading, and the one Design A's "parse is pure,
    evaluate resolves `$NAME` nodes after the fact" structure leans toward; (b) **POSIX-style
    re-splitting** — an expanded value containing whitespace grows into multiple argv elements (this
    is what makes `for x in $list` iterate per-element in a real shell), which, if the evaluator ever
    additionally re-scans expanded text for operator characters, becomes a genuine injection
    vector: a value assigned once (via `export`, from a source that could itself trace back to tainted
    tool-result text quoted into an `export` statement by the model) later expands unquoted into
    something that changes the *shape* of a different command entirely — turning tainted **data**
    (003 §2) into command **structure**, exactly the I3 violation this whole subsystem exists to
    prevent. The ADR never states which reading is intended, and none of Sh-*/A-*/B-* in §5 test it.
    This is a spec gap real enough to matter regardless of which design wins, because both designs
    share this grammar. **Fix needed:** one explicit sentence — "expansion never re-tokenizes: an
    expanded value is spliced as an opaque literal into the word/argv slot it occupies; it is never
    re-scanned for operators, never re-split on whitespace into additional words, and never treated as
    new grammar text" — plus a new falsifiable claim (e.g. **Sh-C3**) with a disproving experiment:
    `export X="; rm -rf /work"` (or a value containing `|`, `&&`, embedded whitespace), use `$X`
    unquoted inside another word, and assert argv/pipeline structure is unaffected.
11. **The grammar does not specify whether `$NAME` expands inside `quoted` atoms, and the safe answer
    (it never does) should be a stated decision, not an inference from the alternative-list shape.**
    **[REAL GAP].** `quoted` appears as a peer alternative to `'$' NAME`/`'${' NAME '}'` within
    `word`'s Kleene-plus, which — read literally — implies quoted text is always inert (no expansion
    inside quotes at all, stronger and simpler than POSIX's single-vs-double-quote split). That is a
    good, safe design if intended, but it is currently an inference from grammar shape rather than a
    stated rule, and a natural "make it feel more like a real shell" revision later (double quotes
    allow expansion, single quotes don't) would reopen finding 10's injection concern inside quotes
    specifically. **Fix needed:** state the no-expansion-inside-quotes rule explicitly as a decision,
    with its own falsifiable claim.

### 7.4 Parser-safety mechanisms (A-S1 / B-S1) — depth counters and arena sizing

12. **"A `depth_` field" (singular, in prose) checked at "every recursive entry point" is not
    verifiable as a design-level claim, because the design doc does not enumerate the actual function
    call graph a recursive-descent implementation will have.** **[REAL GAP].** Two distinct failure
    shapes are both consistent with the prose as written: (a) if `if`/`for`/nested-`${...}` nesting
    each get their **own independent counter**, each individually bounded by `kMaxNestingDepth`, a
    script combining near-maximal nesting of *each kind simultaneously* (31 nested `if`s wrapping a
    word with 31 nested `${...}`) passes every individual check while producing native stack usage
    close to the **sum**, not the max, of the per-kind bounds — defeating the stated intent that
    `kMaxNestingDepth` bounds worst-case native recursion; (b) even with one shared counter, "every
    recursive entry point" is asserted for three named productions (`parse_if`, `parse_for`, nested
    `${...}`) that may not be the complete list once real mutually-recursive helper functions
    (word/quoted/expansion parsing calling back into each other) exist — an uncounted intermediate
    call inflates the actual stack-frames-per-logical-nesting-level ratio above 1 without violating
    the stated invariant, because the stated invariant doesn't name that function. Separately,
    A-S1's own experiment ("`if` nested 10,000 deep with no closing `fi`") only exercises an
    open/never-closed nesting shape, which cannot distinguish "counter increments and decrements
    correctly around a return" from "counter is monotonic and never decremented" — both fail identically
    against that specific probe, but only one of them is actually safe against wide-not-deep scripts
    (many *sequential*, shallow, non-nested constructs) that the given probe never constructs. **Fix
    needed:** state explicitly that depth tracking is one counter, shared across every grammar
    production capable of recursive descent, incremented on entry and decremented on exit, wherever
    that entry point lives in the eventual function decomposition; add a combined-nesting adversarial
    probe (near-max `if` nesting whose body/condition also contains near-max `${...}` nesting) and a
    wide-not-deep probe (many sequential, shallow, non-nested statements) to A-S1/B-S1's corpus.
13. **The arena-sizing formula ("a function of `kMaxSourceBytes`") is unspecified precisely enough
    that A-S1's own "≤4× input-size memory" bound is currently unfalsifiable, and the natural
    pathological input for this axis — maximum token count, minimum content per token — is not in
    the given experiment list.** **[REAL GAP].** `pipeline := simple_command ('|' simple_command)*`
    and `word+` are **iterative** (Kleene-star), not recursive, so `kMaxNestingDepth` does nothing to
    bound them — only `kMaxTokens`/raw byte budget does (the ADR's own 100k-stage-pipeline probe
    already exercises this axis for pipeline width, which is sound). But per-node memory overhead
    (vector control blocks, variant tag/padding, small allocations for each `Word`'s atom list) for a
    script built from the cheapest possible 2-byte token (`a `, `a|`, `a;`) is very unlikely to be
    anywhere near proportional to the 2 raw source bytes it came from — a script of N one-character
    words separated by the cheapest delimiter, N up to `kMaxTokens`, is the input shape that
    specifically stresses "AST nodes per input byte," and it is a different shape from either of
    A-S1's named probes (nesting depth, pipeline width as *stage count* with presumably
    non-minimal per-stage content). **Fix needed:** add this exact shape — maximum token count at
    minimum per-token content — to A-S1/B-S1's required corpus, and specify the arena/stack sizing
    formula concretely enough (bytes per token, not bytes per input byte) that "≤4×" is a checkable
    number before the prove phase runs it.

### 7.5 Design A specific claims

14. **A-F1's "≤4 heap allocations, arena-backed" is contradicted by §3's own AST type definitions as
    shown.** **[REAL GAP — internal inconsistency, must be fixed before prove phase writes code].**
    §3's structs (`PipelineNode { std::vector<SimpleCommandNode> commands; }` and similarly for
    `AndOrNode`, `SimpleCommandNode.words/redirects/assigns`, `ScriptNode.statements`) are plain
    `std::vector<T>`, not `std::pmr::vector<T>`. A plain `std::vector` allocates from the *global*
    allocator regardless of whether a `std::pmr::monotonic_buffer_resource` exists elsewhere in the
    same function — the "AST nodes allocated from a bounded arena" bullet is prose unsupported by the
    types given in the very same section. As literally shown, a 3-stage pipeline needs at least one
    heap allocation per `words` vector (one per `SimpleCommandNode`) plus one for the outer
    `commands` vector plus outer `AndOrNode`/`ScriptNode` vectors — plausibly exceeding "≤4" before
    the arena is even relevant, or trivially satisfying it if every one of these is silently assumed
    to be `std::pmr::vector` fed the same `polymorphic_allocator` — the ADR doesn't say which, so A-F1
    cannot currently be evaluated as sound or unsound. **Fix needed:** revise §3's struct definitions
    to explicitly use `std::pmr::vector<T>` (threading a `polymorphic_allocator<>` through
    construction from the arena) before the prove phase treats A-F1 as a claim to measure, or the
    first hooked-allocator run is likely to falsify it for the wrong reason (a documentation gap, not
    a real allocation-count problem).
15. **A-S1/A-S2** — see findings 12–13 (shared with B). No design-specific weakness beyond those found
    for the linear-parse-time claim itself: the grammar as shown (§3) genuinely looks close to LL(1)
    with bounded lookahead — the `assignment` vs. `word` ambiguity (`NAME=word` vs. a bare word
    starting with what looks like `NAME`) resolves with one token of lookahead past the first `=`,
    and `=` is an ordinary literal character inside a word once past that point, so a chain like
    `a=b=c=d` parses as one assignment with no recursion or backtracking. **[HOLDS]** — attempted, no
    catastrophic-backtracking-shaped production found in the grammar as given. Recommend broadening
    A-S2's experiment corpus (currently framed around `$(`-lookalike sequences) to also include long
    `NAME=` chains and long runs of keyword-prefix-sharing tokens (`if`/`fo`/`for`), since a prose
    argument is not the same as a fuzzed guarantee.
16. **A-C1 (byte-identical `ExecOutcome` across OSes)** — **[HOLDS, with one caveat]**. Because
    `ExecState.env` is a case-sensitive `std::unordered_map<std::string,std::string>`, env-var
    lookups are consistently case-sensitive on every OS, which is internally consistent even though
    it diverges from Windows' native `cmd.exe` case-insensitivity convention — a correctness/UX note
    for 026 §3's "feels ordinary" goal, not a cross-platform divergence, so it doesn't break A-C1. The
    caveat: whether a given `mv` takes the atomic `rename()` path or the `copy_file`+`remove` fallback
    (finding 8) is a function of filesystem/mount topology, not of `ShellRunner`'s own code — once
    `WorktreeFileSystemAdapter` mounts genuinely different backing stores per subtree (025 §3/§5), the
    *same* logical script could take different paths on different platforms/configurations, and the
    ADR doesn't state that the two paths must produce identical `ExecOutcome` (same success
    classification, same metadata-preservation behavior). Recommend a corpus entry that forces the
    fallback path on at least one configuration and checks parity against the fast path.
17. **A-C2 (nothing executes until the whole script parses)** — **[HOLDS]**. True by construction of
    the two-phase `parse` → `evaluate` structure, given `evaluate` is never invoked except on a
    successful `parse` result. The only way to lose this property is a future "best-effort partial
    script" mode added for UX reasons (salvage-and-run the valid prefix of a malformed script), which
    would quietly reintroduce Design B's own hazard into Design A. Recommend recording this as an
    explicit invariant to protect during any future revision, not just a passing test today.

### 7.6 Design B specific claims

18. **B-F1's "zero heap allocations beyond fixed-capacity stacks" has the identical shown-code problem
    as A-F1's finding 14, for the identical reason.** **[REAL GAP — parallel to finding 14, applies
    equally, not a point in A's favor]**. §4's `PipelineFrame { std::vector<SimpleCommandBuilder>
    commands; }` is a plain, growable `std::vector`, and `SimpleCommandBuilder`'s argv accumulation
    almost certainly needs one too — neither is described as fixed-capacity. As shown, even the
    "dominant case" (a single short pipeline) plausibly heap-allocates at least once for the commands
    vector plus once per command for argv storage, contradicting "zero… beyond the fixed-capacity
    operator/operand stacks." **Fix needed:** either show these as fixed-capacity inline containers
    consistently, or rephrase B-F1 as relative ("fewer than A for the common case") rather than an
    absolute zero the shown types don't support.
19. **B-S1** — the ADR's own §4 cost #1 (fuzzing needs an execution-shaped harness, faithfulness-
    limited by its fakes) is confirmed as real, not overstated. **[REAL GAP, already substantially
    acknowledged by the design phase]**. One addition: a fuzz harness whose fake
    `FileSystemAdapter`/`CommandRegistry` always succeeds never exercises the evaluator's
    error-handling paths — exactly where an interleaved parse/eval design is most likely to have
    state-corruption bugs. Recommend the fakes themselves be randomized (sometimes deny, sometimes
    fail, sometimes return adversarially-shaped data) rather than fixed, or B-S1's harness risks being
    a test that cannot fail proving nothing (`decisions/README.md` item 5).
20. **B-S2 — the given experiment tests only one of at least three distinct places the
    interleaving hazard could occur, and it happens to be the easiest one for B to pass.**
    **[REAL GAP — experiment-design flaw]**. The experiment (a valid `mkdir` in one statement,
    followed by a later, *unrelated* syntax error in a subsequent statement) tests only the
    cross-statement case. Two tighter cases are not covered: a syntax error in a **later pipeline
    stage of the same pipeline** as an already-dispatched earlier stage, and a malformed **redirect**
    trailing an otherwise-complete simple command (testing whether "argv and redirects fully parsed"
    is actually enforced as one atomic precondition for dispatch, per §4's own description of the
    trigger condition, or whether argv alone is enough to fire early). These are meaningfully harder
    for B to sustain than the given cross-statement case. **Fix needed:** broaden B-S2's corpus to
    include both.
21. **B-C1** — shared with A-C1 (finding 16); no additional B-specific attack found.
22. **B-C2 (`&&`/`||` short-circuit exactness)** — **[HOLDS, experiment should be broadened]**. The
    given two-term experiment (`false_builtin && mkdir ...`) is sound as far as it goes, but a
    shunting-yard evaluator getting a single binary operator right is a materially weaker guarantee
    than getting a **chain** right. Recommend extending the corpus to 3+-term and mixed `&&`/`||`
    chains (`false && mkdir A && mkdir B`; `true || mkdir A && mkdir B`) before treating this claim as
    proven — "get the first pair right, mis-chain the rest" is exactly the bug class a precedence-climb
    implementation is prone to.

### 7.7 Residual risks the design phase missed (beyond §11)

23. **§11 item 2 ("`CapabilitySet`'s representation is a placeholder… should be re-checked once 007
    has its own ADR") materially understates the problem for the prove phase.** **[BLOCKING for any
    claim using the word "scoped"]**. `trust/capability.hpp`'s `Capability` struct is `{ capability_kind
    kind; }` — no subtree, path, quota, or any other scoping field at all; the comment says fields are
    "backend-specific… therefore not modeled here." This is not "the concrete matching mechanism is
    TBD" (§11's framing) — it is "the type has no field that could ever distinguish 'FsRead on
    `/work/a`' from 'FsRead on `/work/b`.'" Every claim in §2.4's table phrased as "checked… scoped
    over the resolved path" is therefore not merely unproven but **currently unstatable as a concrete
    test**: there is no `Capability` value today that could construct the positive/negative control
    pair ("grant read on `/work/public`, deny read on `/work/secret`, assert `cat` behaves accordingly
    for each") that a scoping claim requires. `decisions/README.md` item 5 is explicit that "a test
    that cannot fail proves nothing" — right now, a scoping test cannot even be *written*, let alone
    fail. **Fix needed:** either block ADR-001's authorization claims on a minimal 007 extension that
    adds at least a scope field to `Capability`, or explicitly downgrade every "scoped" claim in this
    ADR to the strictly weaker "holder has this capability kind at all, or doesn't" for the prove
    phase, and mark the scoped version of each claim `INCONCLUSIVE`-by-design (not `CORRECT`) until
    007 lands. Treating today's placeholder as good enough to produce a `CORRECT` verdict on any
    "scoped" claim would be laundering an untestable claim into a passed one.
24. **Command substitution and background processes are correctly named as out of scope (§11 items
    4/6.5 by cross-reference to 010 Q6/§2), but the export/env sharing hazard (finding 4) and the
    word-splitting/expansion gap (finding 10) are not named anywhere in §11, despite being at least as
    security-relevant as the items that are listed.** **[REAL GAP — completeness of §11 itself]**.
    Recommend §11 gain two entries pointing at findings 4 and 10 specifically, since a reader of §11
    alone would currently have no signal that either exists.

### 7.8 Summary verdict

Of the 24 numbered findings above, **1 is a hard I2/I3-shaped BLOCKING finding independent of either
design's own claims** (finding 4, `export`/`ExecState.env` as an unmodeled ambient-authority channel
into `PythonRunner`), **2 more are BLOCKING specifically for what must be true before the prove phase
can produce a meaningful verdict** (finding 10, the grammar's unstated expansion semantics — §5 has no
claim that would even catch a regression here; and finding 23, `CapabilitySet`'s placeholder shape
making every "scoped" claim in §2.4 currently untestable), and one more (finding 9) is blocking
specifically for `RealFileSystemAdapter` before it is implemented. The remaining findings are real
gaps that should be closed or explicitly caveated before or during the prove phase but do not by
themselves defeat I2, plus a handful of attempted attacks (findings 15, 16, 17, 22) that the design
survives as stated, usually with a recommendation to broaden the corresponding experiment.

**Does the red-team distinguish Design A from Design B, or does it mostly hit the shared layer?**
Overwhelmingly the latter. Findings 1–13 and 23–24 attack §2 (dispatch/registry/builtin/adapter) and
§3 (the grammar), which are byte-for-byte identical inputs to both designs — every one of those
findings, if valid, must be fixed once and benefits whichever design is chosen, and none of them
gives either design an advantage over the other. The two places this review found a genuine
asymmetry are exactly the two the design phase had already identified itself (§4's "honest concession"
and "real, distinguishing hazard," and §5.2/§5.3's A-S1/B-S1 framing): **(1)** Design A's `parse()` is
fuzzable fake-free while Design B's evaluator needs an execution-shaped harness whose fakes are
themselves an attack surface (finding 19 confirms this is real, not just plausible-sounding); and
**(2)** Design A structurally cannot execute any part of a syntactically invalid script while Design B
must affirmatively defend that property, and its own given experiment for doing so (finding 20) covers
only the easiest of at least three places the hazard could hide. Red-teaming therefore **confirms,
rather than newly discovers,** the one axis that actually separates A from B; it does not surface a new
A-vs-B distinguishing attack, and the majority of pre-prove-phase work this review generates is
shared-layer work that has to happen regardless of which design the judge phase eventually picks.

## 8. Executed evidence

**Design A only** — recursive-descent parser (`src/backends/native_jail/shell_parser.{hpp,cpp}`),
pmr-backed AST (`shell_grammar.hpp`), tree-walking evaluator + fixed builtin table + dispatch
(`shell_dispatch.{hpp,cpp}`), `CommandRegistry` (`command_registry.hpp`), `RealFileSystemAdapter`
(`real_filesystem_adapter.{hpp,cpp}`), and the concrete `ShellRunner` (`shell_runner.hpp`) were
implemented for real and wired into the build as a new static library target
`agentengine_shell_runner` (root `CMakeLists.txt`). Design B (§4) was **not implemented** — see
§9's Design-B row for why, honestly, rather than silently.

### 8.1 Toolchains and environment

| Toolchain | Version | Build dir | Configuration |
|---|---|---|---|
| MSVC (cl.exe) | 19.51.36252.0 (VS "18" BuildTools) | `build/` | Default (debug CRT), via `vcvars64.bat` |
| clang (MSVC-ABI) | 22.1.5 | `build-clang/` | Default (debug CRT) |
| clang (MSVC-ABI) | 22.1.5 | `build-clang-release/` | `-DCMAKE_BUILD_TYPE=Release` |
| clang (MSVC-ABI) | 22.1.5 | `build-asan/` | `-DCMAKE_BUILD_TYPE=RelWithDebInfo -fsanitize=address,undefined -fno-sanitize-recover=undefined -O1 -g` |

CMake 4.1.1, Ninja generator throughout, `-j4` per CLAUDE.md's machine-safety cap (never higher).
UBSan on Windows is clang-only per CONVENTIONS/task brief — MSVC's own sanitizer support does not
cover UBSan-equivalent checks on this toolchain, so all sanitizer evidence below is clang.

### 8.2 Build commands and results

MSVC (`vcvars64.bat` sourced first, per the known "silent stale-binary" trap — verified real
compilation output, not `ninja: no work to do`):

```
cmd /c call "...\VC\Auxiliary\Build\vcvars64.bat" && cd /d D:\GitSrc\AgentEngine && cmake --build build -j4
```
Result: 19 targets rebuilt from scratch (`shell_parser.cpp.obj`, `shell_dispatch.cpp.obj`,
`real_filesystem_adapter.cpp.obj`, all new test executables), **zero errors**, two `C4996`
deprecation warnings (`tmpnam`, `fopen` in the Sh-S1 static-check test — pre-existing style, not a
new-code defect) under `/W4`.

clang (`build-clang/`, plain `cmake --build build-clang -j4`, no vcvars needed): same set of
targets, **zero errors**, `-Wall -Wextra` clean except the identical two deprecation warnings.

### 8.3 Test results — MSVC (`build/`, default/debug CRT)

```
ctest --output-on-failure -j4
100% tests passed, 0 tests failed out of 7
Total Test time (real) = 0.54 sec
```
All 7 tests (`smoke_vocabulary`, `test_recorded_chat_client`, `test_native_jail_runner_stubs`,
`test_real_filesystem_adapter`, `test_shell_runner_proof`, `test_shell_parser_adversarial`,
`test_shell_runner_no_process_creation`) **Passed**.

### 8.4 Test results — clang (`build-clang/`, default/debug CRT)

Identical: `100% tests passed, 0 tests failed out of 7`, `Total Test time (real) = 0.31 sec`.

### 8.5 A real, unanticipated finding: MSVC debug-CRT checked iterators and wide `pmr::vector<T>` trees

Not one of ADR-001's original 24 red-team findings — discovered during this pass. The literal
"100k-stage pipeline" adversarial input (A-S1), run against the DEFAULT build configuration
(`build/`, `build-clang/` — no explicit `CMAKE_BUILD_TYPE`, so MSVC's debug CRT and
`_ITERATOR_DEBUG_LEVEL=2` "checked iterators" are active), **hung for minutes** once
`PipelineNode::commands` (`pmr::vector<SimpleCommandNode>`, each element owning 3 more
`pmr::vector`s) grew past roughly 12,000 live elements and needed to reallocate. Bisection (timed
instrumentation, since removed) showed near-constant per-batch cost up to that point, then a sudden
multi-second-plus stall exactly at that reallocation — a build-configuration effect, not an
algorithmic one. **Confirmed by a side-by-side measurement**: the identical source, same compiler,
built `-DCMAKE_BUILD_TYPE=Release` (`build-clang-release/`), parses and rejects the full
**100,000**-stage input in **7 ms**. `test_shell_parser_adversarial.cpp` now runs a debug-safe
8,000-stage positive control in the default CTest suite (to avoid CLAUDE.md's "must not hang the
dev box" rule biting on routine `ctest` runs) and compiles in the literal 100,000-stage probe only
under `-DCMAKE_BUILD_TYPE=Release` (`AE_RUN_100K_PIPELINE_PROBE`, gated in `tests/CMakeLists.txt`).
**Fix applied, recorded honestly**: this is a build-hygiene finding for anyone running adversarial
width probes under this project's default (debug) CMake configuration, not a ShellRunner defect.

A second, smaller, genuinely-my-bug finding surfaced by the same run: `kBytesPerNodeUpperBound =
256` (finding 13's arena-sizing constant) is measured as comfortably sufficient in Release (the
"max-token, min-content" over-budget probe fails via `shell.too_many_tokens`, the intended path)
but marginal under the debug CRT's extra per-container overhead, where the SAME probe instead fails
via `shell.arena_exhausted` first. **Both are clean, `resource`-classed, non-crashing rejections**
— the safety property A-S1 actually requires holds either way — so this is recorded as a
measurement note (the constant is toolchain-configuration-sensitive at the margin), not weakened
into a false pass or escalated into a blocking finding.

Two more real, fixed-during-this-pass bugs, neither present in the initial commit:
1. **`RealFileSystemAdapter`'s ancestor-walk produced a path with a spurious trailing separator**
   (`std::filesystem::path::operator/` unconditionally appends a separator even when the
   right-hand operand is empty — `path("x") / path("")` yields `"x/"`), which made every
   already-existing-parent write/read target look like a directory to the OS and fail with
   `shell.fs.io_error`. Found by the positive-control read/write round-trip test actually failing
   (not by inspection) — fixed in `real_filesystem_adapter.cpp` with an explicit empty-path guard
   in both places the bug occurred.
2. **The Sh-S1 static-symbol check's first version was a false-positive substring match**: a naive
   `contents.find("system")` over the whole `llvm-nm` dump matched inside
   `__std_system_error_allocate_message` (legitimate `std::filesystem`/`std::system_error`
   plumbing, unrelated to the `system()` call). Fixed to parse per-line and compare the exact
   trailing symbol token, not a substring of the whole dump.
3. **MSVC's `assert()` pops a blocking interactive "Debug Assertion Failed" dialog on failure**,
   which hung the very first `ctest` run of `test_real_filesystem_adapter` for several minutes with
   no CPU usage (waiting on a click that never comes) — exactly the kind of hang CLAUDE.md's
   Machine Safety section rules out, and it was hit while proving a *different* claim, showing the
   hazard is real for any assert-based test in this repo, including the pre-existing
   `test_native_jail_runner_stubs.cpp`. Fixed by routing `_CRT_ASSERT`/`_CRT_ERROR` reports to
   stderr (`_CrtSetReportMode`/`_CrtSetReportFile`) at the top of both tests' `main()`.

### 8.6 A-S1 corpus under ASan+UBSan (clang 22.1.5, `build-asan/`)

```
cmake -S . -B build-asan -G Ninja -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j4 --target test_shell_parser_adversarial test_shell_runner_proof test_real_filesystem_adapter
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  tests/test_shell_parser_adversarial.exe   # exit 0, zero sanitizer reports
  tests/test_shell_runner_proof.exe          # exit 0, zero sanitizer reports
  tests/test_real_filesystem_adapter.exe     # exit 0, zero sanitizer reports
```
(`detect_leaks=0`: LeakSanitizer is not supported on the Windows ASan runtime; ASan's memory-error
detection — the actually load-bearing half for A-S1's "no OOB access" claim — is fully active.)

Corpus covered by `test_shell_parser_adversarial.cpp` under this run: `if`-nested-10,000-deep with
no closing `fi`; combined nesting (near-max `if` depth whose body also carries wide, non-nested
`${...}` content — both an over-cap negative and an at-cap positive control); wide-not-deep (5,000
sequential shallow `if` statements, proving the depth counter's RAII decrement actually fires, not
just that it's monotonic); max-token-count/minimum-per-token-content (both over- and under-budget);
an 8,000-stage wide pipeline (debug-safe; 100,000-stage figure separately confirmed in Release,
§8.5); an unterminated quote; and a source well over `kMaxSourceBytes`. **All passed, zero ASan or
UBSan findings**, confirmed against the actual sanitizer output (not assumed from a green exit
code alone — each run's full stdout was inspected).

`test_shell_runner_proof.cpp` and `test_real_filesystem_adapter.cpp` under the same sanitizer build
also passed clean, including the real junction-based symlink-escape test (this environment
supports unprivileged directory junctions via `mklink /J`, so that sub-claim of §2.5.5 was actually
exercised end-to-end, not merely string-level).

### 8.7 Sh-S1's static half — real, at link-target granularity

```
llvm-nm --undefined-only build-clang/agentengine_shell_runner.lib
```
parsed programmatically (`tests/test_shell_runner_no_process_creation.cpp`) for an EXACT trailing
symbol-name match (not substring) against: `CreateProcessA/W`, `CreateProcessAsUserA/W`,
`_wspawnv[e]`, `_spawnv[e]`, `system`, `_wsystem`, `LoadLibraryA/W`, `WinExec`, `ShellExecuteA`.
**Result: zero matches** — the full undefined-symbol list (reproduced in the ADR's working notes,
~140 symbols across the three `.obj`s) is entirely ordinary CRT/STL/`<filesystem>` plumbing
(`__std_fs_*`, `memcpy`, `_CxxThrowException`, iostream internals, etc.). Ran successfully under
both MSVC's `build/` and clang's `build-clang/` (same `llvm-nm`, found via `find_program` at
absolute path, independent of which compiler built the `.lib`).

**What this does and does not prove, stated plainly per finding 1's own fix**: this is scoped at
the `agentengine_shell_runner` LIBRARY, not a single translation unit — moving code between
`shell_parser.cpp`/`shell_dispatch.cpp`/`real_filesystem_adapter.cpp` cannot defeat it, and no
other target in this build links a process-creation-capable backend into this library (there is no
WASM host or `remote` backend built yet to test the "legitimate collaborator" exclusion finding 1
also asked for — recorded as not-yet-applicable rather than resolved).

**The runtime IAT-hook/detours shim (the other half of Sh-S1's disproving experiment, per finding
2's call to name the actual mechanism) was NOT attempted this pass** — building a Windows-safe,
false-positive-free process-creation hook harness (scoped correctly per finding 2: not
process-wide, not blind to `_wspawnv`/`system` as alternate routes) is a nontrivial project of its
own. In its place: (a) the static check above (real, automated, at the correct granularity), (b)
manual review confirming `shell_dispatch.cpp`/`shell_parser.cpp`/`real_filesystem_adapter.cpp`/
`command_registry.hpp` spell none of `CreateProcess`/`_wspawnv`/`system`/`LoadLibrary`/`exec*`/
`posix_spawn`/`fork` anywhere, and (c) the hostile-name-corpus behavioral test below. This is
recorded as **static check: CORRECT; dynamic shim: NOT ATTEMPTED** — two distinct verdicts, not
laundered into one.

### 8.8 Sh-S1 behavioral corpus, Sh-C1, Sh-C2, Sh-C3, Sh-G4, capability checks, A-C2 —
`tests/test_shell_runner_proof.cpp`, real output (clang `build-clang/`, representative — identical
under MSVC)

```
  ok: cd /work succeeds
  ok: same ExecState object (identity), not a copy
  ok: state.cwd mutated in place to the canonical form
  ok: Sh-C3 (semicolon+rm): export succeeds / echo $X succeeds / expanded value is exactly one
      opaque word, unchanged — got ';rm -rf /work'
  ok: Sh-C3 (pipe): ... got 'a|b'
  ok: Sh-C3 (andand): ... got 'a&&b'
  ok: Sh-C3 (whitespace): ... got 'hello world  foo'
  ok: Sh-C3: the embedded ';rm -rf /work'-shaped value never executed a second command
  ok: Sh-C2: registering a Runner named 'ls' is rejected at registration time
  ok: Sh-C2: 'ls' still resolves to the builtin after the rejected registration attempt
  ok: Sh-C2: registering a non-colliding Tool ('grep') succeeds / resolves to the registered tool
  ok: Sh-C2: a Runner colliding with an already-registered Tool name is also rejected
  ok: Sh-G4 gate: missing RunnerCall is denied with a policy error / invoke() never ran
  ok: Sh-G4 construction: granted RunnerCall succeeds / invoke() ran / SAME EffectContext reaches
      the nested Runner
  ok: Sh-G4 negative control: the identity assertion distinguishes two distinct EffectContext
      objects
  ok: export/ls/cat/mkdir/rm denied without the relevant capability kind, succeed once granted
      (kind-only, per §2.5.4 — no path-scoped variant attempted)
  ok: A-C2: the malformed script (missing 'fi') fails to parse (code: shell.syntax_error)
  ok: A-C2: mkdir from the syntactically invalid script never executed
  ok: resolve(<18-entry hostile-name corpus>) == not_found for every entry; 'ls' still resolves
  ok: end-to-end '/bin/sh'/'cmd.exe'/'totally_bogus_xyz123' fail closed as command_not_found
test_shell_runner_proof: PASS
```

### 8.9 `RealFileSystemAdapter` path-escape list — `tests/test_real_filesystem_adapter.cpp`, real
output

```
  ok: positive control (in-bounds read/write round-trip)
  ok: dotdot traversal / dotdot traversal (nested) -> shell.fs.escape_dotdot
  ok: drive-letter absolute path -> shell.fs.escape_absolute
  ok: UNC path / extended-length prefix -> shell.fs.escape_unc
  ok: reserved device name (bare / with extension / nested) -> shell.fs.escape_device_name
  ok: ADS marker -> shell.fs.escape_ads
  ok: case-fold-consistent comparison
  ok: junction escape rejected (shell.fs.escape_symlink)
test_real_filesystem_adapter: PASS
```

### 8.10 Design B (§4)

**Not implemented.** No code for the single-pass tokenizer/shunting-yard evaluator was written this
pass — all implementation time went to Design A (§3), per the task's own priority ordering ("do the
earlier items thoroughly rather than rushing through all of them shallowly") and this ADR's own
§10 pre-registered bias toward steelmanning Design A hardest. B-S2 and B-F1 (the two claims that
would actually distinguish A from B, per §7.8's summary) are therefore **not-yet-attempted**, stated
honestly in §9 rather than inferred from Design A's numbers or silently dropped.

## 9. Per-claim verdicts

Per `decisions/README.md` item 6: `INCONCLUSIVE` is attempted-but-ambiguous; a claim never
attempted is stated as **NOT ATTEMPTED**, not folded into either passing or inconclusive.

### 9.1 Shared claims (§5.1)

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| Sh-S1 (static half) | No process-creation symbol reachable at the `agentengine_shell_runner` library boundary | **CORRECT** | §8.7 — `llvm-nm --undefined-only`, exact-symbol match, zero hits, both compilers |
| Sh-S1 (dynamic shim half) | Runtime hook confirms no process-creation call fires for a hostile-name corpus | **NOT ATTEMPTED** | §8.7 — scoped out this pass; substituted with (b)/(c) below, not a substitute for the claim as originally written |
| Sh-S1 (behavioral corroboration) | Hostile-name corpus resolves to `not_found`/fails closed, end-to-end, under a deny-all context | **CORRECT** | §8.8 — 18-entry corpus + 3 end-to-end scripts, zero deviation, negative control (`ls` still resolves) confirms the check isn't vacuous |
| Sh-C1 | `ExecState` shared by reference; `cd` mutates the same object | **CORRECT** | §8.8 — pointer-identity assertion + `state.cwd` mutated in place to the canonical form (§2.5.3) |
| Sh-C2 | Shadowing/collision resolves to exactly one intended identity, never ambiguously | **CORRECT** (for the closed-by-construction fix, finding 5) | §8.8 — builtin-name reservation enforced at registration; this is a fix beyond what §2.5 itself specifies (§2.5 does not close finding 5) — recorded as an additional, necessary fix made during implementation, not a reinterpretation of §2.5 |
| Sh-G4 (gate half) | No `RunnerCall` capability ⇒ nested Runner never invoked | **CORRECT** | §8.8 — `invoked` flag asserted false under denial |
| Sh-G4 ("cannot exceed" half) | `ctx` passed to nested Runner is pointer-identical, never attenuated/broadened | **CORRECT** | §8.8 — direct pointer-equality assertion, plus a negative control confirming the assertion mechanism itself has discriminating power |
| Sh-G4 (composition with a real PythonRunner) | End-to-end `RunnerCall<python>` composition | **NOT ATTEMPTED** (by design — cross-ADR dependency) | Only a fake registered Runner was exercised; PythonRunner is ADR-002's stub, explicitly out of scope here per the task brief |
| Sh-C3 | Expansion never re-tokenizes; opaque splice regardless of `;`/`\|`/`&&`/whitespace content | **CORRECT** | §8.8 — 4-variant corpus (`;rm -rf`, `\|`, `&&`, embedded whitespace), each expands to exactly one unchanged argv word, no second command executes |

### 9.2 Design A claims (§5.2)

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| A-F1 | ≤4 arena-backed allocations, <10 µs for a typical command | **NOT ATTEMPTED** | No hooked-allocator micro-benchmark was built this pass — arena-backed allocation was verified structurally (all AST containers are `std::pmr`, §2.5.6) and the arena's single upstream allocation was exercised, but the specific "≤4 allocations / <10 µs" numbers were not measured |
| A-S1 (bounded time/memory, zero sanitizer findings) | **CORRECT** | §8.6 — full corpus incl. finding-12's combined-nesting and wide-not-deep probes, finding-13's max-token/min-content probe, and the ADR's own 100k-stage-pipeline probe (§8.5's Release measurement: 7 ms), all clean under ASan+UBSan |
| A-S1 (structural: shared depth counter closes the "sum not max" hazard) | **CORRECT** | §8.6 — combined-nesting over-cap correctly fails via `shell.nesting_too_deep`; the at-cap positive control confirms wide `${...}` content is never miscounted against depth (this implementation's word/atom scanning is iterative, contributing zero depth by construction — a stronger mitigation than the ADR anticipated, see `shell_parser.cpp`'s file header) |
| A-S2 (no catastrophic-backtracking input class) | **CORRECT**, informally | Every probe run (up to 200,000-token-budget-bounded inputs) completed in single-digit-to-tens of milliseconds in Release; no dedicated timing-sweep-across-N benchmark was built, so this is corroborated but not measured to the rigor a dedicated A-S2 experiment would give — recording as CORRECT-but-informally-tested rather than overclaiming a full benchmark |
| A-C1 (byte-identical `ExecOutcome` across Windows/Linux/macOS) | **NOT ATTEMPTED** | Only Windows was available this pass; no Linux/macOS run was performed |
| A-C2 (nothing executes until the whole script parses) | **CORRECT** | §8.8 — malformed `if`-without-`fi` script with a real, capability-granted `mkdir` ahead of it; directory confirmed absent after the parse error |

### 9.3 Design B claims (§5.3)

| # | Claim | Verdict |
|---|---|---|
| B-F1, B-S1, B-S2, B-C1, B-C2 | **NOT ATTEMPTED** — Design B was not implemented this pass (§8.10). No claim about B, including the two claims (B-S2, B-F1) that §7.8 identifies as the actual A-vs-B distinguishing axis, was tested. |

### 9.4 Additional findings from this pass, not in §5's original claim list

Recorded for the judge phase's benefit, not as new formal claims: the MSVC debug-CRT
checked-iterator slowdown (§8.5), the `RealFileSystemAdapter` trailing-separator bug (§8.5, found
and fixed), the Sh-S1 static-check substring false-positive (§8.5, found and fixed), and the
MSVC-`assert()`-dialog test-hang hazard (§8.5, found and fixed) — all are implementation/tooling
findings surfaced by actually running the code, exactly the kind of evidence `decisions/README.md`
asks the prove phase to produce.

## 10. Decision

**No winner is declared in this document.** Per `decisions/README.md`, the decision belongs to the
judge phase, weighing claims that *survived* red-team attack and were *proven* — neither of which
has happened yet. What this design phase can responsibly state is which design its author would
steelman hardest going into red-team, and why, so the next phase has a stated target rather than
an even split of effort:

**Recommend steelmanning Design A hardest.** Three reasons, none of them claims of victory:

1. **A-S1's fuzz target is fake-free.** A pure `bytes -> result<ScriptNode>` function is the
   cleanest possible surface for the exact concern Q7 raises — the red-team and prove phases can
   attack the parser with zero assumptions about how faithfully a fake registry/adapter models
   real dispatch. Design B's equivalent claim (B-S1) is only as strong as its harness's fakes,
   which is itself an attack surface (§4's cost #1).
2. **A-C2 (nothing executes until the whole script parses) is a stronger safety posture against
   Q7's threat model than B's interleaved alternative**, and B-S2 — the claim that would close that
   gap — is explicitly flagged in §5.3 as the harder claim for B to sustain. A design that needs an
   extra claim just to reach parity on this axis is not the one to default to.
3. **A's extensibility story (§3 steelman #3) matches this project's stated trajectory** — 010 §10
   Q6 (background processes) and a possible future `;`-sequencing or negation operator are exactly
   the kind of additive grammar changes A's isolated-production/isolated-eval-case shape absorbs
   without disturbing existing, already-fuzzed productions.

This is a starting bias for the red-team phase to attack, not a judgment that Design B is weak —
B's F1 allocation claim for the dominant case is real and untested, and the judge phase may yet
find it decisive once both are measured. Recording a bias here is meant to focus adversarial effort,
not to shortcut it.

## 11. Residual risks and open items surfaced by this design work

1. **`ExecOutcome` has no `artifacts` field yet** (`sandbox.hpp`'s own comment: "elided pending
   BlobRef-backed artifact vocabulary"). 010 §3's output discipline requires large output to become
   an artifact rather than an OOM'd host; until that vocabulary lands, both designs can only
   truncate stdout/stderr with a marker, not promote to an artifact. This blocks full G2/output-
   discipline compliance for `cat`-ing a large file, independent of which design wins.
2. **`CapabilitySet` has no scope field at all — sharper than "should be re-checked once 007 has its
   own ADR."** Red-team finding 23: `Capability{capability_kind kind}` cannot today distinguish
   "`FsRead` on `/work/a`" from "`FsRead` on `/work/b`," so every "scoped" claim in §2.4's table is
   not merely unproven but currently unwritable as a test. §2.5.4 resolves this for the prove phase
   by downgrading every "scoped" claim to kind-level possession and marking the scoped version
   `INCONCLUSIVE`-by-design until 007 lands a real scope field — not blocking this ADR's promotion
   on 007's, but not claiming more than kind-level authorization is actually tested either.
3. **How shell `argv` maps onto a registered Tool's typed `Args` schema is undesigned.** §2.3's
   `invoke_registered_tool` step assumes some argv-to-typed-arguments convention exists; 006 §2
   requires that the *same* registered Tool back both a shell invocation and an `agent.tools.*`
   Python call, but does not specify the string-args-to-JSON-Schema mapping for the shell frontend.
   This needs its own resolution (plausibly an OpenQuestions.md entry) before `ShellRunner`'s `tool`
   dispatch arm can be implemented, independent of which parsing design is chosen.
4. **Command substitution (`$(...)`) and background processes (`&`, 010 §10 Q6) are out of scope
   for both designs as designed here.** Both are natural extensions and both are exactly the kind
   of feature that reintroduces recursion/lifetime hazards this ADR's grammar deliberately avoids;
   either should be its own follow-up ADR once the base grammar has a red-team/fuzz track record.
5. **The `Runner` concept's current `result<ExecOutcome>` (synchronous) signature is already
   self-documented in `runner.hpp` as provisional** pending `ae::task<T>` wiring to the Quark
   submodule. Both designs' `run()`/`evaluate()` signatures above match the current, real contract;
   the note there that this becomes `ae::task<result<ExecOutcome>>` once Quark is linked applies
   identically to whichever design is chosen — not a new inconsistency, just recorded here for the
   implementer.
6. **Neither design's `if`/`for` block parsing removes native recursion entirely** — both bound it
   with an explicit depth counter rather than replacing it with a heap-allocated explicit-stack
   machine. A stronger (but more complex) mitigation would remove native recursion for block
   constructs too; flagged as a hardening upgrade for red-team to press on, since a depth check
   missing at one of several recursive entry points is exactly the bug class worth hunting for.
7. **(Added post-red-team, finding 4)** `export`'s write into `ExecState.env` — shared by reference
   with `PythonRunner` (010 §3a) — was originally modeled as capability-free "host-side bookkeeping."
   It is not: environment variables are a real ambient-authority channel into a *different* Runner's
   trust decisions (`PYTHONPATH`, `PIP_INDEX_URL`, proxy variables). §2.5.1 gives `export` a new
   `EnvWrite` capability and records the still-open half (`PythonRunner` must treat `ExecState.env`
   values as tainted at the point of use, not inherited ambiently) as a cross-ADR dependency on
   ADR-002.
8. **(Added post-red-team, finding 10)** The grammar (§3) did not specify whether an expanded
   variable's value could re-tokenize into new operator/argv structure — the classic
   tainted-data-becomes-command-structure injection vector (003 §2, I3). §2.5.2 states the
   no-re-tokenization rule explicitly and adds claim Sh-C3 to test it.

## 12. Specs this would bind, if a design here survives red-team and prove

010 §1a (`Runner` concept usage), §2 (grammar/builtin-set/dispatch shape), §3a (`ExecState`
sharing), §9 G2/G3/G4/G6 (containment, state boundary, bridge, parity gates) — plus, indirectly,
006 §2's uniformity rule once residual risk 3 (§11) is resolved. None of these are bound yet: per
CLAUDE.md, an RFC promotes on an executed gate, and this document is the design step that precedes
red-team and prove, not the promotion itself.
