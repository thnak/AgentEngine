// ADR-037 Phase 2, Slice 1: ports the real behavioral claims of four Quark-actor-based test files
// onto agentengine::rt::AgentSession directly, consolidated into this one file. Each of the four old
// files (still present, untouched, superseded by this file's own coverage) targeted
// agentengine::AgentSession (include/agentengine/core/agent_session.hpp, the Quark-actor version)
// through quark::TestKit<Session>/kit.ask<...>(...); this file drives agentengine::rt::AgentSession
// directly via start_run()/drive<T>(), the same pattern test_rt_agent_session.cpp already established.
//
//   SECTION 1 (from test_agent_session_admission.cpp, 018 §2's Admission check) -- fully portable.
//     rt::AgentSession's start_run() runs the exact same admission check as the Quark original
//     (principal_admitted_for() against request.caller, before ChatClientT is ever reached) --
//     verified by reading rt/agent_session.hpp's start_run() directly. All five claims port with no
//     mechanism change, only the driving idiom (drive(session.start_run(...)) instead of
//     kit.ask<AgentResponse>(...)).
//
//   SECTION 2 (from test_agent_session_isolation.cpp, 001 §1's "one actor instance per session_id")
//     -- ADAPTED, one piece deliberately NOT ported. The old file's Part 1 exercised
//     `ae::session_actor_id()`, a `quark::ActorId`-producing pure function of the session_id string --
//     rt::AgentSession has NO actor-id concept at all: it is a plain host-held C++ object, not
//     something a router resolves by id, so there is nothing for session_actor_id() to correspond to
//     here. That half of the old file is a deliberately-not-ported claim, not silently dropped: this
//     section restates what the claim was actually FOR once the Quark-specific mechanism is stripped
//     away -- "the same session_id never lets two independently-constructed instances observe or
//     mutate each other's state" -- and proves that directly (Part 1 below) instead of via actor-id
//     equality. Part 2/3 (real two-session isolation with content that would visibly cross-contaminate
//     if broken) port unchanged in spirit, only the driving idiom differs.
//
//   SECTION 3 (from test_agent_session_poison_run.cpp, 019 §4's PoisonRunPolicy<N>) -- fully
//     portable, with one adaptation named plainly: `agentengine::PoisonRunPolicy<N>` itself lives in
//     core/agent_session.hpp, a header that pulls in quark/core/actor.hpp and friends -- including it
//     here would defeat this file's own Quark-free point for the sake of one pure, zero-dependency
//     struct (`is_quarantined()` is nothing but `consecutive_failures >= MaxAttempts`, no Quark type
//     anywhere in its signature or body). Rather than pull in that header just for this, the policy is
//     copied verbatim into this file's own anonymous namespace below (same name, same shape, same
//     four static_asserts the old file had) -- a faithful copy of a dependency-free type, not an
//     invented substitute. The BEHAVIORAL claim under test (a run that fails repeatedly on resume is
//     quarantined after exactly N bounded attempts, state preserved, never retried forever) is
//     unchanged and is exercised against the real rt::AgentSession::start_run().
//
//   SECTION 4 (from test_agent_session_run_identity.cpp, 001 §1/§2's Run/Turn identity) -- fully
//     ported, NOT found redundant. The task briefing named two specific already-ported rt:: files
//     (test_rt_agent_session_lifecycle.cpp, test_rt_agent_session_tooling_and_delegation.cpp) as
//     likely-redundant coverage to check first -- NEITHER FILE EXISTS in this worktree checkout
//     (verified: `tests/` contains only test_rt_agent_session.cpp,
//     test_rt_agent_session_background_task.cpp, and test_rt_agent_session_snapshot.cpp for
//     rt::AgentSession, confirmed by directory listing and a repo-wide grep for both filenames finding
//     nothing). Every rt::AgentSession test file that DOES exist was grepped for run_id/turn_index
//     assertions: none assert that a ChatClientT's EffectContext actually carries real run_id/
//     turn_index values (test_rt_agent_session_background_task.cpp checks a RunEvent's run_id field,
//     a different code path; test_rt_agent_session_snapshot.cpp checks last_run_id() after a record
//     restore, not live delivery through EffectContext during chat()). So this section is genuinely
//     uncovered ground, ported in full -- and extended slightly past the old file's own scope (which
//     only ever observed turn_index == 0, since its ChatClient always converged in one round): this
//     version scripts a tool-call round followed by a converging round, proving turn_index actually
//     advances (0 -> 1) WITHIN one run as multiple rounds happen, not just that it starts at 0 --
//     genuinely uncovered ground the task briefing specifically flagged as worth adding if found.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::ResolveInteraction;
using agentengine::rt::SessionCaller;
using agentengine::rt::StartRun;
using agentengine::task;

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

// Same driving idiom as test_rt_agent_session.cpp -- every ChatClientT fixture in this file is fully
// synchronous under the hood (chat() never suspends on anything external), so a plain "resume until
// done" loop is safe here. See that file's own comment for the fuller explanation.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Text;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;
using agentengine::ToolCall;
using agentengine::ContentItem;
using agentengine::call_provenance;
using agentengine::Principal;

// -- Section 3's adaptation: a faithful, dependency-free copy of agentengine::PoisonRunPolicy<N>
// (core/agent_session.hpp) -- see file banner for why it is copied rather than included. -----------
template <std::uint32_t MaxAttempts>
    requires(MaxAttempts >= 1)
struct PoisonRunPolicy {
    [[nodiscard]] static constexpr bool is_quarantined(std::uint32_t consecutive_failures) noexcept {
        return consecutive_failures >= MaxAttempts;
    }
};

// -- Shared message-building helpers, same shapes as test_rt_agent_session.cpp ----------------------
Message text_response(std::string text) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message tool_call_response(std::string call_id, std::string tool_name, std::string args) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    ToolCall call;
    call.call_id        = std::move(call_id);
    call.tool_name       = std::move(tool_name);
    call.arguments_json  = std::move(args);
    call.provenance      = call_provenance::vendor_structured;
    item.value = call;
    m.content.push_back(item);
    return m;
}

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

[[nodiscard]] std::string reply_text(agentengine::rt::AgentResponse const& r) {
    auto const* t = std::get_if<Text>(&r.message.content.front().value);
    return t != nullptr ? t->text : std::string{"<non-text reply>"};
}

SessionCaller as_caller(Principal const& p) { return SessionCaller{p.id, p.tenant_id}; }

// == Section 1 fixture: CannedChatClient (test_agent_session_admission.cpp) =========================
// A fixed reply is sufficient here -- this section is about whether chat() is reached AT ALL, not
// about what it returns.
class CannedChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        co_return ChatResponse{text_response("reply"), Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // unused -- stream_model_calls_ stays false throughout this file
    }
};
static_assert(agentengine::ChatClient<CannedChatClient>);

// == Section 2 fixture: EchoChatClient (test_agent_session_isolation.cpp) ===========================
// Echoes the last user message's text back, prefixed -- unlike a fixed canned reply, this makes
// cross-session leakage VISIBLE: if session A's turn loop ever saw session B's history, the echoed
// text would show it.
class EchoChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest const& request, EffectContext&) {
        std::string last_text = "<no user text>";
        if (!request.messages.empty()) {
            auto const& item = request.messages.back().content.front();
            if (std::holds_alternative<Text>(item.value)) last_text = std::get<Text>(item.value).text;
        }
        co_return ChatResponse{text_response("echo:" + last_text), Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }
};
static_assert(agentengine::ChatClient<EchoChatClient>);

// == Section 3 fixture: AlwaysFailingChatClient (test_agent_session_poison_run.cpp) =================
// Simulates "a run that fails repeatedly on resume" (019 §4) -- every single call fails, the worst
// case the quarantine bound exists to catch.
class AlwaysFailingChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        co_return std::unexpected(agentengine::error{
            agentengine::failure_class::transient, "simulated provider outage", "chat.always_fails"});
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }
};
static_assert(agentengine::ChatClient<AlwaysFailingChatClient>);

// == Section 4 fixture: RecordingChatClient (test_agent_session_run_identity.cpp, extended) =========
// Records the (run_id, turn_index) it actually observes through EffectContext on EVERY call --
// proves the identity AgentSession hands the ChatClient is real, not a parallel/unwired parameter --
// and plays back a scripted sequence of outcomes (same "next scripted outcome, repeats the last once
// exhausted" shape as test_rt_agent_session.cpp's ScriptedChatClient), so a single run can be made to
// run multiple rounds (tool-call round, then a converging round) and show turn_index actually
// advancing within that one run, not just starting at 0.
struct ScriptedOutcome {
    Message message;
    Usage usage;
};

struct RecordedCall {
    std::string run_id;
    std::uint64_t turn_index = 0;
};

class RecordingChatClient {
public:
    RecordingChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<ScriptedOutcome> script;
        std::vector<RecordedCall> log;
        std::size_t call_count = 0;
    };

    void set_script(std::vector<ScriptedOutcome> script) { state_->script = std::move(script); }
    [[nodiscard]] std::vector<RecordedCall> const& log() const { return state_->log; }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext& ctx) {
        state_->log.push_back(RecordedCall{ctx.run_id, ctx.turn_index});
        std::size_t const idx = state_->call_count < state_->script.size()
                                     ? state_->call_count
                                     : state_->script.size() - 1;
        ScriptedOutcome const& o = state_->script[idx];
        ++state_->call_count;
        co_return ChatResponse{o.message, o.usage};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<RecordingChatClient>);

}  // namespace

int main() {
    // =================================================================================================
    // SECTION 1 -- Admission (018 §2). Ported from test_agent_session_admission.cpp's H2-R1..R13.
    // =================================================================================================

    // --- (1) A matching caller is admitted; the run proceeds normally --------------------------------
    {
        AgentSession<CannedChatClient> session;
        Principal const owner = agentengine::make_embedded_principal("p-owner", "tenant-a");
        session.initialize("s-match", owner);
        session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hello"), as_caller(owner)}));
        check(outcome.has_value(), "ADM-1: a caller matching the session's owning principal is admitted");
        check(session.history().size() == 2,
              "ADM-2: an admitted run appends both the input and the reply to history");
        check(session.admission_denied_count() == 0,
              "ADM-3: an admitted run never increments admission_denied_count()");
    }

    // --- (2) An id mismatch (same tenant) is denied, fail-closed -------------------------------------
    {
        AgentSession<CannedChatClient> session;
        Principal const owner    = agentengine::make_embedded_principal("p-owner", "tenant-a");
        Principal const stranger = agentengine::make_embedded_principal("p-stranger", "tenant-a");
        session.initialize("s-mismatch", owner);
        session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hello"), as_caller(stranger)}));
        check(!outcome.has_value(),
              "ADM-4: a caller whose id doesn't match the session's owner is denied, never reaching "
              "the ChatClientT");
        check(!outcome.has_value() && outcome.error().code == "run.admission_denied",
              "ADM-5: the denial is specifically an admission denial");
        check(session.history().empty(),
              "ADM-6: a denied run never appends anything to history -- denial happens before "
              "run_counter_ even increments");
        check(session.admission_denied_count() == 1,
              "ADM-7: admission_denied_count() observes the denial even though start_run() itself "
              "returned an error, not a hang");
    }

    // --- (3) Same id, different tenant is ALSO denied -- id match alone is not ownership -------------
    {
        AgentSession<CannedChatClient> session;
        Principal const owner        = agentengine::make_embedded_principal("p-shared-id", "tenant-a");
        Principal const other_tenant = agentengine::make_embedded_principal("p-shared-id", "tenant-b");
        session.initialize("s-cross-tenant", owner);
        session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hello"), as_caller(other_tenant)}));
        check(!outcome.has_value(),
              "ADM-8: a same-id caller from a DIFFERENT tenant is denied -- an id collision across "
              "tenants is not ownership (018 §6)");
        check(session.admission_denied_count() == 1, "ADM-9: the cross-tenant attempt is counted");
    }

    // --- (4) A principal derived on_behalf_of the owner is NOT admitted to run on the owner's session
    // using its OWN identity -- SessionCaller can't express delegation, so this wire-level rule is
    // deliberately conservative: exact match only. -----------------------------------------------------
    {
        AgentSession<CannedChatClient> session;
        Principal const owner = agentengine::make_embedded_principal("p-owner", "tenant-a");
        session.initialize("s-delegated", owner);
        session.emplace_chat_client();

        auto derived = agentengine::derive_on_behalf_of(owner, "sub-agent-1");
        check(derived.has_value(), "ADM-10: deriving a first-hop delegated principal succeeds");

        auto outcome = drive(session.start_run(StartRun{user_message("hello"), as_caller(*derived)}));
        check(!outcome.has_value(),
              "ADM-11: a principal derived on_behalf_of the owner is DENIED when it presents its own "
              "(derived) id at the StartRun boundary -- delegation never implicitly grants a "
              "sub-agent authority to start runs on its parent's own session");
        check(session.admission_denied_count() == 1, "ADM-12: the attempt is counted as a denial");
    }

    // --- (5) Omitting `caller` entirely preserves unconditional-admission behavior -------------------
    {
        AgentSession<CannedChatClient> session;
        session.initialize("s-legacy", Principal{"p-legacy", ""});
        session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("hello")}));  // no caller asserted
        check(outcome.has_value(),
              "ADM-13: a StartRun with no caller asserted skips the admission check entirely");
        check(session.admission_denied_count() == 0, "ADM-14: no check ran, so nothing was denied");
    }

    // =================================================================================================
    // SECTION 2 -- Isolation (001 §1). Ported from test_agent_session_isolation.cpp's A1-R1..R10, with
    // session_actor_id() replaced by a direct behavioral proof -- see file banner.
    // =================================================================================================

    // --- Part 1: two independently-constructed instances given the SAME session_id never share or
    // interfere with each other's state -- the restated form of "one actor instance per session_id"
    // once there is no actor/router lookup to check: rt::AgentSession is a plain object, so the ONLY
    // way "same session_id" could matter is if something accidentally shared storage keyed off the
    // string. Proven by running a real turn on one instance and confirming the other, despite sharing
    // the identical session_id, observes nothing from it. ---------------------------------------------
    {
        AgentSession<EchoChatClient> session_x1;
        AgentSession<EchoChatClient> session_x2;
        session_x1.initialize("s-A", Principal{"p-alice", ""});
        session_x2.initialize("s-A", Principal{"p-alice", ""});
        session_x1.emplace_chat_client();
        session_x2.emplace_chat_client();

        check(session_x1.session_id() == session_x2.session_id(),
              "ISO-1: two separate instances given the same session_id both report that session_id "
              "back -- session_id is just a label a host manages, not a derived actor-id lookup key");

        auto r = drive(session_x1.start_run(StartRun{user_message("hello from X1")}));
        check(r.has_value(), "ISO-2: session_x1's own run completes");
        check(session_x1.history().size() == 2, "ISO-3: session_x1's history grew from its own run");
        check(session_x2.history().empty(),
              "ISO-4: session_x2, despite sharing the identical session_id string, observes NOTHING "
              "from session_x1's run -- two instances are never secretly the same storage just "
              "because their session_id matches");
    }

    // --- Part 2/3: real two-session isolation, proven with content that would cross-contaminate if
    // broken (an echoing mock, not a fixed canned reply). ----------------------------------------------
    {
        AgentSession<EchoChatClient> session_a;
        AgentSession<EchoChatClient> session_b;
        session_a.initialize("s-A", Principal{"p-alice", ""});
        session_b.initialize("s-B", Principal{"p-bob", ""});
        session_a.emplace_chat_client();
        session_b.emplace_chat_client();

        check(session_a.session_id() == "s-A" && session_b.session_id() == "s-B",
              "ISO-5: initialize() sets session_id, retrievable afterward");
        check(session_a.principal().id == "p-alice" && session_b.principal().id == "p-bob",
              "ISO-6: initialize() sets principal, retrievable afterward");

        auto r_a = drive(session_a.start_run(StartRun{user_message("hello from A")}));
        auto r_b = drive(session_b.start_run(StartRun{user_message("hello from B")}));

        check(r_a.has_value() && r_b.has_value(), "ISO-7: both sessions complete their own turn");
        if (r_a.has_value() && r_b.has_value()) {
            check(reply_text(*r_a) == "echo:hello from A",
                  "ISO-8: session A's reply reflects ONLY session A's input");
            check(reply_text(*r_b) == "echo:hello from B",
                  "ISO-9: session B's reply reflects ONLY session B's input, not A's");
        }

        check(session_a.history().size() == 2 && session_b.history().size() == 2,
              "ISO-10: each session's history grew by exactly its own turn + reply");

        // A second turn on A must still see only A's history (no leakage introduced by B's activity
        // running in between).
        auto r_a2 = drive(session_a.start_run(StartRun{user_message("second from A")}));
        check(r_a2.has_value() && reply_text(*r_a2) == "echo:second from A",
              "ISO-11: session A's second turn still reflects only A's own history");
        check(session_a.history().size() == 4,
              "ISO-12: session A's history grew independently of session B's activity");
        check(session_b.history().size() == 2,
              "ISO-13: session B's history is untouched by session A's second turn");
    }

    // =================================================================================================
    // SECTION 3 -- Poison run (019 §4). Ported from test_agent_session_poison_run.cpp's E4-R1..R4.
    // =================================================================================================
    {
        using Policy = PoisonRunPolicy<3>;
        static_assert(!Policy::is_quarantined(0), "0 consecutive failures is never quarantined");
        static_assert(!Policy::is_quarantined(2), "one attempt short of the bound is never quarantined");
        static_assert(Policy::is_quarantined(3), "exactly MaxAttempts consecutive failures IS quarantined");
        static_assert(Policy::is_quarantined(4), "past the bound stays quarantined, never un-quarantines");

        AgentSession<AlwaysFailingChatClient> session;
        session.initialize("s-poison", Principal{"p-remy", ""});
        session.emplace_chat_client();

        std::uint32_t consecutive_failures = 0;
        int attempts_made = 0;
        bool quarantined = false;
        for (int attempt = 1; attempt <= 10; ++attempt) {
            auto outcome = drive(session.start_run(StartRun{user_message("t" + std::to_string(attempt))}));
            attempts_made = attempt;
            if (outcome.has_value()) {
                consecutive_failures = 0;  // never reached against AlwaysFailingChatClient
            } else {
                ++consecutive_failures;
            }
            if (Policy::is_quarantined(consecutive_failures)) {
                quarantined = true;
                break;
            }
        }

        check(quarantined, "POISON-1: the poison run is eventually quarantined, not retried forever");
        check(attempts_made == 3,
              "POISON-2: quarantine triggers at EXACTLY the bound (3 attempts) -- never early, never "
              "late");

        // "State preserved for inspection... not discarded" -- every failed attempt still appended its
        // user turn to history_ (start_run()'s own push_back happens BEFORE the chat() call that then
        // fails), and nothing about quarantining ever calls clear_in_process_state()/delete_session().
        check(session.history().size() == 3,
              "POISON-3: the session's history has exactly 3 entries (one per failed attempt) -- fully "
              "intact and inspectable, never cleared just because the run was quarantined");
        check(session.session_id() == "s-poison" && session.principal().id == "p-remy",
              "POISON-4: session identity is untouched -- quarantine is a HOST-side bookkeeping "
              "decision, never a call into AgentSession's own delete/clear paths");
    }

    // =================================================================================================
    // SECTION 4 -- Run/Turn identity (001 §1/§2). Ported from test_agent_session_run_identity.cpp's
    // A3-R1..R6, extended with a within-run turn_index-advances proof -- see file banner for why this
    // section is not redundant with any existing rt:: test in this checkout.
    // =================================================================================================
    {
        AgentSession<RecordingChatClient> session;
        session.initialize("s-run", Principal{"p-dave", ""});
        RecordingChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("call-1", "get_weather", "{}"), Usage{1, 1, 0, 0, 0.0}},
            {text_response("final answer"), Usage{1, 1, 0, 0, 0.0}},
        });

        auto r1 = drive(session.start_run(StartRun{user_message("first")}));
        check(r1.has_value(), "RID-1: a multi-round run (tool-call round, then converging round) still "
                               "completes successfully");
        if (r1.has_value()) {
            check(reply_text(*r1) == "final answer",
                  "RID-2: start_run() returns the LAST round's response");
        }
        check(client.log().size() == 2,
              "RID-3: the ChatClientT was genuinely called twice -- once per round -- within this ONE "
              "run");
        if (client.log().size() == 2) {
            check(client.log()[0].run_id == "s-run:run:1" && client.log()[0].turn_index == 0,
                  "RID-4: round 1 of the run observes run_id=\"s-run:run:1\", turn_index=0 through "
                  "EffectContext -- the real identity, not a parallel/unwired parameter");
            check(client.log()[1].run_id == "s-run:run:1" && client.log()[1].turn_index == 1,
                  "RID-5: round 2 of the SAME run keeps the identical run_id but advances turn_index "
                  "to 1 -- turn_index is real round-within-run identity that actually moves as rounds "
                  "happen, not a value that merely starts at 0 and is never observed again");
        }
        check(session.last_run_id() == "s-run:run:1",
              "RID-6: last_run_id() matches what the ChatClient actually received");

        // A second StartRun on the SAME session mints a NEW, distinct run_id and resets turn_index to
        // 0 for the new run -- each StartRun is its own run (001 §1), deterministic from the session's
        // own monotonic counter, not wall-clock.
        client.set_script({{text_response("second"), Usage{1, 1, 0, 0, 0.0}}});
        auto r2 = drive(session.start_run(StartRun{user_message("second")}));
        check(r2.has_value() && reply_text(*r2) == "second",
              "RID-7: a second StartRun on the same session mints a new, incremented run_id");
        check(!client.log().empty() && client.log().back().run_id == "s-run:run:2" &&
                  client.log().back().turn_index == 0,
              "RID-8: the second run's own round observes run_id=\"s-run:run:2\" and turn_index RESET "
              "to 0 -- turn_index is scoped to the current run, never cumulative across runs");
        check(session.last_run_id() == "s-run:run:2", "RID-9: last_run_id() advances with each new run");

        // Two different sessions never mint the same run_id for their respective first runs, even
        // though both start their own counter at 1 -- the session_id prefix is what keeps them apart.
        AgentSession<RecordingChatClient> session_other;
        session_other.initialize("s-run-other", Principal{"p-erin", ""});
        RecordingChatClient& client_other = session_other.emplace_chat_client();
        client_other.set_script({{text_response("hi"), Usage{1, 1, 0, 0, 0.0}}});

        auto r_other = drive(session_other.start_run(StartRun{user_message("hi")}));
        check(r_other.has_value() && reply_text(*r_other) == "hi",
              "RID-10: a different session's first run completes normally");
        check(client_other.log().size() == 1 && client_other.log()[0].run_id == "s-run-other:run:1" &&
                  client_other.log()[0].turn_index == 0,
              "RID-11: a different session's first run has a distinct run_id "
              "(\"s-run-other:run:1\") despite an identical internal counter value (1), because the "
              "session_id prefix differs -- run_id is never accidentally shared across sessions");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_identity_and_admission: ALL PASS\n");
    return 0;
}
