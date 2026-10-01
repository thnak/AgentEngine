#pragma once
// Implements decisions/ADR-234-harness-composition.md (issue #60): the Harness composition point -- one
// call that turns a declared agent plus the caller's client and authority into a session carrying an
// opinionated default stack, the AgentEngine counterpart of MAF's `chatClient.AsHarnessAgent()` /
// `create_harness_agent()` (docs/research/2026-10-01-agent-harness.md has the MAF source read for this).
// 002 §2.2 (added by ADR-234) is the spec text.
//
// Shape (ADR-234 §3): a builder-time convenience ON TOP of the ADR-226 run bridge
// (core/agent_session_bridge.hpp), not a `HarnessAgent<A, Overrides...>` template and not a third
// session-construction path. `make_harness_session<A>()` / `bind_harness_session()` mirror
// `make_agent_session<A>()` / `bind_agent_session()` exactly, take one more argument (`HarnessOptions`),
// and run the SAME `agent_bind_detail::bind()`: every ADR-226 guarantee (declared tools only, the caller's
// grants narrowed to the ceiling, narrowing-only overrides, the approval floor, bind-time validation) holds
// for a harness session because it IS a bridge session -- the harness only adds pieces around it.
//
// The pieces, and their default (ADR-234 §4 decides each one against I2 and ADR-070):
//   instructions   ON   host-authored operating guidance placed ahead of the agent's own instructions.
//   todo           ON   TodoProvider (ADR-166): five session-local, capability-free todos_* tools.
//   plan_execute   ON   PlanExecuteMode (ADR-167) + its PolicyDecider, composed with the host's own policy;
//                       the harness also makes the gate COVER the agent's declared tools (see below).
//   approval       ON   a call that needs approval parks the run for a human (`run.suspended_for_approval`,
//                       resumed with `resolve()`) instead of being denied; never approves anything itself.
//   telemetry      ON   in-memory counters over the run-event stream + an optional host sink, capped by the
//                       agent's own `Telemetry<Capture>` (none -> off; metadata_only -> payloads stripped).
//   compaction     OFF  `HistoryT` (a compile-time strategy) -- `HistoryProvider<Window<0>>` keeps all of it;
//                       `Window<N>`/`Summarize<N, S>` drop or rewrite content, so the host chooses them.
//   skills         OFF  a host-constructed `SkillsProvider<>` (host-chosen sources); never a directory scan.
//                       Advertisement only: a skill's `allowed-tools` unlocks nothing in a harness session.
//   reflection     OFF  needs a host evaluator (ADR-168); no default evaluator exists to be honest about.
//   background     --   not a harness piece: `schedule_wakeup` is offered only when the caller's grants,
//                       narrowed to the agent's ceiling, hold `cap::Schedule` -- unchanged engine behaviour.
// Every ON piece has a `disable_*` flag; every OFF piece is enabled only by passing what it needs.
//
// I2: nothing here mints a `Capability`. The bridge computes the session's authority from the caller's
// grants alone; the harness tools need none; the plan gate and the approval posture only narrow or delay.
// I3: every decision input is host code -- the gate opens on a real `plan_ready` call that re-checks a real
// `todos_add` happened (ADR-167), the reflection evaluator is host code (ADR-168).

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/context_assembly.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/plan_execute_mode.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/session_builder.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/core/todo_provider.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/bounded_reflection.hpp"

namespace agentengine {

// The operating guidance the `instructions` piece places ahead of the agent's own instructions. Engine-
// authored constant text (never model output), so it rides the untainted static-instruction channel.
inline constexpr std::string_view kDefaultHarnessInstructions =
    "You are working inside an agent harness. Break multi-step work into concrete steps before you act. "
    "Use the tools you are offered to gather facts and to act, and say briefly what you learned before "
    "the next step. If a tool call fails or is denied, change your approach instead of repeating it. "
    "Finish with a short summary of what you did and what you found.";

// Counters the `telemetry` piece maintains from the session's run-event stream. In-memory, per session,
// no I/O: AgentEngine has no OpenTelemetry exporter (016 is not built), and inventing a default
// destination would be either a no-op or ambient I/O. A host that wants the events themselves passes
// `HarnessOptions::telemetry_sink`.
// ae-naming-lint: allow HarnessTelemetry — ADR-234 (issue #60 harness); 027 not yet updated
struct HarnessTelemetry {
    std::atomic<std::uint64_t> events{0};
    std::atomic<std::uint64_t> runs_started{0};
    std::atomic<std::uint64_t> runs_finished{0};
    std::atomic<std::uint64_t> runs_failed{0};
    std::atomic<std::uint64_t> model_calls{0};
    std::atomic<std::uint64_t> tool_calls{0};
    std::atomic<std::uint64_t> approvals_requested{0};
    std::atomic<std::uint64_t> policy_decisions{0};
    std::atomic<std::uint64_t> sink_failures{0};  // a host sink that threw (contained, ADR-234 R6)
};

// The harness's context-provider slot, behind the agent's `AgentToolSurface` (ADR-226): the enabled
// pieces, in a fixed order (skills, history, todo, plan_execute), assembled by the same
// `assemble_context()` every composed provider uses (005 §3 order, ADR-066 attribution stamping).
//
// Two differences from `ComposedContextProvider<Ms...>`, each from ADR-234's red-team:
//   - the piece set is chosen at RUNTIME (every piece is individually disable-able without a different
//     C++ type per combination), so it holds type-erased `ContextProviderDescriptor`s built by
//     `make_context_provider_descriptor()`;
//   - it FAILS CLOSED when any piece fails. `assemble_context()` skips a failing contributor (005 has no
//     "one bad contributor aborts" rule), which for a harness means a failing history strategy (a
//     `Summarize<N, S>` whose summarizer errored) silently sends the model a round with NO conversation,
//     and a failing plan piece silently drops the gate's instructions while its decider still denies
//     (red-team R3). Each piece's `on_context` is wrapped to record its error; a recorded error fails the
//     round (`harness_context.piece_failed` carries the piece's own code in the message).
// Move-only, single `engage()`, and an unengaged instance fails the round -- the AgentToolSurface/
// ComposedContextProvider precedents (ADR-226 R6/R9, ADR-074).
// ae-naming-lint: allow HarnessContextProvider — ADR-234 (issue #60 harness); 027 not yet updated
class HarnessContextProvider {
public:
    static constexpr std::string_view name = "harness";  // ae-naming-lint: allow name — ADR-066 §3 contributor identity

    HarnessContextProvider() = default;
    HarnessContextProvider(HarnessContextProvider const&)            = delete;
    HarnessContextProvider& operator=(HarnessContextProvider const&) = delete;
    HarnessContextProvider(HarnessContextProvider&& other) noexcept
        : pieces_(std::move(other.pieces_)), names_(std::move(other.names_)), failure_(std::move(other.failure_)),
          engaged_(other.engaged_) {
        other.pieces_.clear();
        other.names_.clear();
        other.failure_ = std::make_shared<std::optional<error>>();
        other.engaged_ = false;
    }
    HarnessContextProvider& operator=(HarnessContextProvider&& other) noexcept {
        if (this != &other) {
            pieces_        = std::move(other.pieces_);
            names_         = std::move(other.names_);
            failure_       = std::move(other.failure_);
            engaged_       = other.engaged_;
            other.pieces_.clear();
            other.names_.clear();
            other.failure_ = std::make_shared<std::optional<error>>();
            other.engaged_ = false;
        }
        return *this;
    }

    // Host-only, once. Refuses a set without a `history` piece: without one the model would see no
    // conversation at all, which no harness configuration means (red-team R3's sibling).
    [[nodiscard]] result<void> engage(std::vector<ContextProviderDescriptor> pieces) {
        if (engaged_) {
            return std::unexpected(error{failure_class::contract,
                                         "HarnessContextProvider::engage() called twice -- a harness's piece set is "
                                         "fixed once the session is bound",
                                         "harness_context.already_engaged"});
        }
        bool const has_history = std::any_of(pieces.begin(), pieces.end(),
                                             [](ContextProviderDescriptor const& d) { return d.name == "history"; });
        if (!has_history) {
            return std::unexpected(error{failure_class::contract,
                                         "a harness context needs a history piece; without one the model sees no "
                                         "conversation",
                                         "harness_context.no_history"});
        }
        std::vector<ContextProviderDescriptor> wrapped;
        std::vector<std::string>               names;
        wrapped.reserve(pieces.size());
        for (ContextProviderDescriptor& d : pieces) {
            names.push_back(d.name);
            ContextProviderDescriptor g = d;
            g.on_context = [fn = std::move(d.on_context), slot = failure_, piece = d.name](
                               SessionContext& sc, EffectContext& ec) { return guarded(fn, slot, piece, sc, ec); };
            wrapped.push_back(std::move(g));
        }
        pieces_  = std::move(wrapped);
        names_   = std::move(names);
        engaged_ = true;
        return {};
    }

    [[nodiscard]] bool engaged() const noexcept { return engaged_; }
    // The enabled pieces' contributor names, in wire order -- what a test or a host inspects to see which
    // pieces a session actually carries.
    [[nodiscard]] std::vector<std::string> const& piece_names() const noexcept { return names_; }

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx, EffectContext& ctx) {
        if (!engaged_) {
            co_return std::unexpected(error{failure_class::contract,
                                            "this harness session's context is not engaged (cleared, moved from, or "
                                            "never bound)",
                                            "harness_context.not_engaged"});
        }
        failure_->reset();
        result<ContextAssemblyResult> assembled = co_await assemble_context(pieces_, session_ctx, ctx);
        if (!assembled) co_return std::unexpected(assembled.error());
        if (failure_->has_value()) {
            error e = **failure_;
            failure_->reset();
            co_return std::unexpected(error{e.klass, e.message + " (" + e.code + ")", "harness_context.piece_failed"});
        }
        co_return std::move(assembled->combined);
    }

    task<std::monostate> on_turn_end(TurnView turn, EffectContext& ctx) {
        for (ContextProviderDescriptor& piece : pieces_) (void)co_await piece.on_turn_end(turn, ctx);
        co_return std::monostate{};
    }

private:
    // A plain coroutine taking everything by value except the two per-call references the caller awaits
    // immediately -- not a capturing coroutine lambda, whose captures would die with the lambda.
    static task<result<ContextContribution>> guarded(ContextProviderDescriptor::OnContextFn fn,
                                                     std::shared_ptr<std::optional<error>> slot, std::string piece,
                                                     SessionContext& sc, EffectContext& ec) {
        result<ContextContribution> r = co_await fn(sc, ec);
        if (!r && !slot->has_value()) {
            error e = r.error();
            e.message = "harness piece '" + piece + "' failed: " + e.message;
            *slot = std::move(e);
        }
        co_return r;
    }

    std::vector<ContextProviderDescriptor>  pieces_;
    std::vector<std::string>                names_;
    std::shared_ptr<std::optional<error>>   failure_ = std::make_shared<std::optional<error>>();
    bool                                    engaged_ = false;
};

static_assert(ContextProvider<HarnessContextProvider>, "HarnessContextProvider must be a ContextProvider (005 §5)");

// ae-naming-lint: allow ReflectionEvaluator — ADR-234 (ADR-168's Evaluator, type-erased for HarnessOptions)
using ReflectionEvaluator = std::function<task<result<rt::EvaluationVerdict>>(rt::AgentResponse const&)>;

// What the CALLER chooses for the harness's own pieces -- host code only (I3). Defaults are ADR-234 §4's.
// ae-naming-lint: allow HarnessOptions — ADR-234 (issue #60 harness); 027 not yet updated
struct HarnessOptions {
    // instructions (ON)
    bool                       disable_harness_instructions = false;
    std::optional<std::string> harness_instructions;  // replaces kDefaultHarnessInstructions when set

    // todo (ON)
    bool disable_todo = false;

    // plan_execute (ON; needs todo). `is_planning_safe` widens what may run before the plan exists (host
    // code; default: pure and capability-free, ADR-167's floor).
    bool                                        disable_plan_execute = false;
    std::function<bool(ToolDescriptor const&)>  is_planning_safe;

    // approval (ON): park approval-needing calls for a human instead of denying them.
    bool disable_approval_suspension = false;

    // telemetry (ON, capped by the agent's Telemetry<Capture>)
    bool                                    disable_telemetry = false;
    std::function<void(RunEvent const&)>    telemetry_sink;

    // skills (OFF): a host-constructed `SkillsProvider<>` over host-chosen sources, wrapped by the host with
    // `make_context_provider_descriptor(std::move(provider), budget)`. Type-erased so this header does not pull
    // in `skill_provider.hpp` (and its worktree-store link dependency) for hosts that never use skills; refused
    // unless the descriptor's contributor name is "skills" (`harness.skills_not_a_skills_provider`).
    std::optional<ContextProviderDescriptor> skills;

    // reflection (OFF): ADR-168's bounded loop, used by `run()`/`ask()` when an evaluator is set.
    ReflectionEvaluator             reflection_evaluator;
    std::uint32_t                   reflection_max_iterations = 3;
    // Aggregate input+output tokens across every iteration. Unset = the session's effective per-run
    // `token_budget` (so a reflective run spends no more than one run of the agent may), or no aggregate
    // bound when the agent declares none.
    std::optional<std::uint64_t>    reflection_max_total_tokens;
};

// The tools the harness's own pieces offer. A declared agent tool with one of these names is refused at
// bind (`harness.tool_name_collision`) rather than at its first round (AgentToolSurface's own check).
inline constexpr std::string_view kHarnessToolNames[] = {"todos_add",     "todos_complete", "todos_remove",
                                                         "todos_get_remaining", "todos_get_all", "plan_ready"};

template <class ChatClientT>
class HarnessBundle;  // ae-naming-lint: allow HarnessBundle — ADR-234 (issue #60 harness); 027 not yet updated

namespace harness_detail {

struct HarnessFactory;

[[nodiscard]] inline error harness_error(std::string message, std::string code) {
    return error{failure_class::contract, std::move(message), std::move(code)};
}

// Red-team round 2, S1: the `skills` slot is recognised by contributor NAME, which host code chooses -- a
// tool-bearing provider wrapped under the name "skills" would put undeclared tools in front of the model
// through the slot the harness documents as "advertisement only". Enforced structurally instead: a skills
// contribution carrying any tool fails the round (and, the harness context being fail-closed, the run).
inline task<result<ContextContribution>> skills_without_tools(ContextProviderDescriptor::OnContextFn fn,
                                                              SessionContext& sc, EffectContext& ec) {
    result<ContextContribution> r = co_await fn(sc, ec);
    if (r && !r->tools.empty()) {
        co_return std::unexpected(harness_error("the skills piece contributed " + std::to_string(r->tools.size()) +
                                                    " tool(s); in a harness session skills advertise, they never "
                                                    "add tools",
                                                "harness.skills_contributed_tools"));
    }
    co_return r;
}

}  // namespace harness_detail

// The running form of a harness session: the ADR-226 bundle (`AgentBundleWith<C, HarnessContextProvider>`)
// plus handles onto the harness's own state. Move-only (it owns a Bundle).
template <class ChatClientT>
// ae-naming-lint: allow HarnessBundle — ADR-234 (issue #60 harness); 027 not yet updated
class HarnessBundle {
public:
    using BundleT  = AgentBundleWith<ChatClientT, HarnessContextProvider>;
    using SessionT = typename BundleT::SessionT;

    HarnessBundle(HarnessBundle&&) noexcept            = default;
    HarnessBundle(HarnessBundle const&)                = delete;
    HarnessBundle& operator=(HarnessBundle const&)     = delete;
    HarnessBundle& operator=(HarnessBundle&&)          = delete;  // Bundle's own reason (destruction order)

    [[nodiscard]] SessionT& session() noexcept { return bundle_.session(); }
    [[nodiscard]] CapabilitySet const& capabilities() const noexcept { return bundle_.capabilities(); }

    // One run of the agent -- or, when a reflection evaluator is configured, ADR-168's bounded loop, whose
    // final response this returns (MAF applies its LoopAgent on every run the same way). A run that parks
    // for approval returns `run.suspended_for_approval`; `resolve()` continues it.
    [[nodiscard]] result<rt::AgentResponse> run(std::string text) {
        if (!reflection_evaluator_) return bundle_.run(std::move(text));
        result<rt::ReflectionOutcome> outcome = run_reflective(std::move(text));
        if (!outcome) return std::unexpected(outcome.error());
        return std::move(outcome->response);
    }

    [[nodiscard]] result<std::string> ask(std::string text) {
        result<rt::AgentResponse> r = run(std::move(text));
        if (!r) return std::unexpected(r.error());
        return text_of(r->message);
    }

    // The reflection loop's whole outcome (satisfied, iterations, aggregate tokens). Refused when no
    // evaluator is configured -- reflection is off by default (ADR-234 §4).
    [[nodiscard]] result<rt::ReflectionOutcome> run_reflective(std::string text) {
        if (!reflection_evaluator_) {
            return std::unexpected(harness_detail::harness_error(
                "this harness session has no reflection evaluator (HarnessOptions::reflection_evaluator)",
                "harness.reflection_disabled"));
        }
        Message input = quickstart::detail::user_message(std::move(text));
        return bundle_.drive([&](SessionT& s) {
            return rt::run_with_bounded_reflection(s, std::move(input), reflection_max_iterations_,
                                                   reflection_evaluator_, reflection_max_total_tokens_);
        });
    }

    // Streams one run's text (no reflection: a loop's intermediate answers are not the stream's text).
    [[nodiscard]] result<stream<std::string>> ask_stream(std::string text) { return bundle_.ask_stream(std::move(text)); }

    // Continue a run parked on an interaction (an approval, under the `approval` piece). The decision is the
    // caller's -- host/human code (I3) -- exactly `AgentSession::resolve_interaction()`'s contract.
    [[nodiscard]] result<rt::AgentResponse> resolve(rt::ResolveInteraction decision) {
        return bundle_.drive([&](SessionT& s) { return s.resolve_interaction(std::move(decision)); });
    }

    // Which pieces this session actually carries, e.g. {"instructions", "history", "todo", "plan_execute",
    // "approval", "telemetry"} -- the context pieces in wire order, then the session-level ones.
    [[nodiscard]] std::vector<std::string> const& pieces() const noexcept { return pieces_; }
    [[nodiscard]] bool has_piece(std::string_view piece) const noexcept {
        return std::find(pieces_.begin(), pieces_.end(), piece) != pieces_.end();
    }
    // Null when the piece is off.
    [[nodiscard]] std::shared_ptr<TodoState const> todo_state() const noexcept { return todo_state_; }
    [[nodiscard]] std::shared_ptr<GateState const> plan_gate() const noexcept { return plan_gate_; }
    [[nodiscard]] std::shared_ptr<HarnessTelemetry const> telemetry() const noexcept { return telemetry_; }
    // The agent's declared tools the plan gate covers (raised from never_require to policy_driven).
    [[nodiscard]] std::vector<std::string> const& plan_gated_tools() const noexcept { return plan_gated_tools_; }

private:
    friend struct harness_detail::HarnessFactory;
    explicit HarnessBundle(BundleT bundle) : bundle_(std::move(bundle)) {}

    BundleT                                 bundle_;
    std::vector<std::string>                pieces_;
    std::shared_ptr<TodoState const>        todo_state_;
    std::shared_ptr<GateState const>        plan_gate_;
    std::shared_ptr<HarnessTelemetry>       telemetry_;
    std::vector<std::string>                plan_gated_tools_;
    ReflectionEvaluator                     reflection_evaluator_;
    std::uint32_t                           reflection_max_iterations_   = 0;
    std::uint64_t                           reflection_max_total_tokens_ = 0;
};

namespace harness_detail {

struct HarnessFactory {
    template <class ChatClientT>
    [[nodiscard]] static HarnessBundle<ChatClientT> make(typename HarnessBundle<ChatClientT>::BundleT bundle) {
        return HarnessBundle<ChatClientT>(std::move(bundle));
    }
    template <class ChatClientT>
    static void fill(HarnessBundle<ChatClientT>& h, std::vector<std::string> pieces,
                     std::shared_ptr<TodoState const> todo, std::shared_ptr<GateState const> gate,
                     std::shared_ptr<HarnessTelemetry> telemetry, std::vector<std::string> gated,
                     ReflectionEvaluator evaluator, std::uint32_t iterations, std::uint64_t max_tokens) {
        h.pieces_                      = std::move(pieces);
        h.todo_state_                  = std::move(todo);
        h.plan_gate_                   = std::move(gate);
        h.telemetry_                   = std::move(telemetry);
        h.plan_gated_tools_            = std::move(gated);
        h.reflection_evaluator_        = std::move(evaluator);
        h.reflection_max_iterations_   = iterations;
        h.reflection_max_total_tokens_ = max_tokens;
    }
};

// Counts every event into `t`; forwards to `sink` at the capture level the AGENT declared -- `full` as
// emitted, `metadata_only` with the payload replaced by `Empty` (run id, sequence and kind only: deltas,
// tool results and error messages are content). `none` never reaches here (the piece is off).
[[nodiscard]] inline std::function<void(RunEvent const&)> make_telemetry_tap(std::shared_ptr<HarnessTelemetry> t,
                                                                            std::function<void(RunEvent const&)> sink,
                                                                            telemetry_capture capture) {
    return [t = std::move(t), sink = std::move(sink), capture](RunEvent const& ev) {
        t->events.fetch_add(1, std::memory_order_relaxed);
        switch (ev.kind) {
            case run_event_kind::run_started: t->runs_started.fetch_add(1, std::memory_order_relaxed); break;
            case run_event_kind::run_finished: t->runs_finished.fetch_add(1, std::memory_order_relaxed); break;
            case run_event_kind::run_failed: t->runs_failed.fetch_add(1, std::memory_order_relaxed); break;
            case run_event_kind::model_call_started: t->model_calls.fetch_add(1, std::memory_order_relaxed); break;
            case run_event_kind::tool_call_started: t->tool_calls.fetch_add(1, std::memory_order_relaxed); break;
            case run_event_kind::approval_requested:
                t->approvals_requested.fetch_add(1, std::memory_order_relaxed);
                break;
            case run_event_kind::policy_decision: t->policy_decisions.fetch_add(1, std::memory_order_relaxed); break;
            default: break;
        }
        if (!sink) return;
        // Red-team R6: the tap runs inside the round, on the session's own emit path. Telemetry must never
        // change a run's outcome, so a throwing host sink is contained and counted, not propagated.
        try {
            if (capture == telemetry_capture::full) {
                sink(ev);
            } else {
                sink(RunEvent{ev.run_id, ev.seq, ev.kind, run_event_payload::Empty{}});
            }
        } catch (...) {
            t->sink_failures.fetch_add(1, std::memory_order_relaxed);
        }
    };
}

template <class HistoryT, class ChatClientT>
[[nodiscard]] result<HarnessBundle<ChatClientT>> bind(AgentMetadata meta, ChatClientT client, AgentSessionOptions opts,
                                                      HarnessOptions hopts, agent_bind_detail::OutputValidator validator,
                                                      ChatClientRegistry const* registry,
                                                      SandboxBackendRegistry const* sandbox_registry) {
    bool const todo_on  = !hopts.disable_todo;
    bool const plan_on  = !hopts.disable_plan_execute;
    // A Telemetry<none> agent has declared no telemetry; the agent's declaration caps the harness's default.
    bool const telem_on = !hopts.disable_telemetry && meta.telemetry != telemetry_capture::none;

    if (plan_on && !todo_on) {
        return std::unexpected(harness_error(
            "plan_execute needs the todo piece -- the plan the gate demands evidence of IS the todo list (ADR-167); "
            "set disable_plan_execute too",
            "harness.plan_execute_requires_todo"));
    }
    if (hopts.telemetry_sink && !telem_on) {
        return std::unexpected(harness_error(
            hopts.disable_telemetry ? "a telemetry_sink was passed with disable_telemetry set"
                                    : "a telemetry_sink was passed for an agent that declares Telemetry<none>",
            "harness.telemetry_sink_unused"));
    }
    if (hopts.skills.has_value() && hopts.skills->name != "skills") {
        return std::unexpected(harness_error("HarnessOptions::skills must be a skills provider's descriptor "
                                             "(contributor name 'skills'), not '" + hopts.skills->name + "'",
                                             "harness.skills_not_a_skills_provider"));
    }
    if (hopts.reflection_evaluator && hopts.reflection_max_iterations == 0) {
        return std::unexpected(harness_error("reflection_max_iterations must be >= 1",
                                             "harness.reflection_zero_iterations"));
    }
    for (ToolDescriptor const& d : meta.tools.descriptors()) {
        bool const clash = std::any_of(std::begin(kHarnessToolNames), std::end(kHarnessToolNames),
                                       [&d](std::string_view n) { return d.name == n; });
        // Red-team R4: the plan decider recognises a gated tool by NAME. `schedule_wakeup` is the one tool the
        // engine itself injects into a round (when `cap::Schedule` is held); a declared tool of that name
        // would put the engine's tool under the gated tool's post-gate auto_approve.
        bool const engine_clash = plan_on && d.name == "schedule_wakeup";
        if ((clash && (todo_on || plan_on)) || engine_clash) {
            return std::unexpected(harness_error("the agent declares a tool named '" + d.name +
                                                     "', which a harness piece also offers; rename it or disable "
                                                     "the piece",
                                                 "harness.tool_name_collision"));
        }
    }

    // instructions: harness guidance first, then the agent's (MAF's order). Host/engine text only.
    if (!hopts.disable_harness_instructions) {
        std::string guidance = hopts.harness_instructions.value_or(std::string(kDefaultHarnessInstructions));
        if (!guidance.empty()) {
            meta.agent_instructions =
                meta.agent_instructions.empty() ? guidance : guidance + "\n\n" + meta.agent_instructions;
        }
    }

    // The context pieces, constructed before the session exists (the gate needs the todo state handle).
    std::optional<TodoProvider>    todo;
    std::optional<PlanExecuteMode> plan;
    std::shared_ptr<TodoState const> todo_state;
    std::shared_ptr<GateState const> gate;
    if (todo_on) {
        todo.emplace();
        todo_state = todo->plan_state_handle();
    }
    if (plan_on) {
        plan.emplace(*todo);
        gate = plan->gate_handle();
    }

    // plan_execute: make the gate COVER the agent's declared tools (red-team R1). ADR-167's decider is
    // consulted only for policy_driven tools, and `Tool<>`'s default is never_require -- so without this the
    // default harness's gate would gate nothing. A declared never_require tool that is not planning-safe is
    // raised to policy_driven (stricter: it now reaches a decider), and the harness decider gives it back
    // exactly its declared behaviour once the gate is open (auto_approve -- which ADR-070 never honours for a
    // text_derived call, the same approval a never_require text_derived call already needed). The harness's
    // own tools are not in this list (they come from the inner context, not the declaration), so planning
    // itself is never gated (red-team R2).
    std::function<bool(ToolDescriptor const&)> planning_safe =
        hopts.is_planning_safe ? hopts.is_planning_safe
                               : std::function<bool(ToolDescriptor const&)>(plan_execute_detail::default_is_planning_safe);
    auto gated = std::make_shared<std::set<std::string>>();
    agent_bind_detail::DescriptorFilter filter;
    if (plan_on) {
        filter = [gated, planning_safe](std::vector<ToolDescriptor>& tools) {
            for (ToolDescriptor& d : tools) {
                if (d.approval == approval_mode::never_require && !planning_safe(d)) {
                    d.approval = approval_mode::policy_driven;
                    gated->insert(d.name);
                }
            }
        };
        PolicyDecider host_policy = std::move(opts.policy);
        PolicyDecider after_gate  = [gated, host_policy](Principal const& caller, ToolDescriptor const& tool,
                                                        bool arguments_tainted) -> policy_decision {
            if (gated->contains(tool.name)) return policy_decision::auto_approve;  // its declared never_require
            if (host_policy) return host_policy(caller, tool, arguments_tainted);
            return policy_decision::require_approval;  // == no decider wired (ADR-070)
        };
        opts.policy = make_plan_execute_policy_decider(gate, std::move(after_gate), planning_safe);
    }

    std::optional<std::uint64_t> const effective_budget = opts.token_budget.has_value() ? opts.token_budget
                                                                                        : meta.token_budget;

    result<AgentBundleWith<ChatClientT, HarnessContextProvider>> bound =
        agent_bind_detail::bind<ChatClientT, HarnessContextProvider>(meta, std::move(client), std::move(opts),
                                                                     std::move(validator), registry,
                                                                     sandbox_registry, filter);
    if (!bound) return std::unexpected(bound.error());

    std::vector<ContextProviderDescriptor> context_pieces;
    if (hopts.skills.has_value()) {
        ContextProviderDescriptor skills = std::move(*hopts.skills);
        skills.on_context = [fn = std::move(skills.on_context)](SessionContext& sc, EffectContext& ec) {
            return skills_without_tools(fn, sc, ec);
        };
        context_pieces.push_back(std::move(skills));
    }
    context_pieces.push_back(make_context_provider_descriptor(HistoryT{}, {}));
    if (todo.has_value()) context_pieces.push_back(make_context_provider_descriptor(std::move(*todo), {}));
    if (plan.has_value()) context_pieces.push_back(make_context_provider_descriptor(std::move(*plan), {}));

    auto& session = bound->session();
    if (auto engaged = session.history_provider().inner().engage(std::move(context_pieces)); !engaged) {
        return std::unexpected(engaged.error());
    }

    std::vector<std::string> pieces;
    if (!hopts.disable_harness_instructions) pieces.emplace_back("instructions");
    for (std::string const& n : session.history_provider().inner().piece_names()) pieces.push_back(n);

    // approval: park for a human. Has no effect where a decider decides (approve_tools, unattended mode) --
    // AgentSession consults the decider first (`approval_waits_for_human()`).
    if (!hopts.disable_approval_suspension) {
        session.set_suspend_for_approval(true);
        pieces.emplace_back("approval");
    }

    std::shared_ptr<HarnessTelemetry> telemetry;
    if (telem_on) {
        telemetry = std::make_shared<HarnessTelemetry>();
        session.set_run_event_tap(make_telemetry_tap(telemetry, std::move(hopts.telemetry_sink), meta.telemetry));
        pieces.emplace_back("telemetry");
    }

    std::uint64_t max_total_tokens = 0;
    if (hopts.reflection_evaluator) {
        max_total_tokens = hopts.reflection_max_total_tokens.value_or(effective_budget.value_or(0));
        pieces.emplace_back("reflection");
    }

    // Recorded for the host, like the bridge's own agent.* entries; never read for a decision.
    session.metadata()["harness.pieces"] = [&pieces] {
        std::string joined;
        for (std::string const& p : pieces) joined += (joined.empty() ? "" : ",") + p;
        return joined;
    }();

    HarnessBundle<ChatClientT> h = HarnessFactory::make<ChatClientT>(std::move(*bound));
    HarnessFactory::fill(h, std::move(pieces), std::move(todo_state), std::move(gate), std::move(telemetry),
                         std::vector<std::string>(gated->begin(), gated->end()), std::move(hopts.reflection_evaluator),
                         hopts.reflection_max_iterations, max_total_tokens);
    return h;
}

}  // namespace harness_detail

// The harness over any compiled `AgentMetadata` (a 015 document via `compile_agent_document()`, I6) --
// `bind_agent_session()` plus the harness pieces. `HistoryT` is the compaction strategy (ADR-234 §4).
template <class HistoryT = HistoryProvider<Window<0>>, class ChatClientT>
    requires(ChatClient<ChatClientT> || ModelCallGatewayLike<ChatClientT>) && ContextProvider<HistoryT> &&
            HasContextProviderName<HistoryT> && std::default_initializable<HistoryT>
[[nodiscard]] result<HarnessBundle<ChatClientT>> bind_harness_session(AgentMetadata const& meta, ChatClientT client,
                                                                      AgentSessionOptions opts = {},
                                                                      HarnessOptions hopts = {},
                                                                      ChatClientRegistry const* registry = nullptr,
                                                                      SandboxBackendRegistry const* sandbox_registry =
                                                                          nullptr) {
    return harness_detail::bind<HistoryT>(meta, std::move(client), std::move(opts), std::move(hopts), {}, registry,
                                          sandbox_registry);
}

// The native entry point: `register_agent<A>()` + the typed `OutputSchema<T>` validator (as
// `make_agent_session<A>()`) + the harness pieces.
template <class A, class HistoryT = HistoryProvider<Window<0>>, class ChatClientT>
    requires(ChatClient<ChatClientT> || ModelCallGatewayLike<ChatClientT>) && ContextProvider<HistoryT> &&
            HasContextProviderName<HistoryT> && std::default_initializable<HistoryT>
[[nodiscard]] result<HarnessBundle<ChatClientT>> make_harness_session(ChatClientT client, AgentSessionOptions opts = {},
                                                                      HarnessOptions hopts = {},
                                                                      ChatClientRegistry const* registry = nullptr,
                                                                      SandboxBackendRegistry const* sandbox_registry =
                                                                          nullptr) {
    result<AgentMetadata> meta = register_agent<A>(registry, sandbox_registry);
    if (!meta) return std::unexpected(meta.error());
    return harness_detail::bind<HistoryT>(std::move(*meta), std::move(client), std::move(opts), std::move(hopts),
                                          agent_bind_detail::output_validator_for<A>(), registry, sandbox_registry);
}

}  // namespace agentengine
