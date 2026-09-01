import { gh } from "../data/apiContent";
import {
  a2aStreamProjectorSnippet,
  agentSessionEnableEventStreamSnippet,
  enableLiveViewSnippet,
  eventsGapRows,
  example25Snippet,
  example29Snippet,
  liveViewTestSnippet,
  runEventKindRows,
  runEventKindSnippet,
  runEventProjectorSnippet,
  workflowEventKindRows,
  workflowEventKindSnippet,
  workflowEventStreamClassSnippet,
  workflowSupervisorEnableEventStreamSnippet,
} from "../data/eventsContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

function Cite({ path, label }: { path: string; label?: string }) {
  return (
    <a
      className="api-cite"
      href={gh(path)}
      target="_blank"
      rel="noreferrer"
      style={{ borderTop: "none", paddingTop: 0, marginTop: 14, display: "block" }}
    >
      {label ?? path}
    </a>
  );
}

const copy = {
  en: {
    eyebrow: "013 §1 — the real run-event stream",
    headingPrefix: "One event stream,",
    headingHighlight: "two places it's emitted from",
    intro: (
      <>
        AgentEngine has exactly one internal notion of "what is happening right now inside a run":
        a monotonic, per-run sequence of typed events. There is no separate UI event model bolted
        on later — every AG-UI, A2A, and (eventually) MCP/SSE wire projection is a pure function of
        the same stream this page documents. It is emitted from two places: an{" "}
        <code>AgentSession</code>'s own turn loop (<code>RunEvent</code>, real since Milestone 7
        Phase A), and a <code>WorkflowSupervisor</code>'s superstep engine (<code>WorkflowEvent</code>,
        real since ADR-152). Both hand back a pull-based <code>stream&lt;T&gt;</code> (
        <code>core/stream.hpp</code>) a caller drains with <code>.next()</code> — the same primitive{" "}
        <a href="./streaming.html">the Streaming page</a> covers for a model's own text.
      </>
    ),
    streamingNote: (
      <>
        <strong>This is not the same thing streaming.html documents — the two compose.</strong> A
        streamed run's <em>text</em> comes from <code>chat_stream()</code>/<code>stream&lt;ChatResponseUpdate&gt;</code> —
        that page's subject. A run's <em>lifecycle and progress</em> — which turn is running, which
        tool call started, whether a round suspended for approval — comes from the event stream
        this page documents. An <code>AgentSession</code> with <code>set_stream_model_calls(true)</code> engaged
        emits both at once: <code>model_delta</code> events here, and <code>ChatResponseUpdate</code>{" "}
        pushes there, over the <em>same underlying call</em>. See{" "}
        <a href="./streaming.html">Streaming →</a>.
      </>
    ),

    s1Eyebrow: "core/run_event.hpp — 013 §1",
    s1Heading: "RunEvent: one session, one ordered, per-run sequence",
    s1Body: (
      <>
        <code>run_event_kind</code> is a 26-value vocabulary, and it is deliberately <em>total</em> —
        013 §1's own twenty-line list names every kind, each with a real payload type, because later
        AG-UI/A2A/MCP projections need the whole shape to bind against, not a subset redesigned per
        consumer. <code>RunEvent::seq</code> starts at 1 for a run's first event (0 is never real),
        so a default-constructed <code>RunEvent</code> is recognizably not one that happened.
      </>
    ),
    s1Note: (
      <>
        <strong>Total vocabulary, partial wiring — and that's stated in the header, not hidden.</strong>{" "}
        <code>run_event.hpp</code>'s own top comment: AgentSession's turn loop today "fires real
        events at AgentSession's actual turn-loop boundaries (Run/Turn/ModelCall/StateChanged)" —
        several kinds (most of the tool-call and sandbox lifecycle) have a real payload shape and no
        confirmed emitter yet, named honestly in the table below rather than left for a reader to
        discover by grepping. One claim in that same header comment is now stale in a good way:
        it lists <code>ModelDelta</code> among the kinds with "no real producer" — but ADR-034's
        streaming turn loop and <code>examples/29_agent_session_events.cpp</code> (passing today)
        prove text deltas fire for real once <code>set_stream_model_calls(true)</code> is set. The
        header predates that wiring; the table below reflects what's actually true now.
      </>
    ),
    runEventCols: ["run_event_kind", "Payload", "Where it stands today"],

    s2Eyebrow: "rt/agent_session.hpp:780-785 — 013 §1",
    s2Heading: "AgentSession::enable_event_stream() — the worked example",
    s2Body: (
      <>
        One call, subscribed <em>before</em> <code>start_run()</code> — a run that already happened
        has nothing to attach events to. The returned <code>stream&lt;RunEvent&gt;</code> stays open
        for the session's whole lifetime; a second run appends more events, renumbered from{" "}
        <code>seq</code> 1 again. <code>examples/29_agent_session_events.cpp</code> is the real,
        passing worked example this section excerpts — it mirrors S1/A2 of{" "}
        <code>tests/test_rt_agent_session_streaming_and_events.cpp</code>.
      </>
    ),
    s2Note: (
      <>
        <strong>The event stream and the returned response never disagree, and the example proves
        it, not just asserts it.</strong> It joins every <code>model_delta</code> event's text and
        checks the concatenation equals both the scripted client's pushed text <em>and</em> the
        final <code>AgentResponse</code>'s own message — the same value reached two structurally
        different ways.
      </>
    ),
    s2Link: (
      <>
        This extends material already on <a href="./runtime.html">the Runtime page →</a>.
      </>
    ),

    s3Eyebrow: "workflow/workflow_event.hpp — ADR-152, GitHub issue #29",
    s3Heading: "WorkflowEvent: structural routing, plus genuinely live per-node deltas",
    s3Body: (
      <>
        A <code>WorkflowSupervisor</code> round can dispatch several executors concurrently (a
        fan-out), each on its own <code>ThreadPool</code> worker thread. <code>WorkflowEvent</code>'s
        19-value vocabulary covers both halves of that: structural decisions the supervisor itself
        makes (routing, checkpointing, fan-out/fan-in) and per-node activity a running executor
        streams on its own (<code>agent_turn_event</code>, <code>moderator_stream_delta</code>).{" "}
        <code>WorkflowEventStream</code> is the merged handle a caller actually holds — its own{" "}
        <code>.next()</code> checks the multiplexed (per-node) bucket first, then the structural
        one, so nothing is lost to poll ordering.
      </>
    ),
    s3Note: (
      <>
        <strong>Why two buckets, not one channel.</strong> The obvious design — one shared channel
        every event pushes into — was red-teamed and found to have a real liveness hazard: a
        channel-backed producer's <code>push()</code> deliberately blocks under backpressure, which
        is correct for routing/lifecycle events but wrong for a <code>ThreadPool</code> worker
        thread also needed for actual compute — enough concurrently-streaming nodes stuck behind a
        lagging UI consumer could stall the workflow itself. The fix was a purpose-built,
        non-blocking MPSC sink (<code>multiplex_sink&lt;T&gt;</code>) for per-node deltas only,
        which drops silently on overflow with a diagnostic counter rather than ever blocking a
        compute thread — proven adversarially in ADR-152's own W9: with the sink's{" "}
        <code>push()</code> temporarily mutated to spin-block, the exact same test hung and was
        killed by a timeout; reverted, all 35 checks passed clean again.
      </>
    ),
    workflowEventCols: ["workflow_event_kind", "Where it stands today"],

    s4Eyebrow: "rt/workflow_supervisor.hpp:926-933, examples/25 — ADR-152",
    s4Heading: "WorkflowSupervisor::enable_event_stream() — the worked example",
    s4Body: (
      <>
        Same shape as the session-level call: subscribe, then run. <code>examples/25_workflow_event_stream_live.cpp</code>{" "}
        builds a real 3-way fan-out (researcher / critic / summarizer) that fans back into one join
        node, with each participant streaming its own progress through{" "}
        <code>ctx.moderator_delta_sink(...)</code>. The example's own consumer polls between{" "}
        <code>resume()</code> calls (an offline drive loop has nowhere else to poll from), but the
        events are pushed the instant each participant emits them, from whichever worker thread is
        running that participant's call.
      </>
    ),
    s4Note: (
      <>
        <strong>What the example actually measures, not just infers.</strong> All 9 real{" "}
        <code>moderator_stream_delta</code> events (3 participants × 3 steps) are counted live,
        exactly one <code>fan_out_dispatched</code> fires carrying all 3 real targets, and exactly
        one <code>fan_in_aggregated</code> fires listing all 3 real contributing sources — never one
        event per converging edge. <code>examples/26_workflow_event_stream_live_openrouter.cpp</code>{" "}
        repeats this against a real OpenRouter model and observes 39 genuine per-token deltas,
        reassembling byte-for-byte into the model's real answer.
      </>
    ),
    s4Link: (
      <>
        This extends material already on <a href="./workflow.html">the Workflow &amp; Orchestration
        page →</a>.
      </>
    ),
    s4BridgeNote: (
      <>
        <strong>A related bridge, worth knowing about even though it's a different page's subject.</strong>{" "}
        <code>examples/28_workflow_as_chat_client.cpp</code> wraps a whole <code>Workflow</code> so
        it satisfies the <code>ChatClient</code> concept directly (<code>WorkflowChatClient</code>).
        When that wrapped workflow's own <code>request_port</code> node suspends, the caller sees it
        as a <code>Custom</code>-typed <code>ChatResponseUpdate</code> (never a fabricated{" "}
        <code>ToolCall</code>, which would let an outer <code>AgentSession</code> silently
        mis-resolve it) — a concrete case of an internal suspension surfacing through a
        chat-shaped API rather than this page's own event stream. It sits between this page and{" "}
        <a href="./streaming.html">Streaming</a>: same underlying suspension machinery, projected
        onto a different consumer-facing shape. The real wire shape, and why <code>Custom</code>{" "}
        beats a naive <code>ToolCall</code> encoding here, now has its own worked section on{" "}
        <a href="./workflow.html#workflow-hitl-chat-client">
          Workflow &amp; Orchestration — HITL over chat_client
        </a>
        .
      </>
    ),

    s5Eyebrow: "rt/workflow_supervisor.hpp:901-912 — 014 §7",
    s5Heading: "enable_live_view(): the coarser, older sibling — still around, still useful",
    s5Body: (
      <>
        Before ADR-152, this was the whole workflow observability surface, and it still ships
        unchanged: one <code>WorkflowLiveEvent</code> per superstep boundary — which executors
        ran/failed/opened a port <em>this round</em>, and how many messages are now in flight — the
        same boundary the checkpoint hook fires from. No per-node streaming, no multiplexing, and
        (unlike <code>WorkflowEventStream</code>) a second call <em>replaces</em> the producer
        rather than composing with it.
      </>
    ),
    s5Note: (
      <>
        <strong>When to reach for which.</strong> <code>enable_live_view()</code> answers "what
        happened this round" in one summary event — enough for a dashboard that only needs
        round-by-round progress, and it is what <code>tests/test_rt_workflow_live_view.cpp</code>{" "}
        proves keeps working across a real suspend/resume boundary (a human-in-the-loop{" "}
        <code>request_port</code> graph). <code>enable_event_stream()</code> answers "what is
        happening <em>right now</em>, including inside this round" — the one to reach for when a UI
        needs to show a fan-out's individual participants streaming concurrently, the way
        examples/25's own three-worker graph does.
      </>
    ),

    s6Eyebrow: "protocol/agui/projection.hpp · protocol/a2a/streaming.hpp — 013 §2-§3",
    s6Heading: "Wire projections: this is the exact mechanism a protocol bridge uses",
    s6Body: (
      <>
        Neither AG-UI nor A2A gets a second, independently-designed event model. Both are pure
        projections of the same <code>RunEvent</code> stream this page documents — a protocol
        bridge is "writing a projection, not a second event model" (013 §1). AG-UI's projector is
        stateful (it has to synthesize and track a <code>messageId</code> to bracket a model's
        incremental text, since <code>RunEvent</code> carries none); A2A's is a pure function per
        event, because A2A's own <code>Task</code> model is coarser-grained than AG-UI's — one
        task-lifecycle state machine per run, with no wire slot for turn/model/tool-call
        granularity, so most internal kinds project to nothing rather than a fabricated status
        transition. Full wire-format depth — AG-UI's event catalogue, A2A's Task/Message/Part
        model — is <a href="./protocols.html">the Protocol surfaces page's</a> job, not this one's.
      </>
    ),
    s6Note: (
      <>
        <strong>Both projectors are honest about their own gaps.</strong>{" "}
        <code>RunEventProjector</code> maps <code>tool_call_delta</code> to a <code>CustomEvent</code>,
        not a fabricated <code>TOOL_CALL_CHUNK</code>, because the cited AG-UI research record gives
        no field shape for one. <code>A2aStreamProjector</code> collapses{" "}
        <code>approval_requested</code> onto <code>TASK_STATE_INPUT_REQUIRED</code> because A2A's{" "}
        <code>task_state</code> enum has no distinct "confirmation" member — the closest real state,
        named as such rather than invented.
      </>
    ),

    s7Eyebrow: "013 §6 G4 · ADR-152 · decisions/README.md",
    s7Heading: "What's still evolving",
    s7Body:
      "The event streams themselves are real and tested — every claim above cites a passing example or a named test function. What's below is the honest boundary around that: one governance item, and a few named, disclosed residuals rather than a silent gap.",
    gapCols: ["What", "Where it actually stands"],
  },

  vi: {
    eyebrow: "013 §1 — luồng run-event thật",
    headingPrefix: "Một luồng sự kiện,",
    headingHighlight: "hai nơi phát sinh ra nó",
    intro: (
      <>
        AgentEngine chỉ có đúng một khái niệm nội bộ cho "điều gì đang xảy ra ngay bây giờ bên trong
        một lần chạy": một chuỗi sự kiện có kiểu, đơn điệu, theo từng lần chạy. Không có một mô hình
        sự kiện UI riêng biệt được gắn thêm sau này — mọi phép chiếu sang AG-UI, A2A, và (rồi đây)
        MCP/SSE đều là một hàm thuần túy trên cùng một luồng mà trang này ghi lại. Nó được phát ra từ
        hai nơi: vòng lặp lượt của chính một <code>AgentSession</code> (<code>RunEvent</code>, đã
        thật từ Milestone 7 Phase A), và động cơ superstep của một <code>WorkflowSupervisor</code> (
        <code>WorkflowEvent</code>, đã thật từ ADR-152). Cả hai đều trả về một{" "}
        <code>stream&lt;T&gt;</code> kiểu pull (<code>core/stream.hpp</code>) mà bên gọi rút cạn bằng{" "}
        <code>.next()</code> — cùng nguyên thủy mà <a href="./streaming.html">trang Streaming</a>{" "}
        đã ghi lại cho văn bản của chính model.
      </>
    ),
    streamingNote: (
      <>
        <strong>Đây không phải thứ streaming.html đã ghi lại — hai thứ này kết hợp với nhau.</strong>{" "}
        <em>Văn bản</em> của một lần chạy có streaming đến từ{" "}
        <code>chat_stream()</code>/<code>stream&lt;ChatResponseUpdate&gt;</code> — chủ đề của trang
        đó. <em>Vòng đời và tiến trình</em> của một lần chạy — lượt nào đang chạy, lệnh gọi tool nào
        vừa bắt đầu, một round có treo lại để chờ phê duyệt hay không — đến từ luồng sự kiện mà trang
        này ghi lại. Một <code>AgentSession</code> có bật <code>set_stream_model_calls(true)</code>{" "}
        phát ra cả hai cùng lúc: sự kiện <code>model_delta</code> ở đây, và các lần đẩy{" "}
        <code>ChatResponseUpdate</code> ở đó, trên <em>cùng một lệnh gọi bên dưới</em>. Xem{" "}
        <a href="./streaming.html">Streaming →</a>.
      </>
    ),

    s1Eyebrow: "core/run_event.hpp — 013 §1",
    s1Heading: "RunEvent: một session, một chuỗi có thứ tự, theo từng lần chạy",
    s1Body: (
      <>
        <code>run_event_kind</code> là một bộ từ vựng 26 giá trị, và nó cố ý mang tính <em>đầy đủ</em> —
        chính danh sách hai mươi dòng của 013 §1 nêu tên mọi loại, mỗi loại có một kiểu payload thật,
        vì các phép chiếu AG-UI/A2A/MCP sau này cần toàn bộ hình dạng đó để ràng buộc vào, không phải
        một tập con được thiết kế lại cho từng bên tiêu thụ. <code>RunEvent::seq</code> bắt đầu từ 1
        cho sự kiện đầu tiên của một lần chạy (0 không bao giờ là thật), nên một <code>RunEvent</code>{" "}
        khởi tạo mặc định nhận biết được ngay là chưa từng xảy ra.
      </>
    ),
    s1Note: (
      <>
        <strong>Từ vựng đầy đủ, nối dây một phần — và điều đó được nói thẳng trong header, không giấu.</strong>{" "}
        Chính chú thích đầu file của <code>run_event.hpp</code>: vòng lặp lượt của AgentSession hôm
        nay "phát các sự kiện thật tại đúng các ranh giới vòng lặp lượt của AgentSession
        (Run/Turn/ModelCall/StateChanged)" — một số loại (phần lớn vòng đời tool-call và sandbox) có
        hình dạng payload thật nhưng chưa có bên phát sinh nào được xác nhận, được nêu tên thẳng
        thắn trong bảng bên dưới thay vì để người đọc tự grep ra. Một khẳng định trong chính chú
        thích đó giờ đã lỗi thời theo hướng tốt: nó liệt kê <code>ModelDelta</code> vào nhóm "chưa có
        bên phát sinh thật" — nhưng vòng lặp lượt có streaming của ADR-034 và{" "}
        <code>examples/29_agent_session_events.cpp</code> (đang pass) chứng minh các delta văn bản
        phát thật một khi <code>set_stream_model_calls(true)</code> được bật. Header đó có trước
        việc nối dây này; bảng bên dưới phản ánh đúng sự thật hiện nay.
      </>
    ),
    runEventCols: ["run_event_kind", "Payload", "Thực tế hôm nay đang ở đâu"],

    s2Eyebrow: "rt/agent_session.hpp:780-785 — 013 §1",
    s2Heading: "AgentSession::enable_event_stream() — ví dụ minh họa",
    s2Body: (
      <>
        Một lệnh gọi, đăng ký <em>trước</em> <code>start_run()</code> — một lần chạy đã xảy ra rồi
        thì không còn gì để gắn sự kiện vào. <code>stream&lt;RunEvent&gt;</code> trả về sống suốt
        vòng đời của session; một lần chạy thứ hai sẽ nối thêm sự kiện, đánh số lại từ{" "}
        <code>seq</code> bằng 1. <code>examples/29_agent_session_events.cpp</code> là ví dụ minh
        họa thật, đang pass, mà mục này trích dẫn — nó phản ánh S1/A2 của{" "}
        <code>tests/test_rt_agent_session_streaming_and_events.cpp</code>.
      </>
    ),
    s2Note: (
      <>
        <strong>Luồng sự kiện và phản hồi trả về không bao giờ mâu thuẫn nhau, và ví dụ này chứng
        minh điều đó, không chỉ khẳng định suông.</strong> Nó nối văn bản của mọi sự kiện{" "}
        <code>model_delta</code> lại và kiểm tra chuỗi ghép đó bằng cả văn bản mà scripted client đã
        đẩy <em>lẫn</em> chính message của <code>AgentResponse</code> cuối cùng — cùng một giá trị,
        đạt được bằng hai con đường khác nhau về cấu trúc.
      </>
    ),
    s2Link: (
      <>
        Điều này mở rộng nội dung đã có trên <a href="./runtime.html">trang Runtime →</a>.
      </>
    ),

    s3Eyebrow: "workflow/workflow_event.hpp — ADR-152, GitHub issue #29",
    s3Heading: "WorkflowEvent: định tuyến cấu trúc, cộng với delta theo từng node thật sự trực tiếp",
    s3Body: (
      <>
        Một round của <code>WorkflowSupervisor</code> có thể phát nhiều executor cùng lúc (một
        fan-out), mỗi executor trên một luồng worker <code>ThreadPool</code> riêng.{" "}
        <code>WorkflowEvent</code> với bộ từ vựng 19 giá trị bao phủ cả hai nửa của điều đó: các
        quyết định cấu trúc mà chính supervisor đưa ra (định tuyến, checkpoint, fan-out/fan-in) và
        hoạt động riêng của từng node đang tự stream (<code>agent_turn_event</code>,{" "}
        <code>moderator_stream_delta</code>). <code>WorkflowEventStream</code> là handle đã hợp
        nhất mà bên gọi thực sự nắm giữ — <code>.next()</code> của chính nó kiểm tra bucket đa hợp
        (theo từng node) trước, rồi mới đến bucket cấu trúc, nên không có gì bị mất vì thứ tự poll.
      </>
    ),
    s3Note: (
      <>
        <strong>Vì sao có hai bucket, không phải một channel.</strong> Thiết kế hiển nhiên — một
        channel dùng chung mà mọi sự kiện đều đẩy vào — đã được red-team và phát hiện một hiểm họa
        liveness có thật: <code>push()</code> của một producer dựa trên channel cố ý chặn lại khi bị
        áp lực ngược, điều này đúng cho các sự kiện định tuyến/vòng đời nhưng sai cho một luồng
        worker <code>ThreadPool</code> vốn cũng cần cho việc tính toán thật — đủ số node đang stream
        đồng thời bị kẹt sau một bên tiêu thụ UI chậm có thể làm nghẽn chính workflow. Bản sửa là
        một sink MPSC không chặn, xây riêng cho mục đích này (<code>multiplex_sink&lt;T&gt;</code>)
        chỉ dành cho delta theo từng node, nó âm thầm loại bỏ khi tràn kèm một bộ đếm chẩn đoán thay
        vì bao giờ chặn một luồng tính toán — được chứng minh đối kháng ngay trong W9 của chính
        ADR-152: khi <code>push()</code> của sink tạm thời bị biến đổi để spin-block, đúng bài kiểm
        thử đó bị treo và bị timeout giết chết; hoàn tác lại, cả 35 kiểm tra pass sạch trở lại.
      </>
    ),
    workflowEventCols: ["workflow_event_kind", "Thực tế hôm nay đang ở đâu"],

    s4Eyebrow: "rt/workflow_supervisor.hpp:926-933, examples/25 — ADR-152",
    s4Heading: "WorkflowSupervisor::enable_event_stream() — ví dụ minh họa",
    s4Body: (
      <>
        Cùng hình dạng với lệnh gọi ở mức session: đăng ký, rồi chạy.{" "}
        <code>examples/25_workflow_event_stream_live.cpp</code> dựng một fan-out 3 chiều thật
        (researcher / critic / summarizer) rồi fan-in lại vào một node join, mỗi người tham gia tự
        stream tiến trình của mình qua <code>ctx.moderator_delta_sink(...)</code>. Bên tiêu thụ của
        chính ví dụ này poll giữa các lần gọi <code>resume()</code> (một vòng lặp drive offline
        không còn chỗ nào khác để poll), nhưng các sự kiện được đẩy ngay tức khắc khi mỗi người
        tham gia phát ra chúng, từ bất kỳ luồng worker nào đang chạy lệnh gọi của người đó.
      </>
    ),
    s4Note: (
      <>
        <strong>Điều ví dụ này thực sự đo được, không chỉ suy luận ra.</strong> Cả 9 sự kiện{" "}
        <code>moderator_stream_delta</code> thật (3 người tham gia × 3 bước) đều được đếm trực
        tiếp, đúng một <code>fan_out_dispatched</code> phát ra mang cả 3 đích thật, và đúng một{" "}
        <code>fan_in_aggregated</code> phát ra liệt kê cả 3 nguồn đóng góp thật — không bao giờ một
        sự kiện cho mỗi cạnh hội tụ. <code>examples/26_workflow_event_stream_live_openrouter.cpp</code>{" "}
        lặp lại điều này với một model OpenRouter thật và quan sát được 39 delta theo từng token
        thật, ghép lại khớp từng byte thành câu trả lời thật của model.
      </>
    ),
    s4Link: (
      <>
        Điều này mở rộng nội dung đã có trên{" "}
        <a href="./workflow.html">trang Workflow &amp; Điều phối →</a>.
      </>
    ),
    s4BridgeNote: (
      <>
        <strong>Một cầu nối liên quan, đáng biết dù thuộc chủ đề của trang khác.</strong>{" "}
        <code>examples/28_workflow_as_chat_client.cpp</code> bọc cả một <code>Workflow</code> để nó
        thỏa mãn trực tiếp khái niệm <code>ChatClient</code> (<code>WorkflowChatClient</code>). Khi
        node <code>request_port</code> của workflow được bọc đó treo lại, bên gọi thấy nó dưới dạng
        một <code>ChatResponseUpdate</code> kiểu <code>Custom</code> (không bao giờ là một{" "}
        <code>ToolCall</code> giả tạo, thứ sẽ khiến một <code>AgentSession</code> bên ngoài âm thầm
        giải quyết sai nó) — một trường hợp cụ thể của việc một sự đình chỉ nội bộ nổi lên qua một
        hình dạng API kiểu chat, chứ không phải qua luồng sự kiện của chính trang này. Nó nằm giữa
        trang này và <a href="./streaming.html">Streaming</a>: cùng cơ chế đình chỉ bên dưới, được
        chiếu lên một hình dạng hướng-tới-bên-tiêu-thụ khác. Hình dạng wire thật, và vì sao{" "}
        <code>Custom</code> tốt hơn một mã hóa <code>ToolCall</code> ngây thơ ở đây, giờ có hẳn
        một mục riêng trên{" "}
        <a href="./workflow.html#workflow-hitl-chat-client">
          Workflow &amp; Điều phối — HITL qua chat_client
        </a>
        .
      </>
    ),

    s5Eyebrow: "rt/workflow_supervisor.hpp:901-912 — 014 §7",
    s5Heading: "enable_live_view(): người anh em thô hơn, ra đời trước — vẫn còn đó, vẫn hữu ích",
    s5Body: (
      <>
        Trước ADR-152, đây là toàn bộ bề mặt quan sát workflow, và nó vẫn xuất xưởng không đổi: một{" "}
        <code>WorkflowLiveEvent</code> cho mỗi ranh giới superstep — những executor nào đã
        chạy/thất bại/mở một port <em>trong round này</em>, và có bao nhiêu message đang trên đường
        đi — đúng ranh giới mà checkpoint hook cũng phát từ đó. Không có streaming theo từng node,
        không đa hợp, và (khác với <code>WorkflowEventStream</code>) một lệnh gọi thứ hai{" "}
        <em>thay thế</em> producer thay vì kết hợp với nó.
      </>
    ),
    s5Note: (
      <>
        <strong>Khi nào dùng cái nào.</strong> <code>enable_live_view()</code> trả lời "round này đã
        xảy ra chuyện gì" bằng một sự kiện tóm tắt duy nhất — đủ cho một dashboard chỉ cần tiến
        trình theo từng round, và đây là thứ mà <code>tests/test_rt_workflow_live_view.cpp</code>{" "}
        chứng minh vẫn hoạt động qua một ranh giới suspend/resume thật (một đồ thị{" "}
        <code>request_port</code> có con người tham gia). <code>enable_event_stream()</code> trả
        lời "điều gì đang xảy ra <em>ngay bây giờ</em>, kể cả bên trong round này" — cái nên dùng
        khi một UI cần hiển thị các thành viên riêng lẻ của một fan-out đang cùng stream, đúng như
        đồ thị ba worker của chính examples/25.
      </>
    ),

    s6Eyebrow: "protocol/agui/projection.hpp · protocol/a2a/streaming.hpp — 013 §2-§3",
    s6Heading: "Phép chiếu sang giao thức: đây chính xác là cơ chế một cầu nối giao thức dùng",
    s6Body: (
      <>
        Không AG-UI cũng không A2A có một mô hình sự kiện thứ hai, được thiết kế độc lập. Cả hai đều
        là phép chiếu thuần túy của cùng luồng <code>RunEvent</code> mà trang này ghi lại — một cầu
        nối giao thức là "viết một phép chiếu, không phải một mô hình sự kiện thứ hai" (013 §1). Bộ
        chiếu của AG-UI có trạng thái (nó phải tổng hợp và theo dõi một <code>messageId</code> để
        đóng khung văn bản tăng dần của model, vì <code>RunEvent</code> không mang theo cái nào cả);
        bộ chiếu của A2A là một hàm thuần túy cho mỗi sự kiện, vì mô hình <code>Task</code> của
        chính A2A thô hơn AG-UI — một máy trạng thái vòng đời task cho mỗi lần chạy, không có chỗ
        trên dây cho độ chi tiết turn/model/tool-call, nên hầu hết các loại nội bộ chiếu ra không có
        gì thay vì một sự chuyển trạng thái giả tạo. Chiều sâu đầy đủ về định dạng trên dây — danh
        mục sự kiện của AG-UI, mô hình Task/Message/Part của A2A — là việc của{" "}
        <a href="./protocols.html">trang Bề mặt giao thức</a>, không phải trang này.
      </>
    ),
    s6Note: (
      <>
        <strong>Cả hai bộ chiếu đều thẳng thắn về khoảng trống của chính mình.</strong>{" "}
        <code>RunEventProjector</code> chiếu <code>tool_call_delta</code> thành một{" "}
        <code>CustomEvent</code>, không phải một <code>TOOL_CALL_CHUNK</code> giả tạo, vì tài liệu
        nghiên cứu AG-UI được trích dẫn không cho hình dạng trường nào cho nó.{" "}
        <code>A2aStreamProjector</code> gộp <code>approval_requested</code> vào{" "}
        <code>TASK_STATE_INPUT_REQUIRED</code> vì enum <code>task_state</code> của A2A không có
        thành viên "confirmation" riêng — trạng thái thật gần nhất, được nêu đúng như vậy thay vì
        bịa ra.
      </>
    ),

    s7Eyebrow: "013 §6 G4 · ADR-152 · decisions/README.md",
    s7Heading: "Những gì vẫn đang thay đổi",
    s7Body:
      "Bản thân các luồng sự kiện là thật và đã được kiểm thử — mọi khẳng định ở trên đều trích dẫn một ví dụ đang pass hoặc một hàm kiểm thử được nêu tên. Bên dưới là ranh giới trung thực quanh điều đó: một mục về quản trị, và vài phần dư được nêu tên, công khai chứ không phải một khoảng trống âm thầm.",
    gapCols: ["Cái gì", "Thực tế đang ở đâu"],
  },
} as const;

export function ApiEventsReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];

  return (
    <section className="section" id="events">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 780 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {tu.statusRealTested}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        <RevealGroup>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.streamingNote}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 1. RunEvent & AgentSession ------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="events-run" style={{ marginTop: 48, marginBottom: 22 }}>
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="core/run_event.hpp">{highlightCpp(runEventKindSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.runEventCols]}
              templateColumns="1.6fr 1.6fr 2.4fr"
              rows={runEventKindRows[lang].map((r) => [
                <code key="k">{r.kind}</code>,
                <code key="p" style={{ fontSize: "0.82em" }}>{r.payload}</code>,
                r.today,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>
              {t.s1Note}
            </p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="rt/agent_session.hpp">
              {highlightCpp(agentSessionEnableEventStreamSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <div className="section-head" style={{ marginTop: 26, marginBottom: 8 }}>
              <h4 style={{ fontSize: "1.05rem", margin: 0 }}>{t.s2Heading}</h4>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/29_agent_session_events.cpp">
              {highlightCpp(example29Snippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s2Note}</p>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 14 }}>{t.s2Link}</p>
          </RevealItem>

          <RevealItem>
            <Cite path="examples/29_agent_session_events.cpp" label="examples/29_agent_session_events.cpp" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. WorkflowEvent & WorkflowEventStream ------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="events-workflow" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="workflow/workflow_event.hpp">
              {highlightCpp(workflowEventKindSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.workflowEventCols]}
              templateColumns="2fr 3fr"
              rows={workflowEventKindRows[lang].map((r) => [<code key="k">{r.kind}</code>, r.today])}
            />
          </RevealItem>

          <RevealItem>
            <CodePanel filename="workflow/workflow_event.hpp">
              {highlightCpp(workflowEventStreamClassSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>
              {t.s3Note}
            </p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="rt/workflow_supervisor.hpp">
              {highlightCpp(workflowSupervisorEnableEventStreamSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <div className="section-head" style={{ marginTop: 26, marginBottom: 8 }}>
              <h4 style={{ fontSize: "1.05rem", margin: 0 }}>{t.s4Heading}</h4>
              <p>{t.s4Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/25_workflow_event_stream_live.cpp">
              {highlightCpp(example25Snippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s4Note}</p>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 14 }}>{t.s4Link}</p>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s4BridgeNote}</p>
          </RevealItem>

          <RevealItem>
            <Cite
              path="examples/25_workflow_event_stream_live.cpp"
              label="examples/25_workflow_event_stream_live.cpp · examples/28_workflow_as_chat_client.cpp"
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. enable_live_view() ------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="events-live-view" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="rt/workflow_supervisor.hpp">{highlightCpp(enableLiveViewSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="tests/test_rt_workflow_live_view.cpp">
              {highlightCpp(liveViewTestSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s5Note}</p>
          </RevealItem>

          <RevealItem>
            <Cite path="tests/test_rt_workflow_live_view.cpp" label="tests/test_rt_workflow_live_view.cpp" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. Wire projections ---------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="events-wire" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s6Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
              <p>{t.s6Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="protocol/agui/projection.hpp">
              {highlightCpp(runEventProjectorSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="protocol/a2a/streaming.hpp">
              {highlightCpp(a2aStreamProjectorSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s6Note}</p>
          </RevealItem>

          <RevealItem>
            <Cite
              path="include/agentengine/protocol/agui/projection.hpp"
              label="include/agentengine/protocol/agui/projection.hpp · include/agentengine/protocol/a2a/streaming.hpp"
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- 5. What's still evolving ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="events-status" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s7Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s7Heading}</h3>
              <p>{t.s7Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.gapCols]}
              templateColumns="1.4fr 3fr"
              rows={eventsGapRows[lang].map((g) => [g.what, g.where])}
            />
          </RevealItem>

          <RevealItem>
            <Cite
              path="decisions/README.md"
              label="decisions/README.md — ADR-152 row: Proposed, implemented, 35/35 checks passing, pending sign-off"
            />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
