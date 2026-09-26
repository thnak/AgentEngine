// #120 S1 (ADR-200): rt::WorkflowSupervisor's member function bodies, moved verbatim out of
// include/agentengine/rt/workflow_supervisor.hpp so they are compiled once here instead of in every file that
// includes the header (46 at the time of the move). The header keeps the class, every declaration and comment,
// small inline accessors, constexpr members, the drive<T>() template and ScopedForwardedEventSink. The design
// and the rules each body implements are documented at its declaration in the header.

#include "agentengine/rt/workflow_supervisor.hpp"

namespace agentengine::rt {

void WorkflowSupervisor::initialize(agentengine::workflow::Workflow graph, std::vector<ExecutorBody> bodies,
                                    std::vector<agentengine::EffectContext> contexts,
                                    std::string designated_stall_reporter) {
    graph_    = std::move(graph);
    bodies_   = std::move(bodies);
    contexts_ = std::move(contexts);
    contexts_.resize(graph_.executors.size());
    designated_stall_reporter_ = std::move(designated_stall_reporter);
    stall_streak_ = 0;
    resets_used_  = 0;
    sub_workflows_.clear();
    pending_sub_workflows_.clear();
    // `valid_base_` covers everything EXCEPT sub_workflow binding -- cached so
    // `bind_sub_workflow()` below can recompute `valid_` directly after each bind, without
    // requiring the caller to call `initialize()` a second time just to pick up bindings that
    // (structurally) can only ever be supplied AFTER this call returns.
    valid_base_ = agentengine::workflow::validate_workflow(graph_).has_value() &&
                  bodies_.size() == graph_.executors.size() &&
                  agentengine::workflow::check_workflow_executable(graph_, contexts_).has_value() &&
                  agent_kind_bodies_are_structurally_agent_backed();
    valid_ = valid_base_ && sub_workflow_kind_nodes_are_bound();
}

void WorkflowSupervisor::bind_sub_workflow(std::string const& executor_id, std::shared_ptr<WorkflowSupervisor> inner) {
    std::size_t const idx = index_of(executor_id);
    if (idx >= graph_.executors.size() || graph_.executors[idx].id != executor_id) return;
    if (graph_.executors[idx].kind != agentengine::workflow::executor_kind::sub_workflow) return;
    for (auto const& [bound_idx, bound_inner] : sub_workflows_) {
        if (bound_idx != idx && bound_inner.get() == inner.get()) return;
    }
    // docs/planning/nested-workflow-threadpool-budget-design-draft.md (issue #42 item 2):
    // structural nesting-depth cap -- refuse rather than let a runaway/buggy graph author
    // build an unbounded chain of bind_sub_workflow() calls. `inner` is not left with a
    // dangling depth on refusal: it simply never gets marked deeper than whatever it already
    // was (0 for a freshly-constructed instance), matching every other refusal in this
    // function leaving the CALLER's own state untouched.
    if (!inner || nesting_depth_ + 1 > kMaxNestingDepth) return;
    inner->nesting_depth_ = nesting_depth_ + 1;
    sub_workflows_[idx] = std::move(inner);
    // ADR-169 (issue #65): a child bound AFTER this supervisor was given an owner inherits it
    // here; a child bound BEFORE inherits it when `set_principal()` runs. Both orderings converge
    // -- see propagate_admission_to_children()'s own comment. Runs after the bind, deliberately,
    // so the just-bound child is included.
    propagate_admission_to_children();
    valid_ = valid_base_ && sub_workflow_kind_nodes_are_bound();
}

std::vector<agentengine::Interaction> WorkflowSupervisor::open_interactions() const {
    std::vector<agentengine::Interaction> out;
    for (auto const& p : ports_) {
        if (!p.resolved) out.push_back(p.interaction);
    }
    for (auto const& [id, pending] : pending_sub_workflows_) {
        (void)id;
        out.push_back(pending.interaction);
    }
    return out;
}

std::vector<WorkflowSupervisor::InteractionAsk> WorkflowSupervisor::open_interaction_asks() const {
    std::vector<InteractionAsk> out;
    for (auto const& p : ports_) {
        if (!p.resolved) out.push_back(InteractionAsk{p.interaction, p.response});
    }
    for (auto const& [id, pending] : pending_sub_workflows_) {
        (void)id;
        out.push_back(InteractionAsk{pending.interaction, agentengine::Message{}});
    }
    return out;
}

task<WorkflowResult> WorkflowSupervisor::run_workflow(RunWorkflow request) {
    using agentengine::workflow::workflow_event_kind;
    using agentengine::workflow::workflow_event_payload::RunFailed;
    AsyncMutex::Guard guard = co_await run_mutex_.lock();  // I1 -- see file banner
    // ADR-169 (issue #65): FIRST -- see admit_caller()'s own comment for why this dominates the
    // `valid_` check below. Starting a fresh run is not a lesser authority than resuming one: it
    // resets `state_`/`ports_`/`pending_sub_workflows_` unconditionally three lines down, so an
    // unadmitted `RunWorkflow` against a SUSPENDED supervisor destroys exactly the open HITL
    // interactions an unadmitted `resume_workflow()` would have hijacked. Gating resume but not
    // run would have left the same authority reachable through a different verb.
    if (!admit_caller(request.caller)) co_return deny_admission();
    if (!valid_) {
        push_structural_event(workflow_event_kind::workflow_run_failed,
                               RunFailed{workflow_status_tag(workflow_status::invalid)});
        co_return WorkflowResult{workflow_status::invalid};
    }

    ++run_counter_;
    run_id_ = graph_.id + ":run:" + std::to_string(run_counter_);
    state_  = RunState{};
    ports_.clear();
    pending_sub_workflows_.clear();  // ADR-157 -- a fresh run carries no stale nested interactions
    rounds_ = 0;
    total_usage_ = agentengine::Usage{};  // GitHub issue #35 follow-up -- see usage()'s own comment
    seed_fan_in_holds();  // issue #62 -- must run AFTER state_ is reset, before round 1
    state_.pending.push_back(Delivery{index_of(graph_.start), request.input});

    push_structural_event(workflow_event_kind::workflow_run_started);
    WorkflowResult r = co_await execute();
    r.usage = total_usage_;  // ADR-193 §9: reset above, so the total IS this run's spend
    co_return r;
}

task<WorkflowResult> WorkflowSupervisor::resume_workflow(ResumeWorkflow request) {
    AsyncMutex::Guard guard = co_await run_mutex_.lock();  // I1 -- see file banner
    agentengine::Usage const before = total_usage_;
    WorkflowResult r = co_await resume_workflow_locked(std::move(request));
    r.usage = usage_added_since(before);
    co_return r;
}

task<WorkflowResult> WorkflowSupervisor::continue_workflow(ContinueWorkflow request) {
    AsyncMutex::Guard guard = co_await run_mutex_.lock();  // I1 -- see file banner
    agentengine::Usage const before = total_usage_;
    WorkflowResult r = co_await continue_workflow_locked(std::move(request));
    r.usage = usage_added_since(before);
    co_return r;
}

task<WorkflowResult> WorkflowSupervisor::resume_workflow_locked(ResumeWorkflow request) {
    using agentengine::workflow::workflow_event_kind;
    using agentengine::workflow::workflow_event_payload::RunFailed;
    // ADR-169 (issue #65) -- THE entry point this ADR exists for. Before `valid_`, and before
    // BOTH lookups below: the pre-ADR-169 body's only guard was id VALIDITY (an unknown or
    // already-resolved `interaction_id` fails closed, proven by E2 in
    // tests/test_rt_workflow_supervisor_request_port.cpp), never OWNERSHIP -- so knowing a live
    // id was sufficient authority to inject this run's next `Message` AND name its `routes`,
    // which on a switch_case/multi_selection edge decides where the run goes next. Ids are not
    // secrets: they cross `WorkflowResult::open_interactions` to the host, and per ADR-061 the
    // host owning the inbound transport is exactly the layer relaying an untrusted caller.
    if (!admit_caller(request.caller)) co_return deny_admission();
    if (!valid_) {
        push_structural_event(workflow_event_kind::workflow_run_failed,
                               RunFailed{workflow_status_tag(workflow_status::invalid)});
        co_return WorkflowResult{workflow_status::invalid};
    }

    // ADR-157 (issues #33/#38): checked FIRST, before ports_ at all -- see the design draft's
    // §2 finding 2/§4 for why this must be a genuinely separate lookup, not a marker on
    // OpenPort. A stale interaction_id here (e.g. from a restored checkpoint that never
    // persisted pending_sub_workflows_) simply falls through to "not found" below -- fails
    // closed, never misroutes.
    if (auto it = pending_sub_workflows_.find(request.interaction_id); it != pending_sub_workflows_.end()) {
        PendingSubWorkflow const pending = it->second;
        pending_sub_workflows_.erase(it);
        std::shared_ptr<WorkflowSupervisor> const inner = sub_workflows_.count(pending.executor_index)
                                                                ? sub_workflows_.at(pending.executor_index)
                                                                : nullptr;
        if (!inner) {
            // The binding was removed (a fresh initialize() since this interaction opened,
            // matching the same "bindings not checkpoint-durable" limitation §4 documents) --
            // fail closed rather than silently drop.
            push_structural_event(agentengine::workflow::workflow_event_kind::workflow_run_failed,
                                   RunFailed{workflow_status_tag(workflow_status::invalid)});
            WorkflowResult r{workflow_status::invalid};
            r.rounds            = rounds_;
            r.open_interactions = open_interactions();
            co_return r;
        }
        // issue #42 item 3: wire this resume dispatch's own forwarded sink/path exactly like
        // run_sub_workflow_job()'s own initial dispatch does -- a resumed nested run is driven
        // through this SAME call shape, just re-entered later, so it needs the identical
        // ScopedForwardedEventSink treatment for events THIS resumption's own inner rounds push.
        WorkflowResult inner_result;
        {
            auto const event_sink = workflow_event_stream_enabled_ ? multiplex_sink_ : nullptr;
            std::vector<std::string> child_path = event_path_prefix_;
            child_path.push_back(graph_.executors[pending.executor_index].id);
            ScopedForwardedEventSink const event_sink_guard(*inner, event_sink, std::move(child_path));
            inner_result =
                // ADR-169 (issue #65) item 5: `request.caller` is FORWARDED, not dropped, so the
                // inner supervisor's own gate genuinely runs and the nested effect stays
                // attributed to the real caller (I4) rather than to "the outer supervisor".
                // `propagate_admission_to_children()` is what keeps that from denying every
                // forwarded caller on an inner the host never separately configured.
                drive(inner->resume_workflow(ResumeWorkflow{pending.inner_interaction_id,
                                                              request.response, request.routes,
                                                              request.caller}));
        }
        // ADR-169 (issue #65), found by A11c during implementation, NOT designed up front: an
        // inner `admission_denied` must NOT fall through to the terminal-failure branch below.
        // That branch converts any non-`completed` inner outcome into a `failure_marker()`
        // message and routes it onward as an ordinary port resolution -- correct for a real inner
        // run FAILURE (ADR-157's own S4 shape), badly wrong for a refusal: the outer would report
        // `completed` while carrying a poisoned payload, and -- worse -- `pending_sub_workflows_`
        // was already erased above, so the nested interaction would be destroyed by an attempt
        // that decided nothing. Restore the entry verbatim and surface the refusal as itself.
        //
        // `admission_denied_count_` is deliberately NOT incremented here: THIS supervisor's gate
        // admitted this caller (that check ran and passed at the top of this function). The
        // refusal happened at the inner's gate and is counted there, which is where a host
        // debugging it needs to look. The structural event IS pushed, so the outer's own event
        // stream still shows the run stopping and why.
        if (inner_result.status == workflow_status::admission_denied) {
            pending_sub_workflows_[request.interaction_id] = pending;
            push_structural_event(
                workflow_event_kind::workflow_run_failed,
                RunFailed{workflow_status_tag(workflow_status::admission_denied)});
            co_return WorkflowResult{workflow_status::admission_denied};
        }
        if (inner_result.status == workflow_status::suspended) {
            // Suspended again -- mint a fresh outer interaction, re-track, report suspended
            // exactly like "other ports still unresolved" already does below for ordinary ports.
            agentengine::Interaction const fresh = mint_interaction(pending.executor_index);
            pending_sub_workflows_[fresh.interaction_id] =
                PendingSubWorkflow{fresh, pending.executor_index,
                                   inner_result.open_interactions.empty()
                                       ? std::string{}
                                       : inner_result.open_interactions.front().interaction_id};
            push_structural_event(
                agentengine::workflow::workflow_event_kind::request_port_opened,
                agentengine::workflow::workflow_event_payload::PortRef{
                    graph_.executors[pending.executor_index].id, fresh.interaction_id});
            WorkflowResult r{workflow_status::suspended};
            r.rounds            = rounds_;
            r.partial           = state_.partial;
            r.output            = state_.selected_output;
            r.open_interactions = open_interactions();
            co_return r;
        }
        // completed, or any terminal failure -- construct an ORDINARY, freshly-built OpenPort.
        // OpenPort's own fields mean exactly what they always meant: the derived value
        // (inner_result.output, or a failure marker) is computed HERE, before this OpenPort is
        // ever constructed, never written into a pre-existing one.
        agentengine::Message const reply_payload = inner_result.status == workflow_status::completed
            ? inner_result.output
            : failure_marker(graph_.executors[pending.executor_index].id, agentengine::failure_class::fatal);
        // GitHub issue #35 follow-up: `inner->usage()` is the nested supervisor's own CUMULATIVE
        // total across its whole suspend/resume lifecycle (never reset by resume_workflow(), only
        // by a fresh run_workflow()) -- reading it HERE, at final resolution, correctly captures
        // everything the nested run ever cost, including rounds that ran before it first
        // suspended. See OpenPort::usage's own comment.
        // Threaded directly into the aggregate-init list (not a post-construction assignment) --
        // an implicit trailing default here would reintroduce the exact -Werror=missing-field-
        // initializers hazard the origin/main CI fix (ExecuteReply's own sites, immediately below
        // this class) closed for this struct's sibling.
        OpenPort resolved_port{pending.interaction, pending.executor_index, reply_payload, {},
                                /*resolved=*/true, inner->usage()};
        ports_.push_back(std::move(resolved_port));
        push_structural_event(
            agentengine::workflow::workflow_event_kind::request_port_resolved,
            agentengine::workflow::workflow_event_payload::PortRef{
                graph_.executors[pending.executor_index].id, pending.interaction.interaction_id});
        // ADR-157: "still unresolved" must also account for OTHER pending sub-workflow
        // interactions (the one just resolved is already erased above), not just ports_ --
        // otherwise a still-open nested interaction elsewhere would be silently ignored and
        // execute() called prematurely.
        bool const still_unresolved =
            !pending_sub_workflows_.empty() ||
            std::any_of(ports_.begin(), ports_.end(), [](OpenPort const& p) { return !p.resolved; });
        if (still_unresolved) {
            push_structural_event(workflow_event_kind::workflow_run_suspended);
            WorkflowResult r{workflow_status::suspended};
            r.rounds            = rounds_;
            r.partial           = state_.partial;
            r.output            = state_.selected_output;
            r.open_interactions = open_interactions();
            co_return r;
        }
        push_structural_event(workflow_event_kind::workflow_run_resumed);
        co_return co_await execute();
    }

    OpenPort* port = nullptr;
    for (auto& p : ports_) {
        if (p.interaction.interaction_id == request.interaction_id) { port = &p; break; }
    }
    if (port == nullptr || port->resolved) {
        push_structural_event(workflow_event_kind::workflow_run_failed,
                               RunFailed{workflow_status_tag(workflow_status::invalid)});
        WorkflowResult r{workflow_status::invalid};
        r.rounds            = rounds_;
        r.open_interactions = open_interactions();
        co_return r;
    }

    port->resolved = true;
    port->response = request.response;
    port->routes   = request.routes;

    // ADR-157: also checks pending_sub_workflows_ -- a still-open nested interaction must keep
    // the run suspended exactly like an unresolved ordinary port already does.
    bool const still_unresolved =
        !pending_sub_workflows_.empty() ||
        std::any_of(ports_.begin(), ports_.end(), [](OpenPort const& p) { return !p.resolved; });
    if (still_unresolved) {
        // Re-affirms the run is STILL suspended (other ports/interactions remain open) --
        // distinct from finish()'s own workflow_run_suspended, which only fires once a full
        // execute() pass confirms suspension; this is the "you resolved one of several, the run
        // hasn't moved yet" case.
        push_structural_event(workflow_event_kind::workflow_run_suspended);
        WorkflowResult r{workflow_status::suspended};
        r.rounds            = rounds_;
        r.partial           = state_.partial;
        r.output            = state_.selected_output;
        r.open_interactions = open_interactions();
        co_return r;
    }

    push_structural_event(workflow_event_kind::workflow_run_resumed);
    co_return co_await execute();
}

task<WorkflowResult> WorkflowSupervisor::continue_workflow_locked(ContinueWorkflow request) {
    using agentengine::workflow::workflow_event_kind;
    using agentengine::workflow::workflow_event_payload::RunFailed;
    // ADR-169 (issue #65) item 4: gated identically to its two siblings. Not an afterthought --
    // this is the entry point a checkpoint-restored run is driven through
    // (rt/workflow_checkpoint_manager.hpp's own `resumed == true` contract), so leaving it open
    // would mean an unadmitted caller could drive any run that had ever been checkpointed to
    // completion, using effects and budget belonging to the run's real owner.
    if (!admit_caller(request.caller)) co_return deny_admission();
    if (!valid_) {
        push_structural_event(workflow_event_kind::workflow_run_failed,
                               RunFailed{workflow_status_tag(workflow_status::invalid)});
        co_return WorkflowResult{workflow_status::invalid};
    }
    push_structural_event(workflow_event_kind::workflow_run_resumed);
    co_return co_await execute();
}

agentengine::Usage WorkflowSupervisor::usage_added_since(agentengine::Usage const& before) const noexcept {
    auto const sub = [](std::uint64_t after, std::uint64_t b) { return after > b ? after - b : std::uint64_t{0}; };
    agentengine::Usage d{};
    d.input_tokens        = sub(total_usage_.input_tokens, before.input_tokens);
    d.output_tokens       = sub(total_usage_.output_tokens, before.output_tokens);
    d.cached_input_tokens = sub(total_usage_.cached_input_tokens, before.cached_input_tokens);
    d.reasoning_tokens    = sub(total_usage_.reasoning_tokens, before.reasoning_tokens);
    d.cache_write_tokens  = sub(total_usage_.cache_write_tokens, before.cache_write_tokens);
    d.cost_estimate = total_usage_.cost_estimate > before.cost_estimate ? total_usage_.cost_estimate - before.cost_estimate
                                                                        : 0.0;
    return d;
}

RunStateRecord WorkflowSupervisor::to_record() const {
    RunStateRecord rec;
    rec.run_counter = run_counter_;
    rec.run_id      = run_id_;
    rec.rounds      = rounds_;
    rec.pending.reserve(state_.pending.size());
    for (auto const& d : state_.pending) {
        rec.pending.push_back(DeliveryRecord{static_cast<std::uint64_t>(d.executor_index), d.payload});
    }
    rec.partial.reserve(state_.partial.size());
    for (auto const& p : state_.partial) {
        rec.partial.push_back(ExecutorOutputRecord{p.executor_id, p.round, p.payload});
    }
    rec.selected_output = state_.selected_output;
    rec.failed_executor  = state_.failed_executor;
    rec.unopened_ports   = state_.unopened_ports;
    rec.elapsed_ns       = state_.elapsed_ns;
    rec.ports.reserve(ports_.size());
    for (auto const& p : ports_) {
        rec.ports.push_back(OpenPortRecord{p.interaction, static_cast<std::uint64_t>(p.executor_index),
                                           p.response, p.routes, p.resolved});
    }
    rec.stall_streak = stall_streak_;
    rec.resets_used  = resets_used_;
    rec.held_fan_in.reserve(state_.held_fan_in.size());
    for (auto const& h : state_.held_fan_in) {
        std::vector<std::uint64_t> awaiting;
        awaiting.reserve(h.awaiting_sources.size());
        for (std::size_t const idx : h.awaiting_sources) awaiting.push_back(static_cast<std::uint64_t>(idx));
        rec.held_fan_in.push_back(HeldFanInRecord{static_cast<std::uint64_t>(h.executor_index), h.payload,
                                                    h.seeded, std::move(awaiting)});
    }
    return rec;
}

void WorkflowSupervisor::restore_from_record(RunStateRecord const& rec) {
    run_counter_ = rec.run_counter;
    run_id_      = rec.run_id;
    rounds_      = rec.rounds;
    state_       = RunState{};
    state_.pending.reserve(rec.pending.size());
    for (auto const& d : rec.pending) {
        state_.pending.push_back(Delivery{static_cast<std::size_t>(d.executor_index), d.payload});
    }
    state_.partial.reserve(rec.partial.size());
    for (auto const& p : rec.partial) {
        state_.partial.push_back(ExecutorOutput{p.executor_id, p.round, p.payload});
    }
    state_.selected_output = rec.selected_output;
    state_.failed_executor = rec.failed_executor;
    state_.unopened_ports  = rec.unopened_ports;
    state_.elapsed_ns      = rec.elapsed_ns;
    ports_.clear();
    ports_.reserve(rec.ports.size());
    for (auto const& p : rec.ports) {
        ports_.push_back(OpenPort{p.interaction, static_cast<std::size_t>(p.executor_index),
                                  p.response, p.routes, p.resolved, agentengine::Usage{}});
    }
    stall_streak_ = rec.stall_streak;
    resets_used_  = rec.resets_used;
    state_.held_fan_in.reserve(rec.held_fan_in.size());
    for (auto const& h : rec.held_fan_in) {
        std::vector<std::size_t> awaiting;
        awaiting.reserve(h.awaiting_sources.size());
        for (std::uint64_t const idx : h.awaiting_sources) awaiting.push_back(static_cast<std::size_t>(idx));
        state_.held_fan_in.push_back(HeldFanIn{static_cast<std::size_t>(h.executor_index), h.payload,
                                                 h.seeded, std::move(awaiting)});
    }
}

task<void> WorkflowSupervisor::run_executor_job(
    ExecutorBody body, agentengine::Message payload, agentengine::EffectContext ctx,
    std::shared_ptr<ExecuteReply> out,
    std::shared_ptr<agentengine::workflow::multiplex_sink<agentengine::workflow::WorkflowEvent>> sink,
    std::string executor_id, std::uint32_t round, std::uint32_t attempt,
    std::vector<std::string> path_prefix) {
    if (sink) {
        ctx.agent_turn_sink = [sink, executor_id, round, attempt,
                                path_prefix](agentengine::RunEvent const& ev) {
            agentengine::workflow::WorkflowEvent we;
            we.kind    = agentengine::workflow::workflow_event_kind::agent_turn_event;
            we.round   = round;
            we.payload = agentengine::workflow::workflow_event_payload::AgentTurn{
                executor_id, attempt, ev, path_prefix};
            (void)sink->push(std::move(we));
        };
        ctx.moderator_delta_sink = [sink, executor_id, round, attempt,
                                     path_prefix](std::string const& delta) {
            agentengine::workflow::WorkflowEvent we;
            we.kind    = agentengine::workflow::workflow_event_kind::moderator_stream_delta;
            we.round   = round;
            we.payload = agentengine::workflow::workflow_event_payload::ModeratorDelta{
                executor_id, attempt, delta, path_prefix};
            (void)sink->push(std::move(we));
        };
    }
    if (!body) {
        *out = ExecuteReply{agentengine::Message{}, {}, false, agentengine::failure_class::contract,
                             false, std::nullopt, agentengine::Usage{}};
        co_return;
    }
    // ADR-193 round 2: what the body spent that its outcome will not carry -- above all a FAILED agent node's
    // whole run (`agent_session_as_executor_body` charges it here, since an error has no usage field). Summed
    // into `out->usage` as it arrives, so a body that throws still leaves it behind for the collector.
    auto const charge_mutex = std::make_shared<std::mutex>();
    ctx.charge_delegated_usage = [out, charge_mutex](agentengine::Usage const& u, std::uint64_t) {
        std::lock_guard<std::mutex> lock(*charge_mutex);
        add_usage(out->usage, u);
    };
    agentengine::result<ExecutorOutcome> outcome = body(payload, ctx);
    agentengine::Usage charged;
    {
        std::lock_guard<std::mutex> lock(*charge_mutex);
        charged = out->usage;
    }
    if (!outcome) {
        *out = ExecuteReply{agentengine::Message{}, {}, false, outcome.error().klass, false, std::nullopt,
                             charged};
        co_return;
    }
    add_usage(outcome->usage, charged);
    // GitHub issue #35 follow-up (ADR-163) -- the one real link this class's own positional
    // ExecuteReply{...} construction was silently dropping: ExecutorOutcome::usage never survived
    // into the ExecuteReply the per-round fold loop later accumulates from, so
    // WorkflowSupervisor::usage() stayed zero even for a real agent-kind dispatch with real,
    // scripted cost. Threaded here as the trailing positional arg (not a post-construction
    // assignment) -- every field explicit, matching the missing-field-initializer discipline the
    // origin/main CI fix (immediately above this class) established for
    // `pending_sub_workflow_inner_interaction_id`; an implicit trailing default here would
    // reintroduce that exact -Werror=missing-field-initializers failure on gcc/clang for this
    // construction's own now-8-field aggregate.
    *out = ExecuteReply{std::move(outcome->payload), std::move(outcome->routes), true,
                         agentengine::failure_class::fatal, outcome->stalled, std::nullopt,
                         outcome->usage};
    co_return;
}

task<void> WorkflowSupervisor::run_sub_workflow_job(
    std::shared_ptr<WorkflowSupervisor> inner, agentengine::Message payload,
    std::shared_ptr<ExecuteReply> out,
    std::shared_ptr<agentengine::workflow::multiplex_sink<agentengine::workflow::WorkflowEvent>> sink,
    std::vector<std::string> path_prefix) {
    if (!inner) {
        *out = ExecuteReply{agentengine::Message{}, {}, false, agentengine::failure_class::contract,
                             false, std::nullopt, agentengine::Usage{}};
        co_return;
    }
    WorkflowResult r;
    {
        ScopedForwardedEventSink const guard(*inner, std::move(sink), std::move(path_prefix));
        r = drive(inner->run_workflow(RunWorkflow{payload}));
    }
    if (r.status == workflow_status::completed) {
        // GitHub issue #35 follow-up (ADR-163): a sub_workflow that completes SYNCHRONOUSLY within
        // one round (never suspends) -- its own inner->usage() is read here and bubbled straight
        // into the outer run's total via the ordinary replies[] fold, the same path an agent-kind
        // node's own usage already takes. The SUSPENDED branch below intentionally does NOT do this
        // -- see OpenPort::usage's own comment for why that case is captured retroactively instead,
        // at final resolution. ADR-193 §9: `r.usage` (this fresh run's whole spend, read under the inner
        // supervisor's lock), the same number `inner->usage()` gave while nothing else drives `inner`.
        *out = ExecuteReply{r.output, {}, true, agentengine::failure_class::fatal, false, std::nullopt,
                             r.usage};
        co_return;
    }
    if (r.status == workflow_status::suspended) {
        ExecuteReply reply{};
        reply.pending_sub_workflow_inner_interaction_id =
            r.open_interactions.empty() ? std::string{} : r.open_interactions.front().interaction_id;
        *out = std::move(reply);
        co_return;
    }
    // ADR-193 round 2: a failed nested run still spent what it spent.
    *out = ExecuteReply{agentengine::Message{}, {}, false, agentengine::failure_class::fatal, false,
                         std::nullopt, r.usage};
    co_return;
}

task<WorkflowResult> WorkflowSupervisor::execute() {
    auto const entered_at = std::chrono::steady_clock::now();
    workflow_status status = workflow_status::completed;

    if (!ports_.empty()) {
        std::vector<Delivery> next = state_.pending;
        fan_in_edges_this_round_.clear();
        for (auto const& p : ports_) {
            push_structural_event(
                agentengine::workflow::workflow_event_kind::request_port_resolved,
                agentengine::workflow::workflow_event_payload::PortRef{
                    graph_.executors[p.executor_index].id, p.interaction.interaction_id});
            ExecuteReply const reply{p.response, p.routes, true, agentengine::failure_class::fatal,
                                      false, std::nullopt, p.usage};
            accumulate_usage(p.usage);  // GitHub issue #35 follow-up -- see OpenPort::usage's comment
            record_partial(state_.partial, p.executor_index, rounds_ - 1, p.response);
            if (is_output_selected(p.executor_index)) state_.selected_output = p.response;
            route_result const rr = route_from(p.executor_index, reply, next);
            if (rr == route_result::ok) continue;
            state_.failed_executor = graph_.executors[p.executor_index].id;
            status = rr == route_result::routing_failed ? workflow_status::routing_failed
                                                          : workflow_status::executor_failed;
            ports_.clear();
            push_fan_in_aggregated_events();
            co_return finish(status, entered_at);
        }
        ports_.clear();
        push_fan_in_aggregated_events();
        state_.pending = std::move(next);
    }

    while (!state_.pending.empty()) {
        if (graph_.bound.max_rounds.has_value() && rounds_ >= *graph_.bound.max_rounds) {
            status = workflow_status::bound_max_rounds;
            break;
        }
        if (graph_.bound.deadline_ms.has_value()) {
            auto const elapsed = std::chrono::nanoseconds{state_.elapsed_ns} +
                                 (std::chrono::steady_clock::now() - entered_at);
            auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            if (static_cast<std::uint64_t>(ms) >= *graph_.bound.deadline_ms) {
                status = workflow_status::bound_deadline;
                break;
            }
        }
        // docs/planning/workflow-mid-run-cancellation-design-draft.md (issue #37): checked at
        // the EXACT SAME point every other termination bound already is -- never a special
        // early gate before the port-resolution prologue above (max_rounds/deadline_ms don't
        // gate that block either, so cancellation doesn't, for consistency). NOT a stronger
        // guarantee than any existing bound already offers: a round already in flight (a wide
        // fan-out, or a sub_workflow dispatch running an entire nested multi-round sub-run
        // synchronously) still runs to completion before this is rechecked -- see cancel_source_
        // 's own comment and the design draft §4 for why that is an inherited, not new,
        // characteristic.
        if (cancel_source_.stop_requested()) {
            status = workflow_status::cancelled;
            break;
        }

        std::vector<Delivery> exec_deliveries;
        std::vector<Delivery> port_deliveries;
        // ADR-157 (issues #33/#38): a third bucket, gathered the same way -- see the design
        // draft §3b. Dispatched separately from exec_deliveries below (own quarantine, own
        // retry loop, own job function -- there is no ExecutorBody/bodies_[idx] for a
        // sub_workflow-kind node, exactly like request_port has none), then a RESOLVED
        // (non-pending) sub_workflow reply is appended onto exec_deliveries/replies before the
        // existing fold loop runs, so every downstream loop (fold/merge-hook/routing/stall-
        // report/eventing) needs zero new branches -- a resolved sub_workflow entry looks
        // exactly like an ordinary exec_deliveries entry to all of them.
        std::vector<Delivery> sub_workflow_deliveries;
        for (auto& d : state_.pending) {
            agentengine::workflow::executor_kind const kind = graph_.executors[d.executor_index].kind;
            if (kind == agentengine::workflow::executor_kind::request_port) {
                port_deliveries.push_back(std::move(d));
            } else if (kind == agentengine::workflow::executor_kind::sub_workflow) {
                sub_workflow_deliveries.push_back(std::move(d));
            } else {
                exec_deliveries.push_back(std::move(d));
            }
        }

        {
            std::vector<std::string> ids;
            ids.reserve(exec_deliveries.size() + port_deliveries.size() + sub_workflow_deliveries.size());
            for (auto const& d : exec_deliveries) ids.push_back(graph_.executors[d.executor_index].id);
            for (auto const& d : port_deliveries) ids.push_back(graph_.executors[d.executor_index].id);
            for (auto const& d : sub_workflow_deliveries) ids.push_back(graph_.executors[d.executor_index].id);
            push_structural_event(agentengine::workflow::workflow_event_kind::superstep_started,
                                   agentengine::workflow::workflow_event_payload::SuperstepBounds{
                                       std::move(ids)});
        }

        // OQ-19 design draft §5 item 2: two ordinary (non-fan_in) edges converging on the SAME
        // agent-kind node in one round would otherwise submit two concurrent `Delivery` entries
        // for the same `AgentSession` -- `AgentSession::start_run()`'s `session_mutex_.lock()`
        // genuinely parks a contended waiter and resumes it from a DIFFERENT thread, which the
        // naive "resume until done" drive loop `rt::agent_session_as_executor_body()` uses
        // (rt/agent_workflow_executor.hpp) cannot survive. Detected HERE, at gather time, before
        // any `pool_.submit()` -- `exec_deliveries` is built once, fully, above, so this cannot be
        // evaded across the retry-attempt loop below. Only the SECOND-and-later delivery to a
        // given agent-kind `executor_index` this round is quarantined (synthetically failed,
        // `contract`-class -- never retried, since `is_retryable()` excludes that class -- and
        // never dispatched at all); the FIRST still runs normally, and every OTHER unrelated
        // delivery this round is completely unaffected. This reuses the EXISTING failure-policy/
        // retry/fallback machinery (a quarantined entry is simply `!ok`, routed exactly like any
        // other real executor failure per that node's own declared edge policy) rather than
        // aborting the whole round -- an earlier "abort the round" resolution was rejected as
        // strictly harsher than the existing `broke` failure path (see the design draft).
        std::vector<bool> quarantined(exec_deliveries.size(), false);
        {
            std::vector<std::size_t> seen_agent_indices;
            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                std::size_t const idx = exec_deliveries[i].executor_index;
                if (graph_.executors[idx].kind != agentengine::workflow::executor_kind::agent) {
                    continue;
                }
                bool seen = false;
                for (std::size_t const s : seen_agent_indices) {
                    if (s == idx) { seen = true; break; }
                }
                if (seen) {
                    quarantined[i] = true;
                    continue;
                }
                seen_agent_indices.push_back(idx);
            }
        }

        std::vector<ExecuteReply> replies(exec_deliveries.size());
        std::vector<std::size_t>  todo;
        todo.reserve(exec_deliveries.size());
        for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
            if (quarantined[i]) {
                replies[i] = ExecuteReply{agentengine::Message{}, {}, false,
                                           agentengine::failure_class::contract, false, std::nullopt,
                                           agentengine::Usage{}};
            } else {
                todo.push_back(i);
            }
        }

        // ADR-152: the round number every executor_dispatched/agent_turn_event/moderator_stream_
        // delta this round is tagged with -- computed once, BEFORE the retry-attempt loop below
        // (round doesn't change across retries within one round; ++rounds_ hasn't run yet),
        // matching what record_partial()'s own post-increment `rounds_ - 1` will resolve to
        // once `++rounds_` DOES run a few lines down.
        std::uint32_t const this_round = static_cast<std::uint32_t>(rounds_) + 1;

        for (std::uint32_t attempt = 0; !todo.empty(); ++attempt) {
            // ---- decision 5, first half: ISSUE every job before awaiting any ----------------
            std::vector<std::future<JobOutcome>>        in_flight;
            std::vector<std::shared_ptr<ExecuteReply>>  slots;
            in_flight.reserve(todo.size());
            slots.reserve(todo.size());
            auto const event_sink = workflow_event_stream_enabled_ ? multiplex_sink_ : nullptr;
            for (std::size_t const i : todo) {
                std::size_t const idx = exec_deliveries[i].executor_index;
                auto slot = std::make_shared<ExecuteReply>();
                slots.push_back(slot);
                push_structural_event(
                    agentengine::workflow::workflow_event_kind::executor_dispatched,
                    agentengine::workflow::workflow_event_payload::ExecutorRef{graph_.executors[idx].id});
                // issue #37: EffectContext already IS the per-call vehicle for exactly this kind
                // of opt-in signal (mirrors capabilities/bound_capabilities) -- a copy with
                // cancellation wired on, rather than one more explicit parameter on
                // run_executor_job() alongside sink/executor_id/round/attempt/path_prefix.
                agentengine::EffectContext ctx = contexts_[idx];
                ctx.cancellation                = cancel_source_.get_token();
                in_flight.push_back(pool_.submit(run_executor_job(
                    bodies_[idx], exec_deliveries[i].payload, std::move(ctx), slot, event_sink,
                    graph_.executors[idx].id, this_round, attempt, event_path_prefix_)));
            }

            // ---- decision 5, second half: COLLECT in fixed index order ---------------------
            std::vector<std::size_t> retry_next;
            for (std::size_t k = 0; k < in_flight.size(); ++k) {
                std::size_t const i       = todo[k];
                JobOutcome         outcome = in_flight[k].get();
                if (outcome.faulted) {
                    // A throwing executor body -- see file banner for why this is classified
                    // transient rather than needing a restart-budget mechanism of its own.
                    replies[i] = ExecuteReply{agentengine::Message{}, {}, false,
                                               agentengine::failure_class::transient, false, std::nullopt,
                                               slots[k]->usage};  // ADR-193 round 2: charged before it threw
                } else {
                    replies[i] = std::move(*slots[k]);
                }
                if (replies[i].ok) continue;
                // ADR-193 round 2: a failed attempt's spend is counted HERE, per attempt -- a retry overwrites
                // replies[i], and the fold below skips failed replies (it used to be the only place usage was
                // counted, so a failing agent node's whole run vanished from `usage()`).
                accumulate_usage(replies[i].usage);

                EdgeFailurePolicy const pol = policy_for(exec_deliveries[i].executor_index);
                if (pol.kind == agentengine::workflow::edge_failure_policy::retry &&
                    attempt < pol.attempts && is_retryable(replies[i].klass)) {
                    retry_next.push_back(i);
                }
            }
            todo = std::move(retry_next);
        }

        // ADR-157 (issues #33/#38): sub_workflow dispatch -- deliberately sequential AFTER
        // exec_deliveries' own retry loop above completes, not concurrent with it (a documented
        // simplification, trading some fan-out parallelism for a simpler, easier-to-verify
        // implementation in this first pass; see the design draft §3b).
        //
        // Quarantine: the SAME same-round-duplicate-delivery dedup exec_deliveries' own OQ-19
        // block already applies to agent-kind, scoped to this separate list -- only the FIRST
        // same-round delivery to a given sub_workflow executor_index is ever dispatched; a
        // second is synthetically quarantined (contract-class, never retried) and merged in
        // below exactly like any other quarantined failure.
        std::vector<bool> sub_workflow_quarantined(sub_workflow_deliveries.size(), false);
        {
            std::vector<std::size_t> seen_sub_workflow_indices;
            for (std::size_t i = 0; i < sub_workflow_deliveries.size(); ++i) {
                std::size_t const idx = sub_workflow_deliveries[i].executor_index;
                bool seen = false;
                for (std::size_t const s : seen_sub_workflow_indices) {
                    if (s == idx) { seen = true; break; }
                }
                if (seen) {
                    sub_workflow_quarantined[i] = true;
                    continue;
                }
                seen_sub_workflow_indices.push_back(idx);
            }
        }

        std::vector<ExecuteReply> sub_workflow_replies(sub_workflow_deliveries.size());
        {
            std::vector<std::size_t> sub_todo;
            sub_todo.reserve(sub_workflow_deliveries.size());
            for (std::size_t i = 0; i < sub_workflow_deliveries.size(); ++i) {
                if (sub_workflow_quarantined[i]) {
                    sub_workflow_replies[i] = ExecuteReply{agentengine::Message{}, {}, false,
                                                            agentengine::failure_class::contract, false,
                                                            std::nullopt, agentengine::Usage{}};
                } else {
                    sub_todo.push_back(i);
                }
            }
            // ADR-152/issue #42 item 3: `event_sink` mirrors the identical local exec_deliveries'
            // own retry loop above already computes -- re-derived here since that one is scoped
            // to that loop's own block.
            auto const event_sink = workflow_event_stream_enabled_ ? multiplex_sink_ : nullptr;
            for (std::uint32_t attempt = 0; !sub_todo.empty(); ++attempt) {
                std::vector<std::future<JobOutcome>>       sub_in_flight;
                std::vector<std::shared_ptr<ExecuteReply>> sub_slots;
                sub_in_flight.reserve(sub_todo.size());
                sub_slots.reserve(sub_todo.size());
                for (std::size_t const i : sub_todo) {
                    std::size_t const idx = sub_workflow_deliveries[i].executor_index;
                    auto slot = std::make_shared<ExecuteReply>();
                    sub_slots.push_back(slot);
                    push_structural_event(
                        agentengine::workflow::workflow_event_kind::executor_dispatched,
                        agentengine::workflow::workflow_event_payload::ExecutorRef{
                            graph_.executors[idx].id});
                    // issue #42 item 3: this dispatch's own child path prefix is THIS
                    // supervisor's current event_path_prefix_ plus the sub_workflow node's own
                    // local id -- computed fresh here, at dispatch time, never cached on `inner`
                    // across calls (see event_path_prefix_'s own comment for why).
                    std::vector<std::string> child_path = event_path_prefix_;
                    child_path.push_back(graph_.executors[idx].id);
                    sub_in_flight.push_back(pool_.submit(run_sub_workflow_job(
                        sub_workflows_.count(idx) ? sub_workflows_.at(idx) : nullptr,
                        sub_workflow_deliveries[i].payload, slot, event_sink,
                        std::move(child_path))));
                }
                std::vector<std::size_t> sub_retry_next;
                for (std::size_t k = 0; k < sub_in_flight.size(); ++k) {
                    std::size_t const i       = sub_todo[k];
                    JobOutcome         outcome = sub_in_flight[k].get();
                    if (outcome.faulted) {
                        sub_workflow_replies[i] = ExecuteReply{agentengine::Message{}, {}, false,
                                                                agentengine::failure_class::transient,
                                                                false, std::nullopt,
                                                                agentengine::Usage{}};
                    } else {
                        sub_workflow_replies[i] = std::move(*sub_slots[k]);
                    }
                    if (sub_workflow_replies[i].ok ||
                        sub_workflow_replies[i].pending_sub_workflow_inner_interaction_id) {
                        continue;
                    }
                    accumulate_usage(sub_workflow_replies[i].usage);  // ADR-193 round 2, as for exec_deliveries
                    EdgeFailurePolicy const pol = policy_for(sub_workflow_deliveries[i].executor_index);
                    if (pol.kind == agentengine::workflow::edge_failure_policy::retry &&
                        attempt < pol.attempts && is_retryable(sub_workflow_replies[i].klass)) {
                        sub_retry_next.push_back(i);
                    }
                }
                sub_todo = std::move(sub_retry_next);
            }
        }

        // Split sub_workflow_replies: a PENDING one becomes a tracked nested interaction (never
        // a reply); everything else (completed or terminally failed) is appended onto
        // exec_deliveries/replies, reusing every downstream loop unchanged (§3d of the design
        // draft).
        for (std::size_t i = 0; i < sub_workflow_deliveries.size(); ++i) {
            std::size_t const idx = sub_workflow_deliveries[i].executor_index;
            if (sub_workflow_replies[i].pending_sub_workflow_inner_interaction_id) {
                agentengine::Interaction const interaction = mint_interaction(idx);
                pending_sub_workflows_[interaction.interaction_id] =
                    PendingSubWorkflow{interaction, idx,
                                       *sub_workflow_replies[i].pending_sub_workflow_inner_interaction_id};
                push_structural_event(
                    agentengine::workflow::workflow_event_kind::request_port_opened,
                    agentengine::workflow::workflow_event_payload::PortRef{
                        graph_.executors[idx].id, interaction.interaction_id});
                continue;
            }
            exec_deliveries.push_back(std::move(sub_workflow_deliveries[i]));
            replies.push_back(std::move(sub_workflow_replies[i]));
        }

        ++rounds_;

        bool merge_failed = false;
        for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
            std::size_t const idx = exec_deliveries[i].executor_index;
            push_structural_event(
                agentengine::workflow::workflow_event_kind::executor_completed,
                agentengine::workflow::workflow_event_payload::ExecutorResult{
                    graph_.executors[idx].id, replies[i].ok});
            if (!replies[i].ok) continue;
            accumulate_usage(replies[i].usage);  // GitHub issue #35 follow-up
            record_partial(state_.partial, idx, rounds_ - 1, replies[i].payload);
            if (is_output_selected(idx)) {
                state_.selected_output = replies[i].payload;
            }
            // 025 §4 / ADR-055 follow-up -- see file banner's "Merge-on-join hook" paragraph.
            // `!merge_failed` stops attempting further same-round merges once one has already
            // failed (the round is terminating regardless; matches the routing loop's own
            // break-on-first-failure convention below).
            if (merge_on_join_hook_ && !merge_failed &&
                graph_.executors[idx].worktree_mode == agentengine::sharing_mode::branch) {
                agentengine::result<void> merged = merge_on_join_hook_(graph_.executors[idx].id);
                if (!merged) {
                    state_.failed_executor = graph_.executors[idx].id;
                    merge_failed = true;
                    push_structural_event(
                        agentengine::workflow::workflow_event_kind::merge_conflict,
                        agentengine::workflow::workflow_event_payload::MergeRef{graph_.executors[idx].id});
                } else {
                    push_structural_event(
                        agentengine::workflow::workflow_event_kind::merge_completed,
                        agentengine::workflow::workflow_event_payload::MergeRef{graph_.executors[idx].id});
                }
            }
        }
        if (merge_failed) {
            status = workflow_status::merge_conflict;
            for (auto const& d : port_deliveries) {
                state_.unopened_ports.push_back(graph_.executors[d.executor_index].id);
            }
            break;
        }

        std::vector<Delivery> next;
        bool                  broke = false;
        fan_in_edges_this_round_.clear();
        // Issue #62: every multi-source fan_in target's hold already exists by now -- seeded
        // once, up front, by `seed_fan_in_holds()` (called from `run_workflow()`, before round 1
        // -- see that function's own comment for why a per-round registration pass here, like the
        // since-removed `register_fan_in_holds()` used to run, is no longer needed).
        for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
            bool const quarantine_echo = is_same_round_quarantine_echo(exec_deliveries, replies, i);
            route_result const rr =
                route_from(exec_deliveries[i].executor_index, replies[i], next, quarantine_echo);
            if (rr == route_result::ok) continue;
            state_.failed_executor = graph_.executors[exec_deliveries[i].executor_index].id;
            status = rr == route_result::routing_failed ? workflow_status::routing_failed
                                                          : workflow_status::executor_failed;
            broke = true;
            break;
        }
        push_fan_in_aggregated_events();
        if (broke) {
            for (auto const& d : port_deliveries) {
                state_.unopened_ports.push_back(graph_.executors[d.executor_index].id);
            }
            break;
        }

        // ADR-149 (issue #28 item 2): stall/reset safety valve. Checked here, using THIS round's
        // already-computed `replies`, in the exact same position as the `broke`/`merge_failed`
        // checks above -- so a trip that ends the run gets the identical terminal-path treatment
        // (unresolved `port_deliveries` become `unopened_ports`, `ports_` stays untouched, never
        // reported as a stale open interaction on a run that has actually ended). This position
        // also FIXES the precedence against `max_rounds`/`deadline_ms`, which are only re-checked
        // at the TOP of the loop for a would-be next round: a stall/reset trip on round N always
        // takes effect before that next check is ever reached (ADR-149 §3 finding 5).
        if (!designated_stall_reporter_.empty()) {
            // A REAL bug an early implementation had, caught by an end-to-end test against a
            // builder-produced manager/participant graph (not the self-loop-only unit tests
            // written first, none of which happened to exercise this): the designated reporter
            // does NOT run every round in a normal manager/participant alternation (the manager
            // runs, then a participant runs, then the manager again). Resetting stall_streak_ on
            // ANY round the reporter didn't run -- as an earlier version of this block did --
            // means the streak can never accumulate past 1 in that shape, silently defeating
            // max_stalls for exactly the graph this feature targets. A round the reporter did
            // not run in is NEUTRAL (leaves stall_streak_ unchanged) -- only a round the reporter
            // DID run in updates it, from that reply's own `stalled` value.
            //
            // A SECOND real bug, found by an independent post-implementation audit: the
            // quarantine block above only dedupes concurrent same-round deliveries to an
            // `agent`-kind executor -- a `function`-kind (or any non-agent-kind) designated
            // reporter can genuinely receive TWO deliveries in one round (e.g. two ordinary
            // edges converging on it), and an earlier version of this loop took only the FIRST
            // matching delivery's `stalled` value and `break`-ed, silently discarding a real
            // stall self-report on the second. Fixed by OR-aggregating `stalled` across EVERY
            // delivery to the reporter's index this round -- a safety valve must fail toward
            // counting a real stall report, not discarding one because of dispatch order.
            bool reporter_ran     = false;
            bool reporter_stalled = false;
            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                if (graph_.executors[exec_deliveries[i].executor_index].id != designated_stall_reporter_) {
                    continue;
                }
                reporter_ran = true;
                if (replies[i].stalled) reporter_stalled = true;
            }
            if (reporter_ran) {
                stall_streak_ = reporter_stalled ? stall_streak_ + 1 : 0;
            }

            if (graph_.bound.max_stalls.has_value() && stall_streak_ >= *graph_.bound.max_stalls) {
                ++resets_used_;
                stall_streak_ = 0;
                bool const trip_ends_run = !graph_.bound.max_resets.has_value() ||
                                            resets_used_ > *graph_.bound.max_resets;
                if (trip_ends_run) {
                    status = graph_.bound.max_resets.has_value() ? workflow_status::bound_max_resets
                                                                   : workflow_status::bound_max_stalls;
                    for (auto const& d : port_deliveries) {
                        state_.unopened_ports.push_back(graph_.executors[d.executor_index].id);
                    }
                    break;
                }
                // Under the ceiling: this reset is silently absorbed and the run continues -- MAF's
                // own "force a replan, capped total resets" shape. The engine never forces a
                // replan itself; that stays the moderator's own job on its next invocation (014
                // §3: "safety valve, not the termination contract").
            }
        }

        for (auto const& d : port_deliveries) {
            agentengine::Interaction const interaction = mint_interaction(d.executor_index);
            push_structural_event(
                agentengine::workflow::workflow_event_kind::request_port_opened,
                agentengine::workflow::workflow_event_payload::PortRef{
                    graph_.executors[d.executor_index].id, interaction.interaction_id});
            ports_.push_back(OpenPort{interaction, d.executor_index, d.payload, {}, false,
                                       agentengine::Usage{}});
        }
        state_.pending = std::move(next);

        // 014 §5: "Checkpoint at superstep boundaries." Right here -- round N's results are fully
        // folded into state_/ports_, round N+1 (or a suspension) has not started yet. Fires
        // whether or not this round is about to suspend, so a checkpoint taken here always has
        // enough to resume from either way -- see file banner's "Checkpoint hook" paragraph.
        if (checkpoint_hook_) {
            checkpoint_hook_(rounds_, to_record());
            push_structural_event(agentengine::workflow::workflow_event_kind::checkpoint_saved,
                                   agentengine::workflow::workflow_event_payload::CheckpointSaved{rounds_});
        }

        // 014 §7's live-view bullet, same superstep boundary -- see file banner. `exec_deliveries`/
        // `replies`/`port_deliveries` are still this iteration's locals, built fresh from THIS
        // round, not reconstructed from `state_` (which no longer distinguishes "ran ok" from "ran
        // and failed" once folded into `partial`/`unopened_ports`).
        if (live_view_producer_.valid()) {
            agentengine::workflow::WorkflowLiveEvent ev;
            ev.round = rounds_;
            ev.executor_states.reserve(exec_deliveries.size() + port_deliveries.size());
            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                auto const st = replies[i].ok ? agentengine::workflow::executor_live_state::ran_ok
                                                : agentengine::workflow::executor_live_state::ran_failed;
                ev.executor_states.push_back(agentengine::workflow::ExecutorLiveState{
                    graph_.executors[exec_deliveries[i].executor_index].id, st});
            }
            for (auto const& d : port_deliveries) {
                ev.executor_states.push_back(agentengine::workflow::ExecutorLiveState{
                    graph_.executors[d.executor_index].id,
                    agentengine::workflow::executor_live_state::port_open});
            }
            ev.in_flight_message_count = state_.pending.size();
            (void)live_view_producer_.push(std::move(ev));
        }
        push_structural_event(agentengine::workflow::workflow_event_kind::superstep_completed);

        // ADR-157: also checks pending_sub_workflows_ -- a round whose ONLY "open" thing is a
        // newly-suspended nested sub-workflow (no ordinary ports_, state_.pending now empty)
        // must still report suspended, not fall through to `completed` (this round loop's own
        // initial status).
        if (!ports_.empty() || !pending_sub_workflows_.empty()) {
            status = workflow_status::suspended;
            break;
        }
    }

    co_return finish(status, entered_at);
}

WorkflowResult WorkflowSupervisor::finish(workflow_status status,
                                          std::chrono::steady_clock::time_point entered_at) {
    state_.elapsed_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - entered_at)
                             .count();
    WorkflowResult r{};
    r.status          = status;
    r.rounds          = rounds_;
    r.output          = state_.selected_output;
    r.partial         = state_.partial;
    r.failed_executor = state_.failed_executor;
    // ADR-157 (issues #33/#38): UNCONDITIONAL, not gated behind `status == suspended` anymore.
    // A sub_workflow node can suspend (added to pending_sub_workflows_) and then a DIFFERENT
    // executor's failure abort the SAME round with a non-suspended status -- pending_sub_
    // workflows_ is a persistent member, never cleared by an unrelated abort, and the caller
    // must still be told a real, live nested interaction exists regardless of the round's own
    // dominant terminal reason (design draft §3e/§4). For every run that has neither open
    // ports_ nor pending sub-workflows (the overwhelming common case), open_interactions()
    // still returns empty -- this is a strict widening of what gets reported, never a behavior
    // change for a graph with no request_port/sub_workflow nodes.
    r.open_interactions = open_interactions();
    r.unopened_ports = state_.unopened_ports;

    // ADR-152 (issue #29): the one choke point every execute()-driven terminal outcome passes
    // through -- covers the round-loop happy path AND every early `co_return finish(...)` this
    // file has (the port-prologue's own routing failure, `merge_failed`, `broke`, the stall/
    // reset trip, `bound_max_rounds`/`bound_deadline`). A consumer reads THIS event, not stream
    // termination, to learn "the run is over" -- see workflow/workflow_event.hpp's own
    // WorkflowEventStream comment for why the underlying producer is never explicitly closed.
    using agentengine::workflow::workflow_event_kind;
    using agentengine::workflow::workflow_event_payload::RunFailed;
    switch (status) {
        case workflow_status::completed:
            push_structural_event(workflow_event_kind::workflow_run_completed);
            break;
        case workflow_status::suspended:
            push_structural_event(workflow_event_kind::workflow_run_suspended);
            break;
        default:
            push_structural_event(workflow_event_kind::workflow_run_failed,
                                   RunFailed{workflow_status_tag(status)});
            break;
    }
    return r;
}

agentengine::Interaction WorkflowSupervisor::mint_interaction(std::size_t executor_index) const {
    agentengine::Interaction i{};
    i.interaction_id = run_id_ + ":port:" + graph_.executors[executor_index].id + ":" +
                       std::to_string(rounds_ == 0 ? 0 : rounds_ - 1);
    i.run_id = run_id_;
    i.reason = agentengine::interaction_reason::input;
    return i;
}

WorkflowSupervisor::route_result WorkflowSupervisor::route_from(std::size_t from_index, ExecuteReply const& reply,
                                                                std::vector<Delivery>& next, bool is_quarantine_echo) {
    using agentengine::workflow::edge_kind;
    using agentengine::workflow::edge_failure_policy;
    std::string const& from_id = graph_.executors[from_index].id;

    if (!reply.ok) {
        EdgeFailurePolicy const pol    = policy_for(from_index);
        agentengine::Message const marker = failure_marker(from_id, reply.klass);

        switch (pol.kind) {
            case edge_failure_policy::fail:
            case edge_failure_policy::retry:
                return route_result::workflow_failed;

            // GitHub issue #52 FIX (2026-09-03, was a KNOWN GAP), GENERALIZED by issue #62
            // (2026-09-03): found live via tests/test_workflow_research_pipeline_live_e2e.cpp (a
            // production-shaped concurrent research pipeline against real OpenRouter calls),
            // pinned offline first by test_workflow_fanin_concurrent_failure_policy_gap.cpp (#52,
            // since promoted to test_workflow_fanin_concurrent_failure_policy_fix.cpp) and again
            // by test_workflow_fanin_uneven_round_sources_fix.cpp (#62, same promotion). Two
            // composition bugs vs a `fan_in` target a SIBLING executor also delivers to normally,
            // possibly several rounds apart:
            //   - `propagate` used to drop the marker (or not) depending on iteration order --
            //     `deliver_once()` was "insert if absent, else no-op", asymmetric with the
            //     normal-path merge loop below (which always appends). Fixed by making
            //     `deliver_or_merge()` (renamed from `deliver_once()`) use that SAME append-or-
            //     insert semantics for a non-fan_in edge, so whichever of {marker, siblings} is
            //     routed first no longer matters there. For a `fan_in` edge specifically, the
            //     marker now goes through `deliver_to_fan_in()` instead (issue #62) -- it IS that
            //     failed source's own contribution to the barrier below, so it must resolve that
            //     source's slot exactly like a genuine successful delivery would, not bypass the
            //     hold via a raw `next`-append that could dispatch the target before a slower
            //     sibling (issue #62's own general defect) ever gets there.
            //   - `fallback` used to run the shared fan_in target TWICE: the named recovery
            //     executor is a different node whose own rejoining edge can only fire in a LATER
            //     round, so the target dispatched once with the succeeding siblings and again,
            //     overwriting the first, with just the recovery's output. Issue #52 fixed the one
            //     shape it was scoped to (a direct rejoin) via a since-removed per-round
            //     `register_fan_in_holds()`; issue #62 replaced that with a target-wide barrier
            //     seeded once up front (`seed_fan_in_holds()`, `HeldFanIn`'s own comment) covering
            //     EVERY multi-source fan_in target, not just this one shape -- the failed source's
            //     OWN slot in that barrier is resolved here, via `resolve_fan_in_await()`, since it
            //     will never independently deliver; the recovery executor is itself an ordinary
            //     declared source (when its own edge back to `edge.to` is ALSO `fan_in`) and
            //     resolves its own slot normally, later, via `deliver_to_fan_in()`.
            // D4 in tests/test_rt_workflow_supervisor_failure_policies.cpp only ever exercised
            // `fallback` on a single-source (non-fan_in-shared) edge; F1-F4 in
            // test_workflow_fanin_concurrent_failure_policy_fix.cpp assert the CORRECT merged-once
            // behavior this fix produces, closing the 014 §8 G1 gap that combination left unproven.
            case edge_failure_policy::propagate:
                if (is_quarantine_echo) return route_result::ok;
                for (auto const& edge : graph_.edges) {
                    if (edge.from != from_id) continue;
                    std::size_t const target = index_of(edge.to);
                    if (edge.kind == edge_kind::fan_in) {
                        deliver_to_fan_in(next, from_index, target, marker);
                    } else {
                        deliver_or_merge(next, target, marker);
                    }
                }
                return route_result::ok;

            case edge_failure_policy::fallback:
                if (is_quarantine_echo) return route_result::ok;
                for (auto const& edge : graph_.edges) {
                    if (edge.from != from_id) continue;
                    if (edge.on_failure.kind != edge_failure_policy::fallback) continue;
                    deliver_or_merge(next, index_of(edge.on_failure.fallback), marker);
                    // issue #62: this failed source itself will never deliver to `edge.to` --
                    // resolve its own barrier slot (if `edge.to` is a held fan_in target) so the
                    // target isn't left waiting forever on a source that has permanently exited.
                    // A no-op when `edge.to` has no hold (a single-source fan_in target, or a
                    // non-fan_in edge kind entirely).
                    if (edge.kind == edge_kind::fan_in) {
                        resolve_fan_in_await(next, index_of(edge.to), from_index);
                    }
                }
                return route_result::ok;
        }
        return route_result::workflow_failed;
    }

    std::size_t switch_edges = 0;
    std::size_t switch_fired = 0;
    std::vector<std::string> fan_out_targets;
    std::vector<std::string> available_cases;
    std::vector<std::string> chosen_cases;

    for (auto const& edge : graph_.edges) {
        if (edge.from != from_id) continue;
        if (edge.kind == edge_kind::switch_case) ++switch_edges;
        if (edge.kind == edge_kind::switch_case || edge.kind == edge_kind::multi_selection) {
            available_cases.push_back(edge.case_label);
        }
        if (!edge_fires(edge, reply)) continue;
        if (edge.kind == edge_kind::switch_case) ++switch_fired;

        std::size_t const target = index_of(edge.to);
        std::string const& to_id = graph_.executors[target].id;

        if (edge.kind == edge_kind::switch_case || edge.kind == edge_kind::multi_selection) {
            chosen_cases.push_back(edge.case_label);
        }
        if (edge.kind == edge_kind::fan_out) fan_out_targets.push_back(to_id);
        if (edge.kind == edge_kind::fan_in) fan_in_edges_this_round_.emplace_back(target, from_id);

        push_structural_event(
            agentengine::workflow::workflow_event_kind::message_routed,
            agentengine::workflow::workflow_event_payload::MessageRouted{
                from_id, to_id, edge.kind, edge.case_label});

        if (edge.kind == edge_kind::fan_in) {
            deliver_to_fan_in(next, from_index, target, reply.payload);
            continue;
        }
        next.push_back(Delivery{target, reply.payload});
    }

    if (!fan_out_targets.empty()) {
        push_structural_event(
            agentengine::workflow::workflow_event_kind::fan_out_dispatched,
            agentengine::workflow::workflow_event_payload::FanOut{from_id, std::move(fan_out_targets)});
    }
    if (!available_cases.empty()) {
        push_structural_event(
            agentengine::workflow::workflow_event_kind::route_selected,
            agentengine::workflow::workflow_event_payload::RouteSelected{
                from_id, std::move(chosen_cases), std::move(available_cases)});
    }

    if (switch_edges > 0 && switch_fired != 1) return route_result::routing_failed;
    return route_result::ok;
}

void WorkflowSupervisor::push_fan_in_aggregated_events() {
    if (fan_in_edges_this_round_.empty()) return;
    std::vector<std::size_t> targets_seen;
    for (auto const& [target, from_id] : fan_in_edges_this_round_) {
        bool already = false;
        for (std::size_t const t : targets_seen) {
            if (t == target) { already = true; break; }
        }
        if (already) continue;
        targets_seen.push_back(target);
        std::vector<std::string> sources;
        for (auto const& [t2, from_id2] : fan_in_edges_this_round_) {
            if (t2 == target) sources.push_back(from_id2);
        }
        push_structural_event(
            agentengine::workflow::workflow_event_kind::fan_in_aggregated,
            agentengine::workflow::workflow_event_payload::FanIn{
                graph_.executors[target].id, std::move(sources)});
    }
    fan_in_edges_this_round_.clear();
}

bool WorkflowSupervisor::is_same_round_quarantine_echo(std::vector<Delivery> const& exec_deliveries,
                                                       std::vector<ExecuteReply> const& replies,
                                                       std::size_t i) noexcept {
    if (replies[i].ok) return false;
    for (std::size_t j = 0; j < exec_deliveries.size(); ++j) {
        if (j != i && exec_deliveries[j].executor_index == exec_deliveries[i].executor_index &&
            replies[j].ok) {
            return true;
        }
    }
    return false;
}

void WorkflowSupervisor::deliver_or_merge(std::vector<Delivery>& next, std::size_t target,
                                          agentengine::Message const& marker) {
    for (auto& d : next) {
        if (d.executor_index != target) continue;
        for (auto const& item : marker.content) d.payload.content.push_back(item);
        return;
    }
    next.push_back(Delivery{target, marker});
}

void WorkflowSupervisor::deliver_to_fan_in(std::vector<Delivery>& next, std::size_t from_index, std::size_t target,
                                           agentengine::Message const& payload) {
    for (auto it = state_.held_fan_in.begin(); it != state_.held_fan_in.end(); ++it) {
        if (it->executor_index != target) continue;
        if (!it->seeded) {
            it->payload = payload;
            it->seeded  = true;
        } else {
            for (auto const& item : payload.content) it->payload.content.push_back(item);
        }
        for (auto r = it->awaiting_sources.begin(); r != it->awaiting_sources.end(); ++r) {
            if (*r != from_index) continue;
            it->awaiting_sources.erase(r);
            break;
        }
        if (it->awaiting_sources.empty()) {
            agentengine::Message released = std::move(it->payload);
            state_.held_fan_in.erase(it);
            for (auto& delivery : next) {
                if (delivery.executor_index != target) continue;
                for (auto const& item : released.content) delivery.payload.content.push_back(item);
                return;
            }
            next.push_back(Delivery{target, std::move(released)});
        }
        return;
    }
    for (auto& delivery : next) {
        if (delivery.executor_index != target) continue;
        for (auto const& item : payload.content) delivery.payload.content.push_back(item);
        return;
    }
    next.push_back(Delivery{target, payload});
}

void WorkflowSupervisor::resolve_fan_in_await(std::vector<Delivery>& next, std::size_t target, std::size_t from_index) {
    for (auto it = state_.held_fan_in.begin(); it != state_.held_fan_in.end(); ++it) {
        if (it->executor_index != target) continue;
        for (auto r = it->awaiting_sources.begin(); r != it->awaiting_sources.end(); ++r) {
            if (*r != from_index) continue;
            it->awaiting_sources.erase(r);
            break;
        }
        if (!it->awaiting_sources.empty()) return;
        bool const had_content = it->seeded;
        agentengine::Message released = std::move(it->payload);
        state_.held_fan_in.erase(it);
        if (!had_content) return;  // every declared source resolved without ever contributing
        for (auto& delivery : next) {
            if (delivery.executor_index != target) continue;
            for (auto const& item : released.content) delivery.payload.content.push_back(item);
            return;
        }
        next.push_back(Delivery{target, std::move(released)});
        return;
    }
}

void WorkflowSupervisor::seed_fan_in_holds() {
    using agentengine::workflow::edge_kind;
    for (std::size_t target = 0; target < graph_.executors.size(); ++target) {
        std::string const& to_id = graph_.executors[target].id;
        std::vector<std::size_t> sources;
        for (auto const& edge : graph_.edges) {
            if (edge.kind != edge_kind::fan_in || edge.to != to_id) continue;
            std::size_t const from_idx = index_of(edge.from);
            bool already = false;
            for (std::size_t const s : sources) {
                if (s == from_idx) { already = true; break; }
            }
            if (!already) sources.push_back(from_idx);
        }
        if (sources.size() < 2) continue;
        state_.held_fan_in.push_back(
            HeldFanIn{target, agentengine::Message{}, false, std::move(sources)});
    }
}

void WorkflowSupervisor::record_partial(std::vector<ExecutorOutput>& partial, std::size_t executor_index,
                                        std::uint32_t round, agentengine::Message const& payload) const {
    std::string const& id = graph_.executors[executor_index].id;
    for (auto& out : partial) {
        if (out.executor_id != id) continue;
        out.round   = round;
        out.payload = payload;
        return;
    }
    partial.push_back(ExecutorOutput{id, round, payload});
}

void WorkflowSupervisor::add_usage(agentengine::Usage& to, agentengine::Usage const& delta) noexcept {
    to.input_tokens += delta.input_tokens;
    to.output_tokens += delta.output_tokens;
    to.cached_input_tokens += delta.cached_input_tokens;
    to.reasoning_tokens += delta.reasoning_tokens;
    to.cost_estimate += delta.cost_estimate;
    to.cache_write_tokens += delta.cache_write_tokens;
}

WorkflowSupervisor::EdgeFailurePolicy WorkflowSupervisor::policy_for(std::size_t executor_index) const {
    std::string const& id = graph_.executors[executor_index].id;
    for (auto const& edge : graph_.edges) {
        if (edge.from == id) return edge.on_failure;
    }
    return EdgeFailurePolicy{};
}

bool WorkflowSupervisor::edge_fires(agentengine::workflow::Edge const& edge,
                                    ExecuteReply const& reply) noexcept {
    using agentengine::workflow::edge_kind;
    switch (edge.kind) {
        case edge_kind::direct:
        case edge_kind::chain:
        case edge_kind::fan_out:
        case edge_kind::fan_in:
            return true;
        case edge_kind::switch_case:
        case edge_kind::multi_selection:
            for (auto const& route : reply.routes) {
                if (route == edge.case_label) return true;
            }
            return false;
    }
    return false;
}

bool WorkflowSupervisor::agent_kind_bodies_are_structurally_agent_backed() const {
    for (std::size_t i = 0; i < graph_.executors.size(); ++i) {
        if (graph_.executors[i].kind != agentengine::workflow::executor_kind::agent) continue;
        if (i >= bodies_.size() || !bodies_[i] ||
            bodies_[i].target<AgentExecutorBodyTag>() == nullptr) {
            return false;
        }
    }
    return true;
}

bool WorkflowSupervisor::sub_workflow_kind_nodes_are_bound() const {
    for (std::size_t i = 0; i < graph_.executors.size(); ++i) {
        if (graph_.executors[i].kind != agentengine::workflow::executor_kind::sub_workflow) continue;
        auto const it = sub_workflows_.find(i);
        if (it == sub_workflows_.end() || !it->second) return false;
    }
    return true;
}

WorkflowResult WorkflowSupervisor::deny_admission() {
    ++admission_denied_count_;
    push_structural_event(
        agentengine::workflow::workflow_event_kind::workflow_run_failed,
        agentengine::workflow::workflow_event_payload::RunFailed{
            workflow_status_tag(workflow_status::admission_denied)});
    return WorkflowResult{workflow_status::admission_denied};
}

void WorkflowSupervisor::propagate_admission_to_children() {
    for (auto const& [idx, inner] : sub_workflows_) {
        (void)idx;
        if (!inner) continue;
        bool const child_owns_its_config =
            !inner->inherited_admission_ &&
            (!inner->principal_.id.empty() || !inner->principal_.tenant_id.empty());
        if (child_owns_its_config) continue;
        inner->principal_            = principal_;
        inner->require_caller_       = require_caller_;
        inner->inherited_admission_  = true;
        inner->propagate_admission_to_children();
    }
}

void WorkflowSupervisor::push_structural_event(agentengine::workflow::workflow_event_kind kind,
                                               agentengine::workflow::WorkflowEventPayload payload) {
    if (!workflow_event_producer_.valid()) return;
    agentengine::workflow::WorkflowEvent ev;
    ev.kind    = kind;
    ev.round   = rounds_;
    ev.payload = std::move(payload);
    (void)workflow_event_producer_.push(std::move(ev));
}

}  // namespace agentengine::rt
