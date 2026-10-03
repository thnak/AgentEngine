// decisions/ADR-236-batch-recording-and-replay.md -- the bodies of include/agentengine/core/batch_recording.hpp.
// The format and the replay rules are documented at the declarations there.

#include "agentengine/core/batch_recording.hpp"
#include "agentengine/core/chat_recording.hpp"

#include <charconv>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

namespace agentengine {

namespace {

using recording_detail::opt_bool;
using recording_detail::opt_string;
using recording_detail::opt_u64;

[[nodiscard]] error bad_record(std::string message) {
    return error{failure_class::contract, std::move(message), "batch_recording.malformed"};
}

[[nodiscard]] result<batch_call_op> op_from_tag(std::string_view s) {
    for (batch_call_op op : {batch_call_op::backend, batch_call_op::admit, batch_call_op::submit, batch_call_op::poll,
                             batch_call_op::cancel, batch_call_op::release, batch_call_op::clock}) {
        if (s == batch_call_op_tag(op)) return op;
    }
    return std::unexpected(bad_record("unknown batch call op: " + std::string(s)));
}

[[nodiscard]] result<batch_item_status> status_from_tag(std::string_view s) {
    for (batch_item_status st : {batch_item_status::succeeded, batch_item_status::errored, batch_item_status::expired,
                                 batch_item_status::canceled}) {
        if (s == batch_item_status_tag(st)) return st;
    }
    return std::unexpected(bad_record("unknown batch item status: " + std::string(s)));
}

[[nodiscard]] json::Value num(std::size_t n) { return json::Value::make_number(static_cast<double>(n)); }

[[nodiscard]] char const* reasoning_effort_tag(reasoning_effort e) noexcept {
    switch (e) {
        case reasoning_effort::off:    return "off";
        case reasoning_effort::low:    return "low";
        case reasoning_effort::medium: return "medium";
        case reasoning_effort::high:   return "high";
    }
    return "off";
}

// Breaks the build when ChatRequest gains or loses a field, so batch_request_to_json cannot fall behind it. When
// this fails: encode the new field in batch_request_to_json (if it can change what a vendor is asked), then
// update the binding.
[[maybe_unused]] void chat_request_arity_guard(ChatRequest const& r) {
    auto const& [messages, tools, output_schema_json, idempotency_key, effort, client_interaction_answers] = r;
    (void)messages, (void)tools, (void)output_schema_json, (void)idempotency_key, (void)effort,
        (void)client_interaction_answers;
}

// The text the supervisor's `guarded()` gives a non-standard exception is its own; replaying one as a non-standard
// throw keeps that text identical.
struct NonStandardReplayThrow {};

}  // namespace

char const* batch_call_op_tag(batch_call_op op) noexcept {
    switch (op) {
        case batch_call_op::backend: return "backend";
        case batch_call_op::admit:   return "admit";
        case batch_call_op::submit:  return "submit";
        case batch_call_op::poll:    return "poll";
        case batch_call_op::cancel:  return "cancel";
        case batch_call_op::release: return "release";
        case batch_call_op::clock:   return "clock";
    }
    return "admit";
}

// ---- codec ------------------------------------------------------------------------------------------------

json::Value batch_request_to_json(ChatRequest const& r) {
    json::Value base = chat_request_to_json(r);
    std::vector<std::pair<std::string, json::Value>> o = base.is_object() ? base.as_object()
                                                                           : std::vector<std::pair<std::string, json::Value>>{};
    if (r.reasoning_effort) {
        o.emplace_back("reasoning_effort", json::Value::make_string(reasoning_effort_tag(*r.reasoning_effort)));
    }
    if (!r.client_interaction_answers.empty()) {
        std::vector<json::Value> answers;
        for (ClientInteractionAnswer const& a : r.client_interaction_answers) {
            std::vector<json::Value> routes;
            for (std::string const& route : a.routes) routes.push_back(json::Value::make_string(route));
            std::vector<std::pair<std::string, json::Value>> ao;
            ao.emplace_back("client_interaction_id", json::Value::make_string(a.client_interaction_id));
            ao.emplace_back("response", message_to_json(a.response));
            ao.emplace_back("routes", json::Value::make_array(std::move(routes)));
            answers.push_back(json::Value::make_object(std::move(ao)));
        }
        o.emplace_back("client_interaction_answers", json::Value::make_array(std::move(answers)));
    }
    return json::Value::make_object(std::move(o));
}

json::Value batch_poll_to_json(BatchPoll const& p) {
    std::vector<json::Value> items;
    items.reserve(p.items.size());
    for (BatchItemResult const& r : p.items) {
        std::vector<std::pair<std::string, json::Value>> o;
        o.emplace_back("custom_id", json::Value::make_string(r.custom_id));
        o.emplace_back("status", json::Value::make_string(batch_item_status_tag(r.status)));
        if (r.status == batch_item_status::succeeded) {
            o.emplace_back("response", chat_response_to_json(r.response));
        } else {
            o.emplace_back("detail", json::Value::make_string(r.detail));
            o.emplace_back("klass", json::Value::make_string(std::string(failure_class_to_wire_string(r.klass))));
        }
        items.push_back(json::Value::make_object(std::move(o)));
    }
    std::vector<std::pair<std::string, json::Value>> o;
    o.emplace_back("ended", json::Value::make_bool(p.ended));
    o.emplace_back("detail", json::Value::make_string(p.detail));
    o.emplace_back("items", json::Value::make_array(std::move(items)));
    return json::Value::make_object(std::move(o));
}

result<BatchPoll> batch_poll_from_json(json::Value const& j) {
    if (!j.is_object()) return std::unexpected(bad_record("a batch poll is not an object"));
    BatchPoll p;
    p.ended  = opt_bool(j, "ended");
    p.detail = opt_string(j, "detail");
    auto const* items = j.find("items");
    if (items == nullptr || !items->is_array()) return p;
    for (json::Value const& it : items->as_array()) {
        BatchItemResult r;
        r.custom_id = opt_string(it, "custom_id");
        auto st = status_from_tag(opt_string(it, "status"));
        if (!st) return std::unexpected(st.error());
        r.status = *st;
        if (r.status == batch_item_status::succeeded) {
            auto const* resp = it.find("response");
            if (resp == nullptr) return std::unexpected(bad_record("a succeeded batch item has no response"));
            auto parsed = chat_response_from_json(*resp);
            if (!parsed) return std::unexpected(parsed.error());
            r.response = std::move(*parsed);
        } else {
            r.detail = opt_string(it, "detail");
            auto klass = failure_class_from_wire_string(opt_string(it, "klass", "transient"));
            if (!klass) return std::unexpected(klass.error());
            r.klass = *klass;
        }
        p.items.push_back(std::move(r));
    }
    return p;
}

json::Value batch_call_record_to_json(BatchCallRecord const& rec) {
    std::vector<std::pair<std::string, json::Value>> o;
    o.emplace_back("op", json::Value::make_string(batch_call_op_tag(rec.op)));
    if (rec.op != batch_call_op::clock) o.emplace_back("group_key", json::Value::make_string(rec.group_key));
    switch (rec.op) {
        case batch_call_op::backend: {
            std::vector<std::pair<std::string, json::Value>> l;
            l.emplace_back("max_items", num(rec.limits.max_items));
            l.emplace_back("max_payload_bytes", num(rec.limits.max_payload_bytes));
            l.emplace_back("min_items", num(rec.limits.min_items));
            o.emplace_back("limits", json::Value::make_object(std::move(l)));
            return json::Value::make_object(std::move(o));
        }
        case batch_call_op::clock:
            o.emplace_back("now_ns", json::Value::make_string(std::to_string(rec.now_ns)));
            return json::Value::make_object(std::move(o));
        case batch_call_op::admit: o.emplace_back("request", rec.request); break;
        case batch_call_op::submit: {
            std::vector<json::Value> items;
            items.reserve(rec.items.size());
            for (auto const& [id, req] : rec.items) {
                std::vector<std::pair<std::string, json::Value>> it;
                it.emplace_back("custom_id", json::Value::make_string(id));
                it.emplace_back("request", req);
                items.push_back(json::Value::make_object(std::move(it)));
            }
            o.emplace_back("items", json::Value::make_array(std::move(items)));
            break;
        }
        case batch_call_op::poll:
        case batch_call_op::cancel:
        case batch_call_op::release: o.emplace_back("job_id", json::Value::make_string(rec.job_id)); break;
    }
    if (rec.op != batch_call_op::admit) {
        o.emplace_back("principal", json::Value::make_string(rec.principal_id));
        o.emplace_back("tenant", json::Value::make_string(rec.tenant_id));
    }
    if (rec.threw_non_standard) {
        o.emplace_back("threw_non_standard", json::Value::make_bool(true));
    } else if (rec.threw) {
        o.emplace_back("threw", json::Value::make_string(*rec.threw));
    } else if (rec.failure) {
        o.emplace_back("error", error_to_json(*rec.failure));
    } else if (rec.op == batch_call_op::admit) {
        o.emplace_back("bytes", num(rec.admitted_bytes));
    } else if (rec.op == batch_call_op::submit) {
        o.emplace_back("job", json::Value::make_string(rec.submitted_job));
    } else if (rec.op == batch_call_op::poll) {
        o.emplace_back("poll", batch_poll_to_json(rec.poll));
    }
    return json::Value::make_object(std::move(o));
}

result<BatchCallRecord> batch_call_record_from_json(json::Value const& j) {
    if (!j.is_object()) return std::unexpected(bad_record("a batch call record is not an object"));
    auto op = op_from_tag(opt_string(j, "op"));
    if (!op) return std::unexpected(op.error());
    BatchCallRecord rec;
    rec.op        = *op;
    rec.group_key = opt_string(j, "group_key");
    switch (rec.op) {
        case batch_call_op::backend: {
            auto const* l = j.find("limits");
            if (l == nullptr || !l->is_object()) return std::unexpected(bad_record("a backend header has no limits"));
            rec.limits.max_items         = static_cast<std::size_t>(opt_u64(*l, "max_items"));
            rec.limits.max_payload_bytes = static_cast<std::size_t>(opt_u64(*l, "max_payload_bytes"));
            rec.limits.min_items         = static_cast<std::size_t>(opt_u64(*l, "min_items", 1));
            return rec;
        }
        case batch_call_op::clock: {
            std::string const s = opt_string(j, "now_ns");
            auto const [end, ec] = std::from_chars(s.data(), s.data() + s.size(), rec.now_ns);
            if (s.empty() || ec != std::errc{} || end != s.data() + s.size()) {
                return std::unexpected(bad_record("a clock record's now_ns is not a decimal integer"));
            }
            return rec;
        }
        case batch_call_op::admit: {
            auto const* req = j.find("request");
            if (req == nullptr) return std::unexpected(bad_record("an admit record has no request"));
            rec.request = *req;
            break;
        }
        case batch_call_op::submit: {
            auto const* items = j.find("items");
            if (items == nullptr || !items->is_array()) return std::unexpected(bad_record("a submit record has no items"));
            for (json::Value const& it : items->as_array()) {
                auto const* req = it.find("request");
                if (req == nullptr) return std::unexpected(bad_record("a submitted item has no request"));
                rec.items.emplace_back(opt_string(it, "custom_id"), *req);
            }
            break;
        }
        case batch_call_op::poll:
        case batch_call_op::cancel:
        case batch_call_op::release: rec.job_id = opt_string(j, "job_id"); break;
    }
    rec.principal_id = opt_string(j, "principal");
    rec.tenant_id    = opt_string(j, "tenant");
    if (opt_bool(j, "threw_non_standard")) {
        rec.threw_non_standard = true;
    } else if (auto const* t = j.find("threw"); t != nullptr && t->is_string()) {
        rec.threw = t->as_string();
    } else if (auto const* e = j.find("error"); e != nullptr && e->is_object()) {
        auto err = error_from_json(*e);
        if (!err) return std::unexpected(err.error());
        rec.failure = std::move(*err);
    } else if (rec.op == batch_call_op::admit) {
        rec.admitted_bytes = static_cast<std::size_t>(opt_u64(j, "bytes"));
    } else if (rec.op == batch_call_op::submit) {
        rec.submitted_job = opt_string(j, "job");
        if (rec.submitted_job.empty()) return std::unexpected(bad_record("a successful submit record has no job id"));
    } else if (rec.op == batch_call_op::poll) {
        auto const* p = j.find("poll");
        if (p == nullptr) return std::unexpected(bad_record("a successful poll record has no poll"));
        auto poll = batch_poll_from_json(*p);
        if (!poll) return std::unexpected(poll.error());
        rec.poll = std::move(*poll);
    }
    return rec;
}

result<void> append_batch_call_record(std::filesystem::path const& path, BatchCallRecord const& rec) {
    std::string const line = json::dump(batch_call_record_to_json(rec));
    // A file ending mid-line was torn by a crash during an append: start this record on a line of its own, so
    // the torn line stays a lone, skippable fragment instead of corrupting this record too.
    bool needs_newline = false;
    {
        std::error_code ec;
        auto const size = std::filesystem::file_size(path, ec);
        if (!ec && size > 0) {
            std::ifstream tail(path, std::ios::binary);
            tail.seekg(-1, std::ios::end);
            char last = '\n';
            needs_newline = tail.get(last) && last != '\n';
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
        return std::unexpected(error{failure_class::resource, "cannot open batch recording for append: " + path.string(),
                                     "batch_recording.open_failed"});
    }
    if (needs_newline) out << '\n';
    out << line << '\n';
    out.flush();
    if (!out) {
        return std::unexpected(error{failure_class::resource, "cannot write batch recording: " + path.string(),
                                     "batch_recording.write_failed"});
    }
    return {};
}

result<std::vector<BatchCallRecord>> read_batch_recording(std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(error{failure_class::resource, "cannot open batch recording: " + path.string(),
                                     "batch_recording.open_failed"});
    }
    std::vector<BatchCallRecord> out;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        bool const terminated = !in.eof();  // getline stopped at a '\n', not at the end of the file
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        auto parsed = json::parse(line, kBatchRecordingParseBudget);
        if (!parsed) {
            if (!terminated) break;  // a torn final write: the record it held was never completed
            return std::unexpected(bad_record("line " + std::to_string(line_no) + ": " + parsed.error().message));
        }
        auto rec = batch_call_record_from_json(*parsed);
        if (!rec) return std::unexpected(bad_record("line " + std::to_string(line_no) + ": " + rec.error().message));
        out.push_back(std::move(*rec));
    }
    return out;
}

// ---- recorder ---------------------------------------------------------------------------------------------

struct BatchRecorder::State {
    std::mutex mu;
    Sink       sink;
    bool       failed = false;

    // Never throws: a recording failure is noted, never turned into a failure of the call being recorded.
    void emit(BatchCallRecord const& rec) noexcept {
        std::lock_guard<std::mutex> lock(mu);
        try {
            if (sink) sink(rec);
        } catch (...) {
            failed = true;
        }
    }
    void note_failure() noexcept {
        std::lock_guard<std::mutex> lock(mu);
        failed = true;
    }
};

namespace {

class RecordingBatchBackend final : public BatchBackend {
public:
    RecordingBatchBackend(std::shared_ptr<BatchRecorder::State> state, std::shared_ptr<BatchBackend> inner)
        : state_(std::move(state)), inner_(std::move(inner)) {}

    [[nodiscard]] std::string group_key() const override { return inner_->group_key(); }
    [[nodiscard]] BatchLimits limits() const override { return inner_->limits(); }

    [[nodiscard]] result<std::size_t> admit(ChatRequest const& request) const override {
        return record(
            [&](BatchCallRecord& rec) {
                rec.op      = batch_call_op::admit;
                rec.request = batch_request_to_json(request);
            },
            [&] { return inner_->admit(request); },
            [](BatchCallRecord& r, std::size_t const& bytes) { r.admitted_bytes = bytes; });
    }

    [[nodiscard]] result<std::string> submit(std::vector<BatchItemRequest> const& items, EffectContext& ctx) override {
        return record(
            [&](BatchCallRecord& rec) {
                rec.op = batch_call_op::submit;
                for (BatchItemRequest const& it : items) rec.items.emplace_back(it.custom_id, batch_request_to_json(it.request));
                attribute(rec, ctx);
            },
            [&] { return inner_->submit(items, ctx); },
            [](BatchCallRecord& r, std::string const& job) { r.submitted_job = job; });
    }

    [[nodiscard]] result<BatchPoll> poll(std::string const& job_id, EffectContext& ctx) override {
        return record(
            [&](BatchCallRecord& rec) {
                rec.op     = batch_call_op::poll;
                rec.job_id = job_id;
                attribute(rec, ctx);
            },
            [&] { return inner_->poll(job_id, ctx); },
            [](BatchCallRecord& r, BatchPoll const& p) { r.poll = p; });
    }

    [[nodiscard]] result<void> cancel(std::string const& job_id, EffectContext& ctx) override {
        return record_void(batch_call_op::cancel, job_id, ctx, [&] { return inner_->cancel(job_id, ctx); });
    }

    [[nodiscard]] result<void> release(std::string const& job_id, EffectContext& ctx) override {
        return record_void(batch_call_op::release, job_id, ctx, [&] { return inner_->release(job_id, ctx); });
    }

private:
    static void attribute(BatchCallRecord& rec, EffectContext const& ctx) {
        rec.principal_id = ctx.principal.id;
        rec.tenant_id    = ctx.principal.tenant_id;
    }

    // Builds the record's inputs, makes the call, then records the outcome. Only the inner call's own exceptions
    // propagate; anything the recording itself throws (building the record, copying a result, the sink) marks
    // the recording failed and leaves the call's outcome untouched.
    template <class Prepare, class Call, class OnValue>
    auto record(Prepare&& prepare, Call&& call, OnValue&& on_value) const -> decltype(call()) {
        std::optional<BatchCallRecord> rec;
        try {
            rec.emplace();
            rec->group_key = inner_->group_key();
            prepare(*rec);
        } catch (...) {
            rec.reset();
            state_->note_failure();
        }
        std::optional<decltype(call())> out;
        std::exception_ptr              thrown;
        try {
            out.emplace(call());
        } catch (...) {
            thrown = std::current_exception();
        }
        // The thrown call is recorded and rethrown OUTSIDE the handler above. A rethrow from a handler that holds
        // its own nested try crashes clang's Windows EH under AddressSanitizer (google/sanitizers#749's family;
        // CI's clang-cl ASanUBSan leg, 2026-10-03), and nothing here needs to run inside the handler.
        if (thrown) {
            if (rec) {
                // The text is copied inside the handler: MSVC's rethrow_exception throws a copy of the stored
                // exception, so a what() pointer would dangle once the handler exits. A bad_alloc from that copy
                // leaves the handler for the OUTER try -- no try nested inside a handler.
                try {
                    try {
                        std::rethrow_exception(thrown);
                    } catch (std::exception const& e) {
                        rec->threw = std::string(e.what());
                    } catch (...) {
                        rec->threw_non_standard = true;
                    }
                    state_->emit(*rec);
                } catch (...) {
                    state_->note_failure();
                }
            }
            std::rethrow_exception(thrown);
        }
        if (rec) {
            try {
                if (*out) {
                    if constexpr (!std::is_void_v<typename std::remove_cvref_t<decltype(*out)>::value_type>) {
                        on_value(*rec, **out);
                    }
                } else {
                    rec->failure = out->error();
                }
                state_->emit(*rec);
            } catch (...) {
                state_->note_failure();
            }
        }
        return std::move(*out);
    }

    template <class Call>
    result<void> record_void(batch_call_op op, std::string const& job_id, EffectContext& ctx, Call&& call) const {
        return record(
            [&](BatchCallRecord& rec) {
                rec.op     = op;
                rec.job_id = job_id;
                attribute(rec, ctx);
            },
            std::forward<Call>(call), [](BatchCallRecord&, auto const&) {});
    }

    std::shared_ptr<BatchRecorder::State> state_;
    std::shared_ptr<BatchBackend>         inner_;
};

}  // namespace

BatchRecorder::BatchRecorder(Sink sink) : state_(std::make_shared<State>()) { state_->sink = std::move(sink); }

std::shared_ptr<BatchBackend> BatchRecorder::wrap(std::shared_ptr<BatchBackend> inner) const {
    if (!inner) return nullptr;
    BatchCallRecord header;
    header.op        = batch_call_op::backend;
    header.group_key = inner->group_key();
    header.limits    = inner->limits();
    state_->emit(header);
    return std::make_shared<RecordingBatchBackend>(state_, std::move(inner));
}

std::function<std::int64_t()> BatchRecorder::clock(std::function<std::int64_t()> inner) const {
    return [state = state_, inner = std::move(inner)] {
        std::int64_t const now = inner();
        BatchCallRecord rec;
        rec.op     = batch_call_op::clock;
        rec.now_ns = now;
        state->emit(rec);
        return now;
    };
}

bool BatchRecorder::failed() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->failed;
}

// ---- replayer ---------------------------------------------------------------------------------------------

struct BatchReplayer::State {
    mutable std::mutex                 mu;
    std::vector<BatchCallRecord>       calls;    // headers removed, in recorded order
    std::map<std::string, BatchLimits> headers;  // group key -> limits
    std::size_t                        cursor   = 0;
    bool                               diverged = false;
    std::string                        why;
    std::int64_t                       last_now = 0;
    DivergenceHook                     hook;

    // Under `mu`. Latches the divergence and hands back the hook to run once the lock is released.
    [[nodiscard]] DivergenceHook latch(std::string reason) {
        diverged = true;
        why      = std::move(reason);
        return std::exchange(hook, nullptr);
    }
};

namespace {

[[nodiscard]] std::string where(std::size_t index) { return "at recorded call " + std::to_string(index); }

[[nodiscard]] bool same_json(json::Value const& a, json::Value const& b) { return json::dump(a) == json::dump(b); }

void run_hook(BatchReplayer::DivergenceHook const& hook, std::string const& why) {
    if (!hook) return;
    try {
        hook(why);
    } catch (...) {
        // The hook is host code reacting to a divergence that is already latched; its failure changes nothing.
    }
}

class ReplayBatchBackend final : public BatchBackend {
public:
    ReplayBatchBackend(std::shared_ptr<BatchReplayer::State> state, std::string key, BatchLimits limits)
        : state_(std::move(state)), key_(std::move(key)), limits_(limits) {}

    [[nodiscard]] std::string group_key() const override { return key_; }
    [[nodiscard]] BatchLimits limits() const override { return limits_; }

    [[nodiscard]] result<std::size_t> admit(ChatRequest const& request) const override {
        json::Value const req = batch_request_to_json(request);
        auto rec = next(batch_call_op::admit, nullptr, [&](BatchCallRecord const& r) -> std::string {
            return same_json(r.request, req) ? std::string{} : "the admitted request differs from the recorded one";
        });
        if (!rec) return std::unexpected(rec.error());
        return answer<std::size_t>(**rec, [](BatchCallRecord const& r) { return r.admitted_bytes; });
    }

    [[nodiscard]] result<std::string> submit(std::vector<BatchItemRequest> const& items, EffectContext& ctx) override {
        auto rec = next(batch_call_op::submit, &ctx, [&](BatchCallRecord const& r) -> std::string {
            if (r.items.size() != items.size()) {
                return "submitted " + std::to_string(items.size()) + " items, recorded " +
                       std::to_string(r.items.size());
            }
            for (std::size_t k = 0; k < items.size(); ++k) {
                if (r.items[k].first != items[k].custom_id) {
                    return "item " + std::to_string(k) + " has custom id '" + items[k].custom_id + "', recorded '" +
                           r.items[k].first + "'";
                }
                if (!same_json(r.items[k].second, batch_request_to_json(items[k].request))) {
                    return "item " + std::to_string(k) + " ('" + items[k].custom_id +
                           "') differs from the recorded request";
                }
            }
            return {};
        });
        if (!rec) return std::unexpected(rec.error());
        return answer<std::string>(**rec, [](BatchCallRecord const& r) { return r.submitted_job; });
    }

    [[nodiscard]] result<BatchPoll> poll(std::string const& job_id, EffectContext& ctx) override {
        auto rec = next(batch_call_op::poll, &ctx, job_check(job_id));
        if (!rec) return std::unexpected(rec.error());
        return answer<BatchPoll>(**rec, [](BatchCallRecord const& r) { return r.poll; });
    }

    [[nodiscard]] result<void> cancel(std::string const& job_id, EffectContext& ctx) override {
        auto rec = next(batch_call_op::cancel, &ctx, job_check(job_id));
        if (!rec) return std::unexpected(rec.error());
        return answer<std::monostate>(**rec, [](BatchCallRecord const&) { return std::monostate{}; })
            .transform([](std::monostate) {});
    }

    [[nodiscard]] result<void> release(std::string const& job_id, EffectContext& ctx) override {
        auto rec = next(batch_call_op::release, &ctx, job_check(job_id));
        if (!rec) return std::unexpected(rec.error());
        return answer<std::monostate>(**rec, [](BatchCallRecord const&) { return std::monostate{}; })
            .transform([](std::monostate) {});
    }

private:
    [[nodiscard]] static std::function<std::string(BatchCallRecord const&)> job_check(std::string const& job_id) {
        return [&job_id](BatchCallRecord const& r) -> std::string {
            return r.job_id == job_id ? std::string{} : "job id '" + job_id + "', recorded '" + r.job_id + "'";
        };
    }

    // Matches the next recorded call against this one; on any mismatch latches the replayer and runs the host's
    // hook after releasing the lock. `ctx` is null for admit, which has no context. The returned pointer stays
    // valid: `calls` is never modified after construction.
    template <class Check>
    [[nodiscard]] result<BatchCallRecord const*> next(batch_call_op op, EffectContext const* ctx, Check&& check) const {
        DivergenceHookCall notify;
        result<BatchCallRecord const*> out = match(op, ctx, std::forward<Check>(check), notify);
        run_hook(notify.hook, notify.why);
        return out;
    }

    struct DivergenceHookCall {
        BatchReplayer::DivergenceHook hook;
        std::string                   why;
    };

    template <class Check>
    [[nodiscard]] result<BatchCallRecord const*> match(batch_call_op op, EffectContext const* ctx, Check&& check,
                                                       DivergenceHookCall& notify) const {
        std::lock_guard<std::mutex> lock(state_->mu);
        auto diverge = [&](std::string reason, char const* code = "batch_replay.diverged") {
            notify.why  = reason;
            notify.hook = state_->latch(std::move(reason));
            return std::unexpected(error{failure_class::contract, "batch replay diverged: " + state_->why, code});
        };
        if (state_->diverged) {
            return std::unexpected(error{failure_class::contract, "batch replay diverged earlier: " + state_->why,
                                         "batch_replay.diverged"});
        }
        std::size_t const at = state_->cursor;
        if (at >= state_->calls.size()) {
            return diverge(std::string(batch_call_op_tag(op)) + " on '" + key_ + "' after the recording ended",
                           "batch_replay.exhausted");
        }
        BatchCallRecord const& r = state_->calls[at];
        if (r.op != op) {
            return diverge(where(at) + ": called " + batch_call_op_tag(op) + ", recorded " + batch_call_op_tag(r.op));
        }
        if (r.group_key != key_) {
            return diverge(where(at) + ": " + batch_call_op_tag(op) + " on '" + key_ + "', recorded on '" +
                           r.group_key + "'");
        }
        if (ctx != nullptr && (ctx->principal.id != r.principal_id || ctx->principal.tenant_id != r.tenant_id)) {
            return diverge(where(at) + ": " + batch_call_op_tag(op) + " under principal '" + ctx->principal.id +
                           "' tenant '" + ctx->principal.tenant_id + "', recorded under '" + r.principal_id +
                           "' tenant '" + r.tenant_id + "'");
        }
        if (std::string mismatch = check(r); !mismatch.empty()) {
            return diverge(where(at) + ": " + batch_call_op_tag(op) + ": " + mismatch);
        }
        ++state_->cursor;
        return &r;
    }

    template <class T, class Get>
    [[nodiscard]] static result<T> answer(BatchCallRecord const& r, Get&& get) {
        if (r.threw_non_standard) throw NonStandardReplayThrow{};
        if (r.threw) throw std::runtime_error(*r.threw);
        if (r.failure) return std::unexpected(*r.failure);
        return get(r);
    }

    std::shared_ptr<BatchReplayer::State> state_;
    std::string                           key_;
    BatchLimits                           limits_;
};

}  // namespace

result<BatchReplayer> BatchReplayer::from_records(std::vector<BatchCallRecord> records) {
    auto state = std::make_shared<State>();
    for (BatchCallRecord& r : records) {
        if (r.op != batch_call_op::clock && r.group_key.empty()) {
            return std::unexpected(bad_record(std::string("a ") + batch_call_op_tag(r.op) + " record has no group key"));
        }
        if (r.op != batch_call_op::backend) {
            state->calls.push_back(std::move(r));
            continue;
        }
        auto const [it, inserted] = state->headers.emplace(r.group_key, r.limits);
        BatchLimits const& had = it->second;
        if (!inserted && (had.max_items != r.limits.max_items || had.max_payload_bytes != r.limits.max_payload_bytes ||
                          had.min_items != r.limits.min_items)) {
            return std::unexpected(bad_record("the recording gives backend '" + r.group_key + "' two different limits"));
        }
    }
    return BatchReplayer(std::move(state));
}

result<std::shared_ptr<BatchBackend>> BatchReplayer::backend(std::string const& group_key) const {
    auto const it = state_->headers.find(group_key);
    if (it == state_->headers.end()) {
        return std::unexpected(error{failure_class::contract, "the recording has no backend '" + group_key + "'",
                                     "batch_replay.unknown_backend"});
    }
    return std::shared_ptr<BatchBackend>(std::make_shared<ReplayBatchBackend>(state_, group_key, it->second));
}

std::function<std::int64_t()> BatchReplayer::clock() const {
    return [state = state_] {
        DivergenceHook hook;
        std::string    why;
        std::int64_t   now = 0;
        {
            std::lock_guard<std::mutex> lock(state->mu);
            std::size_t const at = state->cursor;
            if (!state->diverged) {
                if (at >= state->calls.size()) {
                    why  = "clock read after the recording ended";
                    hook = state->latch(why);
                } else if (state->calls[at].op != batch_call_op::clock) {
                    why  = where(at) + ": read the clock, recorded " + batch_call_op_tag(state->calls[at].op);
                    hook = state->latch(why);
                } else {
                    state->last_now = state->calls[at].now_ns;
                    ++state->cursor;
                }
            }
            now = state->last_now;
        }
        run_hook(hook, why);
        return now;
    };
}

void BatchReplayer::on_divergence(DivergenceHook hook) const {
    std::string why;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->diverged) {
            state_->hook = std::move(hook);
            return;
        }
        why = state_->why;
    }
    run_hook(hook, why);  // already diverged: tell the late subscriber at once
}

bool BatchReplayer::diverged() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->diverged;
}

std::string BatchReplayer::divergence() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->why;
}

std::size_t BatchReplayer::remaining() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->calls.size() - state_->cursor;
}

}  // namespace agentengine
