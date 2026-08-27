#pragma once
// A10 finding 4 (task_branch_sandbox.hpp's own header comment), closed at the design level:
// "No capability-declaration design exists for who may call start_task_branch/commit_task_branch/
// discard_task_branch at all, unlike RunShellTool's real Capabilities<cap::decl::FsRead<"work">,
// ...> precedent."
//
// Mirrors the REAL, production `cap::decl::*` shape (include/agentengine/trust/capability.hpp)
// exactly -- a compile-time declaration tag distinct from a runtime capability instance, converted
// via a `to_capability()` overload, meant for a future `Tool<>`'s `Capabilities<...>` template
// parameter -- but lives here, in prove-phase code, deliberately: this design has never been wired
// into `include/agentengine/` (ADR-099's own "no production code has been written or merged from
// this design" statement, confirmed explicitly to the user mid-session), and extending the REAL
// `Capability` variant/`capability_kind` enum/`subsumes_payload()` exhaustive switches for a feature
// with no real caller yet would be exactly the premature-production-surgery this whole design track
// has consistently declined to do. If/when a real `TaskBranchStartTool`/etc. gets built, promoting
// these two tags into `agentengine::cap::decl` verbatim (plus the matching `capability_kind`
// enumerators and switch-arm additions every other real `cap::X` already required) is the expected
// path -- not a redesign.
//
// TWO tags, not one -- a real, reasoned split, not the simplest possible shape (confirmed with the
// project owner before building this): start_task_branch/run_in_task_branch/discard_task_branch stay
// fully isolated on a child branch and NEVER touch the session's own main line; commit_task_branch
// merges real work INTO main -- a meaningfully more consequential authority, the same
// read-vs-write distinction `cap::FsRead`/`cap::FsWrite` already draw on the identical mount. A host
// can grant "may try things in isolation" without also granting "may merge into main."
//
// PRECISION (correctness red-team, 2026-08-27): the FsRead/FsWrite analogy is real but imperfect,
// worth stating exactly rather than left implicit. FsRead and FsWrite are each independently
// meaningful on their own (write-only is unusual but coherent); `TaskBranchCommit` alone is NEVER
// independently meaningful -- `commit_task_branch()` can only ever operate on a handle that
// `start_task_branch()` (itself gated by `TaskBranch`) produced. That dependency is real, but it is
// enforced entirely by the TOOL's own declared ceiling (a real `TaskBranchCommitTool` must declare
// BOTH tags together, per this file's own usage sketch below -- `tool_pipeline.hpp`'s real step 4/7
// loop requires every declared capability to bind, so an agent lacking `TaskBranch` is rejected
// regardless of holding `TaskBranchCommit` alone), never by any relationship between the two grants
// themselves. A host that somehow granted `TaskBranchCommit` without `TaskBranch` would simply find
// no real tool ever accepts that combination -- an inert grant, not an exploitable gap.
//
// What this declaration is NOT: a live concurrent-branch-count limiter. `cap::decl::Background
// <MaxConcurrent>` (the closest real precedent for a NUMERIC capability parameter) is checked by a
// SPECIAL-CASED step inside `tool_pipeline.hpp::invoke_tool()` (its own G9 comment: "the CALLER's own
// Background<max_concurrent> ceiling, checked against a LIVE count"), not the ordinary
// `capability_ceiling`/`held.bind()` membership check every other capability gets. Adding an
// equivalent live-count special case for TaskBranch would mean touching that same real, shipped
// production pipeline file for a feature with no real caller yet -- exactly what this design track
// declines to do. The REAL, already-proven rate/resource limiting for task branches already lives
// correctly in `TaskBranchSandbox`'s own constructor-injected `AsyncQuota<BranchCost>`/
// `AsyncQuota<RunCost>`/`AsyncQuota<StorageBytes>` references (§39/§40) -- these two tags are a pure
// membership/audit gate ("is this agent even allowed to have task-branch tooling, and to commit with
// it, at all"), deliberately not duplicating what the quota objects already do correctly.

#include "agentengine/core/policy_tags.hpp"

namespace probe {

// Runtime capability instances -- both markers, no fields, matching the real system's own
// `cap::Entropy`/`cap::Elicit`/`cap::NetListen` precedent for a capability whose entire authority is
// "may this be invoked at all," with no further parameter to narrow.
namespace cap {
struct TaskBranch {};
struct TaskBranchCommit {};
}  // namespace cap

// Compile-time declaration tags -- mirrors `agentengine::cap::decl`'s own namespace nesting
// (`cap::decl`, not a flat name) for the identical reason the real one is nested: keeps this
// self-documenting as "the decl-tag sibling of a runtime cap:: type," not a a free-standing,
// easily-confused name. Parameterless, exactly like the runtime types above -- no `fixed_string`
// non-type template parameter is needed (unlike `cap::decl::FsRead<Mount>`), since there is nothing
// here to parameterize.
namespace cap::decl {
struct TaskBranch {};
struct TaskBranchCommit {};
}  // namespace cap::decl

// One `to_capability()` overload per tag, matching the real system's own bridge-function shape and
// naming exactly (`agentengine::to_capability(cap::decl::FsRead<Mount> const&)` etc.) -- this is the
// function a future real `register_agent<A>()`-equivalent would call to compute an agent's actual
// capability ceiling from its declared tags, the identical role the real overloads already play.
[[nodiscard]] inline cap::TaskBranch to_capability(cap::decl::TaskBranch const&) {
    return cap::TaskBranch{};
}
[[nodiscard]] inline cap::TaskBranchCommit to_capability(cap::decl::TaskBranchCommit const&) {
    return cap::TaskBranchCommit{};
}

// Real, checked proof (not merely asserted) against the REAL production template, not a mirrored
// stand-in: `agentengine::Capabilities<class... Cs>` (core/policy_tags.hpp) is genuinely
// unconstrained -- any types compile as its arguments -- so these two prove-phase-only tags DO
// compile as ordinary type arguments to the real container a Tool<>'s Policies pack ultimately uses.
using TaskBranchToolCapabilities = agentengine::Capabilities<cap::decl::TaskBranch>;
using TaskBranchCommitToolCapabilities =
    agentengine::Capabilities<cap::decl::TaskBranch, cap::decl::TaskBranchCommit>;

// REAL FINDING, found by actually trying it rather than assuming the claim above generalizes: a real
// `agentengine::Tool<Derived, agentengine::Capabilities<cap::decl::TaskBranch>>` subclass calling its
// own `declared_capabilities()` does NOT compile with these tags as-is. Confirmed by a real, isolated
// compile attempt (a throwaway `FakeTaskBranchStartTool`), not reasoned from the header alone. Two
// separate, precise reasons, both real:
//
//   1. `tool_detail::policy_capabilities<Capabilities<Cs...>>::get()` (tool.hpp) calls the
//      unqualified name `to_capability(Cs{})` from CODE PHYSICALLY DEFINED INSIDE `namespace
//      agentengine` -- the real `agentengine::to_capability()` overloads are found there by ORDINARY
//      (non-ADL) nested-namespace lookup, not by ADL at all. This file's own `to_capability()`
//      overloads live in `namespace probe` (one level above `probe::cap::decl`, mirroring the real
//      system's OWN nesting exactly) -- but `policy_capabilities::get()` is not, and never will be
//      without editing it, inside `namespace probe`, so ordinary lookup never sees them; and ADL
//      (searching the ARGUMENT type's own namespace, `probe::cap::decl`) doesn't find them either,
//      since they are declared one level up from there, not inside it.
//   2. Even fixing (1) would not be enough: `declared_capabilities()` requires a
//      `std::vector<agentengine::Capability>`, and `agentengine::Capability` is a CLOSED
//      `std::variant<cap::FsRead, cap::FsWrite, ...>` with no `TaskBranch`/`TaskBranchCommit`
//      alternative. This file's own `to_capability()` overloads correctly return
//      `probe::cap::TaskBranch`/`probe::cap::TaskBranchCommit` -- genuinely different, unrelated
//      types, not convertible into the real variant, and there is no way to make them convertible
//      without adding real alternatives to that real, production, closed variant.
//
// CORRECTION (correctness red-team, 2026-08-27): these two reasons are NOT equally unavoidable, and
// an earlier version of this comment overstated reason 1's inevitability. Reason 1 (the ADL/namespace
// gap) is real but self-inflicted by this file's own choice to mirror the real system's exact nesting
// (`to_capability()` one level above the tag's own namespace) -- moving `to_capability()` INTO
// `probe::cap::decl` itself (confirmed by a separate, isolated test during red-team) makes ADL find
// it immediately, entirely within prove-phase code, no promotion needed. It was left un-"fixed" here
// specifically to keep the exact structural mirror of the real system byte-for-byte, so the eventual
// promotion diff is as small as possible -- a deliberate tradeoff, not an unavoidable limitation.
// **Reason 2 (the closed `Capability` variant, confirmed directly: `capability.hpp`'s variant has 19
// alternatives, none named `TaskBranch`) is the genuinely unavoidable one** -- no amount of
// rearranging this file's own namespaces closes it; only editing the real, production, closed variant
// would, which is exactly the premature-production-surgery this design track declines to do before a
// real caller exists. Promoting these tags into `agentengine::cap::decl` for real -- verbatim, this
// file's own header comment already says so -- inherits a real `to_capability()` living in the right
// namespace for free (reason 1 disappears by construction) and requires the SAME `Capability`
// variant/`capability_kind` enum/exhaustive-switch extension every other real `cap::X` addition
// already needed (reason 2 is not special to TaskBranch, it is the normal, accepted cost of adding
// ANY new capability kind to the real system) -- named here precisely so that cost is not a surprise
// at promotion time, not something this design pretends to have already paid.
//
// What a real Tool<> binding would look like once promoted, mirroring RunShellTool's real, shipped
// shape (session_shell_wiring.hpp) exactly:
//
//   struct TaskBranchStartTool   : Tool<TaskBranchStartTool,   Capabilities<cap::decl::TaskBranch>,   ...> { ... };
//   struct TaskBranchRunTool     : Tool<TaskBranchRunTool,     Capabilities<cap::decl::TaskBranch>,   ...> { ... };
//   struct TaskBranchDiscardTool : Tool<TaskBranchDiscardTool, Capabilities<cap::decl::TaskBranch>,   ...> { ... };
//   struct TaskBranchCommitTool  : Tool<TaskBranchCommitTool,  Capabilities<cap::decl::TaskBranch,
//                                                                             cap::decl::TaskBranchCommit>, ...> { ... };
//
// -- four separate tools (matching this file's own header comment on why start/run/commit/discard
// are each their own Tool<>, not one multi-verb tool), the commit tool alone carrying BOTH tags. Once
// promoted, the ORDINARY `capability_ceiling`/`held.bind()` loop (tool_pipeline.hpp's own step 4/7,
// the same path FsRead/FsWrite/every other non-Background capability already goes through) is
// sufficient -- no new pipeline step, no special case, unlike Background<MaxConcurrent>'s own
// live-count check.

}  // namespace probe
