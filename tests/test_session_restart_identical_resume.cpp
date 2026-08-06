// Milestone 4 Phase H1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md), the
// roadmap's own M4 exit criterion (001 §9 G2 / 019 §7 G1): "a session survives process restart
// with an identical resumed run." Phase D1 (test_agent_session_checkpoint.cpp) already proved a
// restored session's NEXT run_id continues the original sequence without reminting; Phase E4
// (test_agent_session_node_loss_fencing.cpp) already proved a zombie's post-fence-loss commit is
// rejected. Neither compared a restarted session's actual OUTPUT against an uninterrupted
// control -- this is Phase H's own job: prove the milestone's exit-criterion claim directly, not
// just the supporting mechanics.
//
// "Kill -9 at a checkpoint boundary" is simulated literally: an in-process AgentSession instance
// is checkpointed after turn 1, then a SECOND turn is run against it but deliberately never
// checkpointed, then the whole instance (and its TestKit) is destroyed by leaving scope --
// nothing survives that isn't in the Store. A fresh instance restores from the last durable
// checkpoint (which, honestly, does NOT contain the uncheckpointed turn 2 -- exactly what a real
// crash between checkpoints loses) and resumes by re-submitting the same turn. `CannedChatClient`
// is a pure function of `EffectContext::run_id` (never wall-clock, never process-local state), so
// an uninterrupted control run against the SAME session_id/principal/turn sequence produces
// byte-identical output to what the resumed run produces for the same run_id -- proving the
// resumed run is indistinguishable from the one that would have happened without the crash.

#include <iostream>
#include <string>

#include "quark/core/activation.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

// Deterministic given ONLY effect_context.run_id (never wall-clock, never a process-local
// counter/address) -- the property that makes "byte-identical across two independent instances"
// a real claim rather than an accident of shared state.
class CannedChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::result<ae::ChatResponse> chat(ae::ChatRequest const&, ae::EffectContext& ctx) {
        ae::ContentItem item{};
        item.value  = ae::Text{"run=" + ctx.run_id};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply-" + ctx.run_id;
        reply.content.push_back(item);
        return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    int chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return 0; }  // unconstrained, unused
};
static_assert(ae::ChatClient<CannedChatClient>,
              "CannedChatClient must satisfy the ChatClient concept (004 §1)");

using Session = ae::AgentSession<CannedChatClient>;

ae::Message make_user_turn(std::string text, std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::move(text)};
    item.origin = ae::content_origin::user;

    ae::Message input{};
    input.role       = ae::role::user;
    input.message_id = std::move(message_id);
    input.content.push_back(item);
    return input;
}

std::string text_of(ae::AgentResponse const& resp) {
    if (resp.message.content.empty()) return {};
    if (auto const* t = std::get_if<ae::Text>(&resp.message.content.front().value)) return t->text;
    return {};
}

bool byte_identical(ae::AgentResponse const& a, ae::AgentResponse const& b) {
    return a.message.role == b.message.role && a.message.message_id == b.message.message_id &&
           text_of(a) == text_of(b) && a.usage.input_tokens == b.usage.input_tokens &&
           a.usage.output_tokens == b.usage.output_tokens &&
           a.usage.cached_input_tokens == b.usage.cached_input_tokens &&
           a.usage.reasoning_tokens == b.usage.reasoning_tokens &&
           a.usage.cost_estimate == b.usage.cost_estimate;
}

} // namespace

int main() {
    ae::Principal const principal{"p-h1", ""};

    // === Control: an uninterrupted session runs all 3 turns straight through ======================
    quark::TestKit<Session> control;
    control.actor().initialize("s-h1", principal);
    auto control_r1 = control.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("one", "m-1")});
    auto control_r2 = control.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("two", "m-2")});
    auto control_r3 = control.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("three", "m-3")});
    AE_CHECK(control_r1.has_value() && control_r2.has_value() && control_r3.has_value(),
             "setup: the uninterrupted control completes all 3 turns");

    // === The crash-simulated variant: same session_id/principal, its OWN Store =====================
    quark::InMemoryStore crash_store;
    quark::result<std::optional<ae::AgentSessionRecord>> loaded_after_crash;
    {
        quark::TestKit<Session> live;
        live.actor().initialize("s-h1", principal);

        auto r1 = live.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("one", "m-1")});
        AE_CHECK(r1.has_value() && control_r1.has_value() && byte_identical(*r1, *control_r1),
                 "H1-R1: turn 1, run identically against a fresh instance, is byte-identical to "
                 "the control's turn 1");

        // --- The checkpoint boundary: this write is the only thing that survives the crash below --
        auto const id    = ae::session_actor_id(live.actor().session_id());
        auto const fence = crash_store.acquire_fence(id);
        auto checkpoint1 =
            ae::save_agent_session_snapshot(live.activation(), crash_store, live.actor(), fence);
        AE_CHECK(checkpoint1.has_value(), "setup: checkpointing after turn 1 succeeds");

        // --- Turn 2 runs locally but is NEVER checkpointed -- exactly the work a real crash
        // between checkpoints loses ----------------------------------------------------------------
        auto r2_before_crash = live.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("two", "m-2")});
        AE_CHECK(r2_before_crash.has_value(), "setup: turn 2 runs locally before the simulated crash");

        // --- "kill -9": `live`'s TestKit and actor are destroyed here, at scope exit, with turn 2's
        // progress never having reached `crash_store` -----------------------------------------------
    }

    loaded_after_crash = ae::load_agent_session_snapshot(crash_store, "s-h1");
    AE_CHECK(loaded_after_crash.has_value() && loaded_after_crash->has_value(),
             "setup: the durable checkpoint from before the crash is still readable");
    AE_CHECK(loaded_after_crash.has_value() && loaded_after_crash->has_value() &&
                 (**loaded_after_crash).run_counter == 1,
             "H1-R2: the durable record honestly reflects only turn 1 (run_counter=1) -- turn 2's "
             "uncheckpointed progress is genuinely gone, matching real crash-recovery semantics "
             "for work done after the last checkpoint");

    // === Restart: a brand-new instance restores from the last checkpoint and resumes ===============
    quark::TestKit<Session> restarted;
    restarted.actor().restore_from_record(**loaded_after_crash);
    AE_CHECK(restarted.actor().last_run_id() == "s-h1:run:1",
             "setup: the restarted instance's run identity picks up exactly where the checkpoint "
             "left off");

    // The caller re-submits turn 2 (the work the crash lost) -- this is what "resume" means when
    // nothing durable remembers that turn 2 was ever attempted.
    auto r2_resumed = restarted.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("two", "m-2")});
    AE_CHECK(r2_resumed.has_value() && restarted.actor().last_run_id() == "s-h1:run:2",
             "H1-R3: the resumed run mints run:2 -- the SAME run_id the control's own turn 2 used, "
             "never reminting run:1 (which already happened) and never skipping to run:3");
    AE_CHECK(r2_resumed.has_value() && control_r2.has_value() && byte_identical(*r2_resumed, *control_r2),
             "H1-R4 (the exit-criterion claim): the resumed run's turn 2 output is BYTE-IDENTICAL "
             "to the uninterrupted control's turn 2 output -- a caller cannot distinguish the "
             "restarted session's behavior from one that never crashed");

    // Continuing past the resume point behaves exactly like the control too.
    auto r3_resumed = restarted.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("three", "m-3")});
    AE_CHECK(r3_resumed.has_value() && restarted.actor().last_run_id() == "s-h1:run:3",
             "H1-R5: the run sequence continues correctly past the resume point (run:3)");
    AE_CHECK(r3_resumed.has_value() && control_r3.has_value() && byte_identical(*r3_resumed, *control_r3),
             "H1-R6: turn 3 after resume is likewise byte-identical to the control's turn 3 -- the "
             "identical-resumed-run property holds for the rest of the run, not just the one turn "
             "immediately after restart");

    std::cout << (g_failures == 0 ? "test_session_restart_identical_resume: OK\n"
                                   : "test_session_restart_identical_resume: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
