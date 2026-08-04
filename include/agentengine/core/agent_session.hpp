#pragma once
// Implements 005-Sessions-State-and-Memory.md §1 and 001-Execution-Model.md §1/§3. Terminology
// (027 §7): the type is `AgentSession`, not the bare `Session` an earlier draft used — "session"
// remains the ordinary word everywhere in prose.
//
// 001 §1: "One Quark actor instance, key = session_id" — `AgentSession` IS the actor, not a plain
// data struct with a separate actor wrapper; 027 gives no second name for a wrapper type. It is a
// template over its `ChatClient` backend (`AgentSession<ChatClientT>`), following this project's
// CRTP-policy idiom (CONVENTIONS' `Sandbox<Strict>`/`MaxTurns<12>` examples) rather than
// type-erasing the seam this early — 004's real `ChatClient` seam (and any type-erasure decision
// for it) isn't due until Milestone 5.
//
// M1 scope only (docs/planning/milestone-1-core-substrate-breakdown.md task 3): `handle()`
// implements 001 §3's turn loop in miniature — assemble context (the full history, trivially),
// call the provider, append the response. Steps 3a-3c (tool-call resolution, approval, invocation)
// don't apply with no tools declared yet (006/002 land in Milestone 2); policy resolution,
// checkpointing (019), and real timestamps (which would be an unrecorded wall-clock read, 001 §7 —
// premature before Clock is a wired capability) are deliberately not touched here.

#include <chrono>
#include <string>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine {

// 001 §1: "An Ask<StartRun, RunResponse> to the session actor." `StartRun` is the literal message
// name 001 gives; the reply type is `AgentResponse` (027 §2's canonical name for "a run's result"),
// not a separate `RunResponse` type — 001's "RunResponse" is prose describing the concept, and 027
// is normative for the actual identifier (027 §1: "Adopt MAF's name wherever the concept is the
// same"). One concept, one name.
struct StartRun {  // ae-naming-lint: allow StartRun — 001 §1 names this message type normatively; 027 has not been updated to list it (same tracked-gap category as the M0 backlog)
    Message input;  // the new turn to append to history and process (001 §3 step 1)
};

// 027 §2's canonical name; already listed there, no suppression needed.
struct AgentResponse {
    Message message;
    Usage   usage;
};

template <class ChatClientT>
    requires ChatClient<ChatClientT>
class AgentSession : public quark::Actor<AgentSession<ChatClientT>, quark::Sequential> {
public:
    using protocol = quark::Protocol<quark::Ask<StartRun, AgentResponse>>;

    // 001 §3's turn loop, in miniature. Synchronous: `ChatClientT::chat()` is itself synchronous at
    // this milestone (chat_client.hpp: `ae::task<T>` not wired in yet), so no coroutine is needed to
    // run a real one-turn run end to end.
    void handle(quark::Ask<StartRun, AgentResponse> const& m) {
        history_.push_back(m.query.input);

        ChatRequest request{history_};
        result<ChatResponse> response = chat_client_.chat(request, effect_context_);
        if (!response) {
            // 001 §6's failure classification isn't wired up at this milestone — fail closed by
            // never responding, rather than fabricating a placeholder AgentResponse. The caller's
            // ask then resolves however the reply-cell's own "never replied" path surfaces it
            // (quark/core/testkit.hpp: "failed by reply-before-teardown if the handler never
            // replied — never a hang").
            return;
        }

        history_.push_back(response->message);
        m.respond(AgentResponse{response->message, response->usage});
    }

    [[nodiscard]] std::vector<Message> const& history() const noexcept { return history_; }

private:
    std::string                           session_id_;
    Principal                             principal_;
    std::vector<Message>                  history_;
    ChatClientT                           chat_client_;
    EffectContext                         effect_context_;
    std::chrono::system_clock::time_point created_at_{};
    std::chrono::system_clock::time_point updated_at_{};
};

} // namespace agentengine
