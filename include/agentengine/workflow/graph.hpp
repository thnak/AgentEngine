#pragma once
// Implements 014-Workflow-and-Orchestration.md §1 (the model) and §2's termination requirement.
// Milestone 6 Phase A (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// This header is THE GRAPH AS DATA, and nothing else -- no execution, no actors, no scheduling.
// That separation is what 014 §7 ("the graph is data: it renders, it validates, it diffs") asks for,
// and it is also what makes decision 6 of the breakdown achievable: `validate_workflow()` below is a
// PLAIN FUNCTION over a plain description, so the declarative loader (015, Milestone 7) calls the
// SAME validator rather than reimplementing it. I6's "declarative and native surfaces are
// equivalent" is a property of sharing this code; two validators that agree today drift tomorrow.
//
// Two layers, deliberately:
//   (1) `Workflow`/`Executor`/`Edge` -- the runtime description, string-typed ports, validated by
//       `validate_workflow()`. Both surfaces produce this.
//   (2) `WorkflowBuilder` -- the C++ authoring form, which carries REAL C++ types and therefore
//       fails a mismatched edge at COMPILE time (014 §1's own words: "at compile time for the C++
//       form, at load for the declarative form"). It emits layer (1), so the C++ form is also
//       checked by the shared validator -- the compile-time check is strictly additional, never a
//       substitute that would let the two surfaces diverge.
//
// Cycles are ALLOWED and deliberately not checked for. 014 §9 Q2 resolved this: type-checking is
// local and pairwise, so a loop-closing edge validates exactly like any other, and §2's round
// counter is a whole-workflow clock, so `MaxRounds` transitively bounds every cycle's iteration
// count by construction. An acyclicity check here would reject the reflection/critic and group-chat
// patterns 014 §3 requires.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/sharing_mode.hpp"

namespace agentengine::workflow {

// -- Message type identity (014 §1) -----------------------------------------------------------
//
// An executor is "typed by its input and output message types". For the C++ form that could just be
// the C++ type -- but the validator must ALSO serve a YAML loader that has never heard of a C++
// type, so port identity has to be something both surfaces can produce. It is a declared string.
//
// Deliberately NOT `quark::detail::canonical_type_name<T>()`, which is real and constexpr but is
// compiler-derived (string surgery over `__FUNCSIG__`/`__PRETTY_FUNCTION__`): its spelling can
// differ between MSVC and GCC for the same type, which is exactly the property a name shared with a
// declarative file must not have. An explicitly declared name is stable by construction, and follows
// the precedent this codebase already sets by requiring `QUARK_SERIALIZE`/`Described` for durable
// types rather than deriving them.
using MessageTypeId = std::string;

// Authors declare a message type's portable name by specializing this, via the macro below. A type
// with no declaration is a COMPILE ERROR at the point an edge referencing it is built, naming what
// to add -- never a silent fallback to a compiler-derived spelling that would work on one toolchain.
template <class T>
struct message_type;  // ae-naming-lint: allow message_type — the trait behind 027 §4's `MessageTypeId`; deliberately lowercase as a trait, not a vocabulary type

// Declares `Type`'s portable, surface-independent message-type name. Must be at namespace scope.
#define AE_WORKFLOW_MESSAGE(Type, Name)                                     \
    template <>                                                             \
    struct agentengine::workflow::message_type<Type> {                      \
        static constexpr std::string_view name = Name;                      \
    }

// True iff `T` has a declared portable name.
template <class T>
concept DeclaredMessage = requires { message_type<T>::name; };

template <DeclaredMessage T>
[[nodiscard]] inline MessageTypeId message_type_id_of() {
    return MessageTypeId{message_type<T>::name};
}

// -- The two closed enumerations of 014 §1 -----------------------------------------------------

// `Edge = direct | fan-out | fan-in | switch/case | multi-selection | chain` (014 §1), verbatim.
enum class edge_kind {
    direct,           // one source, one target
    fan_out,          // one source, many targets, all fired
    fan_in,           // many sources, one target (the aggregator)
    switch_case,      // one source, many targets, exactly one selected by a case label
    multi_selection,  // one source, many targets, a caller-chosen SUBSET fired
    chain,            // sugar for a run of `direct` edges; kept distinct because §3's Sequential
                      // pattern names it, and a rendered graph (§7) should say what was authored
};

// `Executor = an agent | a function | a sub-workflow | a request port` (014 §1), verbatim.
enum class executor_kind { agent, function, sub_workflow, request_port };

// -- The description (014 §1's `Workflow = { executors[], edges[], start, output_selection, ... }`)

struct Executor {
    std::string   id;  // unique within one Workflow; what edges reference and §7 renders
    executor_kind kind = executor_kind::function;
    MessageTypeId input_type;
    MessageTypeId output_type;
    // 014§1's named gap ("nothing states whether a workflow executor... gets its own sub-worktree,
    // inherits the caller's, or shares one") -- closed by `workflow/worktree_scoping.hpp` (ADR-032).
    // Default is `branch`, UNCONDITIONALLY, not 025 §3's "sequential defaults to shared" rule
    // applied per-node: `WorkflowSupervisor::execute()`'s superstep model means any two executors
    // reachable in the same round get concurrent asks (not only explicit `fan_out` edges -- two
    // ordinary `direct` edges out of one source behave identically), and general graphs here have
    // cycles and dynamic `switch_case` routing (§9 Q2), so statically proving two specific nodes can
    // never co-occur in a round is not attempted -- it would need a full round-reachability analysis
    // that cycles and dynamic routing make unsound to do cheaply. Getting "these are safely
    // sequential, default to shared" WRONG is a real cross-executor data-visibility hazard (025 §4);
    // defaulting to `branch` when `shared` would have been fine only costs an unnecessary (cheap,
    // 025 §3's own table) merge. An author who knows a specific node is safe to share sets this
    // explicitly -- mirroring 025 §3's own already-accepted precedent that `shared` for concurrent
    // siblings "is never reached by a plain default."
    sharing_mode  worktree_mode = sharing_mode::branch;

    friend bool operator==(Executor const&, Executor const&) = default;
};

// -- Failure policy (014 §6) -------------------------------------------------------------------
//
// "An executor failure is classified (001 §6) and handled by the edge's declared policy: propagate,
// retry, route to a fallback branch, or fail the workflow." Four alternatives, so this is an enum
// and not a set of composable flags. A retry that then falls back would be a fifth option the RFC
// does not list; if a real graph needs it, that is an amendment to §6, not a quiet extension here.
enum class edge_failure_policy {
    fail,       // stop the workflow, preserving partial results (§6's third bullet). The DEFAULT --
                // see the note on `Edge::on_failure` for why the default is the strict one
    propagate,  // deliver a failure marker along this edge; the TARGET decides what to do. This is
                // how an error-handling node is authored -- the workflow keeps running
    retry,      // re-invoke the failed executor, bounded, within the same round. On exhaustion this
                // becomes `fail` (see `is_retryable` in supervisor.hpp for which classes retry AT
                // ALL -- a `contract` or `policy` failure is not retried even once)
    fallback,   // route a failure marker to a NAMED recovery executor instead of this edge's target
};

// The policy as declared on one edge. `attempts` and `fallback` are each meaningful for exactly one
// kind, and the validator rejects them elsewhere rather than ignoring them at runtime -- the same
// discipline `case_label` already gets.
struct EdgeFailurePolicy {
    edge_failure_policy kind = edge_failure_policy::fail;
    // `retry` only: ADDITIONAL invocations after the first. `retry` with 0 would be a no-op spelled
    // as a policy, which is a mis-authored graph, so the validator requires >= 1.
    std::uint32_t attempts = 0;
    // `fallback` only: the recovery executor's id. Type-checked against the FAILED executor's output
    // port exactly like any other edge out of it, so a fallback branch is drawable (§7) and cannot
    // be the one edge in a graph that skips 014 §1's type rule.
    std::string fallback;

    friend bool operator==(EdgeFailurePolicy const&, EdgeFailurePolicy const&) = default;
};

struct Edge {
    std::string from;
    std::string to;
    edge_kind   kind = edge_kind::direct;
    // switch_case and multi_selection only. A `switch_case` edge without one is unroutable; any
    // other kind carrying one is a mis-authored graph, and both are validation failures rather than
    // fields quietly ignored at runtime.
    std::string case_label;
    // 014 §6. DECLARED ON THE EDGE because §6 says "the edge's declared policy" -- but the thing it
    // decides (what happens when the SOURCE executor fails) is a property of the source, so the
    // validator requires every edge out of one executor to declare the SAME kind and the same retry
    // budget (`workflow.conflicting_failure_policy`). Only the fallback TARGET may differ, which is
    // a fan-out of recovery and reads as one. Resolving a disagreement by precedence instead --
    // "most severe wins", "first declared wins" -- would be a rule no reader of the graph could
    // predict, on the code path that runs when something has already gone wrong.
    //
    // The default is `fail`, deliberately the strict one: a graph whose author has not thought about
    // failure stops and says so rather than continuing with a hole in its data.
    EdgeFailurePolicy on_failure;

    friend bool operator==(Edge const&, Edge const&) = default;
};

// 014 §2: "Termination is by output selection, by an explicit terminal executor, or by bound
// (`MaxRounds`, deadline, budget). An unbounded workflow does not run -- the bound is required."
//
// A struct rather than a policy tag because that last sentence makes it mandatory: this is a field a
// `Workflow` cannot omit, not a refinement some workflows opt into. `validate_workflow` enforces it.
struct TerminationBound {
    std::optional<std::uint32_t> max_rounds;
    std::optional<std::uint64_t> deadline_ms;
    std::optional<std::uint64_t> token_budget;

    [[nodiscard]] bool any() const noexcept {
        return max_rounds.has_value() || deadline_ms.has_value() || token_budget.has_value();
    }

    friend bool operator==(TerminationBound const&, TerminationBound const&) = default;
};

struct Workflow {
    std::string           id;
    // Gap-2 fix (2026-08-14, decisions/ADR-044-*.md): 015 §3's own document shape already carries
    // `metadata.version`/`metadata.description` (agent_yaml_compiler.hpp's sibling compiler for
    // Agent documents found the identical gap first) -- this struct simply had no slot for either.
    // `description` defaults empty (matching `id`'s own "not declared" convention); `version` is
    // `optional` since "undeclared" and "declared empty" are different states.
    std::string               description;
    std::optional<std::string> version;
    std::vector<Executor> executors;
    std::vector<Edge>     edges;
    std::string           start;
    // 014 §1's `output_selection`: which executors' outputs constitute the workflow's result. Empty
    // is legal -- a workflow may instead terminate at an explicit terminal executor (§2) -- so this
    // is not itself the required bound.
    std::vector<std::string> output_selection;
    TerminationBound         bound;

    [[nodiscard]] Executor const* find(std::string_view executor_id) const noexcept {
        for (auto const& e : executors) {
            if (e.id == executor_id) return &e;
        }
        return nullptr;
    }

    friend bool operator==(Workflow const&, Workflow const&) = default;
};

// -- The shared validator (014 §1, breakdown decision 6) ---------------------------------------
//
// Plain function over the plain description: no templates, no C++ type knowledge, so the 015 loader
// (Milestone 7) calls exactly this. Every failure is `failure_class::contract` -- a graph that does
// not validate is an authoring mistake, never a transient condition.
[[nodiscard]] inline result<void> validate_workflow(Workflow const& wf) {
    auto fail = [](std::string message, std::string code) -> result<void> {
        return std::unexpected(error{failure_class::contract, std::move(message), std::move(code)});
    };

    if (wf.id.empty()) return fail("workflow has no id", "workflow.no_id");

    if (wf.executors.empty()) {
        return fail("workflow declares no executors", "workflow.no_executors");
    }

    // Unique ids. Checked before anything that resolves an id, so a duplicate is reported as a
    // duplicate rather than as whichever downstream check happened to trip over it first.
    for (std::size_t i = 0; i < wf.executors.size(); ++i) {
        if (wf.executors[i].id.empty()) return fail("an executor has an empty id", "workflow.empty_executor_id");
        // `worktree_scoping.hpp` (ADR-032) splices an executor id directly into a worktree ref name
        // and a guest-facing mount id ("/agents/" + id). A '/' would let one id inject a bogus path
        // segment into that splice -- 025 §5's "path escape is a security bug, not a bug" applies to
        // the id namespace exactly as much as to a guest-supplied path, so it is rejected here,
        // structurally, rather than left as something only the worktree-scoping layer defends
        // against (defense at the one place every consumer -- including a future declarative loader
        // that never touches worktree_scoping.hpp at all -- is guaranteed to pass through).
        if (wf.executors[i].id.find('/') != std::string::npos) {
            return fail("executor id '" + wf.executors[i].id + "' must not contain '/'",
                        "workflow.executor_id_contains_slash");
        }
        for (std::size_t j = i + 1; j < wf.executors.size(); ++j) {
            if (wf.executors[i].id == wf.executors[j].id) {
                return fail("duplicate executor id: " + wf.executors[i].id, "workflow.duplicate_executor_id");
            }
        }
    }

    // Every port must be typed. An untyped port would make the type check below vacuous -- the
    // failure mode where a validator "passes" because it had nothing to compare.
    for (auto const& e : wf.executors) {
        if (e.input_type.empty() || e.output_type.empty()) {
            return fail("executor '" + e.id + "' has an untyped input or output port",
                        "workflow.untyped_port");
        }
    }

    if (wf.start.empty()) return fail("workflow has no start executor", "workflow.no_start");
    if (wf.find(wf.start) == nullptr) {
        return fail("start executor '" + wf.start + "' is not declared", "workflow.unknown_start");
    }

    for (auto const& sel : wf.output_selection) {
        if (wf.find(sel) == nullptr) {
            return fail("output_selection names undeclared executor '" + sel + "'",
                        "workflow.unknown_output_selection");
        }
    }

    for (auto const& edge : wf.edges) {
        Executor const* from = wf.find(edge.from);
        Executor const* to   = wf.find(edge.to);
        if (from == nullptr) {
            return fail("edge references undeclared source executor '" + edge.from + "'",
                        "workflow.unknown_edge_endpoint");
        }
        if (to == nullptr) {
            return fail("edge references undeclared target executor '" + edge.to + "'",
                        "workflow.unknown_edge_endpoint");
        }

        // 014 §1: "An edge that connects incompatible types fails to build." Local and pairwise --
        // which is precisely why it needs no special case for an edge that closes a cycle (§9 Q2).
        if (from->output_type != to->input_type) {
            return fail("edge '" + edge.from + "' -> '" + edge.to + "' connects incompatible types: " +
                            from->output_type + " -> " + to->input_type,
                        "workflow.edge_type_mismatch");
        }

        bool const labelled = edge.kind == edge_kind::switch_case ||
                              edge.kind == edge_kind::multi_selection;
        if (labelled && edge.case_label.empty()) {
            return fail("edge '" + edge.from + "' -> '" + edge.to +
                            "' is switch/case or multi-selection but carries no case label",
                        "workflow.missing_case_label");
        }
        if (!labelled && !edge.case_label.empty()) {
            return fail("edge '" + edge.from + "' -> '" + edge.to +
                            "' carries a case label but is not a switch/case or multi-selection edge",
                        "workflow.unexpected_case_label");
        }

        // -- 014 §6, the per-edge failure policy ---------------------------------------------
        auto const& pol = edge.on_failure;
        bool const retrying  = pol.kind == edge_failure_policy::retry;
        bool const recovering = pol.kind == edge_failure_policy::fallback;

        if (retrying && pol.attempts == 0) {
            return fail("edge '" + edge.from + "' -> '" + edge.to +
                            "' declares the retry failure policy but no attempt budget; a retry of 0 "
                            "attempts is a no-op spelled as a policy",
                        "workflow.retry_attempts_required");
        }
        if (!retrying && pol.attempts != 0) {
            return fail("edge '" + edge.from + "' -> '" + edge.to +
                            "' carries a retry attempt budget but does not declare the retry failure "
                            "policy",
                        "workflow.unexpected_retry_attempts");
        }
        if (recovering && pol.fallback.empty()) {
            return fail("edge '" + edge.from + "' -> '" + edge.to +
                            "' declares the fallback failure policy but names no fallback executor",
                        "workflow.fallback_target_required");
        }
        if (!recovering && !pol.fallback.empty()) {
            return fail("edge '" + edge.from + "' -> '" + edge.to +
                            "' names a fallback executor but does not declare the fallback failure "
                            "policy",
                        "workflow.unexpected_fallback_target");
        }
        if (recovering) {
            Executor const* recovery = wf.find(pol.fallback);
            if (recovery == nullptr) {
                return fail("edge '" + edge.from + "' -> '" + edge.to +
                                "' names undeclared fallback executor '" + pol.fallback + "'",
                            "workflow.unknown_fallback_target");
            }
            // A fallback branch is an edge out of `from` in every respect except when it fires, so
            // it gets 014 §1's type rule too. Exempting it would leave exactly one edge kind in the
            // graph unchecked -- the one that only ever runs when something has already failed.
            if (from->output_type != recovery->input_type) {
                return fail("fallback branch '" + edge.from + "' -> '" + pol.fallback +
                                "' connects incompatible types: " + from->output_type + " -> " +
                                recovery->input_type,
                            "workflow.fallback_type_mismatch");
            }
        }
    }

    // §6's policy decides what happens to the SOURCE executor, so every edge out of one executor
    // must agree on it. Checked as its own pass (rather than inside the loop above) so the message
    // can name both conflicting edges.
    for (std::size_t i = 0; i < wf.edges.size(); ++i) {
        for (std::size_t j = i + 1; j < wf.edges.size(); ++j) {
            if (wf.edges[i].from != wf.edges[j].from) continue;
            auto const& a = wf.edges[i].on_failure;
            auto const& b = wf.edges[j].on_failure;
            if (a.kind == b.kind && a.attempts == b.attempts) continue;
            return fail("executor '" + wf.edges[i].from +
                            "' declares conflicting failure policies on its outgoing edges ('" +
                            wf.edges[i].from + "' -> '" + wf.edges[i].to + "' vs '" +
                            wf.edges[j].from + "' -> '" + wf.edges[j].to +
                            "'); 014 §6's policy decides what happens to the source executor, so its "
                            "edges must agree",
                        "workflow.conflicting_failure_policy");
        }
    }

    // 014 §2's hard requirement. Stated as its own check with its own code because "the workflow ran
    // forever" is the failure this prevents, and an operator reading the code should find it named.
    if (!wf.bound.any()) {
        return fail("workflow declares no termination bound -- 014 §2 requires MaxRounds, a deadline, "
                    "or a budget; an unbounded workflow does not run",
                    "workflow.unbounded");
    }
    if (wf.bound.max_rounds.has_value() && *wf.bound.max_rounds == 0) {
        return fail("workflow declares max_rounds = 0, which can never execute a round",
                    "workflow.zero_max_rounds");
    }

    // Reachability. 014 §7 makes the graph reviewable; an executor no edge can reach is dead weight
    // in a rendered graph and almost always an authoring slip. Cycles are fine here -- this is a
    // forward walk, not an acyclicity test.
    std::vector<std::string_view> reachable;
    reachable.push_back(wf.start);
    for (std::size_t i = 0; i < reachable.size(); ++i) {
        std::string_view const current = reachable[i];
        auto visit = [&reachable](std::string_view target) {
            for (auto const& r : reachable) {
                if (r == target) return;
            }
            reachable.push_back(target);
        };
        for (auto const& edge : wf.edges) {
            if (edge.from != current) continue;
            visit(edge.to);
            // A fallback branch (§6) is reached only on failure, but it IS reached -- omitting it
            // here would make every recovery node look like dead weight and reject the graph.
            if (edge.on_failure.kind == edge_failure_policy::fallback) visit(edge.on_failure.fallback);
        }
    }
    for (auto const& e : wf.executors) {
        bool seen = false;
        for (auto const& r : reachable) {
            if (r == e.id) { seen = true; break; }
        }
        if (!seen) {
            return fail("executor '" + e.id + "' is unreachable from start '" + wf.start + "'",
                        "workflow.unreachable_executor");
        }
    }

    return {};
}

// -- Can THIS BUILD run it? (Milestone 6 Phase E) ----------------------------------------------
//
// A SECOND question, deliberately not folded into `validate_workflow` above. That function answers
// "is this a well-formed graph", and its answer must not depend on how much of 014 happens to be
// implemented -- 014 §7's rendering, diffing, and review all run over graphs this build cannot
// execute, and the 015 loader (M7) validates for those purposes too.
//
// This one answers "can this build execute it", and the distinction matters because the failure it
// prevents is silent: `WorkflowSupervisor` asks every non-port node through `FunctionExecutor`, so
// an `agent` or `sub_workflow` node would be run exactly as if it were a plain function -- producing
// plausible output from a graph whose behaviour differs from what its author declared. §1 lists four
// executor kinds; Milestone 6 built `function` (Phase B) and `request_port` (Phase E).
//
// Delete a kind's branch when that kind lands. A refused graph is recoverable; a quietly
// reinterpreted one is not.
[[nodiscard]] inline result<void> check_workflow_executable(Workflow const& wf) {
    for (auto const& e : wf.executors) {
        if (e.kind != executor_kind::agent && e.kind != executor_kind::sub_workflow) continue;
        return std::unexpected(error{
            failure_class::contract,
            "executor '" + e.id +
                "' declares an executor kind this build does not implement (014 §1's `agent` and "
                "`sub_workflow` nodes are not built yet); running it would treat it as a plain "
                "function node, which is not what the graph says",
            "workflow.executor_kind_unsupported"});
    }
    return {};
}

// -- The C++ authoring form (014 §1's "at compile time for the C++ form") ----------------------
//
// `TypedExecutor<In, Out>` carries the real C++ types, so `WorkflowBuilder::connect` can reject a
// mismatched edge with a `static_assert` -- a compile error, not a runtime `result`. It emits the
// string-typed description above, so the shared validator still runs over the C++ form too.
template <class In, class Out>
    requires DeclaredMessage<In> && DeclaredMessage<Out>
struct TypedExecutor {  // ae-naming-lint: allow TypedExecutor — the C++-form authoring handle for 027 §4's `Executor`; 027 lists the concept, not this spelling
    using input_type  = In;
    using output_type = Out;

    std::string   id;
    executor_kind kind = executor_kind::function;
    // `Executor::worktree_mode`'s escape hatch, reachable from here: `TypedExecutor` is a plain
    // aggregate, so an author sets this via designated-initializer syntax
    // (`TypedExecutor<In, Out>{.id = "reviewer", .worktree_mode = sharing_mode::shared}`) before
    // calling `WorkflowBuilder::add` -- no separate builder method needed, and `describe()` below
    // is the one place that has to remember to forward it.
    sharing_mode  worktree_mode = sharing_mode::branch;

    [[nodiscard]] Executor describe() const {
        return Executor{id, kind, message_type_id_of<In>(), message_type_id_of<Out>(), worktree_mode};
    }
};

class WorkflowBuilder {
public:
    explicit WorkflowBuilder(std::string workflow_id) { wf_.id = std::move(workflow_id); }

    template <class In, class Out>
    WorkflowBuilder& add(TypedExecutor<In, Out> const& executor) {
        wf_.executors.push_back(executor.describe());
        return *this;
    }

    // The compile-time half of 014 §1. `static_assert` rather than a `requires` clause so the
    // diagnostic names the actual problem instead of reporting "no matching overload".
    template <class FromIn, class FromOut, class ToIn, class ToOut>
    WorkflowBuilder& connect(TypedExecutor<FromIn, FromOut> const& from,
                             TypedExecutor<ToIn, ToOut> const&     to,
                             edge_kind kind = edge_kind::direct, std::string case_label = {},
                             EdgeFailurePolicy on_failure = {}) {
        static_assert(std::is_same_v<FromOut, ToIn>,
                      "WorkflowBuilder::connect: incompatible edge -- the source executor's output "
                      "message type is not the target executor's input message type (014 §1)");
        wf_.edges.push_back(Edge{from.id, to.id, kind, std::move(case_label), std::move(on_failure)});
        return *this;
    }

    WorkflowBuilder& start_at(std::string executor_id) {
        wf_.start = std::move(executor_id);
        return *this;
    }

    // Gap-2 fix: native-authoring parity with the declarative form's `metadata.description`/
    // `metadata.version` -- optional, chainable, matching this builder's own existing style.
    WorkflowBuilder& description(std::string text) {
        wf_.description = std::move(text);
        return *this;
    }
    WorkflowBuilder& version(std::string v) {
        wf_.version = std::move(v);
        return *this;
    }

    WorkflowBuilder& select_output(std::string executor_id) {
        wf_.output_selection.push_back(std::move(executor_id));
        return *this;
    }

    WorkflowBuilder& max_rounds(std::uint32_t n) {
        wf_.bound.max_rounds = n;
        return *this;
    }

    WorkflowBuilder& deadline_ms(std::uint64_t ms) {
        wf_.bound.deadline_ms = ms;
        return *this;
    }

    WorkflowBuilder& token_budget(std::uint64_t tokens) {
        wf_.bound.token_budget = tokens;
        return *this;
    }

    // Returns the validated description, or the same `contract` error the declarative loader would
    // get for the same mistake. The C++ form is validated by the SHARED validator, not merely by its
    // own `static_assert`s -- the compile-time check covers edge types and nothing else.
    [[nodiscard]] result<Workflow> build() const {
        auto valid = validate_workflow(wf_);
        if (!valid) return std::unexpected(valid.error());
        return wf_;
    }

    // The unvalidated description, for tests that need to prove the validator rejects something the
    // builder can still construct (an unreachable executor, a missing bound). Not the normal path.
    [[nodiscard]] Workflow const& described() const noexcept { return wf_; }

private:
    Workflow wf_;
};

}  // namespace agentengine::workflow
