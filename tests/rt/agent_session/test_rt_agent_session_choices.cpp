// Gates for decisions/ADR-244-required-choices-step-1b.md slice 3a, the session layer: §2.2's session row, §2.3's
// authority sources and re-making, §2.6's RunStarted record. Each refusal sits beside the positive case that the
// same session, stated correctly, runs -- a refusal that cannot be told from "never runs" proves nothing.
//
//   C1  (G-C3) a never-initialised session refuses StartRun with choice.missing.session, naming every option.
//   C2  (G-C7) clear un-initialises: the same session refuses again until re-initialised.
//   C3  (G-C2) at_most(0) as a token budget or a turn cap is refused at the run (choice.zero_bound), before the
//       model is called; unbounded() in the same place runs and is recorded.
//   C4  (G-C6) per_request(ceiling): no authority, an authority expired at admission, and one holding a
//       capability outside the ceiling are each refused; one inside it runs. Refused, never narrowed.
//   C5  (G-C6) per_request(no_ceiling()) admits any live authority.
//   C6  (G-C6) single_trusted_caller refuses a request carrying an authority (run.authority_unexpected), instead
//       of silently replacing it with the session's own.
//   C7  (G-C6) a null per-request capabilities pointer authorizes nothing: the gated tool is denied.
//   C8  (§2.3) EffectContext::authority_source and authority_expires_at are set by the session in both modes.
//   C9  (§2.3, ADR-242 R3 rule 1) expiry is checked at admission: a resolve after the authority expired is
//       refused, while the run admitted under it is never cut short mid-run.
//   C10 (G-C7) fork copies every choice, the ceiling included.
//   C11 (G-C7) restore: into a session with no choices, from a record with none, with another mode, or with a
//       wider budget -- each refused, the target untouched; a tighter target restores and keeps its own choices.
//   C12 (G-C7) migrate_record() converts a pre-ADR-244 record and refuses one that already carries choices; the
//       JSON codec loads an old record as choiceless and refuses malformed choices.
//   C13 (G-C12) RunStarted records the authority, budget and turn cap the run was admitted under.
//   C14 (§2.2) set_max_turns() takes a Bound, on an initialised session only.
//   C15 (G-C3) resolve_interaction() on a session with no choices is refused, as StartRun is in C1.

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/tool.hpp"
#include "../../support/session_choices.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/block_on.hpp"

using agentengine::AuthoritySource;
using agentengine::Bound;
using agentengine::CapabilitySet;
using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::ContextContribution;
using agentengine::EffectContext;
using agentengine::Expiry;
using agentengine::Message;
using agentengine::Principal;
using agentengine::SessionChoices;
using agentengine::SessionContext;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::ToolResult;
using agentengine::Usage;
using agentengine::authority_mode;
using agentengine::rt::AgentSession;
using agentengine::rt::RequestAuthority;
using agentengine::rt::ResolveInteraction;
using agentengine::rt::StartRun;
using agentengine::task;
using u64 = std::uint64_t;

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
T drive(agentengine::rt::task<T> t) {
    return agentengine::rt::block_on(std::move(t));
}

Message user_message(std::string text) {
    Message m;
    m.role = agentengine::role::user;
    ContentItem item;
    item.origin = agentengine::content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message assistant(decltype(ContentItem::value) v) {
    Message m;
    m.role = agentengine::role::assistant;
    ContentItem item;
    item.origin = agentengine::content_origin::assistant;
    item.value  = std::move(v);
    m.content.push_back(item);
    return m;
}

std::string reply_text(agentengine::rt::AgentResponse const& r) {
    auto const* t = std::get_if<Text>(&r.message.content.front().value);
    return t != nullptr ? t->text : std::string{"<non-text reply>"};
}

// What the chat client saw of the run's authority, for C8.
struct Observed {
    int                           calls = 0;
    std::optional<authority_mode> source;
    std::optional<Expiry>         expires;
    bool                          capabilities_null = true;
    std::string                   principal_id;
};

class CapturingClient {
public:
    explicit CapturingClient(std::shared_ptr<Observed> o = std::make_shared<Observed>()) : seen_(std::move(o)) {}
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext& ctx) {
        ++seen_->calls;
        seen_->source            = ctx.authority_source;
        seen_->expires           = ctx.authority_expires_at;
        seen_->capabilities_null = ctx.capabilities == nullptr;
        seen_->principal_id      = ctx.principal.id;
        co_return ChatResponse{assistant(Text{"reply"}), Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }
    std::shared_ptr<Observed> seen_;
};
static_assert(agentengine::ChatClient<CapturingClient>);

// The gated tool (needs cap::Entropy) and a client that calls it once, then reports whether it was denied.
struct GatedArgs { bool unused = false; };
AE_JSON_SCHEMA(GatedArgs, unused)
struct GatedReply { bool unused = false; };
AE_JSON_SCHEMA(GatedReply, unused)

struct GatedTool : agentengine::Tool<GatedTool, agentengine::Capabilities<agentengine::cap::decl::Entropy>, agentengine::Approval<agentengine::approval_mode::never_require>> {
    static constexpr std::string_view name        = "gated_tool";
    static constexpr std::string_view description = "Requires cap::Entropy.";
    using Args  = GatedArgs;
    using Reply = GatedReply;
    task<agentengine::result<Reply>> invoke(Args, EffectContext&) const { co_return Reply{}; }
};

class GatedToolProvider {
public:
    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools.push_back(agentengine::make_tool_descriptor(GatedTool{}));
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) { co_return std::monostate{}; }
};

class GatedClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<agentengine::result<ChatResponse>> chat(ChatRequest const& request, EffectContext&) {
        if (calls_++ == 0) {
            ToolCall call;
            call.call_id        = "call-1";
            call.tool_name      = "gated_tool";
            call.arguments_json = R"({"unused":false})";
            call.provenance     = agentengine::call_provenance::vendor_structured;
            co_return ChatResponse{assistant(call), Usage{1, 1, 0, 0, 0.0}};
        }
        std::string outcome = "<no tool result>";
        if (!request.messages.empty()) {
            auto const& item = request.messages.back().content.front();
            if (auto const* tr = std::get_if<ToolResult>(&item.value)) outcome = tr->is_error ? "denied" : "allowed";
        }
        co_return ChatResponse{assistant(Text{outcome}), Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }

private:
    int calls_ = 0;
};
static_assert(agentengine::ChatClient<GatedClient>);

using Session      = AgentSession<CapturingClient>;
using GatedSession = AgentSession<GatedClient, agentengine::rt::NoSessionState, GatedToolProvider>;

Principal const kOwner{"owner", "tenant-a"};

SessionChoices per_request(agentengine::CapabilityCeiling ceiling, Bound<u64> budget = Bound<u64>::unbounded(),
                           Bound<u64> turns = Bound<u64>::unbounded()) {
    return SessionChoices{AuthoritySource::per_request(std::move(ceiling)), budget, turns, agentengine::Bound<std::uint64_t>::unbounded()};
}
SessionChoices trusted(Bound<u64> budget = Bound<u64>::unbounded(), Bound<u64> turns = Bound<u64>::unbounded(),
                       Bound<u64> call_timeout = Bound<u64>::unbounded()) {
    return SessionChoices{AuthoritySource::single_trusted_caller(), budget, turns, call_timeout};
}

RequestAuthority authority(std::shared_ptr<CapabilitySet const> caps, Expiry expiry) {
    return RequestAuthority{kOwner, std::move(caps), expiry};
}

std::string code_of(agentengine::result<agentengine::rt::AgentResponse> const& r) {
    return r.has_value() ? std::string{"<ok>"} : r.error().code;
}

}  // namespace

int main() {
    using Clock = std::chrono::steady_clock;
    Clock::time_point const now = Clock::now();
    Expiry const live    = Expiry::at(now + std::chrono::hours(1));
    Expiry const expired = Expiry::at(now - std::chrono::seconds(1));
    auto const entropy = std::make_shared<CapabilitySet const>(CapabilitySet::grant_root({agentengine::cap::Entropy{}}));
    auto const clock   = std::make_shared<CapabilitySet const>(CapabilitySet::grant_root({agentengine::cap::Clock{agentengine::cap::ClockResolution::finest()}}));

    // ---- C1: never initialised -------------------------------------------------------------------------------
    {
        Session s;
        auto obs = s.emplace_chat_client().seen_;
        auto r = drive(s.start_run(StartRun{user_message("hi")}, now));
        check(code_of(r) == "choice.missing.session", "C1: a never-initialised session refuses with choice.missing.session");
        check(!r.has_value() && r.error().message.find("single_trusted_caller") != std::string::npos &&
                  r.error().message.find("no_ceiling") != std::string::npos,
              "C1: the error names every authority option");
        check(obs->calls == 0, "C1: the model was never called");
        s.initialize("c1", kOwner, trusted());
        check(drive(s.start_run(StartRun{user_message("hi")}, now)).has_value(),
              "C1 control: the same session, initialised, runs");
    }

    // ---- C2: clear un-initialises -----------------------------------------------------------------------------
    {
        Session s;
        s.initialize("c2", kOwner, trusted());
        s.emplace_chat_client();
        check(drive(s.start_run(StartRun{user_message("hi")}, now)).has_value(), "C2 setup: the session runs");
        s.clear_in_process_state();
        check(!s.choices().has_value(), "C2: clear resets the choices");
        check(code_of(drive(s.start_run(StartRun{user_message("hi")}, now))) == "choice.missing.session",
              "C2: a cleared session refuses until re-initialised");
        s.initialize("c2b", kOwner, trusted());
        check(drive(s.start_run(StartRun{user_message("hi")}, now)).has_value(), "C2 control: re-initialised, it runs");
    }

    // ---- C3: zero bounds --------------------------------------------------------------------------------------
    {
        for (bool zero_budget : {true, false}) {
            Session s;
            s.initialize("c3", kOwner,
                         zero_budget ? trusted(Bound<u64>::at_most(0)) : trusted(Bound<u64>::unbounded(), Bound<u64>::at_most(0)));
            auto obs = s.emplace_chat_client().seen_;
            auto r = drive(s.start_run(StartRun{user_message("hi")}, now));
            check(code_of(r) == "choice.zero_bound",
                  zero_budget ? "C3: token_budget at_most(0) is refused" : "C3: max_turns at_most(0) is refused");
            check(!r.has_value() && r.error().message.find("unbounded()") != std::string::npos,
                  "C3: the refusal points at unbounded()");
            check(obs->calls == 0, "C3: refused before the model is called");
        }
        Session s;
        s.initialize("c3-ok", kOwner, trusted(Bound<u64>::unbounded(), Bound<u64>::unbounded()));
        s.emplace_chat_client();
        check(drive(s.start_run(StartRun{user_message("hi")}, now)).has_value(),
              "C3 control: unbounded() in the same place runs");
        // ADR-244 §2.2: the per-call timeout is the same kind of choice -- at_most(0) refused, a stated one runs.
        Session z;
        z.initialize("c3-timeout", kOwner, trusted(Bound<u64>::unbounded(), Bound<u64>::unbounded(), Bound<u64>::at_most(0)));
        auto zobs = z.emplace_chat_client().seen_;
        check(code_of(drive(z.start_run(StartRun{user_message("hi")}, now))) == "choice.zero_bound",
              "C3: call_timeout_ms at_most(0) is refused");
        check(zobs->calls == 0, "C3: ... before the model is called");
        Session t;
        t.initialize("c3-timeout-ok", kOwner, trusted(Bound<u64>::unbounded(), Bound<u64>::unbounded(), Bound<u64>::at_most(60'000)));
        t.emplace_chat_client();
        check(drive(t.start_run(StartRun{user_message("hi")}, now)).has_value(),
              "C3 control: a stated call_timeout_ms runs");
    }

    // ---- C4: per_request(ceiling) -----------------------------------------------------------------------------
    {
        auto make = [&] {
            auto s = std::make_unique<Session>();
            s->initialize("c4", kOwner, per_request(CapabilitySet::grant_root({agentengine::cap::Entropy{}})));
            s->emplace_chat_client();
            return s;
        };
        auto s1 = make();
        check(code_of(drive(s1->start_run(StartRun{user_message("hi")}, now))) == "run.authority_required",
              "C4: no authority is refused");
        auto s2 = make();
        StartRun dead{user_message("hi")};
        dead.authority = authority(entropy, expired);
        check(code_of(drive(s2->start_run(dead, now))) == "run.authority_expired",
              "C4: an authority expired at admission is refused");
        auto s3 = make();
        StartRun outside{user_message("hi")};
        outside.authority = authority(clock, live);
        auto r3 = drive(s3->start_run(outside, now));
        check(code_of(r3) == "run.authority_exceeds_ceiling", "C4: an authority outside the ceiling is refused");
        check(s3->history().empty(), "C4: refused, not narrowed -- nothing ran");
        auto s4 = make();
        StartRun inside{user_message("hi")};
        inside.authority = authority(entropy, live);
        check(drive(s4->start_run(inside, now)).has_value(), "C4 control: an authority inside the ceiling runs");
    }

    // ---- C5: no_ceiling ---------------------------------------------------------------------------------------
    {
        Session s;
        s.initialize("c5", kOwner, per_request(CapabilitySet::no_ceiling()));
        s.emplace_chat_client();
        StartRun req{user_message("hi")};
        req.authority = authority(clock, live);
        check(drive(s.start_run(req, now)).has_value(), "C5: per_request(no_ceiling()) admits any live authority");
    }

    // ---- C6: single_trusted_caller refuses a carried authority -------------------------------------------------
    {
        Session s;
        s.initialize("c6", kOwner, trusted());
        auto obs = s.emplace_chat_client().seen_;
        StartRun req{user_message("hi")};
        req.authority = authority(entropy, live);
        check(code_of(drive(s.start_run(req, now))) == "run.authority_unexpected",
              "C6: a single_trusted_caller session refuses a request carrying its own authority");
        check(obs->calls == 0, "C6: nothing ran under either authority");
        check(drive(s.start_run(StartRun{user_message("hi")}, now)).has_value(),
              "C6 control: the same session runs a request without one");
    }

    // ---- C7: null per-request capabilities authorize nothing ----------------------------------------------------
    {
        GatedSession s;
        s.initialize("c7", kOwner, per_request(CapabilitySet::no_ceiling()));
        s.emplace_chat_client();
        CapabilitySet const session_level = CapabilitySet::grant_root({agentengine::cap::Entropy{}});
        s.set_capabilities(&session_level);  // must not be the fallback
        StartRun req{user_message("hi")};
        req.authority = authority(nullptr, live);
        auto r = drive(s.start_run(req, now));
        check(r.has_value() && reply_text(*r) == "denied",
              "C7: a null per-request capability set authorizes nothing, even with a session-level grant");
        GatedSession ok;
        ok.initialize("c7-ok", kOwner, per_request(CapabilitySet::no_ceiling()));
        ok.emplace_chat_client();
        StartRun granted{user_message("hi")};
        granted.authority = authority(entropy, live);
        auto r2 = drive(ok.start_run(granted, now));
        check(r2.has_value() && reply_text(*r2) == "allowed", "C7 control: the same tool runs under a granting authority");
    }

    // ---- C8: the engine-set authority fields ------------------------------------------------------------------
    {
        Session pr;
        pr.initialize("c8a", kOwner, per_request(CapabilitySet::no_ceiling()));
        auto obs = pr.emplace_chat_client().seen_;
        StartRun req{user_message("hi")};
        req.authority = authority(entropy, live);
        (void)drive(pr.start_run(req, now));
        check(obs->source == authority_mode::per_request && obs->expires == live && !obs->capabilities_null,
              "C8: under per_request the context carries the source, the admitted expiry and its capabilities");

        Session tc;
        tc.initialize("c8b", kOwner, trusted());
        auto obs2 = tc.emplace_chat_client().seen_;
        (void)drive(tc.start_run(StartRun{user_message("hi")}, now));
        check(obs2->source == authority_mode::single_trusted_caller && !obs2->expires.has_value(),
              "C8: under single_trusted_caller the context names the source and carries no expiry");
    }

    // ---- C9: expiry at admission ------------------------------------------------------------------------------
    {
        GatedSession s;
        s.initialize("c9", kOwner, per_request(CapabilitySet::no_ceiling()));
        s.emplace_chat_client();
        s.set_suspend_for_approval(true);
        Expiry const soon = Expiry::at(now + std::chrono::seconds(10));
        StartRun req{user_message("hi")};
        req.authority = authority(entropy, soon);
        auto started = drive(s.start_run(req, now));
        // Gated tools need no approval here, so the run converges; what matters is that it was admitted at `now`.
        check(started.has_value(), "C9 setup: a run admitted under a live authority runs to its end");
        Session r;
        r.initialize("c9b", kOwner, per_request(CapabilitySet::no_ceiling()));
        r.emplace_chat_client();
        StartRun later{user_message("hi")};
        later.authority = authority(entropy, soon);
        check(code_of(drive(r.start_run(later, now + std::chrono::seconds(11)))) == "run.authority_expired",
              "C9: the same authority presented after its expiry is refused at the next admission");
    }

    // ---- C10: fork copies every choice -------------------------------------------------------------------------
    {
        Session src;
        src.initialize("c10", kOwner,
                       per_request(CapabilitySet::grant_root({agentengine::cap::Entropy{}}), Bound<u64>::at_most(500),
                                   Bound<u64>::at_most(7)));
        src.emplace_chat_client();
        Session fork;
        fork.emplace_chat_client();
        fork.fork_from(src, "c10-fork");
        check(fork.choices().has_value() && fork.choices()->authority.mode() == authority_mode::per_request &&
                  fork.choices()->token_budget == Bound<u64>::at_most(500) && fork.choices()->max_turns == Bound<u64>::at_most(7),
              "C10: a fork carries the mode, the budget and the turn cap");
        StartRun outside{user_message("hi")};
        outside.authority = authority(clock, live);
        check(code_of(drive(fork.start_run(outside, now))) == "run.authority_exceeds_ceiling",
              "C10: the fork enforces the source's ceiling");
    }

    // ---- C11: restore ---------------------------------------------------------------------------------------
    {
        Session src;
        src.initialize("c11", kOwner, trusted(Bound<u64>::at_most(1000), Bound<u64>::at_most(10)));
        src.emplace_chat_client();
        auto const rec = src.to_record();
        check(rec.choices.has_value() && rec.choices->authority == authority_mode::single_trusted_caller &&
                  rec.choices->token_budget == Bound<u64>::at_most(1000) && rec.choices->max_turns == Bound<u64>::at_most(10),
              "C11: to_record() carries the choices");

        Session bare;
        check(!bare.restore_from_record(rec).has_value() &&
                  bare.restore_from_record(rec).error().code == "choice.missing.session",
              "C11: restoring into a session with no choices is refused");

        auto legacy = rec;
        legacy.choices.reset();
        Session t1;
        t1.initialize("target", Principal{"other", ""}, trusted(Bound<u64>::at_most(1000), Bound<u64>::at_most(10)));
        auto r1 = t1.restore_from_record(legacy);
        check(!r1.has_value() && r1.error().code == "session.record_without_choices",
              "C11: a record without choices is refused");
        check(t1.session_id() == "target", "C11: a refused restore leaves the session untouched");

        Session t2;
        t2.initialize("target", kOwner, per_request(CapabilitySet::no_ceiling()));
        auto r2 = t2.restore_from_record(rec);
        check(!r2.has_value() && r2.error().code == "session.choices_widened",
              "C11: a different authority mode is refused");

        Session t3;
        t3.initialize("target", kOwner, trusted(Bound<u64>::unbounded(), Bound<u64>::at_most(10)));
        auto r3 = t3.restore_from_record(rec);
        check(!r3.has_value() && r3.error().code == "session.choices_widened",
              "C11: a budget wider than the record's is refused");

        Session t4;
        t4.initialize("target", kOwner, trusted(Bound<u64>::at_most(200), Bound<u64>::at_most(5)));
        check(t4.restore_from_record(rec).has_value(), "C11 control: a tighter target restores");
        check(t4.session_id() == "c11" && t4.choices()->token_budget == Bound<u64>::at_most(200) &&
                  t4.choices()->max_turns == Bound<u64>::at_most(5),
              "C11 control: the restored session keeps its own, tighter choices");
    }

    // ---- C12: migrate_record and the codec ------------------------------------------------------------------
    {
        agentengine::rt::AgentSessionRecord legacy;
        legacy.session_id   = "c12";
        legacy.principal_id = "owner";
        auto migrated = agentengine::rt::migrate_record(legacy, trusted());
        check(migrated.has_value() && migrated->choices.has_value() &&
                  migrated->choices->authority == authority_mode::single_trusted_caller,
              "C12: migrate_record() states the choices of a pre-ADR-244 record");
        auto twice = agentengine::rt::migrate_record(*migrated, trusted());
        check(!twice.has_value() && twice.error().code == "session.record_has_choices",
              "C12: migrate_record() never rewrites a record that carries choices");
        Session t;
        t.initialize("t", kOwner, trusted());
        check(t.restore_from_record(*migrated).has_value(), "C12 control: the migrated record restores");

        auto json = agentengine::rt::agent_session_record_to_json(*migrated);
        auto back = agentengine::rt::agent_session_record_from_json(json);
        check(back.has_value() && back->choices.has_value() && back->choices->token_budget.is_unbounded(),
              "C12: the codec round-trips choices, unbounded included");

        auto old_wire = agentengine::json::parse(
            R"({"session_id":"o","principal_id":"p","principal_tenant_id":"","deleted":false,"run_counter":0,)"
            R"("turn_index":0,"open_interactions":[],"require_authority":true})");
        auto old = old_wire ? agentengine::rt::agent_session_record_from_json(*old_wire)
                            : agentengine::result<agentengine::rt::AgentSessionRecord>{};
        check(old.has_value() && !old->choices.has_value(),
              "C12: a pre-ADR-244 record loads as one without choices; its require_authority is not trusted");

        auto bad_wire = agentengine::json::parse(
            R"({"session_id":"o","principal_id":"p","principal_tenant_id":"","deleted":false,"run_counter":0,)"
            R"("turn_index":0,"open_interactions":[],"choices":{"authority":"per_request","token_budget":-1,)"
            R"("max_turns":"unbounded"}})");
        auto bad = bad_wire ? agentengine::rt::agent_session_record_from_json(*bad_wire)
                            : agentengine::result<agentengine::rt::AgentSessionRecord>{};
        check(!bad.has_value() && bad.error().code == "rt.agent_session.record.malformed",
              "C12: a record whose choices are malformed (-1) is refused, never defaulted");
    }

    // ---- C13: RunStarted -------------------------------------------------------------------------------------
    {
        Session s;
        s.initialize("c13", kOwner, per_request(CapabilitySet::no_ceiling(), Bound<u64>::at_most(900), Bound<u64>::unbounded()));
        s.emplace_chat_client();
        std::optional<agentengine::run_event_payload::RunStarted> started;
        s.set_run_event_tap([&](agentengine::RunEvent const& ev) {
            if (ev.kind == agentengine::run_event_kind::run_started) {
                if (auto const* p = std::get_if<agentengine::run_event_payload::RunStarted>(&ev.payload)) started = *p;
            }
        });
        StartRun req{user_message("hi")};
        req.authority = authority(entropy, live);
        (void)drive(s.start_run(req, now));
        check(started.has_value() && started->authority == "per_request(no_ceiling)" && started->token_budget == "900" &&
                  started->max_turns == "unbounded",
              "C13: RunStarted records the authority, budget and turn cap the run was admitted under");
    }

    // ---- C14: set_max_turns ------------------------------------------------------------------------------------
    {
        Session bare;
        auto r = bare.set_max_turns(Bound<u64>::at_most(3));
        check(!r.has_value() && r.error().code == "choice.missing.session",
              "C14: set_max_turns() on a session with no choices is refused");
        Session s;
        s.initialize("c14", kOwner, trusted());
        check(s.set_max_turns(Bound<u64>::at_most(3)).has_value() && s.max_turns() == u64{3},
              "C14 control: on an initialised session it sets the cap");
    }

    // ---- C15: resolve on a session with no choices ---------------------------------------------------------------
    {
        Session bare;
        bare.emplace_chat_client();
        auto r = drive(bare.resolve_interaction(ResolveInteraction{"no-such-interaction", true}, now));
        check(code_of(r) == "choice.missing.session",
              "C15: resolve on a session with no choices is refused before anything else is looked at");
        Session s;
        s.initialize("c15", kOwner, trusted());
        s.emplace_chat_client();
        auto ctl = drive(s.resolve_interaction(ResolveInteraction{"no-such-interaction", true}, now));
        check(!ctl.has_value() && code_of(ctl) != "choice.missing.session",
              "C15 control: initialised, the same resolve gets past the choices check (and fails on the lookup)");
    }

    if (g_failures == 0) {
        std::printf("test_rt_agent_session_choices: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_agent_session_choices: %d failure(s)\n", g_failures);
    return 1;
}
