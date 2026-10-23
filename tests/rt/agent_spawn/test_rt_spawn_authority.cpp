// Gates for decisions/ADR-244-required-choices-step-1b.md slice 3c: §2.3's rules for the paths that start a run on a
// session's behalf -- both spawn paths, the bounded-reflection loop and the builders' ask -- and for the chat
// client's own secrets. Each refusal sits beside the same path, stated correctly, running.
//
//   A1 (G-C7) run_child_agent_session under per_request: the child is a per_request session whose run carries the
//      derived principal, exactly the minted set and the caller's expiry; it holds nothing of its own.
//   A2 (G-C7) the child is admitted against the caller's expiry: an expired one is refused before its model runs.
//   A3 (G-C1) nothing is defaulted: no source, no budget, no turn cap, and per_request without an expiry are each
//      refused (choice.missing.spawn.*) before the model runs.
//   A4 (G-C7) under single_trusted_caller the child holds the minted set itself, with no expiry.
//   P1 (G-C7) perform_agent_spawn from a per_request context: the child inherits the source and the expiry.
//   P2 (G-C7) perform_agent_spawn refuses an expired caller, and a context with no source, before anything is spent.
//   P3 (G-C2) perform_agent_spawn states tighter(target, remaining) as the child's budget and the target's declared
//      turn cap (RunStarted).
//   M1 (G-C7) multi_agent::spawn from a per_request context: the child is re-initialised per_request and its run
//      carries the narrowed set and the caller's expiry.
//   M2 (G-C7) multi_agent::spawn refuses an expired caller, a context with no source, a StartRun carrying its own
//      authority, and a factory session with no choices.
//   M3 (G-C2, I8) multi_agent::spawn tightens the child's budget to what the caller has left and charges the child's
//      spend to the caller.
//   R1 (G-C7) run_with_bounded_reflection on a per_request session carries the authority on every iteration; the
//      overload without one is refused (run.authority_required), and an expired authority is refused.
//   K1 (G-C7) the chat client's secrets reach the model call only: the model call's context holds the secret and not
//      the run's authority, a tool's context holds the run's authority and not the secret, under both sources.
//   K2 set_chat_client_secrets takes cap::Secret only.
//   B1 (G-C7) the builders' ask/run on a per_request session (bound by the agent bridge) carry the caller's
//      authority; without one they are refused. The bridge's chat_client_grants reach the model call only.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../support/agent_options.hpp"
#include "../../support/session_choices.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/agent_spawn.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/bounded_reflection.hpp"
#include "agentengine/rt/multi_agent.hpp"

namespace ae  = agentengine;
namespace art = agentengine::rt;
using u64     = std::uint64_t;
using BoundU  = ae::Bound<u64>;
using Clock   = std::chrono::steady_clock;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

template <class T>
T drive(art::task<T> t) {
    return art::block_on(std::move(t));
}

template <class T>
std::string code_of(ae::result<T> const& r) {
    return r.has_value() ? std::string{} : r.error().code;
}

ae::Message text_message(std::string text, ae::role r = ae::role::user) {
    ae::Message m;
    m.role = r;
    ae::ContentItem item;
    item.origin = r == ae::role::user ? ae::content_origin::user : ae::content_origin::assistant;
    item.value  = ae::Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

ae::Message tool_call_message(std::string id, std::string tool) {
    ae::Message m;
    m.role = ae::role::assistant;
    ae::ContentItem it{};
    it.origin = ae::content_origin::assistant;
    ae::ToolCall c;
    c.call_id        = std::move(id);
    c.tool_name      = std::move(tool);
    c.arguments_json = R"({"note":"x"})";
    c.provenance     = ae::call_provenance::vendor_structured;
    it.value         = c;
    m.content.push_back(it);
    return m;
}

ae::Capability const kSecret{ae::cap::Secret{"api-key", std::chrono::seconds{0}}};
ae::Capability const kEntropy{ae::cap::Entropy{}};

// What one call's EffectContext looked like.
struct Seen {
    std::optional<ae::authority_mode> source;
    std::optional<ae::Expiry>         expires_at;
    std::string                       principal_id;
    bool                              has_caps    = false;
    bool                              has_secret  = false;
    bool                              has_entropy = false;
    std::size_t                       cap_count   = 0;
};
Seen see(ae::EffectContext const& ctx) {
    Seen s;
    s.source       = ctx.authority_source;
    s.expires_at   = ctx.authority_expires_at;
    s.principal_id = ctx.principal.id;
    s.has_caps     = ctx.capabilities != nullptr;
    if (ctx.capabilities != nullptr) {
        s.has_secret  = ctx.capabilities->contains(kSecret);
        s.has_entropy = ctx.capabilities->contains(kEntropy);
        s.cap_count   = ctx.capabilities->size();
    }
    return s;
}

// Records the context of every model call; answers with text. With `call_probe_first`, the first reply calls the
// `probe` tool instead.
class CtxClient {
public:
    struct State {
        std::vector<Seen> calls;
        bool              call_probe_first = false;
    };
    CtxClient() : state_(std::make_shared<State>()) {}
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const {
        ae::ChatClientCapabilities c{};
        c.tool_calling = true;
        return c;
    }
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest, ae::EffectContext& ctx) {
        state_->calls.push_back(see(ctx));
        if (state_->call_probe_first && state_->calls.size() == 1) {
            co_return ae::ChatResponse{tool_call_message("c1", "probe"), ae::Usage{2, 3, 0, 0, 0.0}};
        }
        co_return ae::ChatResponse{text_message("done", ae::role::assistant), ae::Usage{2, 3, 0, 0, 0.0}};
    }
    [[nodiscard]] ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest, ae::EffectContext&) { return {}; }
    [[nodiscard]] std::shared_ptr<State> const& state() const { return state_; }

private:
    std::shared_ptr<State> state_;
};
static_assert(ae::ChatClient<CtxClient>);

// The probe tool records the context a tool receives.
std::vector<Seen> g_tool_calls;
struct ProbeArgs {
    std::string note;
};
AE_JSON_SCHEMA(ProbeArgs, note)
struct ProbeReply {
    bool ok = true;
};
AE_JSON_SCHEMA(ProbeReply, ok)
struct ProbeTool : ae::Tool<ProbeTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>, ae::Approval<ae::approval_mode::never_require>> {
    static constexpr std::string_view name        = "probe";
    static constexpr std::string_view description = "records its context";
    using Args  = ProbeArgs;
    using Reply = ProbeReply;
    ae::task<ae::result<Reply>> invoke(Args, ae::EffectContext& ctx) const {
        g_tool_calls.push_back(see(ctx));
        co_return Reply{true};
    }
};
class ProbeProvider {
public:
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc, ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        ae::ToolTable const table = ae::ToolTable::from(ProbeTool{});
        for (auto const& t : table.descriptors()) c.tools.push_back(t);
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};

ae::Principal const kCaller{"p-caller", "tenant-a"};

// A ChildSpawnRequest with every choice stated; the test removes or changes one.
art::ChildSpawnRequest child_request(ae::CapabilitySet caps, std::optional<ae::authority_mode> source,
                                     std::optional<ae::Expiry> expires_at) {
    art::ChildSpawnRequest req;
    req.input                = text_message("sub-task");
    req.capabilities         = std::move(caps);
    req.principal            = kCaller;
    req.token_budget         = BoundU::at_most(1000);
    req.max_turns            = BoundU::at_most(4);
    req.authority_source     = source;
    req.authority_expires_at = expires_at;
    return req;
}

ae::result<art::AgentResponse> run_child(art::ChildSpawnRequest req, std::shared_ptr<CtxClient::State>& state,
                                         std::vector<ae::RunEvent>* events = nullptr) {
    if (events != nullptr) {
        req.event_sink = [events](ae::RunEvent const& ev) { events->push_back(ev); };
    }
    return drive(art::run_child_agent_session<CtxClient>(
        "child-x", std::move(req), [&state](CtxClient& c) { state = c.state(); }));
}

std::optional<ae::run_event_payload::RunStarted> run_started_of(std::vector<ae::RunEvent> const& events) {
    for (ae::RunEvent const& ev : events) {
        if (ev.kind != ae::run_event_kind::run_started) continue;
        if (auto const* p = std::get_if<ae::run_event_payload::RunStarted>(&ev.payload)) return *p;
    }
    return std::nullopt;
}

// perform_agent_spawn() over one target "X", whose child is a real run_child_agent_session<CtxClient>.
struct DirectSpawn {
    art::InMemoryAppendLogStore store;
    art::SpawnCostBudget        pool;
    std::unique_ptr<art::SpawnPump<art::InMemoryAppendLogStore>> pump;
    art::SpawnTargetRegistry registry;
    art::SpawnQuotaTracker   tracker;
    ae::CapabilitySet held = ae::CapabilitySet::grant_root({ae::cap::AgentCall{"X", ae::trust::SpawnBudget::mint_root(3)}});
    std::shared_ptr<CtxClient::State> child_state;
    std::vector<ae::RunEvent>         child_events;
    int                               child_runs = 0;

    DirectSpawn(u64 child_token_budget, std::optional<u64> declared_budget, std::uint32_t declared_turns) {
        pool.initialize(10);
        pump = std::make_unique<art::SpawnPump<art::InMemoryAppendLogStore>>(pool, store);
        art::SpawnTargetDescriptor d;
        d.worktree_mode         = ae::sharing_mode::scratch;
        d.child_token_budget    = child_token_budget;
        d.metadata              = ae_test::stated_metadata();
        d.metadata.token_budget = declared_budget ? ae::Bound<u64>::at_most(*declared_budget) : ae::Bound<u64>::unbounded();
        d.metadata.max_turns    = ae::Bound<u64>::at_most(declared_turns);
        d.run_child = [this](std::string id, art::ChildSpawnRequest req) -> art::task<ae::result<art::AgentResponse>> {
            ++child_runs;
            req.event_sink = [this](ae::RunEvent const& ev) { child_events.push_back(ev); };
            co_return co_await art::run_child_agent_session<CtxClient>(
                std::move(id), std::move(req), [this](CtxClient& c) { child_state = c.state(); });
        };
        (void)registry.register_target("X", std::move(d));
    }
    ae::result<art::AgentSpawnReply> spawn(std::optional<ae::authority_mode> source, std::optional<ae::Expiry> expiry,
                                           std::optional<u64> remaining) {
        ae::EffectContext ctx;
        ctx.capabilities           = std::shared_ptr<ae::CapabilitySet const>(&held, [](ae::CapabilitySet const*) {});
        ctx.principal              = kCaller;
        ctx.authority_source       = source;
        ctx.authority_expires_at   = expiry;
        ctx.remaining_token_budget = remaining;
        art::AgentSpawnArgs const args{"X", "task"};
        art::SpawnQuota const     q{100};
        ae::Ref const             caller_ref{"session:direct", ""};
        return drive(art::perform_agent_spawn(args, ctx, registry, *pump, q, tracker, caller_ref, "/caller"));
    }
};

using ChildSession = art::AgentSession<CtxClient>;

struct EntropyAgent : ae::Agent<EntropyAgent, ae::ChatClientId<"test:ctx">, ae::Capabilities<ae::cap::decl::Entropy>,
                                ae::MaxTurns<ae::at_most(4)>, ae::TokenBudget<ae::unbounded()>,
                                ae::Approval<ae::approval_mode::policy_driven>,
                                ae::Concurrency<ae::concurrency_mode::sequential>, ae::SandboxProfile<ae::Strict>> {
    static constexpr std::string_view name         = "entropy-agent";
    static constexpr std::string_view instructions = "Declares cap::Entropy only.";
};

}  // namespace

int main() {
    auto const now     = Clock::now();
    auto const live    = ae::Expiry::at(now + std::chrono::hours(1));
    auto const expired = ae::Expiry::at(now - std::chrono::seconds(1));
    ae::CapabilitySet const minted = ae::CapabilitySet::grant_root({kEntropy});

    // ---- A1: run_child_agent_session under per_request ---------------------------------------------------------
    {
        std::shared_ptr<CtxClient::State> state;
        std::vector<ae::RunEvent>         events;
        auto r = run_child(child_request(minted, ae::authority_mode::per_request, live), state, &events);
        check(r.has_value(), "A1: a per_request child runs");
        check(state && state->calls.size() == 1, "A1: the child's model ran once");
        if (state && state->calls.size() == 1) {
            Seen const& s = state->calls[0];
            check(s.source == ae::authority_mode::per_request, "A1: the child's run is per_request");
            check(s.expires_at.has_value() && *s.expires_at == live, "A1: the child's run carries the caller's expiry");
            check(!s.principal_id.empty() && s.principal_id != kCaller.id,
                  "A1: the child's run is under the derived on-behalf-of principal, never the caller's own");
            check(s.has_entropy && s.cap_count == 1, "A1: the child's authority is exactly the minted set");
        }
        auto const started = run_started_of(events);
        check(started.has_value() && started->authority == "per_request(ceiling of 1)",
              "A1: RunStarted records the child's per_request source, its ceiling exactly the minted set");
    }

    // ---- A2: the child is admitted against the caller's expiry ---------------------------------------------------
    {
        std::shared_ptr<CtxClient::State> state;
        auto r = run_child(child_request(minted, ae::authority_mode::per_request, expired), state);
        check(code_of(r) == "run.authority_expired", "A2: an expired caller authority is refused at the child's admission");
        check(!state || state->calls.empty(), "A2: the child's model never runs");
    }

    // ---- A3: nothing is defaulted ------------------------------------------------------------------------------
    {
        std::shared_ptr<CtxClient::State> state;
        auto no_source = run_child(child_request(minted, std::nullopt, std::nullopt), state);
        check(code_of(no_source) == "choice.missing.spawn.authority_source", "A3: no authority source is refused");
        art::ChildSpawnRequest nb = child_request(minted, ae::authority_mode::single_trusted_caller, std::nullopt);
        nb.token_budget.reset();
        check(code_of(run_child(std::move(nb), state)) == "choice.missing.spawn.token_budget",
              "A3: no token budget is refused");
        art::ChildSpawnRequest nt = child_request(minted, ae::authority_mode::single_trusted_caller, std::nullopt);
        nt.max_turns.reset();
        check(code_of(run_child(std::move(nt), state)) == "choice.missing.spawn.max_turns", "A3: no turn cap is refused");
        auto no_expiry = run_child(child_request(minted, ae::authority_mode::per_request, std::nullopt), state);
        check(code_of(no_expiry) == "choice.missing.spawn.authority_expires_at",
              "A3: per_request without the caller's expiry is refused");
        check(!state || state->calls.empty(), "A3: no refused child ever reached its model");
    }

    // ---- A4: single_trusted_caller ----------------------------------------------------------------------------
    {
        std::shared_ptr<CtxClient::State> state;
        auto r = run_child(child_request(minted, ae::authority_mode::single_trusted_caller, std::nullopt), state);
        check(r.has_value() && state && state->calls.size() == 1, "A4: a single_trusted_caller child runs");
        if (state && state->calls.size() == 1) {
            Seen const& s = state->calls[0];
            check(s.source == ae::authority_mode::single_trusted_caller && !s.expires_at.has_value(),
                  "A4: the child holds its set itself, with no expiry");
            check(s.has_entropy && s.cap_count == 1, "A4: the child holds exactly the minted set");
        }
    }

    // ---- P1/P2/P3: perform_agent_spawn ----------------------------------------------------------------------------
    {
        DirectSpawn ds(50'000, std::nullopt, 16);
        auto r = ds.spawn(ae::authority_mode::per_request, live, std::nullopt);
        check(r.has_value(), "P1: a spawn from a per_request context runs");
        check(ds.child_state && ds.child_state->calls.size() == 1 &&
                  ds.child_state->calls[0].source == ae::authority_mode::per_request &&
                  ds.child_state->calls[0].expires_at == std::optional<ae::Expiry>(live),
              "P1: the child inherits the caller's per_request source and expiry");
    }
    {
        DirectSpawn ds(50'000, std::nullopt, 16);
        u64 const before = ds.pool.remaining();
        auto old = ds.spawn(ae::authority_mode::per_request, expired, std::nullopt);
        check(code_of(old) == "agent_spawn.authority_expired", "P2: an expired caller is refused at the spawn");
        auto none = ds.spawn(std::nullopt, std::nullopt, std::nullopt);
        check(code_of(none) == "choice.missing.spawn.authority_source", "P2: a context with no source is refused");
        auto no_expiry = ds.spawn(ae::authority_mode::per_request, std::nullopt, std::nullopt);
        check(code_of(no_expiry) == "agent_spawn.authority_expired",
              "P2: a per_request context with no expiry is refused");
        check(ds.pool.remaining() == before && ds.child_runs == 0,
              "P2: the refused spawns spent no cost token and ran no child");
        auto ok = ds.spawn(ae::authority_mode::single_trusted_caller, std::nullopt, std::nullopt);
        check(ok.has_value() && ds.pool.remaining() == before - 1 && ds.child_runs == 1,
              "P2 control: a trusted spawn spends one cost token and runs the child");
    }
    {
        DirectSpawn ds(50'000, 300, 7);
        (void)ds.spawn(ae::authority_mode::single_trusted_caller, std::nullopt, 200);
        auto const started = run_started_of(ds.child_events);
        check(started.has_value() && started->token_budget == BoundU::at_most(200).describe(),
              "P3: the child's budget is what the caller has left when that is tightest (200 < 300 < 50000)");
        check(started.has_value() && started->max_turns == BoundU::at_most(7).describe(),
              "P3: the child's turn cap is the target's declared one (7)");
        ds.child_events.clear();
        (void)ds.spawn(ae::authority_mode::single_trusted_caller, std::nullopt, 1000);
        auto const second = run_started_of(ds.child_events);
        check(second.has_value() && second->token_budget == BoundU::at_most(300).describe(),
              "P3: the target's declared budget when that is tightest (300 < 1000)");
    }

    // ---- P4 (ADR-244 slices 4b/4c): a target states its turn cap and budget, and neither is zero -- refused at
    //      registration, before any spawn exists to spend anything.
    {
        auto const make = [] {
            art::SpawnTargetDescriptor d;
            d.worktree_mode = ae::sharing_mode::scratch;
            d.metadata      = ae_test::stated_metadata();
            return d;
        };
        art::SpawnTargetRegistry reg;
        auto no_turns = make();
        no_turns.metadata.max_turns.reset();
        check(code_of(reg.register_target("A", std::move(no_turns))) == "choice.missing.max_turns",
              "P4: a target with no stated turn cap is refused at registration (choice.missing.max_turns)");
        auto no_budget = make();
        no_budget.metadata.token_budget.reset();
        check(code_of(reg.register_target("B", std::move(no_budget))) == "choice.missing.token_budget",
              "P4: ... and one with no stated budget (choice.missing.token_budget)");
        auto zero = make();
        zero.metadata.max_turns = BoundU::at_most(0);
        check(code_of(reg.register_target("C", std::move(zero))) == "choice.zero_bound",
              "P4: ... and a zero turn cap (choice.zero_bound)");
        check(reg.find("A") == nullptr && reg.find("B") == nullptr && reg.find("C") == nullptr,
              "P4: none of the refused targets is registered");
        check(reg.register_target("D", make()).has_value() && reg.find("D") != nullptr,
              "P4 control: a target stating both registers");
    }

    // ---- M1/M2/M3: multi_agent::spawn ----------------------------------------------------------------------------
    {
        ae::CapabilitySet const parent_caps = ae::CapabilitySet::grant_root({kEntropy, ae::Capability{ae::cap::Clock{ae::cap::ClockResolution::finest()}}});
        std::shared_ptr<CtxClient::State> state;
        std::vector<ae::RunEvent>         events;
        int                               factory_calls = 0;
        bool                              initialize_child = true;
        art::multi_agent::SessionFactory<CtxClient, art::NoSessionState, ae::HistoryProvider<ae::UnboundedWindow>> const
            factory = [&](ae::Principal const& p) {
                ++factory_calls;
                auto s = std::make_unique<ChildSession>();
                if (initialize_child) s->initialize("child-m", p, ae_test::trusted_unbounded());
                state = s->emplace_chat_client().state();
                s->set_run_event_tap([&events](ae::RunEvent const& ev) { events.push_back(ev); });
                return s;
            };
        ae::Usage charged{};
        auto make_ctx = [&](std::optional<ae::authority_mode> source, std::optional<ae::Expiry> expiry,
                            std::optional<u64> remaining) {
            ae::EffectContext ctx;
            ctx.capabilities = ae::borrow_capabilities(parent_caps);
            ctx.principal    = kCaller;
            ctx.authority_source       = source;
            ctx.authority_expires_at   = expiry;
            ctx.remaining_token_budget = remaining;
            ctx.charge_delegated_usage = [&charged](ae::Usage const& u, u64) {
                charged.input_tokens += u.input_tokens;
                charged.output_tokens += u.output_tokens;
            };
            return ctx;
        };
        std::vector<ae::Capability> const narrower{kEntropy};

        {
            ae::EffectContext ctx = make_ctx(ae::authority_mode::per_request, live, std::nullopt);
            auto r = drive(art::multi_agent::spawn(ctx, factory, art::StartRun{text_message("go")}, narrower));
            check(r.has_value(), "M1: a per_request multi_agent::spawn runs");
            check(state && state->calls.size() == 1 && state->calls[0].source == ae::authority_mode::per_request &&
                      state->calls[0].expires_at == std::optional<ae::Expiry>(live),
                  "M1: the child is re-initialised per_request, admitted under the caller's expiry");
            check(state && state->calls.size() == 1 && state->calls[0].has_entropy && state->calls[0].cap_count == 1,
                  "M1: the child's authority is the narrowed set (Entropy), not the caller's whole grant (Entropy, Clock)");
        }
        {
            factory_calls = 0;
            ae::EffectContext old = make_ctx(ae::authority_mode::per_request, expired, std::nullopt);
            check(code_of(drive(art::multi_agent::spawn(old, factory, art::StartRun{text_message("go")}, narrower))) ==
                      "multi_agent.authority_expired",
                  "M2: an expired caller is refused");
            ae::EffectContext none = make_ctx(std::nullopt, std::nullopt, std::nullopt);
            check(code_of(drive(art::multi_agent::spawn(none, factory, art::StartRun{text_message("go")}, narrower))) ==
                      "choice.missing.spawn.authority_source",
                  "M2: a context with no source is refused");
            ae::EffectContext trusted = make_ctx(ae::authority_mode::single_trusted_caller, std::nullopt, std::nullopt);
            art::StartRun carried{text_message("go")};
            carried.authority = art::RequestAuthority{kCaller, ae::borrow_capabilities(parent_caps), live};
            check(code_of(drive(art::multi_agent::spawn(trusted, factory, std::move(carried), narrower))) ==
                      "multi_agent.request_authority_unexpected",
                  "M2: a StartRun carrying its own authority is refused, not replaced");
            check(factory_calls == 0, "M2: no refused spawn reached the factory");
            initialize_child = false;
            check(code_of(drive(art::multi_agent::spawn(trusted, factory, art::StartRun{text_message("go")}, narrower))) ==
                      "choice.missing.spawn.child_choices",
                  "M2: a factory session with no choices is refused");
            initialize_child = true;
        }
        {
            events.clear();
            charged = ae::Usage{};
            ae::EffectContext ctx = make_ctx(ae::authority_mode::single_trusted_caller, std::nullopt, 50);
            auto r = drive(art::multi_agent::spawn(ctx, factory, art::StartRun{text_message("go")}, narrower));
            auto const started = run_started_of(events);
            check(r.has_value() && started.has_value() && started->token_budget == BoundU::at_most(50).describe(),
                  "M3: the child's budget (unbounded by the factory) is tightened to what the caller has left (50)");
            check(charged.input_tokens == 2 && charged.output_tokens == 3, "M3: the child's spend is charged to the caller");
        }
    }

    // ---- R1: bounded reflection carries the authority -----------------------------------------------------------
    {
        ChildSession s;
        s.initialize("s-r1", kCaller, ae_test::per_request_no_ceiling());
        std::shared_ptr<CtxClient::State> const state = s.emplace_chat_client().state();
        auto caps = std::make_shared<ae::CapabilitySet const>(ae::CapabilitySet::grant_root({kEntropy}));
        int evaluations = 0;
        auto evaluator = [&evaluations](art::AgentResponse const&) -> art::task<ae::result<art::EvaluationVerdict>> {
            ++evaluations;
            co_return art::EvaluationVerdict{evaluations >= 2, "try again"};
        };
        auto r = drive(art::run_with_bounded_reflection(s, art::RequestAuthority{kCaller, caps, live},
                                                        text_message("q"), 3, evaluator));
        check(r.has_value() && r->iterations_used == 2, "R1: two iterations ran on a per_request session");
        check(state->calls.size() == 2 && state->calls[0].source == ae::authority_mode::per_request &&
                  state->calls[1].source == ae::authority_mode::per_request && state->calls[1].has_entropy,
              "R1: every iteration was admitted under the carried authority");
        auto none = drive(art::run_with_bounded_reflection(s, text_message("q"), 3, evaluator));
        check(code_of(none) == "run.authority_required", "R1: the overload without an authority is refused");
        auto old = drive(art::run_with_bounded_reflection(s, art::RequestAuthority{kCaller, caps, expired},
                                                          text_message("q"), 3, evaluator));
        check(code_of(old) == "run.authority_expired", "R1: an expired authority is refused");
    }

    // ---- K1/K2: the chat client's secrets ----------------------------------------------------------------------
    using ProbeSession = art::AgentSession<CtxClient, art::NoSessionState, ProbeProvider>;
    {
        // per_request: the run's authority (Entropy) and the chat client's secret, held apart.
        ProbeSession s;
        s.initialize("s-k1", kCaller, ae_test::per_request_no_ceiling());
        std::shared_ptr<CtxClient::State> const state = s.emplace_chat_client().state();
        state->call_probe_first = true;
        check(s.set_chat_client_secrets({kSecret}).has_value(), "K1 setup: the chat client's secret is set");
        g_tool_calls.clear();
        auto caps = std::make_shared<ae::CapabilitySet const>(ae::CapabilitySet::grant_root({kEntropy}));
        auto r = drive(s.start_run(art::StartRun{text_message("q"), std::nullopt, art::RequestAuthority{kCaller, caps, live}}));
        check(r.has_value() && state->calls.size() == 2 && g_tool_calls.size() == 1,
              "K1 setup: per_request -- the model called the probe tool, then answered");
        if (state->calls.size() == 2 && g_tool_calls.size() == 1) {
            check(state->calls[0].has_secret && state->calls[1].has_secret,
                  "K1: per_request -- the model call's context holds the chat client's secret");
            check(!state->calls[0].has_entropy, "K1: per_request -- the model call's context does not hold the run's authority");
            check(g_tool_calls[0].has_entropy, "K1 control: per_request -- the tool's context holds the run's authority");
            check(!g_tool_calls[0].has_secret, "K1: per_request -- the tool's context does not hold the chat client's secret");
        }
    }
    {
        // single_trusted_caller: the same split over the session's own grants.
        ProbeSession s;
        s.initialize("s-k1t", kCaller, ae_test::trusted_unbounded());
        std::shared_ptr<CtxClient::State> const state = s.emplace_chat_client().state();
        state->call_probe_first = true;
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({kEntropy});
        s.set_capabilities(&held);
        check(s.set_chat_client_secrets({kSecret}).has_value(), "K1 setup: the chat client's secret is set (trusted)");
        g_tool_calls.clear();
        auto r = drive(s.start_run(art::StartRun{text_message("q")}));
        check(r.has_value() && state->calls.size() == 2 && g_tool_calls.size() == 1,
              "K1 setup: trusted -- the model called the probe tool, then answered");
        if (state->calls.size() == 2 && g_tool_calls.size() == 1) {
            check(state->calls[0].has_secret && !state->calls[0].has_entropy,
                  "K1: trusted -- the model call's context holds the secret and not the session's grants");
            check(g_tool_calls[0].has_entropy && !g_tool_calls[0].has_secret,
                  "K1: trusted -- the tool's context holds the session's grants and not the secret");
        }
    }
    {
        ProbeSession s;
        s.initialize("s-k2", kCaller, ae_test::trusted_unbounded());
        auto refused = s.set_chat_client_secrets({kEntropy});
        check(code_of(refused) == "session.chat_client_grant_not_secret" && s.chat_client_secrets() == nullptr,
              "K2: a non-secret capability is refused and nothing is set");
    }

    // ---- B1: the builders' ask/run carry the authority ---------------------------------------------------------
    {
        ae::AgentSessionOptions o = ae_test::declared_options<EntropyAgent>();
        o.principal               = kCaller;
        o.choices.authority       = ae::AuthoritySource::per_request(ae::CapabilitySet::no_ceiling());
        o.chat_client_grants      = {kSecret};
        CtxClient client;
        std::shared_ptr<CtxClient::State> const state = client.state();
        auto bundle = ae::make_agent_session<EntropyAgent>(std::move(client), o, nullptr, ae_test::sandboxes());
        check(bundle.has_value(), "B1 setup: a per_request agent session binds");
        if (bundle.has_value()) {
            auto caps = std::make_shared<ae::CapabilitySet const>(ae::CapabilitySet::grant_root({kEntropy}));
            check(code_of(bundle->ask("q")) == "run.authority_required", "B1: ask() without an authority is refused");
            auto asked = bundle->ask("q", art::RequestAuthority{kCaller, caps, live});
            check(asked.has_value() && *asked == "done", "B1: ask(text, authority) runs");
            auto ran = bundle->run("q", art::RequestAuthority{kCaller, caps, live});
            check(ran.has_value(), "B1: run(text, authority) runs");
            check(code_of(bundle->run("q", art::RequestAuthority{kCaller, caps, expired})) == "run.authority_expired",
                  "B1: an expired authority is refused");
            check(state->calls.size() == 2 && state->calls[0].has_secret && !state->calls[0].has_entropy &&
                      state->calls[1].has_secret,
                  "B1: the bridge's chat_client_grants reach the model call, the run's authority does not");
            check(!bundle->capabilities().contains(kSecret),
                  "B1: the chat client's secret is not in the session's own grants");
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "test_rt_spawn_authority: %d FAIL\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_rt_spawn_authority: all checks passed\n");
    return 0;
}
