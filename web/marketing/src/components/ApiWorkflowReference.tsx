import {
  gh,
  workflowEdgeKinds,
  workflowEntries,
  workflowGraphSnippet,
  workflowPatterns,
} from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiTable } from "./ApiTable";
import { type PatternId, patternHref } from "./ApiWorkflowPatternDetail";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

function StatusBadge({ status }: { status: "real" | "design" }) {
  const { lang } = useLang();
  const t = ui[lang];
  return (
    <span className={`status-badge status-${status}`}>
      {status === "real" ? t.statusRealTested : t.statusDesignedNotBuilt}
    </span>
  );
}

const copy = {
  en: {
    eyebrow: "014 — Workflow and Orchestration",
    headingPrefix: "A workflow is",
    headingHighlight: "data",
    headingSuffix: ", run round by round",
    statusBadge: "Real & tested — Milestone 6, complete",
    intro: (
      <>
        A <code>Workflow</code> is executors, edges, a start node, an output selection, and a
        termination bound — nothing here is an actor, a scheduler, or a runtime decision.{" "}
        <code>WorkflowSupervisor</code> is what actually runs one, over AgentEngine's own{" "}
        <code>agentengine::rt::</code> runtime, one superstep round at a time. 014 §3's eight named
        orchestration patterns (Sequential, Concurrent, Handoff, Router, and four more) are
        proven as configurations of the same six edge kinds below — not eight separate
        subsystems to build. See{" "}
        <code>examples/04_first_workflow.cpp</code>,{" "}
        <code>examples/09_concurrent_workflow.cpp</code>, and{" "}
        <code>examples/10_conditional_routing.cpp</code> for small, runnable programs built on
        exactly this shape.
      </>
    ),
    s1Eyebrow: "graph.hpp — edge_kind",
    s1Heading: "Six edge kinds are the whole vocabulary",
    s1Body: (
      <>
        Every one of 014 §3's eight patterns is a graph built from nothing but these six
        kinds, a termination bound, and (for <code>fan_out</code>/<code>fan_in</code>) the
        fact that the superstep engine fires every executor reachable in one round
        concurrently, not just the ones an explicit <code>fan_out</code> edge names.
      </>
    ),
    edgeTableColumns: ["edge_kind", "Meaning"],
    fanOutNote: (
      <>
        The superstep engine fires every executor reachable in one round concurrently — that's
        true even without an explicit <code>fan_out</code> edge naming them; <code>fan_out</code>{" "}
        exists so a rendered graph says what was actually authored, not because it's the only
        way concurrency happens.
      </>
    ),
    sourceExecutorSub: "one source executor",
    fanOutArrow: "fan_out — every target fires in the SAME superstep round, concurrently",
    fanInArrow: "fan_in — the aggregator runs exactly ONCE per round",
    fanInResultSub: "one ContentItem per contributing branch, in graph-declared order — never once per inbound edge",
    graphShapeEyebrow: "The graph, verbatim",
    graphShapeHeading: (
      <>
        <code>Workflow</code> / <code>Executor</code> / <code>Edge</code> — the real shape
      </>
    ),
    s2Eyebrow: "Running one",
    s2Heading: "The superstep engine, fan-in merging, routing, and worktree scoping",

    patternsEyebrow: "014 §3 — patterns, not subsystems",
    patternsHeading: "Eight named patterns, each a configuration of the same graph",
    patternsBody: (
      <>
        Every row below is built from nothing but the six edge kinds above, executors, and a
        termination bound — proven for real in <code>tests/test_rt_workflow_supervisor_patterns.cpp</code>, not
        just asserted here.
      </>
    ),
    patternsTableColumns: ["Pattern", "Graph shape", "Use case"],
    patternsGuidance: (
      <>
        <strong>Guidance.</strong> An agent-to-agent interaction with structure belongs here, not
        in a handoff chain (002 §4) — Handoff is for one hop; a graph is for a process.{" "}
        <strong>Group chat/debate vs. Planner:</strong> pick Group chat/debate when the caller
        wants to author the routing and stopping condition explicitly; pick Planner when the
        caller wants to hand over a goal and let the moderator own routing, completion, and
        recovery from a stalled round.
      </>
    ),
    patternsDetailIntro: (
      <>
        Each pattern below has its own page: the real graph, the real code a runnable example or
        test builds — trimmed for the page, not invented for it — and, for the tested ones, what
        it actually proves.
      </>
    ),
    patternsReadMore: "Read the graph & code →",

    hitlEyebrow: "014 §4 — Human-in-the-loop",
    hitlHeading: (
      <>
        <code>request_port</code>: the same suspend/resume shape as tool approval and A2A
      </>
    ),
    hitlBody: (
      <>
        A request port is an executor that emits <code>InputRequired</code> and suspends the
        workflow until a response arrives — one shape, four surfaces (013 §5): this is the same
        mechanism as <code>AgentSession</code>'s tool-approval suspend and A2A's{" "}
        <code>INPUT_REQUIRED</code>. Unlike the <code>agent</code>/<code>sub_workflow</code> kinds
        above, <code>request_port</code> shipped in Milestone 6 Phase E —{" "}
        <code>check_workflow_executable</code> runs it for real.
      </>
    ),
    hitlStep01Title: "A superstep round reaches a request_port executor",
    hitlStep01Body: "Ordinary graph traversal — the port is just another node in the round.",
    hitlStep02Title: "The workflow suspends — genuinely, holding no resources",
    hitlStep02Body: (
      <>
        A real <code>Interaction</code> opens; the workflow is checkpointed and its activations
        passivate. It resumes on the response, on a durable reminder, or never — an abandoned
        workflow is garbage-collected by policy, not leaked.
      </>
    ),
    hitlStep03Title: "Time passes, out of band",
    hitlStep03Body: "A human, a tool-approval UI, or an A2A INPUT_REQUIRED reply — whichever surface the deployment uses.",
    hitlStep04Title: (
      <>
        <code>resume_workflow(ResumeWorkflow{"{interaction_id, response, routes}"})</code>
      </>
    ),
    hitlStep04Body: (
      <>
        Resumes the SAME run from its checkpoint — <code>routes</code> lets the response steer a
        pending <code>switch_case</code>/<code>multi_selection</code> decision.
      </>
    ),
    hitlNote: (
      <>
        <strong>Concurrent ports are a set, not a singleton.</strong> Multiple request ports open
        at once in different branches produce multiple concurrent <code>Interaction</code> records
        on the same run — <code>WorkflowResult::open_interactions</code> and{" "}
        <code>unopened_ports</code> report all of them, not just one.
      </>
    ),

    checkpointEyebrow: "014 §5 — Checkpoint, resume, time-travel",
    checkpointHeading: "Rewind to any retained checkpoint — the effects don't rewind with it",
    checkpointStep01Title: "Checkpoint at every superstep boundary",
    checkpointStep01Body: (
      <>
        Executor states, in-flight deliveries, and open ports are captured as a{" "}
        <code>RunStateRecord</code>, backed by the same durable store as sessions.
      </>
    ),
    checkpointStep02Title: "Resume restores exactly, on the same node",
    checkpointStep02Body: "No multi-node cluster story exists to place a resumed run elsewhere — a real, permanent narrowing since ADR-037 removed Quark, not a renamed mechanism.",
    checkpointStep03Title: "Time-travel rewinds to ANY retained checkpoint",
    checkpointStep03Body: (
      <>
        Not just the latest one — a session's own store is single-slot, overwrite-latest by
        design, which can't serve this. <code>rewind_workflow()</code> commits the rewind in two
        phases against a durable append-log, and every rewind is audited.
      </>
    ),
    checkpointStep04Title: "Re-running forward re-executes tools",
    checkpointStep04Body: (
      <>
        Effects are NOT rewound with the state — idempotency keys (019) are what keep a re-run
        from double-charging someone.
      </>
    ),
    checkpointNote: (
      <>
        <strong>This is stated loudly on purpose.</strong> The alternative — pretending rewind is
        safe — is how time-travel becomes a footgun. Rewinding a workflow that already had
        external effects is a correctness hazard the operator must own, not the engine.
      </>
    ),
    checkpointVizNote: (
      <>
        <strong>The graph stays inspectable while this runs.</strong>{" "}
        <code>render_mermaid()</code>/<code>render_dot()</code> and <code>diff_workflows()</code>{" "}
        are total functions over any <code>Workflow</code> — never fail, never throw — and a live
        view emits one event per superstep boundary (<code>round</code>, per-executor state,
        in-flight message count) so a running workflow can be watched, not just replayed after the
        fact.
      </>
    ),

    failureEyebrow: "014 §6 — Failure",
    failureHeading: "Per-edge policy, per-round containment",
    failureBody: (
      <>
        Every <code>Edge</code> declares its own <code>EdgeFailurePolicy</code> — the graph, not a
        global default, decides what happens when an executor it points at fails:
      </>
    ),
    failFail: "fail",
    failFailMeaning: "the workflow fails",
    failPropagate: "propagate",
    failPropagateMeaning: "the failure is carried forward as data",
    failRetry: "retry",
    failRetryMeaning: "up to attempts times, only for a retryable failure_class",
    failFallback: "fallback",
    failFallbackMeaning: "control transfers to a named fallback branch",
    failBody2: (
      <>
        Underneath that policy, a throwing executor can't take the process with it:{" "}
        <code>rt::ThreadPool::submit()</code> returns a <code>JobOutcome{"{faulted, fault_ptr}"}</code>{" "}
        rather than letting the exception propagate raw, so one faulted job in a fan-out round
        never crashes the collector or hangs its siblings. A faulted job is classified — only
        <code>failure_class::transient</code> or <code>::resource</code> is retryable, everything
        else goes straight to the edge's non-retry policy — and a failed workflow's{" "}
        <strong>partial results are preserved</strong>, not discarded.
      </>
    ),

    notYetTitle: "Not yet built:",
    notYet: (
      <>
        {" "}<code>agent</code>-kind and <code>sub_workflow</code>-kind
        executors are real, validator-checked graph shapes (a graph naming one still
        validates), but <code>check_workflow_executable</code> refuses to RUN one — this
        build asks every non-port node through <code>FunctionExecutor</code>, so an{" "}
        <code>agent</code> node would silently run as a plain function otherwise. A refused
        graph is recoverable; a quietly reinterpreted one is not.
      </>
    ),
  },
  vi: {
    eyebrow: "014 — Workflow and Orchestration",
    headingPrefix: "Một workflow là",
    headingHighlight: "dữ liệu",
    headingSuffix: ", chạy theo từng vòng",
    statusBadge: "Đã có thật & kiểm thử — Milestone 6, hoàn tất",
    intro: (
      <>
        Một <code>Workflow</code> là các executor, edge, một node bắt đầu, một lựa chọn đầu
        ra, và một termination bound — không có gì ở đây là một actor, một scheduler, hay một
        quyết định lúc chạy. <code>WorkflowSupervisor</code> mới là thứ thực sự chạy nó, trên
        chính runtime <code>agentengine::rt::</code> của AgentEngine, từng vòng superstep một.
        Tám mẫu điều phối được nêu tên ở 014 §3 (Sequential, Concurrent, Handoff, Router, và
        bốn mẫu khác) được chứng minh chỉ là các cấu hình của cùng sáu loại edge bên dưới —
        không phải tám phân hệ riêng biệt cần xây dựng. Xem{" "}
        <code>examples/04_first_workflow.cpp</code>,{" "}
        <code>examples/09_concurrent_workflow.cpp</code>, và{" "}
        <code>examples/10_conditional_routing.cpp</code> để có các chương trình nhỏ, chạy
        được, xây trên đúng hình dạng này.
      </>
    ),
    s1Eyebrow: "graph.hpp — edge_kind",
    s1Heading: "Sáu loại edge là toàn bộ từ vựng",
    s1Body: (
      <>
        Mỗi một trong tám mẫu của 014 §3 đều là một đồ thị chỉ được xây từ sáu loại này, một
        termination bound, và (với <code>fan_out</code>/<code>fan_in</code>) sự thật rằng
        superstep engine kích hoạt đồng thời mọi executor chạm tới được trong một vòng, không
        chỉ những executor mà một cạnh <code>fan_out</code> tường minh nêu tên.
      </>
    ),
    edgeTableColumns: ["edge_kind", "Ý nghĩa"],
    fanOutNote: (
      <>
        Superstep engine kích hoạt đồng thời mọi executor chạm tới được trong một vòng — điều
        đó đúng ngay cả khi không có cạnh <code>fan_out</code> tường minh nào nêu tên chúng;{" "}
        <code>fan_out</code> tồn tại để một đồ thị được vẽ ra thể hiện đúng những gì thực sự
        được viết, không phải vì đó là cách duy nhất đồng thời xảy ra.
      </>
    ),
    sourceExecutorSub: "một executor nguồn",
    fanOutArrow: "fan_out — mọi đích được kích hoạt trong CÙNG một vòng superstep, đồng thời",
    fanInArrow: "fan_in — bộ tổng hợp chạy đúng MỘT LẦN mỗi vòng",
    fanInResultSub: "một ContentItem cho mỗi nhánh đóng góp, theo đúng thứ tự khai báo trong đồ thị — không bao giờ một lần cho mỗi cạnh đi vào",
    graphShapeEyebrow: "Đồ thị, nguyên văn",
    graphShapeHeading: (
      <>
        <code>Workflow</code> / <code>Executor</code> / <code>Edge</code> — hình dạng thật
      </>
    ),
    s2Eyebrow: "Chạy một workflow",
    s2Heading: "Superstep engine, gộp fan-in, định tuyến, và giới hạn phạm vi worktree",

    patternsEyebrow: "014 §3 — các mẫu, không phải các phân hệ",
    patternsHeading: "Tám mẫu được nêu tên, mỗi mẫu là một cấu hình của cùng một đồ thị",
    patternsBody: (
      <>
        Mỗi hàng bên dưới chỉ được xây từ sáu loại edge ở trên, các executor, và một
        termination bound — được chứng minh thật trong{" "}
        <code>tests/test_rt_workflow_supervisor_patterns.cpp</code>, không chỉ là khẳng định suông ở đây.
      </>
    ),
    patternsTableColumns: ["Mẫu", "Hình dạng đồ thị", "Use case"],
    patternsGuidance: (
      <>
        <strong>Hướng dẫn.</strong> Một tương tác agent-với-agent có cấu trúc thuộc về đây,
        không phải một chuỗi handoff (002 §4) — Handoff dành cho một bước chuyển; một đồ thị
        dành cho cả một quy trình. <strong>Group chat/debate và Planner:</strong> chọn Group
        chat/debate khi caller muốn tự quyết định rõ ràng cách định tuyến và điều kiện dừng;
        chọn Planner khi caller muốn giao một mục tiêu và để moderator tự chịu trách nhiệm
        định tuyến, hoàn tất, và khôi phục khi một vòng bị đình trệ.
      </>
    ),
    patternsDetailIntro: (
      <>
        Mỗi mẫu bên dưới có trang riêng: đồ thị thật, mã nguồn thật mà một ví dụ hoặc test chạy
        được xây dựng — được cắt gọn cho trang tài liệu, không phải bịa ra cho nó — và, với
        những mẫu đã được kiểm thử, điều nó thực sự chứng minh.
      </>
    ),
    patternsReadMore: "Xem đồ thị & mã nguồn →",

    hitlEyebrow: "014 §4 — Human-in-the-loop",
    hitlHeading: (
      <>
        <code>request_port</code>: cùng hình dạng suspend/resume như tool approval và A2A
      </>
    ),
    hitlBody: (
      <>
        Một request port là một executor phát ra <code>InputRequired</code> và tạm dừng
        workflow cho tới khi có phản hồi — một hình dạng, bốn bề mặt (013 §5): đây là cùng cơ
        chế với suspend chờ approval tool của <code>AgentSession</code> và{" "}
        <code>INPUT_REQUIRED</code> của A2A. Khác với các loại <code>agent</code>/
        <code>sub_workflow</code> ở trên, <code>request_port</code> đã được phát hành trong
        Milestone 6 Phase E — <code>check_workflow_executable</code> chạy nó thật sự.
      </>
    ),
    hitlStep01Title: "Một vòng superstep chạm tới một executor request_port",
    hitlStep01Body: "Duyệt đồ thị bình thường — port chỉ là một node khác trong vòng đó.",
    hitlStep02Title: "Workflow tạm dừng — thực sự, không giữ tài nguyên nào",
    hitlStep02Body: (
      <>
        Một <code>Interaction</code> thật được mở ra; workflow được checkpoint và các
        activation của nó ngừng hoạt động (passivate). Nó tiếp tục khi có phản hồi, khi có một
        durable reminder, hoặc không bao giờ — một workflow bị bỏ rơi sẽ bị garbage-collect
        theo chính sách, không bị rò rỉ.
      </>
    ),
    hitlStep03Title: "Thời gian trôi qua, ngoài luồng chính",
    hitlStep03Body: "Một con người, một giao diện approval tool, hoặc một phản hồi A2A INPUT_REQUIRED — tùy bề mặt mà deployment sử dụng.",
    hitlStep04Title: (
      <>
        <code>resume_workflow(ResumeWorkflow{"{interaction_id, response, routes}"})</code>
      </>
    ),
    hitlStep04Body: (
      <>
        Khôi phục lại CHÍNH run đó từ checkpoint của nó — <code>routes</code> cho phép phản hồi
        định hướng một quyết định <code>switch_case</code>/<code>multi_selection</code> đang
        chờ.
      </>
    ),
    hitlNote: (
      <>
        <strong>Các port đồng thời là một tập hợp, không phải một giá trị đơn.</strong> Nhiều
        request port mở cùng lúc ở các nhánh khác nhau tạo ra nhiều bản ghi{" "}
        <code>Interaction</code> đồng thời trên cùng một run —{" "}
        <code>WorkflowResult::open_interactions</code> và <code>unopened_ports</code> báo cáo
        tất cả, không chỉ một.
      </>
    ),

    checkpointEyebrow: "014 §5 — Checkpoint, resume, time-travel",
    checkpointHeading: "Tua lại về bất kỳ checkpoint nào còn giữ lại — nhưng effect thì không tua theo",
    checkpointStep01Title: "Checkpoint tại mỗi ranh giới superstep",
    checkpointStep01Body: (
      <>
        Trạng thái executor, các delivery đang bay, và các port đang mở được ghi lại thành một{" "}
        <code>RunStateRecord</code>, dùng chung kho lưu trữ bền vững với session.
      </>
    ),
    checkpointStep02Title: "Resume khôi phục chính xác, trên cùng một node",
    checkpointStep02Body: "Không có câu chuyện cluster nhiều node nào để đặt một run được resume ở nơi khác — một sự thu hẹp thật sự, vĩnh viễn kể từ khi ADR-037 loại bỏ Quark, không phải một cơ chế đổi tên.",
    checkpointStep03Title: "Time-travel tua lại về BẤT KỲ checkpoint nào còn giữ lại",
    checkpointStep03Body: (
      <>
        Không chỉ checkpoint gần nhất — kho lưu trữ của riêng session vốn chỉ có một khe, ghi
        đè lên bản mới nhất theo thiết kế, nên không thể phục vụ việc này.{" "}
        <code>rewind_workflow()</code> thực hiện việc tua lại qua hai giai đoạn trên một
        append-log bền vững, và mọi lần tua lại đều được ghi audit.
      </>
    ),
    checkpointStep04Title: "Chạy tiến trở lại sẽ thực thi lại các tool",
    checkpointStep04Body: (
      <>
        Effect KHÔNG được tua lại cùng trạng thái — các idempotency key (019) là thứ giữ cho
        việc chạy lại không tính phí ai đó hai lần.
      </>
    ),
    checkpointNote: (
      <>
        <strong>Điều này được nói rõ ràng một cách có chủ đích.</strong> Cách khác — giả vờ
        rằng việc tua lại là an toàn — chính là cách time-travel biến thành một cái bẫy. Tua
        lại một workflow đã có effect ra bên ngoài là một rủi ro về tính đúng đắn mà người vận
        hành phải tự chịu trách nhiệm, không phải engine.
      </>
    ),
    checkpointVizNote: (
      <>
        <strong>Đồ thị vẫn có thể quan sát được trong lúc này diễn ra.</strong>{" "}
        <code>render_mermaid()</code>/<code>render_dot()</code> và <code>diff_workflows()</code>{" "}
        là các hàm toàn phần (total function) trên bất kỳ <code>Workflow</code> nào — không bao
        giờ thất bại, không bao giờ throw — và một live view phát ra một sự kiện cho mỗi ranh
        giới superstep (<code>round</code>, trạng thái từng executor, số message đang bay) để
        một workflow đang chạy có thể được quan sát trực tiếp, không chỉ replay lại sau đó.
      </>
    ),

    failureEyebrow: "014 §6 — Failure",
    failureHeading: "Chính sách theo từng edge, ngăn chặn theo từng vòng",
    failureBody: (
      <>
        Mỗi <code>Edge</code> tự khai báo <code>EdgeFailurePolicy</code> của riêng nó — đồ
        thị, không phải một mặc định toàn cục, quyết định điều gì xảy ra khi executor mà nó
        trỏ tới thất bại:
      </>
    ),
    failFail: "fail",
    failFailMeaning: "workflow thất bại",
    failPropagate: "propagate",
    failPropagateMeaning: "sự thất bại được mang tiếp về phía trước như dữ liệu",
    failRetry: "retry",
    failRetryMeaning: "thử lại tối đa attempts lần, chỉ với một failure_class có thể thử lại",
    failFallback: "fallback",
    failFallbackMeaning: "quyền điều khiển chuyển sang một nhánh fallback được nêu tên",
    failBody2: (
      <>
        Bên dưới chính sách đó, một executor throw lỗi không thể kéo theo cả tiến trình:{" "}
        <code>rt::ThreadPool::submit()</code> trả về một <code>JobOutcome{"{faulted, fault_ptr}"}</code>{" "}
        thay vì để exception lan ra thô, nên một job lỗi trong một vòng fan-out không bao giờ
        làm sập bộ thu thập hay treo các job anh em của nó. Một job lỗi được phân loại — chỉ{" "}
        <code>failure_class::transient</code> hoặc <code>::resource</code> mới có thể thử
        lại, mọi loại khác đi thẳng tới chính sách không-retry của edge — và{" "}
        <strong>kết quả từng phần của một workflow thất bại vẫn được giữ lại</strong>, không
        bị loại bỏ.
      </>
    ),

    notYetTitle: "Chưa được xây dựng:",
    notYet: (
      <>
        {" "}các executor loại <code>agent</code> và <code>sub_workflow</code> là các hình
        dạng đồ thị thật, được validator kiểm tra (một đồ thị nêu tên một trong hai vẫn xác
        thực được), nhưng <code>check_workflow_executable</code> từ chối CHẠY một đồ thị như
        vậy — build này yêu cầu mọi node không phải port chạy qua{" "}
        <code>FunctionExecutor</code>, nên nếu không một node <code>agent</code> sẽ âm thầm
        chạy như một function bình thường. Một đồ thị bị từ chối vẫn có thể khắc phục; một đồ
        thị bị âm thầm diễn giải sai thì không.
      </>
    ),
  },
} as const;

export function ApiWorkflowReference() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" id="workflow">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
            {t.headingSuffix}
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {t.statusBadge}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginBottom: 22 }} id="workflow-edge-kinds">
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.edgeTableColumns]}
              templateColumns="1fr 3.2fr"
              rows={workflowEdgeKinds[lang].map((k) => [<code key="kind">{k.kind}</code>, k.meaning])}
            />
          </RevealItem>
        </RevealGroup>

        <RevealGroup style={{ marginTop: 40 }}>
          <RevealItem>
            <div className="flow glass">
              <div className="flow-node is-purple">
                <div className="flow-node-title">A</div>
                <div className="flow-node-sub">{t.sourceExecutorSub}</div>
              </div>
              <div className="flow-arrow">{t.fanOutArrow}</div>
              <div className="flow-row">
                <div className="flow-node">
                  <div className="flow-node-title">B</div>
                </div>
                <div className="flow-node">
                  <div className="flow-node-title">C</div>
                </div>
                <div className="flow-node">
                  <div className="flow-node-title">D</div>
                </div>
              </div>
              <div className="flow-arrow">{t.fanInArrow}</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">E</div>
                <div className="flow-node-sub">{t.fanInResultSub}</div>
              </div>
            </div>
            <div className="flow-loop-note" style={{ marginTop: 14 }}>{t.fanOutNote}</div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup className="anchor-target" style={{ marginTop: 48 }} id="workflow-graph-shape">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginBottom: 22 }}>
                <span className="eyebrow">{t.graphShapeEyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.graphShapeHeading}</h3>
              </div>
            </RevealItem>
            <RevealItem>
              <CodePanel filename="workflow/graph.hpp">{highlightCpp(workflowGraphSnippet)}</CodePanel>
            </RevealItem>
          </RevealGroup>
        </RevealGroup>

        <RevealGroup className="anchor-target" style={{ marginTop: 48 }} id="workflow-patterns-catalog">
          <RevealItem>
            <div className="section-head" style={{ marginBottom: 22 }}>
              <span className="eyebrow">{t.patternsEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.patternsHeading}</h3>
              <p>{t.patternsBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.patternsTableColumns]}
              templateColumns="1.1fr 1.7fr 2.2fr"
              rows={workflowPatterns[lang].map((p) => [
                <strong key="pattern">{p.pattern}</strong>,
                <code key="shape" style={{ fontSize: "0.82rem" }}>{p.shape}</code>,
                p.useCase,
              ])}
            />
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.patternsGuidance}</p>
          </RevealItem>
          <RevealItem>
            <p style={{ marginTop: 32, color: "var(--text-dim)", fontSize: "0.92rem" }}>{t.patternsDetailIntro}</p>
          </RevealItem>
        </RevealGroup>

        <RevealGroup style={{ marginTop: 24, marginBottom: 8 }}>
          <RevealItem>
            <div className="pattern-card-grid">
              {workflowPatterns[lang].map((p) => (
                <a key={p.id} href={patternHref(p.id as PatternId)} className="pattern-card glass">
                  <span className="pattern-card-name">{p.pattern}</span>
                  <code className="pattern-card-shape">{p.shape}</code>
                  <span className="pattern-card-use">{p.useCase}</span>
                  <span className="pattern-card-cta">{t.patternsReadMore}</span>
                </a>
              ))}
            </div>
          </RevealItem>
        </RevealGroup>
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="workflow-engine">
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
            </div>
          </RevealItem>
          <RevealItem>
            <div className="doc-entries">
              {workflowEntries[lang].map((e) => (
                <article className="doc-entry" id={e.id} key={e.id}>
                  <div className="doc-entry-head">
                    <code className="api-tag">{e.tag}</code>
                    <StatusBadge status={e.status} />
                  </div>
                  <h3>{e.title}</h3>
                  <p>{e.body}</p>
                  <a className="api-cite" href={e.href} target="_blank" rel="noreferrer">
                    {e.cite}
                  </a>
                </article>
              ))}
            </div>
          </RevealItem>
        </RevealGroup>

        {/* ---- Human-in-the-loop: request ports ------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="workflow-hitl">
              <span className="eyebrow">{t.hitlEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.hitlHeading}</h3>
              <p>{t.hitlBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              <div className="ladder-step">
                <span className="ladder-index">01</span>
                <div>
                  <h4>{t.hitlStep01Title}</h4>
                  <p>{t.hitlStep01Body}</p>
                </div>
              </div>
              <div className="ladder-step is-current">
                <span className="ladder-index">02</span>
                <div>
                  <h4>{t.hitlStep02Title}</h4>
                  <p>{t.hitlStep02Body}</p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">03</span>
                <div>
                  <h4>{t.hitlStep03Title}</h4>
                  <p>{t.hitlStep03Body}</p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">04</span>
                <div>
                  <h4>{t.hitlStep04Title}</h4>
                  <p>{t.hitlStep04Body}</p>
                </div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.hitlNote}</p>
          </RevealItem>

          <RevealItem>
            <a
              className="api-cite"
              href={gh("include/agentengine/rt/workflow_supervisor.hpp")}
              target="_blank"
              rel="noreferrer"
              style={{ borderTop: "none", paddingTop: 0, marginTop: 18, display: "block" }}
            >
              tests/test_rt_workflow_supervisor_request_port.cpp
            </a>
          </RevealItem>
        </RevealGroup>

        {/* ---- Checkpoint, resume, time-travel --------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="workflow-checkpoint">
              <span className="eyebrow">{t.checkpointEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.checkpointHeading}</h3>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              <div className="ladder-step">
                <span className="ladder-index">01</span>
                <div>
                  <h4>{t.checkpointStep01Title}</h4>
                  <p>{t.checkpointStep01Body}</p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">02</span>
                <div>
                  <h4>{t.checkpointStep02Title}</h4>
                  <p>{t.checkpointStep02Body}</p>
                </div>
              </div>
              <div className="ladder-step is-current">
                <span className="ladder-index">03</span>
                <div>
                  <h4>{t.checkpointStep03Title}</h4>
                  <p>{t.checkpointStep03Body}</p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">04</span>
                <div>
                  <h4>{t.checkpointStep04Title}</h4>
                  <p>{t.checkpointStep04Body}</p>
                </div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.checkpointNote}</p>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 14 }}>{t.checkpointVizNote}</p>
          </RevealItem>

          <RevealItem>
            <a
              className="api-cite"
              href={gh("include/agentengine/rt/workflow_time_travel.hpp")}
              target="_blank"
              rel="noreferrer"
              style={{ borderTop: "none", paddingTop: 0, marginTop: 18, display: "block" }}
            >
              include/agentengine/rt/workflow_time_travel.hpp
            </a>
          </RevealItem>
        </RevealGroup>

        {/* ---- Failure handling ------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="workflow-failure">
              <span className="eyebrow">{t.failureEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.failureHeading}</h3>
              <p>{t.failureBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="fail-policy-row">
              {[
                [t.failFail, t.failFailMeaning],
                [t.failPropagate, t.failPropagateMeaning],
                [t.failRetry, t.failRetryMeaning],
                [t.failFallback, t.failFallbackMeaning],
              ].map(([kind, meaning]) => (
                <div className="fail-policy-chip glass" key={kind}>
                  <code>{kind}</code>
                  <span>{meaning}</span>
                </div>
              ))}
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.failBody2}</p>
          </RevealItem>

          <RevealItem>
            <a
              className="api-cite"
              href={gh("include/agentengine/rt/thread_pool.hpp")}
              target="_blank"
              rel="noreferrer"
              style={{ borderTop: "none", paddingTop: 0, marginTop: 18, display: "block" }}
            >
              tests/test_rt_workflow_supervisor.cpp
            </a>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="gs-note" style={{ marginTop: 28, borderLeftColor: "var(--accent-pink)" }}>
              <strong>{t.notYetTitle}</strong>
              {t.notYet}
              <div style={{ marginTop: 8 }}>
                <a href={gh("include/agentengine/workflow/graph.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/workflow/graph.hpp:416-443
                </a>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
