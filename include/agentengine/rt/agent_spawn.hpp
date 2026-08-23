#pragma once
// Implements docs/planning/agent-spawn-runtime-design-draft.md item 1 (§2, §4.1) --
// 026-Agent-Facing-Runtime-Surface.md §5's `agent.spawn` -- OpenQuestions.md OQ-14, named the
// project's own "sharpest case". This is the REAL, wired, end-to-end call path: a real
// `Tool<>`-conforming surface a running agent can actually invoke, orchestrating already-landed
// items 2 (rt/agent_spawn_child_run.hpp's run_child_agent_session), 3
// (core/agent_spawn_worktree.hpp's mint_spawn_worktree/derive_spawn_child_id/allocate_spawn_seq), 4
// (trust::check_and_consume_spawn_depth/ADR-006, rt::SpawnCostBudget/ADR-031), and 5
// (trust::mint_child_spawn_capabilities, Design B, trust/agent_spawn_capability.hpp) into one
// function real production code can call, per §2's nine-step pipeline.
//
// Item 6 (OQ-16's push_side_summary() wired into session_builder.hpp for the PARENT session, plus a
// computed instructions string for the CHILD) is now LANDED (2026-08-23,
// docs/planning/agent-spawn-runtime-design-draft.md §4.6): `perform_agent_spawn()`'s own step [7]
// below computes `trust::push_side_summary(child_grant->capabilities)` -- the CHILD's own manifest,
// from its own already-attenuated capabilities, never a copy of the caller's -- and passes it as
// `ChildSpawnRequest::instructions`, which `run_child_agent_session()` (item 2,
// rt/agent_spawn_child_run.hpp) now threads into `AgentSession::set_static_instructions()` before
// `start_run()`. The PARENT-session half of item 6 (wiring the identical mechanism into
// `core/session_builder.hpp`'s `QuickstartSessionBuilder`/`ComposedQuickstartSessionBuilder::build()`)
// is a separate file's own change -- see `session_builder.hpp`'s own top comment.
//
// -- §4.4c SpawnPump, built here (bounded to this file, not a separate design/red-team/prove pass) --
// The design doc's own RC-2/WT-2 findings are real and load-bearing, not merely theoretical: a naive
// "while (!t.done()) t.resume()" drive loop (rt/agent_spawn_child_run.hpp's own
// agent_spawn_detail::drive(), reused unmodified below) is safe ONLY when nothing else can ever
// resume the SAME coroutine handle concurrently. `rt::SpawnCostBudget::consume()` (ADR-031) is
// guarded by a real `rt::AsyncMutex`, whose `unlock()` resumes a queued waiter's coroutine handle
// DIRECTLY FROM THE UNLOCKING THREAD (async_mutex.hpp's own file banner) -- under genuine
// cross-thread contention (two `AgentSession`s, on two OS threads, each spawning at once against the
// one host-process-wide `SpawnCostBudget` §4.4b requires), a caller's own naive drive loop can race
// that external resume() and double-resume the same handle: undefined behavior, not a rare edge
// case. `SpawnPump` below closes this by construction: ONE dedicated worker thread is the ONLY
// thread that ever calls `resume()` on a `consume()` coroutine, or mints a `spawn_seq`/child_id, or
// performs `mint_spawn_worktree()`'s ref-store read-then-write -- every other thread only ever
// `submit()`s a request and blocks on a `std::future` for the result. Exactly ONE `SpawnPump`
// instance should exist per host process (constructed alongside the one `SpawnCostBudget` and one
// ref-store instance it owns references to, §4.4b/§4.4c) -- never per-session.
//
// Reuses, never reimplements: trust::SpawnBudget (ADR-006), rt::SpawnCostBudget (ADR-031),
// trust::mint_child_spawn_capabilities()/check_and_consume_spawn_depth() (item 5,
// trust/agent_spawn_capability.hpp), mint_spawn_worktree()/derive_spawn_child_id()/
// allocate_spawn_seq() (item 3, core/agent_spawn_worktree.hpp), run_child_agent_session() (item 2,
// rt/agent_spawn_child_run.hpp). No new capability-minting logic lives here -- this file is
// orchestration/wiring plus the one new SpawnPump synchronization primitive §4.4c calls for.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include "agentengine/core/agent_registry.hpp"        // AgentMetadata
#include "agentengine/core/agent_spawn_worktree.hpp"  // allocate_spawn_seq, derive_spawn_child_id,
                                                       // check_child_id, mint_spawn_worktree
#include "agentengine/core/content.hpp"               // Message, ContentItem, Text, role, content_origin
#include "agentengine/core/context_provider.hpp"      // ContextProvider, ContextContribution, SessionContext
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/sharing_mode.hpp"
#include "agentengine/core/tool.hpp"                  // agentengine::Tool<>
#include "agentengine/core/tool_call_extraction.hpp"  // text_of
#include "agentengine/core/tool_pipeline.hpp"         // make_tool_descriptor_with_invoke
#include "agentengine/core/worktree.hpp"               // Ref
#include "agentengine/rt/agent_spawn_child_run.hpp"    // ChildSpawnRequest, run_child_agent_session,
                                                        // agent_spawn_detail::drive
#include "agentengine/rt/agent_session.hpp"            // AgentResponse
#include "agentengine/rt/append_log_store.hpp"         // AppendLogStore concept
#include "agentengine/rt/spawn_cost_budget.hpp"        // SpawnCostBudget, ConsumeSpawnTokens, SpawnTokenGrant
#include "agentengine/trust/agent_library_manifest.hpp"  // push_side_summary (item 6, OQ-16)
#include "agentengine/trust/agent_spawn_capability.hpp"  // SpawnWorktreeGrant, ChildSpawnGrant,
                                                          // check_and_consume_spawn_depth,
                                                          // mint_child_spawn_capabilities
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/spawn_budget.hpp"

namespace agentengine::rt {

// ---------------------------------------------------------------------------------------------
// §4.1: the model-facing surface.
// ---------------------------------------------------------------------------------------------

struct AgentSpawnArgs {
    std::string agent_id;  // host-curated registry key ONLY -- never trusted as authority (I3); an
                            // id the registry doesn't recognize fails closed at step [1].
    std::string input;     // plain text, becomes the child's one user-role input message.
};
AE_JSON_SCHEMA(AgentSpawnArgs, agent_id, input)
// Deliberately NO depth/budget/ceiling field anywhere on this struct -- there is nothing here for
// model-written code to even attempt to widen (I3): every numeric bound this mechanism enforces
// comes from the CALLER's already-held CapabilitySet/SpawnBudget, never from the call's own args.

struct AgentSpawnReply {
    std::string   output;
    std::uint64_t input_tokens  = 0;
    std::uint64_t output_tokens = 0;
};
AE_JSON_SCHEMA(AgentSpawnReply, output, input_tokens, output_tokens)

// Poison-sentinel CRTP shape, matching ScheduleWakeupTool's own established precedent
// (rt/agent_session.hpp) exactly -- this static invoke() must never actually run; it exists only so
// AgentSpawnTool satisfies Tool<Derived,...>'s contract for make_tool_descriptor_with_invoke<...>()
// to extract Args/Reply/schemas from. No Capabilities<...> policy tag declared here either, for the
// SAME reason ScheduleWakeupTool declares none: enforcement is a LIVE, per-call, per-agent_id check
// (perform_agent_spawn()'s own steps [2]/[3] below) a static compile-time ceiling entry cannot
// express (it would have to name every spawnable agent_id and its own budget at compile time, which
// the model's own call selects at runtime from a HOST-curated set, not a compile-time-fixed one).
struct AgentSpawnTool : agentengine::Tool<AgentSpawnTool> {
    static constexpr std::string_view name = "agent.spawn";
    static constexpr std::string_view description =
        "Run a sub-agent (from a host-curated, closed set) on the given input and return its "
        "converged result. Depth- and cost-budgeted; fails closed if either is exhausted or you "
        "hold no grant for the named agent.";
    using Args  = AgentSpawnArgs;
    using Reply = AgentSpawnReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "agent.spawn invoked without its host-bound dispatch closure -- unreachable",
            "agent_spawn.unreachable_static_invoke"});
    }
};

// ---------------------------------------------------------------------------------------------
// §4.1: host-curated registry -- the identical discipline core/tool_registry.hpp's ToolRegistry
// already establishes ("nothing is ever added except by an explicit register call the host itself
// makes"). Nothing here self-registers; a model's own agent_id argument only ever indexes into this
// closed table, never widens it.
// ---------------------------------------------------------------------------------------------

// Type-erased "construct a fresh, concretely-typed child AgentSession, wire it, drive it to
// completion, tear it down" thunk -- the SAME closure-based erasure ToolDescriptor::invoke already
// performs over a concrete ToolT (core/tool_pipeline.hpp), applied here over a concrete
// ChatClientT/StateT/HistoryProviderT (item 2's own template parameters). Host-authored ONLY, at
// registry-build time -- never anything derived from model output (I3).
using ChildRunner =
    std::function<agentengine::result<agentengine::rt::AgentResponse>(std::string child_id,
                                                                        ChildSpawnRequest request)>;

struct SpawnTargetDescriptor {
    agentengine::AgentMetadata metadata;              // register_agent<TargetAgentType>()'s output
    std::uint64_t    spawn_cost = 1;                  // HOST-configured; never model-supplied (I3)
    agentengine::sharing_mode worktree_mode = agentengine::sharing_mode::branch;  // 025 §3
    // HOST-configured LLM cost ceiling for the CHILD's own run -- never left unbounded (§9 I2-3).
    std::uint64_t    child_token_budget = 50'000;
    ChildRunner       run_child;
};

// HOST-configured, per-caller-Principal soft ceiling on spawn attempts, checked at step [0] --
// BEFORE the shared, non-refundable rt::SpawnCostBudget pool is ever touched (§9 RC-3/WT-5). Bounds
// how much of the WHOLE deployment's shared pool any one caller can burn, adversarially or by
// ordinary child-run failure; it does not itself refund anything (ADR-031's own no-refund-by-design
// choice is unchanged) -- it only bounds the blast radius of one caller against everyone else
// sharing the pool.
struct SpawnQuota {
    std::uint64_t max_spawns_per_principal = 100;  // lifetime-of-process default; host tunes per deployment
};

// Thread-safe (multiple sessions/principals may legitimately spawn concurrently against one
// host-wide instance) -- a plain mutex-guarded counter map, deliberately simpler than SpawnPump
// below: this only ever protects an in-memory std::unordered_map, never a coroutine, so there is no
// AsyncMutex-style cross-thread-resume hazard to close here.
class SpawnQuotaTracker {
public:
    [[nodiscard]] bool try_consume(std::string const& principal_id, std::uint64_t max_spawns) {
        std::lock_guard<std::mutex> lk(m_);
        std::uint64_t& count = counts_[principal_id];
        if (count >= max_spawns) return false;
        ++count;
        return true;
    }

private:
    std::mutex m_;
    std::unordered_map<std::string, std::uint64_t> counts_;
};

class SpawnTargetRegistry {
public:
    [[nodiscard]] agentengine::result<void> register_target(std::string agent_id,
                                                              SpawnTargetDescriptor descriptor) {
        if (targets_.contains(agent_id)) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "a spawn target is already registered under this agent_id",
                "agent_spawn.target_already_registered"});
        }
        targets_.emplace(std::move(agent_id), std::move(descriptor));
        return {};
    }

    [[nodiscard]] SpawnTargetDescriptor const* find(std::string_view agent_id) const {
        auto it = targets_.find(std::string(agent_id));
        return it == targets_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, SpawnTargetDescriptor> targets_;
};

// ---------------------------------------------------------------------------------------------
// §4.4c -- SpawnPump: the single-threaded serialization point. See this file's own top comment for
// the full RC-2/WT-2 rationale. One dedicated worker thread, owned by whoever constructs this
// instance (host bootstrap code, per §4.4b/§4.4c -- exactly one per host process), with a bounded-
// in-practice request queue (bounded by how many concurrent spawns a real deployment ever has in
// flight -- no explicit cap here, matching SpawnCostBudget's own "no admission control of its own"
// scope). EVERY call site that would otherwise touch shared spawn-time state directly --
// SpawnCostBudget::consume(), child_id minting (derive_spawn_child_id() + allocate_spawn_seq()), and
// mint_spawn_worktree()'s read-then-write over the ref_store -- submits a request and blocks the
// CALLING thread on a std::future for the result. The pump's own worker thread is the ONLY thread
// that ever calls resume() on any of these coroutines, or performs the worktree mint's own
// read-then-write, by construction.
// ---------------------------------------------------------------------------------------------

template <agentengine::rt::AppendLogStore StoreT>
class SpawnPump {
public:
    struct SpawnMintRequest {
        std::uint64_t                 cost = 0;
        agentengine::Ref              caller_ref;
        agentengine::Principal        caller_principal;
        agentengine::CapabilitySet const* caller_held = nullptr;  // borrowed; must outlive submit()
        agentengine::sharing_mode     worktree_mode = agentengine::sharing_mode::branch;
        std::string                   caller_mount_id;
    };
    struct SpawnMintResult {
        SpawnTokenGrant                    token_grant;
        std::string                        child_id;
        agentengine::SpawnWorktreeGrant    worktree_grant;
    };

    SpawnPump(SpawnCostBudget& cost_pool, StoreT& ref_store)
        : cost_pool_(cost_pool), ref_store_(ref_store), worker_([this] { run(); }) {}

    ~SpawnPump() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    SpawnPump(SpawnPump const&)            = delete;
    SpawnPump& operator=(SpawnPump const&) = delete;
    SpawnPump(SpawnPump&&)                 = delete;
    SpawnPump& operator=(SpawnPump&&)      = delete;

    // Blocks the calling (arbitrary) thread until the pump's worker thread has processed this
    // request; internally: consume() first (cheapest to fail, and the "spend before worktree"
    // ordering from §2 is preserved inside the pump, not reordered by it), THEN child_id derivation,
    // THEN mint_spawn_worktree(). Exhaustion or mint failure => fail closed, no partial state
    // committed for this request beyond whichever earlier sub-step already genuinely succeeded
    // (matching §2's own accepted residual: a spent cost token is not refunded if the worktree mint
    // fails afterward -- ADR-031's own no-refund-by-design choice, §8/§9 RC-3).
    [[nodiscard]] agentengine::result<SpawnMintResult> submit(SpawnMintRequest req) {
        std::promise<agentengine::result<SpawnMintResult>> prom;
        std::future<agentengine::result<SpawnMintResult>>  fut = prom.get_future();
        {
            std::lock_guard<std::mutex> lk(m_);
            queue_.push_back(Job{std::move(req), std::move(prom)});
        }
        cv_.notify_one();
        return fut.get();
    }

private:
    struct Job {
        SpawnMintRequest                                    req;
        std::promise<agentengine::result<SpawnMintResult>> prom;
    };

    void run() {
        for (;;) {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop_) return;
                continue;
            }
            Job job = std::move(queue_.front());
            queue_.pop_front();
            lk.unlock();
            job.prom.set_value(process(job.req));
        }
    }

    [[nodiscard]] agentengine::result<SpawnMintResult> process(SpawnMintRequest const& req) {
        // §2 [4]: cost consumed FIRST -- cheapest-to-fail mutating step (§2's own ordering
        // rationale). Safe to drive with the naive "resume until done" loop here (reused, unmodified,
        // from rt/agent_spawn_child_run.hpp's own agent_spawn_detail::drive()) for the SAME reason
        // it is safe there: this worker thread processes jobs strictly one at a time, so this call
        // never overlaps with another drive() call touching the SAME cost_pool_ -- there is
        // structurally only one thread that could ever contend cost_pool_'s internal AsyncMutex, by
        // construction (this file's own top comment).
        agentengine::result<SpawnTokenGrant> grant =
            agent_spawn_detail::drive(cost_pool_.consume(ConsumeSpawnTokens{req.cost}));
        if (!grant) return std::unexpected(grant.error());

        std::uint64_t const seq = agentengine::allocate_spawn_seq();
        agentengine::result<std::string> child_id =
            agentengine::derive_spawn_child_id(req.caller_ref, req.caller_principal, seq);
        if (!child_id) return std::unexpected(child_id.error());

        agentengine::result<agentengine::SpawnWorktreeGrant> worktree_grant = agentengine::mint_spawn_worktree(
            ref_store_, req.caller_ref, *child_id, req.worktree_mode, *req.caller_held, req.caller_mount_id);
        if (!worktree_grant) return std::unexpected(worktree_grant.error());

        return SpawnMintResult{*grant, *child_id, *worktree_grant};
    }

    SpawnCostBudget&         cost_pool_;
    StoreT&                  ref_store_;
    std::mutex               m_;
    std::condition_variable  cv_;
    std::deque<Job>          queue_;
    bool                     stop_ = false;
    std::thread              worker_;  // declared LAST: must start only once every member above it
                                        // is already fully constructed (member init runs in
                                        // declaration order regardless of the constructor's own
                                        // init-list order).
};

// ---------------------------------------------------------------------------------------------
// §2 -- the orchestration function, the nine steps in order. `ctx` is the CALLER's own
// EffectContext (the tool-pipeline-supplied one, reaching this via AgentSpawnTool's custom_invoke
// closure, below); `ctx.capabilities` is the ONLY real ceiling this function ever trusts.
// `caller_worktree_ref`/`caller_mount_id` are HOST/session-configured (never derived from the call
// itself, I3) -- see core/agent_spawn_worktree.hpp's own top comment for why `mint_spawn_worktree`
// takes an explicit mount id rather than guessing one from a Ref.
// ---------------------------------------------------------------------------------------------

template <agentengine::rt::AppendLogStore StoreT>
[[nodiscard]] agentengine::result<AgentSpawnReply> perform_agent_spawn(
    AgentSpawnArgs const& args, agentengine::EffectContext& ctx, SpawnTargetRegistry const& registry,
    SpawnPump<StoreT>& pump, SpawnQuota const& quota, SpawnQuotaTracker& quota_tracker,
    agentengine::Ref const& caller_worktree_ref, std::string const& caller_mount_id) {
    // -- [0] GUARD --------------------------------------------------------------------------------
    // Mirrors invoke_agent_tool()'s own explicit null guard (agent_registry.hpp, ADR-059) --
    // restated here in this function's own contract (§9 I3-2), since the two functions take the
    // ceiling input differently (EffectContext::capabilities* vs. a bound CapabilitySet const&).
    if (!ctx.capabilities) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "agent.spawn requires a real, non-null capability set to attenuate from",
            "agent_spawn.no_capabilities"});
    }
    // Per-session/per-principal spawn quota -- a cheap, local, pre-pool gate, checked BEFORE the
    // shared cost pool is ever touched (§4.1, §9 RC-3).
    if (!quota_tracker.try_consume(ctx.principal.id, quota.max_spawns_per_principal)) {
        return std::unexpected(agentengine::error{agentengine::failure_class::resource,
                                                    "per-principal agent.spawn quota exhausted",
                                                    "agent_spawn.quota_exhausted"});
    }

    // -- [1] RESOLVE ------------------------------------------------------------------------------
    // Unknown id and "known but caller holds no grant" (checked at [2]) share ONE error code
    // (agent_spawn.not_available, §9 I3-3) -- a model cannot distinguish "this agent doesn't exist"
    // from "it exists but you were never granted it".
    SpawnTargetDescriptor const* target = registry.find(args.agent_id);
    if (!target) {
        return std::unexpected(agentengine::error{agentengine::failure_class::policy,
                                                    "agent.spawn: unknown or ungranted agent id",
                                                    "agent_spawn.not_available"});
    }

    // -- [2] DEPTH --------------------------------------------------------------------------------
    // trust::check_and_consume_spawn_depth() (item 4/§4.4a, already landed) returns the SAME
    // "agent_spawn.not_available" code when the caller holds no AgentCall grant for this id at all,
    // or ADR-006's own "spawn_budget.depth_exhausted" once a grant is known to exist but is
    // exhausted. Pure -- no shared/durable state touched yet.
    agentengine::result<agentengine::trust::SpawnBudget> depth =
        agentengine::trust::check_and_consume_spawn_depth(*ctx.capabilities, args.agent_id);
    if (!depth) {
        return std::unexpected(depth.error());
    }

    // -- [3] CEILING ------------------------------------------------------------------------------
    // Pure, side-effect-free coverage check, BEFORE either mutating step below (§2's own ordering
    // rationale: no reason to spend a cost token or touch the ref store for a request the ceiling
    // check would reject anyway). The derived set is discarded here; mint_child_spawn_capabilities()
    // (step [6]) re-runs the identical check and is what actually builds the child's real grants.
    agentengine::result<agentengine::CapabilitySet> const covered =
        ctx.capabilities->attenuate(target->metadata.capability_ceiling);
    if (!covered) {
        return std::unexpected(covered.error());
    }

    // -- [4]/[5] PUMP-ENTER + WORKTREE --------------------------------------------------------------
    // Cost consumed, child_id minted, and the worktree mint performed, all as ONE serialized unit on
    // SpawnPump (§4.4c) -- *** first real, mutating side effects ***.
    typename SpawnPump<StoreT>::SpawnMintRequest mint_request;
    mint_request.cost             = target->spawn_cost;
    mint_request.caller_ref       = caller_worktree_ref;
    mint_request.caller_principal = ctx.principal;
    mint_request.caller_held      = ctx.capabilities.get();
    mint_request.worktree_mode    = target->worktree_mode;
    mint_request.caller_mount_id  = caller_mount_id;

    agentengine::result<typename SpawnPump<StoreT>::SpawnMintResult> minted_pump =
        pump.submit(std::move(mint_request));
    if (!minted_pump) {
        return std::unexpected(minted_pump.error());
    }

    // -- [6] MINT ---------------------------------------------------------------------------------
    // Design B (§3/§4.5, item 5, already landed) -- final child CapabilitySet: [3]'s verified-
    // covered target ceiling, AgentCall entries re-rooted to the tighter bound, plus [5]'s scoped
    // worktree grants appended.
    agentengine::result<agentengine::trust::ChildSpawnGrant> child_grant =
        agentengine::trust::mint_child_spawn_capabilities(*ctx.capabilities, target->metadata,
                                                            minted_pump->worktree_grant, *depth);
    if (!child_grant) {
        return std::unexpected(child_grant.error());
    }

    // -- [7]/[8] IDENTITY + RUN ---------------------------------------------------------------------
    // Item 2 (already landed): run_child_agent_session() derives the child's Principal itself
    // (derive_on_behalf_of, 018 §2) -- this function never re-derives or reuses the caller's raw
    // Principal directly (§5 item 4).
    ChildSpawnRequest child_request;
    agentengine::Message input_message;
    input_message.role = agentengine::role::user;
    agentengine::ContentItem input_item{};
    input_item.origin = agentengine::content_origin::user;
    input_item.value  = agentengine::Text{args.input};
    input_message.content.push_back(std::move(input_item));
    child_request.input         = std::move(input_message);
    child_request.capabilities  = child_grant->capabilities;
    child_request.principal     = ctx.principal;
    child_request.token_budget  = target->child_token_budget;
    // §4.6 (item 6, OQ-16, landed 2026-08-23): the child's OWN manifest, computed from its OWN
    // already-attenuated `child_grant->capabilities` -- never a copy of the caller's own
    // `push_side_summary(*ctx.capabilities)`, which would misrepresent what the child actually holds.
    child_request.instructions =
        agentengine::trust::push_side_summary(child_grant->capabilities);

    agentengine::result<agentengine::rt::AgentResponse> child_response =
        target->run_child(minted_pump->child_id, std::move(child_request));
    if (!child_response) {
        return std::unexpected(child_response.error());
    }

    // -- [9] REPLY --------------------------------------------------------------------------------
    return AgentSpawnReply{agentengine::text_of(child_response->message),
                            child_response->usage.input_tokens, child_response->usage.output_tokens};
}

// ---------------------------------------------------------------------------------------------
// Session wiring: a ContextProvider conformer, reusing the existing composition seam
// (core/composed_context_provider.hpp) rather than a second edit to rt/agent_session.hpp's
// run_rounds() -- ScheduleWakeupTool's own injection site lives INSIDE AgentSession only because its
// dispatch closure needs to capture the session's own `this` (no ContextProvider owns a
// back-reference to its AgentSession, that file's own comment). agent.spawn needs no such
// back-reference: perform_agent_spawn() only needs `ctx` (EffectContext, real per-call) plus this
// provider's own host-configured members, so an ordinary ContextProvider conformer is the right
// shape (matches the design doc's own §4.1 statement).
// ---------------------------------------------------------------------------------------------

template <agentengine::rt::AppendLogStore StoreT>
class AgentSpawnToolProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3 -- every conformer wrapped by
    // ComposedContextProvider needs this (HasContextProviderName), the same "name" convention
    // HistoryProvider<Window<N>>/ScheduleWakeupTool's own precedents already establish.
    static constexpr std::string_view name = "agent_spawn";  // ae-naming-lint: allow name — ADR-033's HasMiddlewareName precedent, reused verbatim per ADR-066 §3

    AgentSpawnToolProvider(SpawnTargetRegistry const& registry, SpawnPump<StoreT>& pump,
                            agentengine::Ref caller_worktree_ref, std::string caller_mount_id,
                            SpawnQuota quota, SpawnQuotaTracker& quota_tracker)
        : registry_(registry),
          pump_(pump),
          caller_worktree_ref_(std::move(caller_worktree_ref)),
          caller_mount_id_(std::move(caller_mount_id)),
          quota_(quota),
          quota_tracker_(quota_tracker) {}

    [[nodiscard]] agentengine::rt::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext&, agentengine::EffectContext& ctx) {
        agentengine::ContextContribution c;
        // Never advertise a tool the model could never successfully call -- ScheduleWakeupTool's own
        // established precedent (rt/agent_session.hpp): gated on SOME AgentCall grant existing at
        // all, not a per-agent_id scan; the per-target check is the LIVE one inside
        // perform_agent_spawn()'s own step [2].
        if (ctx.capabilities && ctx.capabilities->contains_kind(agentengine::capability_kind::agent_call)) {
            c.tools.push_back(agentengine::make_tool_descriptor_with_invoke<AgentSpawnTool>(
                [this](AgentSpawnArgs a, agentengine::EffectContext& call_ctx) {
                    return perform_agent_spawn(a, call_ctx, registry_, pump_, quota_, quota_tracker_,
                                                caller_worktree_ref_, caller_mount_id_);
                }));
        }
        co_return c;
    }

    agentengine::rt::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        co_return std::monostate{};
    }

private:
    SpawnTargetRegistry const& registry_;
    SpawnPump<StoreT>&         pump_;
    agentengine::Ref           caller_worktree_ref_;
    std::string                caller_mount_id_;
    SpawnQuota                 quota_;
    SpawnQuotaTracker&         quota_tracker_;
};

// Item 6 is now landed (see this file's own top comment for the account). Explicitly out of scope,
// unchanged: a future embedded CPython `agent` module binding (dir()/help(), 026 §5a) is real future
// work, named in the design doc's own §4.6 -- 026 is still Draft, and
// trust/agent_library_manifest.hpp's own file banner already names this as the next step once it
// exists.

}  // namespace agentengine::rt
