// Content for the Events detail page (api/events.html). Same rule as apiContent.ts: every claim
// here is grounded in a real header, source file, RFC section, ADR, or test in THIS repo, with a
// citation -- don't invent a shape or a claim these sources don't make, and never blur "real and
// tested" with "a Reviewed RFC describes this but nothing implements it yet."
//
// Bilingual (EN/VI): every translatable field below is a Record<Lang, T>. Code snippets,
// identifiers, RFC/ADR numbers, file paths, and proper nouns stay identical in both languages --
// only surrounding prose is translated.

import type { Lang } from "../i18n/LanguageContext";

export interface EventsSection {
  id: string;
  label: string;
}

// Ids match the `id` attributes ApiEventsReference renders -- the right-rail scroll-spy TOC reads
// this list.
export const eventsSections: Record<Lang, EventsSection[]> = {
  en: [
    { id: "events", label: "Overview" },
    { id: "events-run", label: "RunEvent & AgentSession" },
    { id: "events-workflow", label: "WorkflowEvent & WorkflowEventStream" },
    { id: "events-live-view", label: "enable_live_view(): the coarser sibling" },
    { id: "events-wire", label: "Wire projections: AG-UI & A2A" },
    { id: "events-status", label: "What's still evolving" },
  ],
  vi: [
    { id: "events", label: "Tổng quan" },
    { id: "events-run", label: "RunEvent & AgentSession" },
    { id: "events-workflow", label: "WorkflowEvent & WorkflowEventStream" },
    { id: "events-live-view", label: "enable_live_view(): người anh em thô hơn" },
    { id: "events-wire", label: "Phép chiếu sang giao thức: AG-UI & A2A" },
    { id: "events-status", label: "Những gì vẫn đang thay đổi" },
  ],
};

// ------------------------------------------------------------------------------------------------
// run_event_kind: 26 values, grouped into rows -- what each is for, and whether AgentSession's own
// turn loop actually fires it today (core/run_event.hpp's own top-of-file comment is the source for
// the "no real producer yet" claims; examples/29 and the ADR-029/057/OQ-21 payload comments are the
// source for the "real" ones).
// ------------------------------------------------------------------------------------------------

export interface RunEventKindRow {
  kind: string;
  payload: string;
  today: string;
}

export const runEventKindRows: Record<Lang, RunEventKindRow[]> = {
  en: [
    {
      kind: "run_started · run_finished · run_failed",
      payload: "Empty · Empty · RunFailed{error_code, message}",
      today: "Real. Bracket every run through AgentSession's turn loop.",
    },
    {
      kind: "run_canceled",
      payload: "Empty",
      today: "Payload defined; run_event.hpp's own top comment names this as one of five kinds with no real AgentSession producer yet.",
    },
    {
      kind: "turn_started · turn_finished",
      payload: "Turn{turn_index}",
      today: "Real. Bracket every turn boundary (001 §2's turn_index).",
    },
    {
      kind: "model_call_started · model_call_finished",
      payload: "Empty",
      today: "Real. Bracket each model call, streamed or not.",
    },
    {
      kind: "model_delta (ModelTextDelta)",
      payload: "ModelDelta{ModelTextDelta{text}}",
      today: "Real for the model's own text once session.set_stream_model_calls(true) is engaged (ADR-034) -- proven byte-for-byte in examples/29_agent_session_events.cpp.",
    },
    {
      kind: "model_delta (ModelToolCallArgumentDelta)",
      payload: "ModelDelta{ModelToolCallArgumentDelta{call_id, tool_name, arguments_fragment, is_final}}",
      today: "Defined for a model streaming a tool call's own arguments incrementally; no real producer located in this review.",
    },
    {
      kind: "warning",
      payload: "Warning{message}",
      today: "Real. Fires once per streamed run, right after run_started, since engaging stream_model_calls_ is itself an operator-visible choice (examples/29's own header comment).",
    },
    {
      kind: "tool_call_started · tool_call_delta · tool_call_finished · sandbox_exec_started · sandbox_exec_finished · artifact_produced",
      payload: "ToolCallStarted · ToolCallDelta{content} · ToolCallFinished{result} · SandboxExec{exec_id} · ArtifactProduced{artifact_id}",
      today: "Defined with real payload shapes. run_event.hpp's own top comment: AgentSession's turn loop \"never reaches the tool pipeline at all\" today -- none of these six kinds has a confirmed real emitter in this review.",
    },
    {
      kind: "state_changed",
      payload: "StateChanged{description}",
      today: "Payload defined and named as the carrier for a run's StandingEffect visibility (006 §6b) -- \"not a new event pair,\" per this file's own comment. No confirmed real caller of emit_event(..., state_changed, ...) located in this review.",
    },
    {
      kind: "input_required · input_resolved · auth_required · auth_resolved · approval_requested · approval_resolved",
      payload: "InteractionRef{interaction_id} · ApprovalRequested{call_id, interaction_id} · ApprovalResolved{call_id, approved, interaction_id}",
      today: "Real. AgentSession::handle() mints a real Interaction and fires the *_required/*_requested half when a round genuinely suspends for human approval or auth (ADR-029).",
    },
    {
      kind: "codeact_ask_requested",
      payload: "CodeActAskRequested{call_id, interaction_id, prompt}",
      today: "Real. Fired immediately before the paired input_required, carrying agent.ask()'s own prompt text so a live consumer doesn't need a second round trip (ADR-057 §9).",
    },
    {
      kind: "hook_decision_requested",
      payload: "HookDecisionRequested{call_id, interaction_id, tool_name}",
      today: "Real. Fired alongside input_required when the tool-call hook stage (core/tool_call_hook.hpp) needs external dispatch (OQ-21).",
    },
    {
      kind: "policy_decision",
      payload: "PolicyDecision{description}",
      today: "Payload defined; no confirmed real producer located in this review.",
    },
  ],
  vi: [
    {
      kind: "run_started · run_finished · run_failed",
      payload: "Empty · Empty · RunFailed{error_code, message}",
      today: "Thật. Đóng khung mọi lần chạy qua vòng lặp lượt của AgentSession.",
    },
    {
      kind: "run_canceled",
      payload: "Empty",
      today: "Payload đã định nghĩa; chú thích đầu file của run_event.hpp nêu tên đây là một trong năm loại chưa có bên phát sinh thật nào từ AgentSession.",
    },
    {
      kind: "turn_started · turn_finished",
      payload: "Turn{turn_index}",
      today: "Thật. Đóng khung mọi ranh giới lượt (turn_index của 001 §2).",
    },
    {
      kind: "model_call_started · model_call_finished",
      payload: "Empty",
      today: "Thật. Đóng khung mỗi lệnh gọi model, dù có streaming hay không.",
    },
    {
      kind: "model_delta (ModelTextDelta)",
      payload: "ModelDelta{ModelTextDelta{text}}",
      today: "Thật đối với văn bản của model một khi session.set_stream_model_calls(true) được bật (ADR-034) -- được chứng minh khớp từng byte trong examples/29_agent_session_events.cpp.",
    },
    {
      kind: "model_delta (ModelToolCallArgumentDelta)",
      payload: "ModelDelta{ModelToolCallArgumentDelta{call_id, tool_name, arguments_fragment, is_final}}",
      today: "Được định nghĩa cho trường hợp model stream dần các tham số của một lệnh gọi tool; không tìm thấy bên phát sinh thật nào trong lần rà soát này.",
    },
    {
      kind: "warning",
      payload: "Warning{message}",
      today: "Thật. Phát đúng một lần cho mỗi lần chạy có streaming, ngay sau run_started, vì việc bật stream_model_calls_ tự nó là một lựa chọn người vận hành nên thấy được (theo chú thích đầu file của examples/29).",
    },
    {
      kind: "tool_call_started · tool_call_delta · tool_call_finished · sandbox_exec_started · sandbox_exec_finished · artifact_produced",
      payload: "ToolCallStarted · ToolCallDelta{content} · ToolCallFinished{result} · SandboxExec{exec_id} · ArtifactProduced{artifact_id}",
      today: "Đã định nghĩa với payload thật. Chú thích đầu file của run_event.hpp: vòng lặp lượt của AgentSession \"không bao giờ chạm tới tool pipeline\" hôm nay -- không có loại nào trong sáu loại này có bên phát sinh thật được xác nhận trong lần rà soát này.",
    },
    {
      kind: "state_changed",
      payload: "StateChanged{description}",
      today: "Payload đã định nghĩa, được nêu tên là nơi mang tính hiển thị của StandingEffect trong một lần chạy (006 §6b) -- \"không phải một cặp sự kiện mới\", theo chính chú thích của file này. Không tìm thấy bên gọi emit_event(..., state_changed, ...) thật nào trong lần rà soát này.",
    },
    {
      kind: "input_required · input_resolved · auth_required · auth_resolved · approval_requested · approval_resolved",
      payload: "InteractionRef{interaction_id} · ApprovalRequested{call_id, interaction_id} · ApprovalResolved{call_id, approved, interaction_id}",
      today: "Thật. AgentSession::handle() tạo ra một Interaction thật và phát nửa *_required/*_requested khi một lượt thực sự treo lại để chờ phê duyệt hoặc xác thực của con người (ADR-029).",
    },
    {
      kind: "codeact_ask_requested",
      payload: "CodeActAskRequested{call_id, interaction_id, prompt}",
      today: "Thật. Được phát ngay trước input_required đi kèm, mang theo chính văn bản câu hỏi của agent.ask() để bên tiêu thụ trực tiếp không cần một vòng round-trip thứ hai (ADR-057 §9).",
    },
    {
      kind: "hook_decision_requested",
      payload: "HookDecisionRequested{call_id, interaction_id, tool_name}",
      today: "Thật. Được phát cùng lúc với input_required khi giai đoạn hook của lệnh gọi tool (core/tool_call_hook.hpp) cần điều phối ra bên ngoài (OQ-21).",
    },
    {
      kind: "policy_decision",
      payload: "PolicyDecision{description}",
      today: "Payload đã định nghĩa; không tìm thấy bên phát sinh thật nào trong lần rà soát này.",
    },
  ],
};

// ------------------------------------------------------------------------------------------------
// workflow_event_kind: 19 values. Real/proven rows cite the actual W-numbered test function
// (tests/test_rt_workflow_event_stream.cpp) or a real example that observes the kind; anything this
// review could not directly trace says so rather than guessing.
// ------------------------------------------------------------------------------------------------

export interface WorkflowEventKindRow {
  kind: string;
  today: string;
}

export const workflowEventKindRows: Record<Lang, WorkflowEventKindRow[]> = {
  en: [
    {
      kind: "workflow_run_started · workflow_run_completed · workflow_run_failed",
      today: "Real -- W1 (tests/test_rt_workflow_event_stream.cpp) and examples/09, /25.",
    },
    {
      kind: "workflow_run_suspended · workflow_run_resumed",
      today: "Defined (workflow_event.hpp); not traced to a specific proof in this review.",
    },
    {
      kind: "superstep_started · superstep_completed",
      today: "Real -- W1: fires at every superstep boundary, the same boundary the checkpoint hook and enable_live_view() fire from.",
    },
    {
      kind: "executor_dispatched · executor_completed",
      today: "Real -- W1.",
    },
    {
      kind: "message_routed",
      today: "Real -- W1, which checks the payload carries the actual a→b edge_kind::direct edge, not just that the kind fired.",
    },
    {
      kind: "fan_out_dispatched · fan_in_aggregated",
      today: "Real -- examples/25_workflow_event_stream_live.cpp: exactly one of each per fan-out/fan-in round, carrying all real targets/sources, never one event per edge.",
    },
    {
      kind: "route_selected",
      today: "Real -- proven by w6_route_selected_events in tests/test_rt_workflow_event_stream.cpp.",
    },
    {
      kind: "request_port_opened · request_port_resolved",
      today: "Real -- proven by w3_request_port_events in tests/test_rt_workflow_event_stream.cpp.",
    },
    {
      kind: "checkpoint_saved",
      today: "Real -- proven by w4_checkpoint_saved in tests/test_rt_workflow_event_stream.cpp.",
    },
    {
      kind: "merge_completed · merge_conflict",
      today: "Defined (workflow_event.hpp), and ADR-055's set_merge_on_join_hook() is real -- but this review found no test naming these two kinds specifically on the event stream. Not claimed either way.",
    },
    {
      kind: "agent_turn_event",
      today: "Real, per ADR-152's own text: the agent-kind bridge (agent_turn_sink) needed no restructuring of the existing agent-as-executor drive loop to wire.",
    },
    {
      kind: "moderator_stream_delta",
      today: "Real -- examples/25: 9 events observed live (3 participants × 3 steps each), and examples/26 against a real OpenRouter model: 39 genuine per-token deltas.",
    },
  ],
  vi: [
    {
      kind: "workflow_run_started · workflow_run_completed · workflow_run_failed",
      today: "Thật -- W1 (tests/test_rt_workflow_event_stream.cpp) và examples/09, /25.",
    },
    {
      kind: "workflow_run_suspended · workflow_run_resumed",
      today: "Đã định nghĩa (workflow_event.hpp); không tìm thấy bằng chứng cụ thể trong lần rà soát này.",
    },
    {
      kind: "superstep_started · superstep_completed",
      today: "Thật -- W1: phát tại mọi ranh giới superstep, đúng ranh giới mà checkpoint hook và enable_live_view() cũng phát từ đó.",
    },
    {
      kind: "executor_dispatched · executor_completed",
      today: "Thật -- W1.",
    },
    {
      kind: "message_routed",
      today: "Thật -- W1, kiểm tra payload mang đúng cạnh a→b edge_kind::direct thật, không chỉ việc loại sự kiện có phát hay không.",
    },
    {
      kind: "fan_out_dispatched · fan_in_aggregated",
      today: "Thật -- examples/25_workflow_event_stream_live.cpp: đúng một sự kiện mỗi loại cho mỗi vòng fan-out/fan-in, mang toàn bộ đích/nguồn thật, không bao giờ một sự kiện cho mỗi cạnh.",
    },
    {
      kind: "route_selected",
      today: "Thật -- được chứng minh bởi w6_route_selected_events trong tests/test_rt_workflow_event_stream.cpp.",
    },
    {
      kind: "request_port_opened · request_port_resolved",
      today: "Thật -- được chứng minh bởi w3_request_port_events trong tests/test_rt_workflow_event_stream.cpp.",
    },
    {
      kind: "checkpoint_saved",
      today: "Thật -- được chứng minh bởi w4_checkpoint_saved trong tests/test_rt_workflow_event_stream.cpp.",
    },
    {
      kind: "merge_completed · merge_conflict",
      today: "Đã định nghĩa (workflow_event.hpp), và set_merge_on_join_hook() của ADR-055 là thật -- nhưng lần rà soát này không tìm thấy bài kiểm thử nào nêu tên riêng hai loại này trên luồng sự kiện. Không khẳng định theo hướng nào.",
    },
    {
      kind: "agent_turn_event",
      today: "Thật, theo chính văn bản của ADR-152: cầu nối cho node dạng agent (agent_turn_sink) không cần tái cấu trúc vòng lặp drive agent-as-executor sẵn có để nối dây.",
    },
    {
      kind: "moderator_stream_delta",
      today: "Thật -- examples/25: 9 sự kiện được quan sát trực tiếp (3 người tham gia × 3 bước mỗi người), và examples/26 với một model OpenRouter thật: 39 delta theo từng token thật.",
    },
  ],
};

// ------------------------------------------------------------------------------------------------
// What's still evolving (section 6) -- free-text rows, not force-fit into the real/design binary
// since several of these are "real code, informal status" rather than cleanly one or the other.
// ------------------------------------------------------------------------------------------------

export interface EventsGapRow {
  what: string;
  where: string;
}

export const eventsGapRows: Record<Lang, EventsGapRow[]> = {
  en: [
    {
      what: "ADR-152 itself (workflow event streaming)",
      where:
        "decisions/README.md's own row: \"Proposed -- implemented, red-teamed twice ... 35/35 new test checks passing, pending full regression ctest and project-owner sign-off.\" Real, tested code; not yet a Judged ADR.",
    },
    {
      what: "Durable replay of either stream",
      where:
        "Neither RunEvent nor WorkflowEvent is QUARK_SERIALIZE'd -- both are live, in-memory streams. 013 §6 G4 (\"a recorded stream replays into a UI identically\") needs a durable form; run_event.hpp names this as a gap for whichever phase builds recording, not solved here.",
    },
    {
      what: "One consumer per enable_event_stream() call",
      where:
        "ADR-152's own \"not claimed\" list: no fan-out to multiple simultaneous WorkflowSupervisor::enable_event_stream() consumers, the same single-consumer convention enable_live_view() already used. A second call on AgentSession replaces the previous consumer's producer identically.",
    },
    {
      what: "Custom.type_id standardization for generic tool-progress UI",
      where:
        "Named directly in ADR-152 as GitHub issue #29's own separate Group F follow-up -- not solved by this event-stream work.",
    },
    {
      what: "Backgrounded-tool progress",
      where: "ADR-152: \"does not touch the backgrounded-tool progress gap (ADR-060 §4's own named residual)\".",
    },
  ],
  vi: [
    {
      what: "Bản thân ADR-152 (workflow event streaming)",
      where:
        "Dòng của chính decisions/README.md: \"Proposed -- đã triển khai, đã red-team hai lần ... 35/35 kiểm tra mới đều pass, đang chờ ctest hồi quy đầy đủ và sự phê duyệt của chủ dự án.\" Mã thật, đã kiểm thử; chưa phải một ADR ở trạng thái Judged.",
    },
    {
      what: "Phát lại bền vững (durable replay) cho cả hai luồng",
      where:
        "Cả RunEvent lẫn WorkflowEvent đều không được QUARK_SERIALIZE -- cả hai đều là luồng sống, chỉ tồn tại trong bộ nhớ. 013 §6 G4 (\"một luồng đã ghi lại phát lại vào UI giống hệt\") cần một dạng bền vững; run_event.hpp nêu tên đây là một khoảng trống dành cho giai đoạn nào đó xây dựng việc ghi lại, chưa được giải quyết ở đây.",
    },
    {
      what: "Một bên tiêu thụ cho mỗi lần gọi enable_event_stream()",
      where:
        "Danh sách \"không khẳng định\" của chính ADR-152: không có fan-out tới nhiều bên tiêu thụ WorkflowSupervisor::enable_event_stream() đồng thời, cùng quy ước một-bên-tiêu-thụ mà enable_live_view() vốn đã dùng. Một lần gọi thứ hai trên AgentSession thay thế y hệt producer của bên tiêu thụ trước.",
    },
    {
      what: "Chuẩn hóa Custom.type_id cho UI tiến trình tool tổng quát",
      where:
        "Được nêu tên trực tiếp trong ADR-152 là phần việc tiếp theo riêng, Group F của chính GitHub issue #29 -- không được giải quyết bởi phần việc luồng sự kiện này.",
    },
    {
      what: "Tiến trình của tool chạy nền (backgrounded)",
      where: "ADR-152: \"không chạm tới khoảng trống về tiến trình của tool chạy nền (phần dư đã nêu tên của chính ADR-060 §4)\".",
    },
  ],
};

// ------------------------------------------------------------------------------------------------
// Snippets (verbatim shapes from the headers/examples, trimmed -- never invented)
// ------------------------------------------------------------------------------------------------

export const runEventKindSnippet = `// core/run_event.hpp:41-57 -- the whole vocabulary, one enum
enum class run_event_kind {
    run_started, run_finished, run_failed, run_canceled,
    turn_started, turn_finished,
    model_call_started, model_delta, model_call_finished,
    tool_call_started, tool_call_delta, tool_call_finished,
    sandbox_exec_started, sandbox_exec_finished,
    state_changed, artifact_produced,
    input_required, input_resolved,
    auth_required, auth_resolved,
    approval_requested, approval_resolved,
    codeact_ask_requested,
    hook_decision_requested,
    warning, policy_decision,
};

// core/run_event.hpp:211-216
struct RunEvent {
    std::string     run_id;
    std::uint64_t   seq = 0;         // 1-based; 0 means "not a real event yet"
    run_event_kind  kind = run_event_kind::run_started;
    RunEventPayload payload = run_event_payload::Empty{};
};`;

export const agentSessionEnableEventStreamSnippet = `// rt/agent_session.hpp:780-785
[[nodiscard]] stream<RunEvent> enable_event_stream(std::pmr::memory_resource* mr,
                                                    stream_config<RunEvent> cfg = {}) {
    auto pair            = make_stream<RunEvent>(mr, cfg);
    run_event_producer_ = std::move(pair.producer);
    return std::move(pair.consumer);
}`;

export const example29Snippet = `// examples/29_agent_session_events.cpp:118-153 (trimmed)
AgentSession<ScriptedChatClient> session;
session.initialize("s-events-demo", Principal{"p-demo", ""});
session.set_capabilities(&held);

// The one flag that engages the streaming turn loop (ADR-034) -- without it, run_model_call()
// dispatches to the plain chat() method instead, and model_delta never fires.
session.set_stream_model_calls(true);

// Subscribed BEFORE start_run(): enable_event_stream() must be live when the run happens.
stream<RunEvent> events = session.enable_event_stream(std::pmr::get_default_resource());

auto result = drive(session.start_run(StartRun{user_message("hi")}));

// The event stream stays open for the session's whole lifetime -- draining it is just "take
// whatever is already buffered," not "wait for it to close."
std::string joined_deltas;
std::size_t delta_count = 0;
while (auto ev = events.next()) {
    if (ev->kind == run_event_kind::model_delta) {
        ++delta_count;
        auto const& d = std::get<run_event_payload::ModelDelta>(ev->payload);
        if (auto const* t = std::get_if<run_event_payload::ModelTextDelta>(&d.value)) {
            joined_deltas += t->text;
        }
    }
}
// delta_count == 3, joined_deltas == "Hello, world!" -- the event stream and the returned
// AgentResponse never disagree.`;

export const workflowEventKindSnippet = `// workflow/workflow_event.hpp:54-63
enum class workflow_event_kind {
    workflow_run_started, workflow_run_suspended, workflow_run_resumed,
    workflow_run_completed, workflow_run_failed,
    superstep_started, superstep_completed,
    executor_dispatched, executor_completed,
    message_routed, fan_out_dispatched, fan_in_aggregated, route_selected,
    request_port_opened, request_port_resolved,
    checkpoint_saved, merge_completed, merge_conflict,
    agent_turn_event, moderator_stream_delta,
};

// workflow/workflow_event.hpp:164-168
struct WorkflowEvent {
    workflow_event_kind  kind  = workflow_event_kind::workflow_run_started;
    std::uint32_t        round = 0;
    WorkflowEventPayload payload = workflow_event_payload::Empty{};
};`;

export const workflowEventStreamClassSnippet = `// workflow/workflow_event.hpp:189-219 -- merges the structural bucket (ordinary channel-backed
// stream<WorkflowEvent>) with the multiplexed bucket (a multiplex_sink<WorkflowEvent> for
// concurrent per-node events -- ADR-152, issue #29)
class WorkflowEventStream {
public:
    WorkflowEventStream(agentengine::stream<WorkflowEvent> structural,
                        std::shared_ptr<multiplex_sink<WorkflowEvent>> multiplexed)
        : structural_(std::move(structural)), multiplexed_(std::move(multiplexed)) {}

    [[nodiscard]] std::optional<WorkflowEvent> next() {
        if (multiplexed_) {
            if (std::optional<WorkflowEvent> ev = multiplexed_->try_pop()) return ev;
        }
        return structural_.next();
    }

    // Diagnostics only -- a caller is never required to check this to stay correct.
    [[nodiscard]] std::size_t multiplexed_dropped_count() const noexcept {
        return multiplexed_ ? multiplexed_->dropped_count() : 0;
    }

private:
    agentengine::stream<WorkflowEvent>              structural_;
    std::shared_ptr<multiplex_sink<WorkflowEvent>>  multiplexed_;
};`;

export const workflowSupervisorEnableEventStreamSnippet = `// rt/workflow_supervisor.hpp:926-933
[[nodiscard]] agentengine::workflow::WorkflowEventStream enable_event_stream(
    std::pmr::memory_resource* mr,
    agentengine::stream_config<agentengine::workflow::WorkflowEvent> cfg = {}) {
    auto pair = agentengine::make_stream<agentengine::workflow::WorkflowEvent>(mr, cfg);
    workflow_event_producer_       = std::move(pair.producer);
    workflow_event_stream_enabled_ = true;
    return agentengine::workflow::WorkflowEventStream(std::move(pair.consumer), multiplex_sink_);
}`;

export const example25Snippet = `// examples/25_workflow_event_stream_live.cpp:139-166 (trimmed) -- a real 3-way concurrent
// fan-out/fan-in round, each participant on its own ThreadPool worker thread
WorkflowSupervisor sup;
sup.initialize(wf, bodies);
WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

agentengine::rt::task<WorkflowResult> run = sup.run_workflow(RunWorkflow{text_message("start")});

// A real caller would drain \`stream\` from a separate thread/UI loop while this drives; here,
// offline and single-threaded, we poll between resumes -- the events queued between polls (from
// the three concurrently-dispatched participants, each on its own worker thread) are what proves
// this is genuinely live, not just observable after the fact.
std::size_t moderator_deltas_seen = 0;
while (!run.done()) {
    run.resume();
    while (std::optional<WorkflowEvent> ev = stream.next()) {
        if (ev->kind == workflow_event_kind::moderator_stream_delta) ++moderator_deltas_seen;
        // ... fan_out_dispatched / fan_in_aggregated counted the same way
    }
}
// moderator_deltas_seen == 9 (3 participants x 3 steps each), all observed live -- not a
// summary after the round.`;

export const enableLiveViewSnippet = `// rt/workflow_supervisor.hpp:901-912 -- 014 §7's live-view bullet: one event per superstep
// boundary, the SAME boundary the checkpoint hook fires from. A second call REPLACES the
// producer -- no fan-out to multiple simultaneous viewers, unlike WorkflowEventStream's own
// per-node multiplexed bucket which this method has no equivalent of.
[[nodiscard]] agentengine::stream<agentengine::workflow::WorkflowLiveEvent> enable_live_view(
    std::pmr::memory_resource* mr,
    agentengine::stream_config<agentengine::workflow::WorkflowLiveEvent> cfg = {}) {
    auto pair            = agentengine::make_stream<agentengine::workflow::WorkflowLiveEvent>(mr, cfg);
    live_view_producer_  = std::move(pair.producer);
    return std::move(pair.consumer);
}

// workflow/live_view.hpp:37-49
struct WorkflowLiveEvent {
    std::uint32_t                   round = 0;
    std::vector<ExecutorLiveState>  executor_states;    // who ran/failed/opened a port THIS round
    std::size_t                     in_flight_message_count = 0;
};`;

export const liveViewTestSnippet = `// tests/test_rt_workflow_live_view.cpp:147-171 (trimmed) -- a human-in-the-loop graph:
// draft -> ready -> approve (a request_port) -> publish
auto viewer = sup.enable_live_view(std::pmr::get_default_resource());
WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
// r1.status == workflow_status::suspended

auto events = drain_all(viewer);
// events.size() == 3 -- exactly one live event per superstep boundary:
//   round 1: draft ran_ok,   1 message in flight
//   round 2: ready ran_ok,   1 message in flight
//   round 3: approve port_open, 0 in flight

// Resuming for real produces a FOURTH event for publish's own round -- the SAME producer keeps
// working across a suspend/resume boundary, not just for the first run.`;

export const runEventProjectorSnippet = `// protocol/agui/projection.hpp:83-104 (trimmed) -- RunEventProjector::project()
case run_event_kind::model_call_started: {
    std::string const message_id = mint_message_id(ev.run_id);
    open_message_id_[ev.run_id] = message_id;
    return {TextMessageStart{message_id, "assistant", std::nullopt}};
}
case run_event_kind::model_delta: {
    auto const& p = std::get<run_event_payload::ModelDelta>(ev.payload);
    if (auto const* text = std::get_if<run_event_payload::ModelTextDelta>(&p.value)) {
        std::string const& message_id = ensure_open_message(ev.run_id);
        return {TextMessageContent{message_id, text->text}};
    }
    auto const& arg = std::get<run_event_payload::ModelToolCallArgumentDelta>(p.value);
    return {ToolCallArgs{arg.call_id, arg.arguments_fragment}};
}
case run_event_kind::model_call_finished: {
    std::string const message_id = ensure_open_message(ev.run_id);
    open_message_id_.erase(ev.run_id);
    return {TextMessageEnd{message_id}};
}`;

export const a2aStreamProjectorSnippet = `// protocol/a2a/streaming.hpp:103-131 (trimmed) -- A2aStreamProjector::project()
// A2A's Task model is COARSER-GRAINED than AG-UI's own event stream (012 §1: "Task <- Run", one
// task-lifecycle state machine per run, no wire slot for turn/model/tool-call granularity).
[[nodiscard]] std::vector<StreamResponse> project(RunEvent const& ev) {
    switch (ev.kind) {
        case run_event_kind::run_started:   return {status(task_state::working)};
        case run_event_kind::run_finished:  return {status(task_state::completed)};
        case run_event_kind::run_failed:    return {status(task_state::failed)};
        case run_event_kind::input_required: return {status(task_state::input_required)};
        case run_event_kind::approval_requested:
            // No native "confirmation" task_state member -- input_required is the closest real
            // state, named as such rather than a fabricated new one.
            return {status(task_state::input_required)};
        case run_event_kind::artifact_produced: { /* -> TaskArtifactUpdateEvent */ }
        default:
            // Every other RunEvent kind has no A2A task-lifecycle wire slot -- an empty vector,
            // not a fabricated status transition.
            return {};
    }
}`;
