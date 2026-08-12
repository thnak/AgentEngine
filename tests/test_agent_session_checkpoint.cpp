// Milestone 4 Phase D1/D2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md):
// 019 §1's checkpoint content ("the run's position... storage shared with sessions, no second
// persistence engine") and §1's cadence policy ("cost is bounded: incremental deltas plus
// periodic full checkpoints, with the cadence a policy") had no implementation before this task.
//
// D1: proves a checkpoint (still the same AgentSessionRecord/save_agent_session_snapshot Phase A4
// built, now extended with run_counter/turn_index) survives a restart with the run's EXACT
// position intact -- restoring into a fresh instance and confirming its next StartRun mints a
// run_id that continues the ORIGINAL sequence, never reusing a number.
//
// D2: proves CheckpointCadence<N>/checkpoint_if_due() actually skips writes between checks (the
// "cost is bounded" half that's provable without a real content-delta yet) -- with N=3 over 7
// completed turns, exactly 2 writes happen (at turn 3 and turn 6), and the durable record after
// turn 7 still reflects turn 6's position, not turn 7's -- an honest, observable trade-off of a
// cadence > 1, not silently hidden.

#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

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

class CannedChatClient {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        ae::ContentItem item{};
        item.value  = ae::Text{"ok"};
        item.origin = ae::content_origin::assistant;

        ae::Message reply{};
        reply.role       = ae::role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ae::ChatResponse{reply, ae::Usage{1, 1, 0, 0, 0.0}};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::ChatResponseUpdate upd;
        upd.delta.origin = ae::content_origin::assistant;
        upd.delta.value  = ae::Text{"ok"};
        upd.is_final     = true;
        upd.usage        = ae::Usage{1, 1, 0, 0, 0.0};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<CannedChatClient>,
              "CannedChatClient must satisfy the ChatClient concept (004 §1)");

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

} // namespace

int main() {
    using Session = ae::AgentSession<CannedChatClient>;

    // === D1: checkpoint content is the run's real position, and restoring it prevents run_id
    // reuse after a restart ==========================================================================
    {
        quark::InMemoryStore store;
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-ckpt", ae::Principal{"p-leo", ""});

        auto ckpt_r1 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("one", "m-1")});
        auto ckpt_r2 = kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("two", "m-2")});
        AE_CHECK(ckpt_r1.has_value() && ckpt_r2.has_value(), "setup: both turns succeed");
        AE_CHECK(kit.actor().last_run_id() == "s-ckpt:run:2",
                 "setup: 2 turns run, last_run_id is s-ckpt:run:2");

        auto const id    = ae::session_actor_id(kit.actor().session_id());
        auto const fence = store.acquire_fence(id);
        auto saved = ae::save_agent_session_snapshot(kit.activation(), store, kit.actor(), fence);
        AE_CHECK(saved.has_value(), "D1-R1: checkpointing after turn 2 succeeds");

        auto loaded = ae::load_agent_session_snapshot(store, "s-ckpt");
        AE_CHECK(loaded.has_value() && loaded->has_value() && (*loaded)->run_counter == 2 &&
                     (*loaded)->turn_index == 0,
                 "D1-R2: the checkpoint's own record carries the run's real position "
                 "(run_counter=2, turn_index=0 -- this milestone's one-model-call-per-run scope)");

        quark::TestKit<Session> fresh_kit;
        fresh_kit.actor().restore_from_record(**loaded);
        AE_CHECK(fresh_kit.actor().last_run_id() == "s-ckpt:run:2",
                 "D1-R3: restoring the checkpoint recovers last_run_id exactly, via the persisted "
                 "run_counter, not a re-parsed string");

        // The real point of D1: a restart must not remint a run_id that already happened.
        auto r3 = fresh_kit.ask<ae::AgentResponse>(ae::StartRun{make_user_turn("three", "m-3")});
        AE_CHECK(r3.has_value() && fresh_kit.actor().last_run_id() == "s-ckpt:run:3",
                 "D1-R4: after restoring from a checkpoint, the NEXT StartRun continues the "
                 "ORIGINAL run sequence (run:3) -- never reminting run:1 or run:2, which already "
                 "happened before whatever this checkpoint survived (001 §1)");
    }

    // === D2: CheckpointCadence<N> actually skips writes, and the durable record honestly lags
    // behind the live session by whatever the cadence skipped ======================================
    {
        quark::InMemoryStore store;
        quark::TestKit<Session> kit;
        kit.actor().initialize("s-cadence", ae::Principal{"p-mia", ""});

        auto const id    = ae::session_actor_id(kit.actor().session_id());
        auto const fence = store.acquire_fence(id);

        std::uint64_t since_last_checkpoint = 0;
        std::vector<bool> fired;
        for (int turn = 1; turn <= 7; ++turn) {
            auto turn_result = kit.ask<ae::AgentResponse>(
                ae::StartRun{make_user_turn("t" + std::to_string(turn), "m-" + std::to_string(turn))});
            AE_CHECK(turn_result.has_value(), "D2 setup: turn " + std::to_string(turn) + " succeeds");
            ++since_last_checkpoint;

            auto due = ae::checkpoint_if_due<ae::CheckpointCadence<3>>(kit.activation(), store,
                                                                         kit.actor(), fence,
                                                                         since_last_checkpoint);
            AE_CHECK(due.has_value(), "D2 setup: checkpoint_if_due never errors against InMemoryStore");
            bool did_fire = due.has_value() && *due;
            fired.push_back(did_fire);
            if (did_fire) since_last_checkpoint = 0;
        }

        AE_CHECK(fired.size() == 7 &&
                     !fired[0] && !fired[1] && fired[2] &&   // turns 1,2 skip, turn 3 fires
                     !fired[3] && !fired[4] && fired[5] &&   // turns 4,5 skip, turn 6 fires
                     !fired[6],                                // turn 7 skips
                 "D2-R1: CheckpointCadence<3> fires at exactly turns 3 and 6 out of 7 -- 5 writes "
                 "skipped, not every turn hitting the Store (019 §1's 'cost is bounded')");

        auto loaded = ae::load_agent_session_snapshot(store, "s-cadence");
        AE_CHECK(loaded.has_value() && loaded->has_value() && (*loaded)->run_counter == 6,
                 "D2-R2: after turn 7, the durable checkpoint still reflects turn 6's position "
                 "(run_counter=6) -- an HONEST lag the cadence policy creates, never silently "
                 "advanced to look current");
        AE_CHECK(kit.actor().last_run_id() == "s-cadence:run:7",
                 "D2-R3: meanwhile the LIVE in-process session is already at run 7 -- the gap "
                 "between live state and the last written checkpoint is exactly what a cadence > 1 "
                 "means, and it's observable here, not hidden");
    }

    std::cout << (g_failures == 0 ? "test_agent_session_checkpoint: OK\n"
                                   : "test_agent_session_checkpoint: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
