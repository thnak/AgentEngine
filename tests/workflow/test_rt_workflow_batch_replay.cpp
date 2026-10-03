// decisions/ADR-236-batch-recording-and-replay.md -- claims R1-R10: a batched workflow run recorded through
// core/batch_recording.hpp's BatchRecorder replays offline through a BatchReplayer to the same results, events
// and time-dependent decisions, and a replay asked anything the recording does not hold diverges instead of
// handing recorded answers to a different run.
//
// testing::ScriptedBatchBackend stands in for the vendor while recording; the replay runs have no vendor at
// all. Each negative claim was also run against a mutated replayer (ADR-236 §5).

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "agentengine/core/batch_recording.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/testing/scripted_batch_backend.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;
using agentengine::rt::BatchableModelCall;
using agentengine::rt::BatchPolicy;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::PollBatches;
using agentengine::rt::RunStateRecord;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;
using agentengine::testing::ScriptedBatchBackend;
using agentengine::workflow::Edge;
using agentengine::workflow::Executor;
using agentengine::workflow::Workflow;
using agentengine::workflow::edge_kind;
using agentengine::workflow::executor_kind;

namespace {

int g_failures = 0;
void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "  ok: %s\n", what.c_str());
    }
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] Message text_message(std::string text, role r = role::user) {
    ContentItem item{};
    item.origin = r == role::assistant ? content_origin::assistant : content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = r;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += "+";
            out += t->text;
        }
    }
    return out;
}

[[nodiscard]] Executor fn(char const* id, bool batch = false) {
    Executor e{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T",
               .worktree_mode = sharing_mode::shared, .capability_ceiling = {}};
    e.batch = batch;
    return e;
}

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

[[nodiscard]] BatchableModelCall model_node(std::string name, std::shared_ptr<BatchBackend> backend,
                                            std::shared_ptr<int> sync_calls) {
    return BatchableModelCall{
        [name](Message const& in, EffectContext&) -> result<ChatRequest> {
            ChatRequest r;
            r.messages.push_back(text_message(name + ":" + all_text_of(in)));
            return r;
        },
        [sync_calls](ChatRequest const& req, EffectContext&) -> result<ChatResponse> {
            if (sync_calls) ++*sync_calls;
            ChatResponse resp;
            resp.message = text_message("sync(" + all_text_of(req.messages.front()) + ")", role::assistant);
            resp.usage.input_tokens = 10;
            return resp;
        },
        [name](Message const&, ChatResponse const& resp) -> result<ExecutorOutcome> {
            return ExecutorOutcome{text_message(name + "=" + all_text_of(resp.message))};
        },
        std::move(backend)};
}

// start -> fan_out -> {w1, w2, w3} (batch model nodes) -> fan_in -> agg.
[[nodiscard]] Workflow fan_graph() {
    Workflow wf;
    wf.id        = "fan";
    wf.executors = {fn("start"), fn("w1", true), fn("w2", true), fn("w3", true), fn("agg")};
    for (char const* w : {"w1", "w2", "w3"}) {
        wf.edges.push_back(Edge{"start", w, edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
    }
    wf.start = "start";
    wf.output_selection.push_back("agg");
    wf.bound.max_rounds = 16;
    return wf;
}

void init(WorkflowSupervisor& sup, std::shared_ptr<BatchBackend> const& backend,
          std::shared_ptr<int> sync_calls = nullptr) {
    sup.initialize(fan_graph(), {appender("start"), model_node("w1", backend, sync_calls),
                                 model_node("w2", backend, sync_calls), model_node("w3", backend, sync_calls),
                                 appender("agg")});
}

// What a run looks like from outside: one line per result the host saw, then the event kinds.
struct Capture {
    std::vector<std::string> lines;

    void add(WorkflowResult const& r) {
        lines.push_back("status=" + std::to_string(static_cast<int>(r.status)) + " out=" + all_text_of(r.output) +
                        " in=" + std::to_string(r.usage.input_tokens) + " outtok=" +
                        std::to_string(r.usage.output_tokens) + " pending=" + std::to_string(r.pending_batches.size()) +
                        " poll_errors=" + std::to_string(r.batch_poll_errors) +
                        " unmatched=" + std::to_string(r.unmatched_batch_results) +
                        " partial=" + std::to_string(r.partial.size()));
    }
    void add_events(workflow::WorkflowEventStream& s) {
        std::string kinds = "events:";
        while (auto ev = s.next()) kinds += " " + std::to_string(static_cast<int>(ev->kind));
        lines.push_back(kinds);
    }
};

[[nodiscard]] bool same(Capture const& a, Capture const& b, std::string const& label) {
    if (a.lines == b.lines) return true;
    std::size_t const n = std::max(a.lines.size(), b.lines.size());
    for (std::size_t i = 0; i < n; ++i) {
        std::string const x = i < a.lines.size() ? a.lines[i] : "<none>";
        std::string const y = i < b.lines.size() ? b.lines[i] : "<none>";
        if (x != y) {
            std::fprintf(stderr, "  [%s] first difference at line %zu:\n    recorded: %s\n    replayed: %s\n",
                         label.c_str(), i, x.c_str(), y.c_str());
            break;
        }
    }
    return false;
}

// What a story runs against. `script` is the vendor while recording, null while replaying (the replayer
// already holds every answer); `clock` feeds BatchPolicy::now_ns.
struct Rig {
    std::shared_ptr<BatchBackend>  backend;
    ScriptedBatchBackend*          script = nullptr;
    std::function<std::int64_t()>  clock;
    BatchReplayer const*           replayer = nullptr;  // replaying: a divergence cancels the run (ADR-236 §3)
    std::shared_ptr<int>           sync_calls = std::make_shared<int>(0);
};

// What every replay host does: a divergence stops the run instead of falling back to live synchronous calls.
void stop_on_divergence(Rig const& rig, WorkflowSupervisor& sup) {
    if (rig.replayer) rig.replayer->on_divergence([&sup](std::string const&) { sup.cancel(); });
}

constexpr std::int64_t kStart = 1'790'987'445'123'456'789;  // past 2^53: exercises the decimal-string encoding

// Story A: submit; a transient poll failure; a partial result; the rest, with one item errored (falls back
// to a synchronous call) and the job ended; the run completes and the job is released.
[[nodiscard]] Capture story_a(Rig const& rig, std::shared_ptr<std::int64_t> const& now, std::string const& input) {
    Capture cap;
    WorkflowSupervisor sup;
    init(sup, rig.backend, rig.sync_calls);
    stop_on_divergence(rig, sup);
    BatchPolicy pol;
    pol.now_ns = rig.clock;
    (void)sup.enable_batch_coalescing(pol);
    auto stream = sup.enable_event_stream(std::pmr::get_default_resource());
    cap.add(drive(sup.run_workflow(RunWorkflow{text_message(input)})));
    if (rig.script) rig.script->fail_next_poll();
    *now += 5'000'000'000;
    cap.add(drive(sup.poll_batches(PollBatches{})));
    if (rig.script) rig.script->complete("job1", "i1", "b", Usage{5, 7});
    *now += 10'000'000'000;
    cap.add(drive(sup.poll_batches(PollBatches{})));
    if (rig.script) {
        rig.script->complete("job1", "i0", "a", Usage{5, 7});
        rig.script->fail_item("job1", "i2", batch_item_status::errored);
        rig.script->finish("job1");
    }
    *now += 10'000'000'000;
    cap.add(drive(sup.poll_batches(PollBatches{})));
    cap.add_events(stream);
    return cap;
}

// Story B (ADR-235 C22's shape): poll errors inside `poll_error_grace` are only reported; once the job is
// older than the grace, the 3rd consecutive one fails the items closed. The outcome depends on the clock.
[[nodiscard]] Capture story_b(Rig const& rig, std::shared_ptr<std::int64_t> const& now) {
    Capture cap;
    WorkflowSupervisor sup;
    init(sup, rig.backend, rig.sync_calls);
    stop_on_divergence(rig, sup);
    BatchPolicy pol;
    pol.max_poll_errors  = 3;
    pol.poll_error_grace = std::chrono::minutes{2};
    pol.now_ns           = rig.clock;
    (void)sup.enable_batch_coalescing(pol);
    cap.add(drive(sup.run_workflow(RunWorkflow{text_message("x")})));
    if (rig.script) rig.script->fail_next_polls(10, failure_class::contract);
    for (int i = 0; i < 3; ++i) {
        *now += 1'000'000'000;
        cap.add(drive(sup.poll_batches(PollBatches{})));
    }
    *now += std::chrono::nanoseconds(std::chrono::minutes{3}).count();
    for (int i = 0; i < 3; ++i) cap.add(drive(sup.poll_batches(PollBatches{})));
    return cap;
}

// Story C: suspend, checkpoint, restore into a fresh supervisor (as another process would), then poll.
// `after_restore` supplies the rig the restored supervisor runs against.
[[nodiscard]] Capture story_c(Rig const& first, std::function<Rig()> const& after_restore,
                              std::shared_ptr<std::int64_t> const& now) {
    Capture cap;
    RunStateRecord saved;
    {
        WorkflowSupervisor sup;
        init(sup, first.backend, first.sync_calls);
        stop_on_divergence(first, sup);
        BatchPolicy pol;
        pol.now_ns = first.clock;
        (void)sup.enable_batch_coalescing(pol);
        sup.set_checkpoint_hook([&saved](std::uint32_t, RunStateRecord const& rec) { saved = rec; });
        cap.add(drive(sup.run_workflow(RunWorkflow{text_message("x")})));
    }
    auto decoded = rt::decode_run_state_record(rt::encode_run_state_record(saved));
    if (!decoded) {
        cap.lines.push_back("decode failed");
        return cap;
    }
    Rig const rig = after_restore();
    WorkflowSupervisor fresh;
    init(fresh, rig.backend, rig.sync_calls);
    stop_on_divergence(rig, fresh);
    BatchPolicy pol;
    pol.now_ns = rig.clock;
    (void)fresh.enable_batch_coalescing(pol);
    fresh.restore_from_record(*decoded);
    if (rig.script) {
        rig.script->complete("job1", "i0", "a");
        rig.script->complete("job1", "i1", "b");
        rig.script->complete("job1", "i2", "c");
        rig.script->finish("job1");
    }
    *now += 60'000'000'000;
    cap.add(drive(fresh.poll_batches(PollBatches{})));
    return cap;
}

// Throws on its first poll, then behaves like the scripted backend it wraps.
class ThrowOnceBackend final : public BatchBackend {
public:
    explicit ThrowOnceBackend(std::shared_ptr<ScriptedBatchBackend> inner) : inner_(std::move(inner)) {}
    [[nodiscard]] std::string group_key() const override { return inner_->group_key(); }
    [[nodiscard]] BatchLimits limits() const override { return inner_->limits(); }
    [[nodiscard]] result<std::size_t> admit(ChatRequest const& r) const override { return inner_->admit(r); }
    [[nodiscard]] result<std::string> submit(std::vector<BatchItemRequest> const& items, EffectContext& c) override {
        return inner_->submit(items, c);
    }
    [[nodiscard]] result<BatchPoll> poll(std::string const& job, EffectContext& c) override {
        if (!thrown_) {
            thrown_ = true;
            throw std::runtime_error("vendor client blew up");
        }
        return inner_->poll(job, c);
    }
    [[nodiscard]] result<void> cancel(std::string const& job, EffectContext& c) override { return inner_->cancel(job, c); }
    [[nodiscard]] result<void> release(std::string const& job, EffectContext& c) override { return inner_->release(job, c); }

private:
    std::shared_ptr<ScriptedBatchBackend> inner_;
    bool                                  thrown_ = false;
};

struct Recorded {
    Capture                      capture;
    std::vector<BatchCallRecord> records;
};

[[nodiscard]] std::function<std::int64_t()> clock_of(std::shared_ptr<std::int64_t> const& now) {
    return [now] { return *now; };
}

[[nodiscard]] Recorded record_story_a(std::string const& input = "x") {
    Recorded out;
    auto now      = std::make_shared<std::int64_t>(kStart);
    auto scripted = std::make_shared<ScriptedBatchBackend>();
    BatchRecorder recorder([&out](BatchCallRecord const& r) { out.records.push_back(r); });
    out.capture = story_a(Rig{recorder.wrap(scripted), scripted.get(), recorder.clock(clock_of(now))}, now, input);
    return out;
}

[[nodiscard]] result<BatchReplayer> replayer_of(std::vector<BatchCallRecord> records) {
    return BatchReplayer::from_records(std::move(records));
}

// A replay rig over `replayer` (which must outlive it), or nullopt -- with a failed check -- when it does not load.
[[nodiscard]] std::optional<Rig> replay_rig(result<BatchReplayer> const& replayer, std::string const& label) {
    if (!replayer) {
        check(false, label + ": the recording loads: " + replayer.error().message);
        return std::nullopt;
    }
    auto backend = replayer->backend("scripted:model");
    if (!backend) {
        check(false, label + ": the replayer has the recorded backend");
        return std::nullopt;
    }
    return Rig{*backend, nullptr, replayer->clock(), &*replayer};
}

[[nodiscard]] std::string line_or_empty(Capture const& c, std::size_t i) { return i < c.lines.size() ? c.lines[i] : ""; }
[[nodiscard]] std::string last_line(Capture const& c) { return c.lines.empty() ? std::string{} : c.lines.back(); }
[[nodiscard]] std::string status_prefix(workflow_status st) { return "status=" + std::to_string(static_cast<int>(st)) + " "; }

}  // namespace

int main() {
    std::filesystem::path const dir =
        std::filesystem::temp_directory_path() / ("ae_batch_replay_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // ---- R1: a recorded run replays, through a JSON Lines file, to the same results and events ----------
    {
        Recorded rec = record_story_a();
        check(line_or_empty(rec.capture, 3).rfind(status_prefix(workflow_status::completed), 0) == 0,
              "R1: the recorded story completes: " + line_or_empty(rec.capture, 3));
        std::filesystem::path const file = dir / "story_a.jsonl";
        bool wrote = true;
        for (BatchCallRecord const& r : rec.records) wrote = wrote && append_batch_call_record(file, r).has_value();
        check(wrote, "R1: every record appends to the file");
        auto read = read_batch_recording(file);
        check(read.has_value() && read->size() == rec.records.size(), "R1: the file reads back record for record");
        auto replayer = replayer_of(read ? *read : std::vector<BatchCallRecord>{});
        if (auto rig = replay_rig(replayer, "R1")) {
            Capture const replayed = story_a(*rig, std::make_shared<std::int64_t>(0), "x");
            check(same(rec.capture, replayed, "R1"), "R1: the replay reproduces every result and every event");
            check(!replayer->diverged(), "R1: the replay never diverged: " + replayer->divergence());
            check(replayer->remaining() == 0, "R1: the replay consumed the whole recording");
        }
    }

    // ---- R2: recording is transparent -- the recorded run equals an unrecorded one ----------------------
    {
        Recorded rec = record_story_a();
        auto now      = std::make_shared<std::int64_t>(kStart);
        auto scripted = std::make_shared<ScriptedBatchBackend>();
        Capture const plain = story_a(Rig{scripted, scripted.get(), clock_of(now)}, now, "x");
        check(same(plain, rec.capture, "R2"), "R2: wrapping the backend and clock changes nothing the host sees");
    }

    // ---- R3: the clock is part of the recording: a time-dependent decision replays ----------------------
    {
        std::vector<BatchCallRecord> records;
        auto now      = std::make_shared<std::int64_t>(kStart);
        auto scripted = std::make_shared<ScriptedBatchBackend>();
        BatchRecorder recorder([&records](BatchCallRecord const& r) { records.push_back(r); });
        Capture const recorded = story_b(Rig{recorder.wrap(scripted), scripted.get(), recorder.clock(clock_of(now))}, now);
        check(last_line(recorded).rfind(status_prefix(workflow_status::executor_failed), 0) == 0,
              "R3: the recorded run fails closed once the job is past the grace");

        auto replayer = replayer_of(records);
        if (auto rig = replay_rig(replayer, "R3")) {
            Capture const replayed = story_b(*rig, std::make_shared<std::int64_t>(0));
            check(same(recorded, replayed, "R3"), "R3: replaying with the recorded clock fails closed at the same poll");
            check(!replayer->diverged() && replayer->remaining() == 0, "R3: and consumes the recording exactly");
        }

        // Control: the same backend answers (clock records dropped) with a live clock that never advances past
        // the grace. The run must NOT reach the recorded outcome -- the clock readings, not the poll answers,
        // decided it -- while the backend replay itself stays in step.
        std::vector<BatchCallRecord> no_clock;
        for (BatchCallRecord const& r : records) {
            if (r.op != batch_call_op::clock) no_clock.push_back(r);
        }
        auto calls_only = replayer_of(no_clock);
        if (auto rig = replay_rig(calls_only, "R3 control")) {
            auto frozen = std::make_shared<std::int64_t>(kStart);
            rig->clock  = [frozen] { return *frozen; };
            Capture const live_clock = story_b(*rig, std::make_shared<std::int64_t>(0));
            check(!calls_only->diverged(), "R3 control: the backend calls replay in step: " + calls_only->divergence());
            check(last_line(live_clock).rfind(status_prefix(workflow_status::suspended), 0) == 0,
                  "R3 control: with a clock that is not the recorded one, the run is still waiting: " +
                      last_line(live_clock));
        }
    }

    // ---- R4: a different input diverges, the run is cancelled, and no live call is made ----------------
    {
        Recorded rec = record_story_a("x");
        auto replayer = replayer_of(rec.records);
        if (auto rig = replay_rig(replayer, "R4")) {
            Capture const replayed = story_a(*rig, std::make_shared<std::int64_t>(0), "y");
            check(replayer->diverged(), "R4: the replay of a different input diverges");
            check(replayer->divergence().find("admit") != std::string::npos,
                  "R4: at the first call that differs (admit): " + replayer->divergence());
            check(line_or_empty(replayed, 0).rfind(status_prefix(workflow_status::cancelled), 0) == 0,
                  "R4: the divergence hook cancels the run: " + line_or_empty(replayed, 0));
            check(*rig->sync_calls == 0, "R4: no node fell back to a live synchronous model call");
            bool leaked = false;
            for (std::string const& l : replayed.lines) {
                leaked = leaked || l.find("w1=a") != std::string::npos || l.find("w2=b") != std::string::npos;
            }
            check(!leaked, "R4: no recorded vendor answer reached the run fed a different input");
        }
    }

    // ---- R5: an edited job id diverges at that poll ------------------------------------------------------
    {
        Recorded rec = record_story_a();
        for (BatchCallRecord& r : rec.records) {
            if (r.op == batch_call_op::poll) {
                r.job_id = "job-someone-else";
                break;
            }
        }
        auto replayer = replayer_of(rec.records);
        if (auto rig = replay_rig(replayer, "R5")) {
            Capture const replayed = story_a(*rig, std::make_shared<std::int64_t>(0), "x");
            check(replayer->diverged() && replayer->divergence().find("job-someone-else") != std::string::npos,
                  "R5: a poll for a job the recording did not poll diverges: " + replayer->divergence());
            check(line_or_empty(replayed, 1).rfind(status_prefix(workflow_status::cancelled), 0) == 0,
                  "R5: and the run is cancelled at that poll: " + line_or_empty(replayed, 1));
        }
    }

    // ---- R6: a truncated recording latches as exhausted, and stays latched ------------------------------
    {
        Recorded rec = record_story_a();
        rec.records.resize(rec.records.size() - 3);
        auto replayer = replayer_of(rec.records);
        if (auto rig = replay_rig(replayer, "R6")) {
            (void)story_a(*rig, std::make_shared<std::int64_t>(0), "x");
            check(replayer->diverged() && replayer->divergence().find("ended") != std::string::npos,
                  "R6: a call past the end of the recording diverges: " + replayer->divergence());
            EffectContext ctx{};
            auto again = rig->backend->poll("job1", ctx);
            check(!again && again.error().code == "batch_replay.diverged", "R6: every later call fails too");
        }
    }

    // ---- R7: a restore in the middle -- one file across two "processes", one replayer -------------------
    {
        std::filesystem::path const file = dir / "story_c.jsonl";
        auto sink = [file](BatchCallRecord const& r) { (void)append_batch_call_record(file, r); };
        auto now      = std::make_shared<std::int64_t>(kStart);
        auto scripted = std::make_shared<ScriptedBatchBackend>();
        BatchRecorder first(sink);
        Capture const recorded = story_c(
            Rig{first.wrap(scripted), scripted.get(), first.clock(clock_of(now))},
            [&] {
                BatchRecorder second(sink);  // the restored process builds its own recorder on the same file
                return Rig{second.wrap(scripted), scripted.get(), second.clock(clock_of(now))};
            },
            now);
        check(last_line(recorded).rfind(status_prefix(workflow_status::completed), 0) == 0,
              "R7: the recorded restore-then-poll completes");
        auto read     = read_batch_recording(file);
        auto replayer = replayer_of(read ? *read : std::vector<BatchCallRecord>{});
        if (auto rig = replay_rig(replayer, "R7 (two headers for one key load as one backend)")) {
            Rig const r = *rig;
            Capture const replayed = story_c(r, [&] { return r; }, std::make_shared<std::int64_t>(0));
            check(same(recorded, replayed, "R7"), "R7: the replay across the restore reproduces the run");
            check(!replayer->diverged() && replayer->remaining() == 0, "R7: and consumes the file exactly");
        }
    }

    // ---- R8: a backend that threw replays as a throw ------------------------------------------------------
    {
        std::vector<BatchCallRecord> records;
        auto now      = std::make_shared<std::int64_t>(kStart);
        auto scripted = std::make_shared<ScriptedBatchBackend>();
        BatchRecorder recorder([&records](BatchCallRecord const& r) { records.push_back(r); });
        Capture const recorded = story_a(
            Rig{recorder.wrap(std::make_shared<ThrowOnceBackend>(scripted)), scripted.get(), recorder.clock(clock_of(now))},
            now, "x");
        bool has_throw = false;
        for (BatchCallRecord const& r : records) has_throw = has_throw || (r.threw && *r.threw == "vendor client blew up");
        check(has_throw, "R8: the thrown poll is recorded with its message");
        auto replayer = replayer_of(records);
        if (auto rig = replay_rig(replayer, "R8")) {
            Capture const replayed = story_a(*rig, std::make_shared<std::int64_t>(0), "x");
            check(same(recorded, replayed, "R8"), "R8: the replay throws at the same call and the run matches");
        }
    }

    // ---- R9: the codec ------------------------------------------------------------------------------------
    {
        BatchCallRecord poll;
        poll.op        = batch_call_op::poll;
        poll.group_key = "k";
        poll.job_id    = "j";
        BatchItemResult ok;
        ok.custom_id = "i0";
        ok.status    = batch_item_status::succeeded;
        ok.response.message = text_message("hi", role::assistant);
        ok.response.usage   = Usage{3, 4};
        BatchItemResult bad;
        bad.custom_id = "i1";
        bad.status    = batch_item_status::errored;
        bad.klass     = failure_class::policy;
        bad.detail    = "denied";
        poll.poll.ended = true;
        poll.poll.items = {ok, bad};
        poll.poll.detail = "done";
        auto back = batch_call_record_from_json(batch_call_record_to_json(poll));
        check(back.has_value() && back->poll.ended && back->poll.items.size() == 2 &&
                  all_text_of(back->poll.items[0].response.message) == "hi" &&
                  back->poll.items[0].response.usage.output_tokens == 4 &&
                  back->poll.items[1].klass == failure_class::policy && back->poll.items[1].detail == "denied",
              "R9: a poll with a succeeded and an errored item round-trips");
        BatchCallRecord clk;
        clk.op     = batch_call_op::clock;
        clk.now_ns = kStart;
        auto cback = batch_call_record_from_json(json::parse(json::dump(batch_call_record_to_json(clk))).value());
        check(cback.has_value() && cback->now_ns == kStart, "R9: a nanosecond clock reading past 2^53 is exact");
        std::filesystem::path const file = dir / "bad.jsonl";
        std::ofstream(file, std::ios::binary) << "{\"op\":\"clock\",\"now_ns\":\"12\"}\n{\"op\":\"teleport\"}\n";
        auto bad_read = read_batch_recording(file);
        check(!bad_read && bad_read.error().message.find("line 2") != std::string::npos,
              "R9: a malformed line is refused, naming the line");
    }

    // ---- R10: the replayer refuses an incoherent recording and an unknown backend ------------------------
    {
        BatchCallRecord h1;
        h1.op        = batch_call_op::backend;
        h1.group_key = "k";
        h1.limits    = BatchLimits{10, 0, 1};
        BatchCallRecord h2 = h1;
        h2.limits.max_items = 20;
        check(!BatchReplayer::from_records({h1, h2}).has_value(), "R10: one key with two limit sets is refused");
        BatchCallRecord nokey;
        nokey.op = batch_call_op::poll;
        check(!BatchReplayer::from_records({h1, nokey}).has_value(), "R10: a call record with no group key is refused");
        auto ok = BatchReplayer::from_records({h1});
        check(ok.has_value() && !ok->backend("other").has_value(), "R10: a backend the recording never named is refused");
        check(ok.has_value() && ok->backend("k").has_value() && (*ok->backend("k"))->limits().max_items == 10,
              "R10: the replay backend reports the recorded limits");
    }

    // ---- R11: a real vendor run, recorded live by tools/batch_infer.cpp, replays offline -----------------
    // The same call sequence the tool made (3 admits, 1 submit, polls to the end, release), answered from the
    // recording: the real OpenRouter responses survive the codec and come back parsed.
    {
        auto read = read_batch_recording(std::filesystem::path(AE_TEST_FIXTURE_DIR) /
                                         "openrouter_gemini_flash_lite_2026-10-03.jsonl");
        check(read.has_value(), "R11: the live recording reads");
        auto replayer = replayer_of(read ? *read : std::vector<BatchCallRecord>{});
        std::string const key =
            "openrouter:openrouter.ai:443/api/v1:chat.completions:google/gemini-2.5-flash-lite:key=openrouter";
        auto backend = replayer ? replayer->backend(key) : result<std::shared_ptr<BatchBackend>>{};
        check(backend.has_value() && (*backend)->limits().max_items == 50000,
              "R11: the recorded OpenRouter backend and its limits");
        if (backend) {
            std::vector<std::string> const prompts = {"Name the capital of France in one word.",
                                                      "What is 2 + 2? Answer with just the number.",
                                                      "Name one primary color."};
            std::vector<BatchItemRequest> items;
            bool admitted = true;
            for (std::size_t i = 0; i < prompts.size(); ++i) {
                ChatRequest req;
                req.messages.push_back(text_message(prompts[i]));
                admitted = admitted && (*backend)->admit(req).has_value();
                items.push_back(BatchItemRequest{"i" + std::to_string(i), std::move(req)});
            }
            check(admitted, "R11: the three admits replay");
            EffectContext ctx{};
            ctx.principal = Principal{"batch-infer", ""};  // the principal tools/batch_infer.cpp runs under
            auto job = (*backend)->submit(items, ctx);
            check(job.has_value() && job->rfind("batch-", 0) == 0, "R11: the submit replays the vendor's job id");
            // The recording holds OpenRouter's real post-submit lag: the first poll answered 404 "not found".
            BatchPoll last;
            std::size_t polls = 0;
            std::size_t vendor_errors = 0;
            while (job && polls < 64 && !replayer->diverged()) {
                ++polls;
                auto p = (*backend)->poll(*job, ctx);
                if (!p) {
                    vendor_errors += p.error().code.rfind("batch_replay.", 0) == 0 ? 0 : 1;
                    continue;
                }
                last = *p;
                if (last.ended) break;
            }
            check(vendor_errors == 1, "R11: the vendor's recorded post-submit 404 replays as a poll error");
            check(last.ended && last.items.size() == 3, "R11: the polls replay to the ended job with 3 results");
            // The vendor returned the results out of order (i2, i1, i0): matched by custom id, as the engine does.
            std::string answers;
            for (BatchItemRequest const& sent : items) {
                for (BatchItemResult const& r : last.items) {
                    if (r.custom_id == sent.custom_id && r.status == batch_item_status::succeeded) {
                        answers += all_text_of(r.response.message) + "|";
                    }
                }
            }
            check(answers == "Paris|4|Red.|", "R11: each prompt's parsed answer comes back by custom id: " + answers);
            check(job && (*backend)->release(*job, ctx).has_value(), "R11: the release replays");
            check(replayer && !replayer->diverged() && replayer->remaining() == 0,
                  "R11: the replay matched every recorded call");
        }
    }

    // ---- R12: a request differing only in a field 004 §6's recording profile omits still diverges ---------
    // (red-team MAJOR 1: `chat_request_to_json` leaves out reasoning_effort and client_interaction_answers.)
    {
        auto scripted = std::make_shared<ScriptedBatchBackend>();
        std::vector<BatchCallRecord> records;
        BatchRecorder recorder([&records](BatchCallRecord const& r) { records.push_back(r); });
        auto wrapped = recorder.wrap(scripted);
        ChatRequest base;
        base.messages.push_back(text_message("q"));
        (void)wrapped->admit(base);
        (void)wrapped->admit(base);

        auto same_req = replayer_of(records);
        if (auto rig = replay_rig(same_req, "R12 control")) {
            check(rig->backend->admit(base).has_value() && !same_req->diverged(),
                  "R12 control: the identical request replays");
        }
        ChatRequest effort = base;
        effort.reasoning_effort = reasoning_effort::high;
        auto r_effort = replayer_of(records);
        if (auto rig = replay_rig(r_effort, "R12")) {
            check(!rig->backend->admit(effort).has_value() && r_effort->diverged(),
                  "R12: a request that adds a reasoning effort diverges");
        }
        ChatRequest answered = base;
        answered.client_interaction_answers.push_back(ClientInteractionAnswer{"ask-1", text_message("yes"), {}});
        auto r_answer = replayer_of(records);
        if (auto rig = replay_rig(r_answer, "R12")) {
            check(!rig->backend->admit(answered).has_value() && r_answer->diverged(),
                  "R12: a request that adds a client interaction answer diverges");
        }
    }

    // ---- R13: a call under another principal or tenant diverges (I4) -----------------------------------
    {
        auto scripted = std::make_shared<ScriptedBatchBackend>();
        std::vector<BatchCallRecord> records;
        BatchRecorder recorder([&records](BatchCallRecord const& r) { records.push_back(r); });
        auto wrapped = recorder.wrap(scripted);
        ChatRequest req;
        req.messages.push_back(text_message("q"));
        EffectContext alice{};
        alice.principal = Principal{"alice", "t1"};
        (void)wrapped->submit({BatchItemRequest{"i0", req}}, alice);
        check(records.size() == 2 && records[1].principal_id == "alice" && records[1].tenant_id == "t1",
              "R13: the submit records whose authority it ran under");
        for (auto const& [who, tenant, label] : {std::tuple{"alice", "t1", "control: the same principal replays"},
                                                 std::tuple{"bob", "t1", "another principal diverges"},
                                                 std::tuple{"alice", "t2", "another tenant diverges"}}) {
            auto replayer = replayer_of(records);
            if (auto rig = replay_rig(replayer, "R13")) {
                EffectContext ctx{};
                ctx.principal = Principal{who, tenant};
                bool const served = rig->backend->submit({BatchItemRequest{"i0", req}}, ctx).has_value();
                bool const should = std::string(label).rfind("control", 0) == 0;
                check(served == should && replayer->diverged() == !should, std::string("R13: ") + label);
            }
        }
    }

    // ---- R14: a throwing sink never changes the call's outcome (red-team MAJOR 3) -----------------------
    {
        auto scripted = std::make_shared<ScriptedBatchBackend>();
        BatchRecorder recorder([](BatchCallRecord const& r) {
            if (r.op != batch_call_op::backend) throw std::runtime_error("disk full");
        });
        auto wrapped = recorder.wrap(scripted);
        ChatRequest req;
        req.messages.push_back(text_message("q"));
        EffectContext ctx{};
        auto job = wrapped->submit({BatchItemRequest{"i0", req}}, ctx);
        check(job.has_value() && *job == "job1", "R14: a submit the vendor accepted still returns its job id");
        check(scripted->submitted().size() == 1, "R14: and the vendor saw exactly one submit");
        check(recorder.failed(), "R14: the recorder reports the recording as incomplete");
        bool clock_threw = false;
        try {
            (void)recorder.clock([] { return std::int64_t{7}; })();
        } catch (...) {
            clock_threw = true;
        }
        check(!clock_threw, "R14: a clock read never throws because the sink did");

        // Control: the same submit through a backend that fails reports the failure, not success.
        scripted->fail_next_submit();
        auto failed = wrapped->submit({BatchItemRequest{"i0", req}}, ctx);
        check(!failed.has_value(), "R14 control: a submit the vendor refused is still reported as refused");
    }

    // ---- R15: a job at a realistic size reads back (red-team MAJOR 4: the default parse budget) ---------
    {
        BatchCallRecord big;
        big.op        = batch_call_op::poll;
        big.group_key = "k";
        big.job_id    = "j";
        big.poll.ended = true;
        for (int i = 0; i < 10'000; ++i) {
            BatchItemResult r;
            r.custom_id = "i" + std::to_string(i);
            r.status    = batch_item_status::succeeded;
            r.response.message = text_message("answer " + std::to_string(i), role::assistant);
            r.response.usage   = Usage{3, 4};
            big.poll.items.push_back(std::move(r));
        }
        check(json::parse(json::dump(batch_call_record_to_json(big))).has_value() == false,
              "R15 control: the JSON parser's default budget refuses a 10,000-item poll line");
        std::filesystem::path const file = dir / "big.jsonl";
        check(append_batch_call_record(file, big).has_value(), "R15: the 10,000-item poll appends");
        auto read = read_batch_recording(file);
        check(read.has_value() && read->size() == 1 && read->front().poll.items.size() == 10'000 &&
                  all_text_of(read->front().poll.items.back().response.message) == "answer 9999",
              "R15: and reads back whole");
    }

    // ---- R16: a torn final line is skipped, and the next append starts a fresh line -------------------
    {
        BatchCallRecord clk;
        clk.op     = batch_call_op::clock;
        clk.now_ns = 42;
        std::filesystem::path const file = dir / "torn.jsonl";
        check(append_batch_call_record(file, clk).has_value(), "R16: one whole record");
        std::ofstream(file, std::ios::binary | std::ios::app) << "{\"op\":\"poll\",\"group_ke";  // crash mid-append
        auto torn = read_batch_recording(file);
        check(torn.has_value() && torn->size() == 1, "R16: the torn final line is skipped, the whole record kept");
        clk.now_ns = 43;
        check(append_batch_call_record(file, clk).has_value(), "R16: the restored process appends again");
        auto after = read_batch_recording(file);
        check(!after.has_value() && after.error().message.find("line 2") != std::string::npos,
              "R16: the torn fragment is now an interior line, refused by number -- never merged into record 3");
        std::filesystem::path const clean = dir / "clean_tail.jsonl";
        (void)append_batch_call_record(clean, clk);
        std::ofstream(clean, std::ios::binary | std::ios::app) << "{\"op\":\"clock\",\"now_ns\":\"44\"}";  // whole, unterminated
        auto unterminated = read_batch_recording(clean);
        check(unterminated.has_value() && unterminated->size() == 2 && unterminated->back().now_ns == 44,
              "R16 control: an unterminated final line that parses is kept");
    }

    // ---- R17: a non-standard throw replays as a non-standard throw -------------------------------------
    {
        BatchCallRecord header;
        header.op        = batch_call_op::backend;
        header.group_key = "scripted:model";
        BatchCallRecord poll;
        poll.op                 = batch_call_op::poll;
        poll.group_key          = "scripted:model";
        poll.job_id             = "job1";
        poll.threw_non_standard = true;
        auto back = batch_call_record_from_json(batch_call_record_to_json(poll));
        auto replayer = replayer_of({header, back ? *back : poll});
        if (auto rig = replay_rig(replayer, "R17")) {
            EffectContext ctx{};
            bool non_standard = false;
            try {
                (void)rig->backend->poll("job1", ctx);
            } catch (std::exception const&) {
                non_standard = false;
            } catch (...) {
                non_standard = true;
            }
            check(back.has_value() && non_standard,
                  "R17: the supervisor sees the same kind of throw, so its event text is the same");
        }
    }

    std::filesystem::remove_all(dir, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "test_rt_workflow_batch_replay: %d FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_rt_workflow_batch_replay: all passed\n");
    return 0;
}
