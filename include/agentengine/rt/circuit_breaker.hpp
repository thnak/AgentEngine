#pragma once
// ADR-037 Phase 1: `agentengine::rt::CircuitBreaker`, reproducing `quark::CircuitBreaker`'s
// (historical: third_party/quark/include/quark/core/governance.hpp) Closed -> Open -> Half-Open
// state machine as new, self-contained AgentEngine code -- zero `quark::` dependency. Lives under
// `agentengine::rt`. RESOLVED by a later ADR-037 Phase 2 pass: `core/model_call_gateway.hpp` now
// wires a real `rt::CircuitBreaker` per backend (see that file's own banner) -- this file's
// standalone proof was the precursor to that migration, not still-pending work.
//
// Why reproduce rather than redesign: `model_call_gateway.hpp`'s `attempt_with_retry` depends on
// this EXACT state machine's exact semantics (022-Resource-Governance-and-Overload-Control §3, as
// implemented by governance.hpp, not as re-derived from the spec prose) -- trip after N CONSECUTIVE
// failures, shed (never queue/delay) for the whole cooldown window, admit exactly ONE half-open
// probe once the cooldown elapses, close on a successful probe, reopen with a FRESH cooldown clock
// on a failed probe. Getting any one of those wrong would silently change retry/failover behavior
// once Phase 2 wires this in, so this type is a faithful port, not a fresh design.
//
// CLOCK: every time-dependent method takes an explicit monotonic `now_ns`, exactly like the Quark
// original -- the caller supplies its own clock (production: a real monotonic clock; tests: a fake,
// hand-advanced one), so this header makes no syscall and is fully deterministic under test.
//
// CONCURRENCY: single-writer, per-instance, no internal synchronization -- same posture as the
// Quark original (one breaker per {provider, model, secret} tier, mutated only by whichever caller
// currently owns that tier's retry loop). A caller sharing one instance across threads must
// serialize its own access; this type does not attempt to.

#include <cstdint>

namespace agentengine::rt {

// The admission verdict this breaker hands back at send time. Deliberately just the two values the
// breaker itself produces -- Quark's `quark::Admit` also has `Delay` for its token-bucket/fair-share
// limiters, which this breaker never returns (the whole point of a circuit breaker is fail-fast, not
// bounded smoothing), so `Delay` is not reproduced here.
// ae-naming-lint: allow Admit — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class Admit : std::uint8_t { Accept = 0, Shed = 1 };

// Per (caller, logical target) circuit breaker. Closed -> Open (fail fast) -> Half-Open (a SINGLE
// probe) -> Closed on success / Open on failure. Trips after `fail_threshold` CONSECUTIVE failures;
// stays Open for `open_ns` (the cooldown), then admits exactly one half-open probe. O(1), no heap
// allocation, single-writer -- identical shape to `quark::CircuitBreaker`.
// ae-naming-lint: allow CircuitBreaker — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class CircuitBreaker {
public:
    enum class State : std::uint8_t { Closed = 0, Open = 1, HalfOpen = 2 };

    constexpr CircuitBreaker() noexcept = default;
    // `fail_threshold` of 0 is clamped to 1 (a breaker that never trips is never useful, and 0
    // consecutive failures makes the trip condition `++consec_fail_ >= 0` true on the very first
    // failure anyway -- clamping to 1 just makes that explicit rather than accidental) -- same
    // clamp the Quark original applies.
    constexpr CircuitBreaker(std::uint32_t fail_threshold, std::int64_t open_duration_ns) noexcept
        : fail_threshold_(fail_threshold == 0 ? 1 : fail_threshold), open_ns_(open_duration_ns) {}

    // Admission check, called BEFORE every attempt (including the very first). Accepts while
    // Closed; while Open, sheds without touching any state UNTIL the cooldown has elapsed, at which
    // point it transitions to Half-Open and admits exactly ONE probe (marking one in flight so a
    // second concurrent on_send() call, before that probe's own on_result() lands, still sheds --
    // "at most one probe in flight" is enforced here, not left to the caller).
    [[nodiscard]] Admit on_send(std::int64_t now_ns) noexcept {
        switch (state_) {
            case State::Closed:
                return Admit::Accept;
            case State::Open:
                if (now_ns - opened_at_ns_ >= open_ns_) {
                    state_ = State::HalfOpen;
                    probe_inflight_ = true;
                    return Admit::Accept;  // the single half-open probe
                }
                return Admit::Shed;  // fail fast -- still cooling down
            case State::HalfOpen:
                if (probe_inflight_) return Admit::Shed;  // one probe at a time
                probe_inflight_ = true;
                return Admit::Accept;
        }
        return Admit::Accept;  // unreachable; keeps the compiler happy
    }

    // Feeds back the outcome of an admitted send (ok = succeeded; !ok = failed/timed out). A caller
    // must only call this for an attempt that on_send() actually admitted (Accept) -- feeding a
    // result for a shed attempt has no defined meaning here, same contract as the Quark original.
    void on_result(bool ok, std::int64_t now_ns) noexcept {
        if (state_ == State::HalfOpen) {
            probe_inflight_ = false;
            if (ok) {
                state_ = State::Closed;
                consec_fail_ = 0;
            } else {
                trip(now_ns);  // still failing -> re-open, restart the cooldown from NOW
            }
            return;
        }
        if (ok) {
            consec_fail_ = 0;  // a success anywhere in Closed resets the CONSECUTIVE-failure count
            return;
        }
        if (++consec_fail_ >= fail_threshold_) trip(now_ns);
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::uint32_t consecutive_failures() const noexcept { return consec_fail_; }
    [[nodiscard]] bool is_open() const noexcept { return state_ == State::Open; }

private:
    void trip(std::int64_t now_ns) noexcept {
        state_ = State::Open;
        opened_at_ns_ = now_ns;
        probe_inflight_ = false;
    }

    std::uint32_t fail_threshold_ = 5;
    std::int64_t open_ns_ = 0;
    std::uint32_t consec_fail_ = 0;
    std::int64_t opened_at_ns_ = 0;
    State state_ = State::Closed;
    bool probe_inflight_ = false;
};

}  // namespace agentengine::rt
