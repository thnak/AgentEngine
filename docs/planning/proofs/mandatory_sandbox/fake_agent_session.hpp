#pragma once
// PROVE-PHASE PROBE (A9): a minimal stand-in mirroring the REAL `agentengine::rt::AgentSession<
// ChatClientT, StateT, HistoryProviderT>`'s own actual, unmodified `fork_from()`/
// `clear_in_process_state()` STATEMENTS exactly (read directly from include/agentengine/rt/
// agent_session.hpp, not guessed) -- deliberately NOT the real 2900-line class itself (modifying or
// even fully replicating that class is implementation-time work, out of scope for a design/prove
// pass; ADR-096's own C2 finding used the identical "throwaway stand-in, not the real class" method
// to prove its own fork_from()-compiles-or-doesn't claim).
//
// The two lines this file exists to exercise, copied verbatim in spirit from the real class:
//   fork_from():              history_provider_ = source.history_provider_;
//   clear_in_process_state(): history_provider_ = HistoryProviderT{};
// Also mirrors `AgentSession::history_provider()`'s own real, public accessor (agent_session.hpp:657)
// -- the seam this file's own probe uses to check `is_bound()`/`runtime()`/`would_fork_succeed()`
// from outside, matching the real class exactly, not a probe-only convenience.

#include <string>
#include <utility>

namespace probe {

template <class HistoryProviderT>
class FakeAgentSession {
public:
    [[nodiscard]] HistoryProviderT& history_provider() noexcept { return history_provider_; }
    [[nodiscard]] HistoryProviderT const& history_provider() const noexcept { return history_provider_; }

    void initialize(std::string session_id) { session_id_ = std::move(session_id); }
    [[nodiscard]] std::string const& session_id() const noexcept { return session_id_; }

    // Mirrors agent_session.hpp:1161-1190's REAL body exactly for the two fields this probe cares
    // about (session_id_, history_provider_) -- the other ~15 fields that function also resets
    // (run_counter_, effect_context_, etc.) are real but orthogonal to what THIS file exists to
    // prove, so are not replicated here (this is a stand-in for the fork_from()/HistoryProviderT
    // interaction specifically, not a full port of the class).
    void fork_from(FakeAgentSession const& source, std::string new_session_id) {
        session_id_ = std::move(new_session_id);
        history_provider_ = source.history_provider_;   // THE real statement this file exists to test
    }

    // Mirrors agent_session.hpp:1210-1238's REAL body exactly for the same two fields.
    void clear_in_process_state() {
        session_id_.clear();
        history_provider_ = HistoryProviderT{};   // THE real statement this file exists to test
    }

private:
    std::string session_id_;
    HistoryProviderT history_provider_;
};

}  // namespace probe
