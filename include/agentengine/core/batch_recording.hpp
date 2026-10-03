#pragma once
// decisions/ADR-236-batch-recording-and-replay.md: the recorded seam for vendor batch results (I5), closing
// ADR-235 §7's "no recorded seam for replaying batch results". 004-Model-Provider-Plane.md §6 records a
// synchronous model call (core/chat_recording.hpp); this file does the same for the `BatchBackend` seam
// (core/batch_backend.hpp) and for the batch clock (`rt::BatchPolicy::now_ns`), because a batched round's
// outcome depends on both: what the vendor answered on each poll, and how old each job was when it did
// (`poll_error_grace`, `max_wait`).
//
// ONE ORDERED STREAM PER RUN. A `BatchRecorder` owns a single sequence of `BatchCallRecord`s. Every backend it
// wraps and the clock it wraps append to that one sequence, in the order the supervisor made the calls -- so a
// replay reproduces the interleaving of polls and clock reads, not just each backend's own answers. The
// supervisor's batch code iterates vectors only (src/rt/workflow_supervisor_batch.cpp), so that order is a
// function of the run's inputs (ADR-236 §7 names the one exception: `deadline_ms`).
//
// REPLAY CHECKS WHAT IT IS ASKED. Unlike `ReplayChatClient`, which ignores the live request, a `BatchReplayer`
// compares every call against the record it is about to answer with: the operation, the backend's group key,
// the job id, the principal and tenant the call ran under (I4), and the requests (`batch_request_to_json`,
// compared byte for byte). A call that does not match latches the replayer as diverged; that call and every
// later one fail with `contract` (`batch_replay.diverged`), the clock stops advancing, and the host's
// `on_divergence` hook runs once. The reason: a batched answer is matched to its node by custom id alone, so a
// replay fed a different input would otherwise hand the recorded answers to prompts they were never generated
// for, and the run would complete looking correct.
//
// A DIVERGENCE MUST STOP THE RUN, NOT ONLY THE BATCH. To the supervisor a failed `admit` means "not batchable"
// and a failed `submit` means "send it synchronously" (ADR-235 §3.3), so on its own a divergence turns into
// synchronous model calls and a run that completes on fresh answers. A replay host passes
// `on_divergence([&sup](auto const&) { sup.cancel(); })`; `WorkflowSupervisor::cancel()` is safe to call from
// inside a round (it requests stop and lets the entry point settle the run).
//
// NOT A CHECKPOINT, NOT AUTHORITY. A recording holds resolved results, which ADR-235 §3.4 keeps out of every
// `RunStateRecord`. Nothing in the engine reads a recording on its own: a host builds a `BatchReplayer` from
// a file it chose, exactly as it builds a `ReplayChatClient`, and hands its backend to a node's
// `BatchableModelCall` in place of the vendor one. No restore path loads it. The records carry requests and
// responses but never a credential -- a backend resolves its secret inside submit()/poll() and the recorder
// sees only the arguments and the return value.
//
// File form: JSON Lines, one record per line, appended as calls happen (`append_batch_call_record`), so a run
// that suspends for hours and is restored in another process keeps adding to the same file. A record is written
// after its call returns, so a crash in between loses that record and the replay then diverges or runs out --
// it never answers wrongly. A torn final line (a crash mid-append) is skipped on read, and the next append
// starts on a fresh line.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/batch_backend.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine {

// `backend` is a header, written when a backend is wrapped: it names the group key and limits a replay
// backend reports. It is not a call and a replay never matches a call against it. `clock` is one read of the
// wrapped clock. The rest are one call each on the `BatchBackend` seam.
enum class batch_call_op { backend, admit, submit, poll, cancel, release, clock };  // ae-naming-lint: allow batch_call_op — ADR-236

[[nodiscard]] char const* batch_call_op_tag(batch_call_op op) noexcept;

struct BatchCallRecord {  // ae-naming-lint: allow BatchCallRecord — ADR-236
    batch_call_op op = batch_call_op::admit;
    std::string   group_key{};  // every op but clock

    // -- inputs (compared on replay) ----------------------------------------------------------------------
    std::string job_id{};                                          // poll, cancel, release
    json::Value request{};                                         // admit: batch_request_to_json(request)
    std::vector<std::pair<std::string, json::Value>> items{};      // submit: (custom_id, request json) in order
    std::string principal_id{};                                    // submit, poll, cancel, release: ctx.principal
    std::string tenant_id{};

    // -- outcome --------------------------------------------------------------------------------------------
    std::optional<error>       failure{};  // the call returned an error
    std::optional<std::string> threw{};    // the call threw a std::exception; the text of what()
    bool                       threw_non_standard = false;  // the call threw something else
    BatchLimits                limits{};   // backend
    std::size_t                admitted_bytes = 0;  // admit, on success
    std::string                submitted_job{};     // submit, on success
    BatchPoll                  poll{};              // poll, on success
    std::int64_t               now_ns = 0;          // clock
};

// Every field of a `ChatRequest` that can change what a vendor is asked, so a replay can tell two requests apart:
// `chat_request_to_json` (004 §6's recording profile) plus `reasoning_effort` and `client_interaction_answers`,
// which that profile leaves out. A compile-time arity check in the .cpp breaks the build when `ChatRequest` gains
// a field, so this cannot silently fall behind it.
[[nodiscard]] json::Value batch_request_to_json(ChatRequest const& r);

[[nodiscard]] json::Value batch_poll_to_json(BatchPoll const& p);
[[nodiscard]] result<BatchPoll> batch_poll_from_json(json::Value const& j);

// `now_ns` is written as a decimal string: nanoseconds since the epoch exceed a double's 2^53 exact range.
[[nodiscard]] json::Value batch_call_record_to_json(BatchCallRecord const& rec);
[[nodiscard]] result<BatchCallRecord> batch_call_record_from_json(json::Value const& j);

// Appends one record as one line, creating the file if needed, and starting a fresh line if the file ends in a
// torn one.
[[nodiscard]] result<void> append_batch_call_record(std::filesystem::path const& path, BatchCallRecord const& rec);
// Reads every line of a JSON Lines recording. Blank lines are skipped; so is a final line with no newline that
// does not parse (a torn write). Any other malformed line is an error naming it. Each line is parsed with a
// budget sized for a full job (`kBatchRecordingParseBudget`), not the JSON parser's default.
[[nodiscard]] result<std::vector<BatchCallRecord>> read_batch_recording(std::filesystem::path const& path);

// A poll or submit record carries every item of a job on one line: up to 50,000 items (the largest documented
// vendor cap) of a few dozen JSON nodes each.
inline constexpr json::ParseBudget kBatchRecordingParseBudget{64, 50'000'000};

// Records every call made through the backends and the clock it wraps, into one ordered sequence.
//
// TRANSPARENT: a wrapped backend returns exactly what its inner backend returned, and rethrows what it threw;
// the wrapped clock returns the inner clock's value. A failure to record -- the sink throwing, or running out
// of memory building a record -- never reaches the caller: the call's real outcome is returned unchanged and
// `failed()` turns true. (A throw there after a successful `submit` would otherwise read as a failed submit and
// orphan a paid vendor job.) The sink runs under the recorder's lock, so records reach it in call order; a sink
// must not call back into a wrapped backend or clock.
class BatchRecorder {  // ae-naming-lint: allow BatchRecorder — ADR-236
public:
    using Sink = std::function<void(BatchCallRecord const&)>;

    explicit BatchRecorder(Sink sink);

    // Emits a `backend` header and returns a backend that records each call before returning its outcome.
    // A null `inner` returns null.
    [[nodiscard]] std::shared_ptr<BatchBackend> wrap(std::shared_ptr<BatchBackend> inner) const;
    // Returns a clock that records each reading of `inner`. Pass the result as `BatchPolicy::now_ns`.
    [[nodiscard]] std::function<std::int64_t()> clock(std::function<std::int64_t()> inner) const;

    // True once any record failed to reach the sink: the recording is incomplete and will not replay exactly.
    [[nodiscard]] bool failed() const;

    struct State;

private:
    std::shared_ptr<State> state_;
};

// Answers the calls a recording holds, in order, checking each one (see the file banner).
class BatchReplayer {  // ae-naming-lint: allow BatchReplayer — ADR-236
public:
    using DivergenceHook = std::function<void(std::string const&)>;

    // Refuses a recording whose headers give one group key two different limit sets, or a record with no
    // group key where one is required.
    [[nodiscard]] static result<BatchReplayer> from_records(std::vector<BatchCallRecord> records);

    // A backend that replays the calls recorded for `group_key`. Refused when the recording has no header
    // for that key.
    [[nodiscard]] result<std::shared_ptr<BatchBackend>> backend(std::string const& group_key) const;
    // A clock that replays the recorded readings. After a divergence it repeats the last value it served.
    [[nodiscard]] std::function<std::int64_t()> clock() const;

    // Runs once, with the reason, when the replay first diverges -- outside the replayer's lock, so it may call
    // `WorkflowSupervisor::cancel()`. See the file banner: a replay host should always set it.
    void on_divergence(DivergenceHook hook) const;

    [[nodiscard]] bool diverged() const;
    // Why the replay diverged (empty when it has not).
    [[nodiscard]] std::string divergence() const;
    // Records not yet answered, headers excluded. A complete replay ends at zero.
    [[nodiscard]] std::size_t remaining() const;

    struct State;

private:
    explicit BatchReplayer(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

}  // namespace agentengine
