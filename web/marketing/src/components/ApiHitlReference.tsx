import {
  agentAskHitlSnippet,
  approvalExampleSnippet,
  gh,
  toolCallHookExampleSnippet,
  workflowChatClientHitlSnippet,
  a2aStreamProjectorInterruptSnippet,
} from "../data/apiContent";
import { magenticPlanSignoffSnippet } from "../data/builderContent";
import { REPO_URL, SITE_BASE } from "../data/content";
import { workflowCheckpointResumeSnippet } from "../data/durabilityContent";
import {
  aguiInterruptMultiCaseSnippet,
  hitlExampleRows,
  hitlMatrixRows,
  interactionRecordSnippet,
  requestPortRawSnippet,
  skillSandboxNoGateSnippet,
  workflowChatClientSessionLoopSnippet,
} from "../data/hitlContent";
import { useLang } from "../i18n/LanguageContext";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiDiagnosticNote } from "./ApiDiagnosticNote";
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
    eyebrow: "One Interaction record, six real surfaces",
    headingPrefix: "Human-in-the-loop:",
    headingHighlight: "one primitive, six places it shows up",
    intro: (
      <>
        AgentEngine has exactly one internal notion of "this run is genuinely waiting on someone
        or something outside itself": a real, checkpointable{" "}
        <code>Interaction{"{interaction_id, run_id, reason, opened_at_ns, expires_at_ns}"}</code>{" "}
        record, tagged by a five-value <code>interaction_reason</code> enum. Every mechanism on
        this page — tool approval, an external dispatch hook, <code>agent.ask()</code>, a workflow{" "}
        <code>request_port</code>, a whole workflow projected through <code>chat_stream()</code>,
        a Magentic plan review — opens one of these, and resolves through one of exactly two calls:{" "}
        <code>AgentSession::resolve_interaction()</code> or{" "}
        <code>WorkflowSupervisor::resume_workflow()</code>. This page is the map between them, not
        a seventh copy of material that already has a real home — each section below links out to
        the page that owns its depth.
      </>
    ),

    matrixEyebrow: "core/interaction.hpp — the shared vocabulary",
    matrixHeading: "The six surfaces, compared",
    matrixBody: (
      <>
        <code>workflow_supervisor.hpp</code> reuses the same <code>agentengine::Interaction</code>{" "}
        type that <code>AgentSession</code> does — not a workflow-specific lookalike. Two distinct
        resume calls exist because two distinct execution models sit underneath: a session's turn
        loop, and a workflow's superstep engine. The concept itself doesn't differ.
      </>
    ),
    matrixCols: ["Mechanism", "Layer", "interaction_reason / encoding", "Resumed via", "Depth"],

    approvalEyebrow: "ADR-029 — the base case",
    approvalHeading: "Tool approval: a synchronous decider, or a genuine suspend",
    approvalBody: (
      <>
        A tool declared <code>Approval&lt;approval_mode::always_require&gt;</code> is answered one
        of two ways. If a synchronous <code>approval_decider_</code> is configured, it answers
        in-process — no suspend at all. Otherwise, with{" "}
        <code>set_suspend_for_approval(true)</code> set, the whole <code>StartRun</code> ask
        genuinely stops. It doesn't hang, and it doesn't fabricate an answer: a real{" "}
        <code>Interaction{"{reason == approval}"}</code> opens, and{" "}
        <code>resolve_interaction({"{id, approved}"})</code> resumes the same run later, never a
        new one.
      </>
    ),
    approvalNote: <>I4 — every effect stays attributable to the run that produced it</>,

    hookEyebrow: "OQ-21 — a different question, the same shape",
    hookHeading: "ToolCallHook: should an external process decide before a human ever sees it?",
    hookBody: (
      <>
        <code>set_tool_call_hook()</code> runs once per call, immediately before the approval
        check above. It can deny the call, rewrite its arguments, or set{" "}
        <code>needs_external_dispatch</code> to hand the decision to an outside process without
        blocking inline. That suspension goes through the same machinery as approval, but under a
        distinct reason, <code>interaction_reason::hook_decision</code> — so an external process's
        dispatch answer can never stand in for a human's approval decision. Resolving one
        re-checks approval need against the real deciders for every remaining call.
      </>
    ),

    codeactEyebrow: "ADR-057 — abort-and-replay",
    codeactHeading: "agent.ask(): a script inside execute_code stops and asks",
    codeactBody: (
      <>
        Calling <code>agent.ask(prompt)</code> from Python running inside <code>execute_code</code>{" "}
        suspends the whole call. A real <code>Interaction{"{reason == codeact_ask}"}</code> opens,
        and <code>resolve_interaction()</code> replays the stored script from its own start once
        an answer arrives. Side effects that already ran before the ask are not undone. That's why
        this design is called abort-and-replay rather than pause-and-continue: the single-worker
        runtime substrate this engine has can build the former, not the latter.
      </>
    ),

    workflowEyebrow: "014 §4 — the graph-native case",
    workflowHeading: "request_port: a workflow node that suspends the whole graph",
    workflowBody: (
      <>
        A request port is an executor that emits <code>InputRequired</code> and suspends the
        workflow until a response arrives, holding no resources while it waits. It's the same
        underlying mechanism as tool approval and A2A's <code>INPUT_REQUIRED</code>, just at the
        graph layer instead of the session layer.{" "}
        <code>resume_workflow({"{interaction_id, response, routes}"})</code> resumes the same run
        from its checkpoint, and <code>routes</code> lets the response steer a pending{" "}
        <code>switch_case</code>/<code>multi_selection</code> decision.
      </>
    ),

    chatClientEyebrow: "GitHub issue #35 (ADR-162/163) — the same suspension, a different API shape",
    chatClientHeading: "WorkflowChatClient: what request_port looks like from outside, through chat_stream()",
    chatClientBody: (
      <>
        Wrap a whole workflow so it satisfies this codebase's <code>ChatClient</code> concept, and
        the same <code>request_port</code> suspension above no longer surfaces as an{" "}
        <code>Interaction</code> record. It arrives instead as a <code>Custom</code>-typed{" "}
        <code>ChatResponseUpdate</code> from <code>chat_stream()</code>, never a{" "}
        <code>ToolCall</code>. This is the literal intersection of chat_client, workflow, and
        streaming: the adapter honestly reports{" "}
        <code>capabilities().streaming == false</code>, since it never streams token-level deltas
        from inside the wrapped graph. Named gap, not silently papered over:{" "}
        <strong>an outer <code>AgentSession</code> bound to this adapter sees the ask, but nothing
        answers it from that position today</strong>. The ask reaches the outer session's own
        caller as an ordinary final answer — proven safe, not corrupted — but that caller has no
        reference to the specific <code>WorkflowChatClient</code> instance underneath, so it can't
        drive the resume call this page's own examples show. Composing{" "}
        <code>WorkflowChatClient</code> as a sub-agent inside another agent's tool set — exactly the
        MAF-parity use case that motivated building it — is not yet reachable; only a direct caller
        can currently answer a paused interaction.
      </>
    ),
    chatClientNote: (
      <>
        A naive <code>ToolCall</code> encoding was tried first and traced to a silent corruption
        of the paused interaction — full account on the Workflow page
      </>
    ),
    chatClientGapNote: (
      <>
        answer-routing through an intermediate AgentSession is unbuilt — tracking issue{" "}
        <a href={`${REPO_URL}/issues/44`} target="_blank" rel="noreferrer">
          #44
        </a>
      </>
    ),

    sessionLoopEyebrow: "examples/31_workflow_chat_client_session_loop.cpp",
    sessionLoopHeading: "Driven as a session, not two hardcoded calls",
    sessionLoopBody: (
      <>
        The snippet above shows the mechanism with a single ask. A real caller doesn't know in
        advance how many asks a wrapped workflow will raise, so it needs a loop: keep calling{" "}
        <code>chat_stream()</code> with the growing history, answer whatever ask arrives, and stop
        when a plain final answer replaces the ask. This example's workflow has two chained{" "}
        <code>request_port</code> nodes to prove the loop actually loops, not just tolerates being
        called twice.
      </>
    ),
    sessionLoopNote: (
      <>bounded at a fixed round count, never a literal unbounded while(true) — the same "bounded, single-resume() loop" convention the Builder API page documents</>
    ),

    magenticEyebrow: "ADR-149, GitHub issue #28 item 3 — typed, not free-text",
    magenticHeading: "Magentic plan sign-off: a request_port with a schema",
    magenticBody: (
      <>
        <code>.require_plan_signoff(port_id)</code> wires a <code>request_port</code> with the
        same <code>TypedExecutor&lt;TaskMsg, ReportMsg&gt;</code> shape as a participant —
        structurally, it's one thing from the graph's own point of view. The manager sends a typed{" "}
        <code>MagenticPlanSignoffRequest{"{plan}"}</code>, never free text a reviewer has to
        parse, and a host resumes with a typed{" "}
        <code>MagenticPlanSignoffResponse{"{approved, feedback}"}</code>.{" "}
        <code>approved == false</code> isn't a dead end — it's the manager's next input, enabling
        a revise-and-resubmit loop.
      </>
    ),

    durabilityEyebrow: "019 — surviving what tool approval and request_port don't promise on their own",
    durabilityHeading: "None of the above assumes the process stays up",
    durabilityBody: (
      <>
        Every open <code>Interaction</code> is checkpoint content. A suspended run — approval,{" "}
        <code>codeact_ask</code>, <code>hook_decision</code>, or a workflow's own{" "}
        <code>request_port</code> — can be resumed after a genuine process restart, not just later
        in the same process. One disclosed gap: <code>pending_codeact_asks_</code>, the stored
        script an <code>agent.ask()</code> replay needs, is not durably checkpointed today.
        Resolving a <code>codeact_ask</code> interaction that survived a restart fails closed with
        a named error code rather than silently guessing.
      </>
    ),
    durabilityNote: <>019 §1</>,

    protocolsEyebrow: "AG-UI & A2A — the same suspension, on the wire",
    protocolsHeading: "How this looks to an external protocol client",
    protocolsBody: (
      <>
        AG-UI has no native pause event: an <code>input_required</code> <code>RunEvent</code>{" "}
        forces it to end the run with a <code>RunFinishedInterrupt</code> instead.{" "}
        <code>approval_requested</code> projects onto AG-UI's own <code>confirmation</code> reason.
        A2A is closer — it has a real, non-terminal <code>TASK_STATE_INPUT_REQUIRED</code>, and the
        same internal <code>input_required</code> <code>RunEvent</code> projects onto it directly.
        Neither side fabricates an intermediate state.
      </>
    ),

    skillSandboxEyebrow: "Named honestly: what does not gate on a human here",
    skillSandboxHeading: "Skill mounting and sandboxed execution are not HITL surfaces",
    skillSandboxBody: (
      <>
        Mounting a skill is a pure contract check: it verifies the name against known mount roots
        and records the mount. There is no <code>Approval&lt;&gt;</code>, no <code>Interaction</code>,
        no <code>agent.ask()</code> gate anywhere in the mount/unmount path — the CLI's own
        comments state plainly that <code>MountSkillTool</code> and <code>ExecuteCodeTool</code>{" "}
        declare no approval policy at all. The one indirect tie: a skill document can teach the
        model about <code>agent.ask</code> as a tool worth reaching for. The skill mechanism
        itself gates nothing.
      </>
    ),
    skillSandboxNote: <>mount_skill, tools/cli_chat.cpp</>,
    skillSandboxBody2: (
      <>
        Sandboxed execution follows the same pattern. Every tool, sandboxed or not, can declare{" "}
        <code>Approval&lt;always_require&gt;</code> via the ordinary, generic <code>Tool&lt;&gt;</code>{" "}
        trait — running inside a jail doesn't change that mechanism. But no distinct,
        sandbox-specific human-review mechanism exists: <code>read_sandbox_file.hpp</code> is the
        one sandbox-touching tool in this codebase that mentions the trait, and it explicitly opts
        out with <code>Approval&lt;approval_mode::never_require&gt;</code>. "Suspend" inside the
        sandbox backends themselves means OS-level job-object suspension or ordinary coroutine
        parking, unrelated to any human decision.
      </>
    ),

    examplesEyebrow: "Runnable, not just described",
    examplesHeading: "Real examples, by mechanism",
    examplesBody: (
      <>
        Every row below is either a runnable, self-checking <code>examples/*.cpp</code> program
        (built + <code>ctest</code>-registered) or, honestly, test-only coverage with no
        standalone example yet.
      </>
    ),
    exampleCols: ["File", "What it proves", "Status"],

    choosingEyebrow: "A decision, not a survey",
    choosingHeading: "Which mechanism do I actually want?",
    choosingBody: (
      <>
        <strong>A single tool call needs a human's yes/no before it runs:</strong> tool approval.{" "}
        <strong>The decision should be made by another system, not a person, without blocking:</strong>{" "}
        <code>ToolCallHook</code> with <code>needs_external_dispatch</code>.{" "}
        <strong>A script running inside <code>execute_code</code> needs one piece of information
        mid-run:</strong> <code>agent.ask()</code>. <strong>A point in a workflow graph needs to
        pause for input, review, or escalation:</strong> a <code>request_port</code> node — typed,
        via Magentic's <code>require_plan_signoff()</code>, if the payload has a real schema.{" "}
        <strong>The workflow itself needs to be reusable as an ordinary model backend, and a
        direct caller answers its paused interactions:</strong>{" "}
        <code>WorkflowChatClient</code>. It also surfaces asks safely when bound as an outer{" "}
        <code>AgentSession</code>'s own backend, but that outer session's own caller can't yet
        answer them from there (issue #44). None of these are mutually exclusive — a real
        deployment composes several.
      </>
    ),
  },
  vi: {
    eyebrow: "Một bản ghi Interaction, sáu bề mặt thật",
    headingPrefix: "Human-in-the-loop:",
    headingHighlight: "một nguyên thủy, sáu nơi nó xuất hiện",
    intro: (
      <>
        AgentEngine có đúng một khái niệm nội bộ cho "lần chạy này thực sự đang chờ ai đó hoặc thứ
        gì đó bên ngoài chính nó": một bản ghi{" "}
        <code>Interaction{"{interaction_id, run_id, reason, opened_at_ns, expires_at_ns}"}</code>{" "}
        thật, có thể checkpoint, được gắn thẻ bởi một enum <code>interaction_reason</code> năm giá
        trị. Mọi cơ chế trên trang này — phê duyệt tool, một hook điều phối ra ngoài,{" "}
        <code>agent.ask()</code>, một node <code>request_port</code> của workflow, cả một workflow
        được chiếu qua <code>chat_stream()</code>, một lần duyệt kế hoạch Magentic — đều mở một
        trong số này, và giải quyết qua đúng một trong hai lệnh gọi:{" "}
        <code>AgentSession::resolve_interaction()</code> hoặc{" "}
        <code>WorkflowSupervisor::resume_workflow()</code>. Trang này là bản đồ giữa chúng, không
        phải một bản sao thứ bảy của nội dung đã có sẵn nơi ở thật — mỗi mục bên dưới liên kết ra
        trang sở hữu phần sâu của nó.
      </>
    ),

    matrixEyebrow: "core/interaction.hpp — từ vựng dùng chung",
    matrixHeading: "Sáu bề mặt, so sánh",
    matrixBody: (
      <>
        <code>workflow_supervisor.hpp</code> tái sử dụng đúng cùng kiểu{" "}
        <code>agentengine::Interaction</code> mà <code>AgentSession</code> dùng — không phải một
        kiểu giống-nhưng-khác riêng cho workflow. Có hai lệnh gọi resume khác nhau vì có hai mô
        hình thực thi khác nhau bên dưới: vòng lặp lượt của một session, và superstep engine của
        một workflow. Bản thân khái niệm không hề khác nhau.
      </>
    ),
    matrixCols: ["Cơ chế", "Tầng", "interaction_reason / mã hóa", "Resume qua", "Độ sâu"],

    approvalEyebrow: "ADR-029 — trường hợp cơ bản",
    approvalHeading: "Phê duyệt tool: một decider đồng bộ, hoặc một sự đình chỉ thật sự",
    approvalBody: (
      <>
        Một tool được khai báo <code>Approval&lt;approval_mode::always_require&gt;</code> được trả
        lời theo một trong hai cách. Nếu một <code>approval_decider_</code> đồng bộ được cấu hình,
        nó trả lời trong tiến trình — không đình chỉ gì cả. Nếu không, với{" "}
        <code>set_suspend_for_approval(true)</code> được thiết lập, toàn bộ yêu cầu{" "}
        <code>StartRun</code> thực sự dừng lại. Nó không bị treo, và nó không bịa ra câu trả lời:
        một <code>Interaction{"{reason == approval}"}</code> thật được mở ra, và{" "}
        <code>resolve_interaction({"{id, approved}"})</code> tiếp tục chính run đó sau này, không
        bao giờ tạo run mới.
      </>
    ),
    approvalNote: <>I4 — mọi hiệu ứng luôn có thể quy về run đã tạo ra nó</>,

    hookEyebrow: "OQ-21 — một câu hỏi khác, cùng hình dạng",
    hookHeading: "ToolCallHook: nên để một tiến trình bên ngoài quyết định, trước cả khi con người thấy nó?",
    hookBody: (
      <>
        <code>set_tool_call_hook()</code> chạy một lần cho mỗi lệnh gọi, ngay trước bước kiểm tra
        approval ở trên. Nó có thể từ chối lệnh gọi, viết lại tham số, hoặc đặt{" "}
        <code>needs_external_dispatch</code> để giao quyết định cho một tiến trình bên ngoài mà
        không chặn đồng bộ. Sự đình chỉ đó đi qua cùng cơ chế với approval, nhưng dưới một lý do
        khác biệt, <code>interaction_reason::hook_decision</code> — để câu trả lời dispatch của
        một tiến trình bên ngoài không bao giờ có thể thay thế cho quyết định phê duyệt của con
        người. Giải quyết nó kiểm tra lại nhu cầu approval trước các decider thật cho mọi lệnh gọi
        còn lại.
      </>
    ),

    codeactEyebrow: "ADR-057 — abort-and-replay",
    codeactHeading: "agent.ask(): một script bên trong execute_code dừng lại và hỏi",
    codeactBody: (
      <>
        Gọi <code>agent.ask(prompt)</code> từ Python đang chạy bên trong <code>execute_code</code>{" "}
        treo lại toàn bộ lệnh gọi đó. Một <code>Interaction{"{reason == codeact_ask}"}</code> thật
        được mở ra, và <code>resolve_interaction()</code> phát lại đoạn script đã lưu từ đầu một
        khi có câu trả lời. Các side effect đã chạy trước câu hỏi không bị hoàn tác. Đó là lý do
        thiết kế này được gọi là abort-and-replay chứ không phải pause-and-continue: nền tảng
        runtime single-worker mà engine này có chỉ có thể xây được cái trước, không phải cái sau.
      </>
    ),

    workflowEyebrow: "014 §4 — trường hợp gốc-đồ-thị",
    workflowHeading: "request_port: một node workflow đình chỉ cả đồ thị",
    workflowBody: (
      <>
        Một request port là một executor phát ra <code>InputRequired</code> và đình chỉ workflow
        cho tới khi có phản hồi, không giữ tài nguyên nào trong lúc chờ. Đó là cùng cơ chế bên
        dưới với phê duyệt tool và <code>INPUT_REQUIRED</code> của A2A, chỉ khác ở tầng đồ thị thay
        vì tầng session. <code>resume_workflow({"{interaction_id, response, routes}"})</code> tiếp
        tục chính run đó từ checkpoint của nó, và <code>routes</code> cho phép phản hồi định hướng
        một quyết định <code>switch_case</code>/<code>multi_selection</code> đang chờ.
      </>
    ),

    chatClientEyebrow: "GitHub issue #35 (ADR-162/163) — cùng sự đình chỉ, một hình dạng API khác",
    chatClientHeading: "WorkflowChatClient: request_port trông ra sao từ bên ngoài, qua chat_stream()",
    chatClientBody: (
      <>
        Bọc cả một workflow để nó thỏa mãn khái niệm <code>ChatClient</code> của codebase này, và
        cùng một sự đình chỉ <code>request_port</code> ở trên không còn hiện ra như một bản ghi{" "}
        <code>Interaction</code> nữa. Nó đến dưới dạng một <code>ChatResponseUpdate</code> kiểu{" "}
        <code>Custom</code> từ <code>chat_stream()</code>, không bao giờ là <code>ToolCall</code>.
        Đây chính là giao điểm của chat_client, workflow, và streaming: adapter báo cáo trung thực{" "}
        <code>capabilities().streaming == false</code>, vì nó không bao giờ stream delta cấp token
        từ bên trong đồ thị được bọc. Một khoảng trống được nêu thẳng, không giấu đi:{" "}
        <strong>một <code>AgentSession</code> bên ngoài gắn với adapter này thấy được câu hỏi,
        nhưng hiện tại không có gì trả lời được nó từ vị trí đó</strong>. Câu hỏi đến tay caller
        của session bên ngoài như một câu trả lời cuối cùng bình thường — an toàn, không bị hỏng —
        nhưng caller đó không có tham chiếu tới đúng instance <code>WorkflowChatClient</code> bên
        dưới, nên không thể thực hiện lệnh gọi resume mà các ví dụ trên trang này minh họa. Việc
        ghép <code>WorkflowChatClient</code> làm sub-agent bên trong tập tool của một agent khác —
        chính use case ngang tầm MAF đã thúc đẩy việc xây dựng nó — hiện chưa thể đạt tới; chỉ một
        caller trực tiếp mới trả lời được một interaction đang tạm dừng.
      </>
    ),
    chatClientNote: (
      <>
        Một mã hóa <code>ToolCall</code> ngây thơ đã được thử trước tiên và truy vết ra một sự làm
        hỏng âm thầm của sự đình chỉ đang treo — xem đầy đủ trên trang Workflow
      </>
    ),
    chatClientGapNote: (
      <>
        định tuyến câu trả lời qua một AgentSession trung gian chưa được xây dựng — issue theo dõi{" "}
        <a href={`${REPO_URL}/issues/44`} target="_blank" rel="noreferrer">
          #44
        </a>
      </>
    ),

    sessionLoopEyebrow: "examples/31_workflow_chat_client_session_loop.cpp",
    sessionLoopHeading: "Chạy như một phiên, không phải hai lệnh gọi cứng",
    sessionLoopBody: (
      <>
        Đoạn code ở trên minh họa cơ chế với đúng một lần hỏi. Một caller thật không biết trước một
        workflow được bọc sẽ đặt ra bao nhiêu câu hỏi, nên nó cần một vòng lặp: tiếp tục gọi{" "}
        <code>chat_stream()</code> với lịch sử ngày càng dài ra, trả lời bất kỳ câu hỏi nào xuất
        hiện, và dừng lại khi một câu trả lời cuối cùng thuần túy thay thế cho một câu hỏi. Workflow
        của ví dụ này có hai node <code>request_port</code> nối tiếp nhau để chứng minh vòng lặp
        thực sự lặp, chứ không chỉ chịu đựng được việc bị gọi hai lần.
      </>
    ),
    sessionLoopNote: (
      <>bị giới hạn ở một số vòng cố định, không bao giờ là một while(true) không giới hạn theo nghĩa đen — cùng quy ước "vòng lặp bounded, single-resume()" mà trang Builder API đã tài liệu hóa</>
    ),

    magenticEyebrow: "ADR-149, GitHub issue #28 item 3 — có kiểu, không phải văn bản tự do",
    magenticHeading: "Magentic plan sign-off: một request_port có schema",
    magenticBody: (
      <>
        <code>.require_plan_signoff(port_id)</code> nối một <code>request_port</code> với đúng
        hình dạng <code>TypedExecutor&lt;TaskMsg, ReportMsg&gt;</code> như một participant — về
        cấu trúc chỉ là một, từ góc nhìn của đồ thị. Manager gửi một{" "}
        <code>MagenticPlanSignoffRequest{"{plan}"}</code> có kiểu, không bao giờ là văn bản tự do
        mà người xem xét phải tự phân tích, và một host khôi phục bằng một{" "}
        <code>MagenticPlanSignoffResponse{"{approved, feedback}"}</code> có kiểu.{" "}
        <code>approved == false</code> không phải ngõ cụt — đó là đầu vào kế tiếp của manager, cho
        phép một vòng lặp sửa-lại-và-gửi-lại thật sự.
      </>
    ),

    durabilityEyebrow: "019 — sống sót qua những gì phê duyệt tool và request_port không tự hứa",
    durabilityHeading: "Không cái nào ở trên giả định tiến trình luôn chạy",
    durabilityBody: (
      <>
        Mọi <code>Interaction</code> đang mở đều là nội dung checkpoint. Một run bị đình chỉ —
        approval, <code>codeact_ask</code>, <code>hook_decision</code>, hoặc{" "}
        <code>request_port</code> của chính một workflow — có thể được tiếp tục sau một lần khởi
        động lại tiến trình thật sự, không chỉ sau đó trong cùng tiến trình. Một khoảng trống đã
        công bố: <code>pending_codeact_asks_</code>, script đã lưu mà một lần phát lại{" "}
        <code>agent.ask()</code> cần, không được checkpoint bền vững hôm nay. Giải quyết một
        interaction <code>codeact_ask</code> đã sống sót qua một lần khởi động lại sẽ thất bại một
        cách rõ ràng với một mã lỗi được đặt tên, thay vì âm thầm đoán.
      </>
    ),
    durabilityNote: <>019 §1</>,

    protocolsEyebrow: "AG-UI & A2A — cùng sự đình chỉ, trên wire",
    protocolsHeading: "Điều này trông ra sao với một client giao thức bên ngoài",
    protocolsBody: (
      <>
        AG-UI không có sự kiện tạm dừng gốc: một <code>RunEvent</code> kiểu{" "}
        <code>input_required</code> buộc nó phải kết thúc run bằng một{" "}
        <code>RunFinishedInterrupt</code> thay vào đó. <code>approval_requested</code> chiếu lên
        lý do <code>confirmation</code> gốc của AG-UI. A2A gần hơn: nó có một{" "}
        <code>TASK_STATE_INPUT_REQUIRED</code> thật, không kết thúc, và chính{" "}
        <code>RunEvent</code> <code>input_required</code> nội bộ đó chiếu thẳng lên nó. Không bên
        nào bịa ra một trạng thái trung gian.
      </>
    ),

    skillSandboxEyebrow: "Nêu tên trung thực: cái gì không bị kiểm soát bởi con người ở đây",
    skillSandboxHeading: "Mount skill và thực thi trong sandbox không phải là bề mặt HITL",
    skillSandboxBody: (
      <>
        Mount một skill là một phép kiểm tra hợp đồng thuần túy: nó xác minh tên so với các mount
        root đã biết và ghi nhận việc mount. Không có <code>Approval&lt;&gt;</code>, không có{" "}
        <code>Interaction</code>, không có cổng <code>agent.ask()</code> nào ở bất cứ đâu trong
        đường mount/unmount — chính chú thích của CLI nói rõ rằng{" "}
        <code>MountSkillTool</code> và <code>ExecuteCodeTool</code> không khai báo policy phê
        duyệt nào cả. Mối liên hệ gián tiếp duy nhất: một tài liệu skill có thể dạy model về{" "}
        <code>agent.ask</code> như một tool đáng dùng. Bản thân cơ chế skill không kiểm soát gì
        cả.
      </>
    ),
    skillSandboxNote: <>mount_skill, tools/cli_chat.cpp</>,
    skillSandboxBody2: (
      <>
        Thực thi trong sandbox cũng theo cùng khuôn mẫu. Mọi tool, dù chạy trong sandbox hay
        không, đều có thể khai báo <code>Approval&lt;always_require&gt;</code> qua trait{" "}
        <code>Tool&lt;&gt;</code> chung, thông thường — việc chạy bên trong một jail không thay đổi
        cơ chế đó. Nhưng không có cơ chế xem xét của con người riêng cho sandbox nào tồn tại:{" "}
        <code>read_sandbox_file.hpp</code> là tool chạm-sandbox duy nhất trong codebase này có
        nhắc tới trait đó, và nó chủ động từ chối dùng với{" "}
        <code>Approval&lt;approval_mode::never_require&gt;</code>. "Suspend" bên trong chính các
        sandbox backend nghĩa là đình chỉ job-object cấp OS hoặc coroutine parking bình thường,
        không liên quan gì tới một quyết định của con người.
      </>
    ),

    examplesEyebrow: "Chạy được, không chỉ được mô tả",
    examplesHeading: "Ví dụ thật, theo từng cơ chế",
    examplesBody: (
      <>
        Mỗi dòng bên dưới hoặc là một chương trình <code>examples/*.cpp</code> chạy được, tự kiểm
        tra (đã build + đăng ký <code>ctest</code>), hoặc, một cách trung thực, chỉ có test bao phủ
        mà chưa có ví dụ độc lập.
      </>
    ),
    exampleCols: ["File", "Chứng minh điều gì", "Trạng thái"],

    choosingEyebrow: "Một quyết định, không phải một khảo sát",
    choosingHeading: "Tôi thực sự cần cơ chế nào?",
    choosingBody: (
      <>
        <strong>Một lệnh gọi tool đơn lẻ cần một cái gật đầu của con người trước khi chạy:</strong>{" "}
        phê duyệt tool. <strong>Quyết định nên do một hệ thống khác đưa ra, không phải một người,
        mà không chặn:</strong> <code>ToolCallHook</code> với <code>needs_external_dispatch</code>.{" "}
        <strong>Một script chạy bên trong <code>execute_code</code> cần một thông tin giữa
        chừng:</strong> <code>agent.ask()</code>. <strong>Một điểm trong một đồ thị workflow cần
        dừng lại để chờ đầu vào, xem xét, hoặc chuyển cấp:</strong> một node{" "}
        <code>request_port</code> — có kiểu, qua <code>require_plan_signoff()</code> của Magentic,
        nếu payload có một schema thật. <strong>Bản thân workflow cần dùng lại được như một
        backend model bình thường, và một caller trực tiếp trả lời các interaction đang tạm
        dừng của nó:</strong> <code>WorkflowChatClient</code>. Nó cũng phơi bày câu hỏi an toàn
        khi được gắn làm backend của một <code>AgentSession</code> bên ngoài, nhưng caller của
        session bên ngoài đó chưa thể trả lời từ vị trí đó (issue #44). Không cái nào loại trừ lẫn
        nhau — một deployment thật thường kết hợp nhiều cái.
      </>
    ),
  },
} as const;

export function ApiHitlReference() {
  const { lang } = useLang();
  const t = copy[lang];

  return (
    <section className="section" id="hitl">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 780 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        <RevealGroup>
          <RevealItem>
            <CodePanel filename="core/interaction.hpp">{highlightCpp(interactionRecordSnippet)}</CodePanel>
          </RevealItem>
        </RevealGroup>

        {/* ---- The comparison matrix -------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-matrix">
              <span className="eyebrow">{t.matrixEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.matrixHeading}</h3>
              <p>{t.matrixBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.matrixCols]}
              templateColumns="1.1fr 1.3fr 1.6fr 1.6fr 0.9fr"
              rows={hitlMatrixRows[lang].map((r) => [
                <strong key="m">{r.mechanism}</strong>,
                r.layer,
                <code key="r" style={{ fontSize: "0.85em" }}>{r.reason}</code>,
                <code key="c" style={{ fontSize: "0.85em" }}>{r.resumeCall}</code>,
                r.page,
              ])}
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- Tool approval ------------------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-approval">
              <span className="eyebrow">{t.approvalEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.approvalHeading}</h3>
              <p>{t.approvalBody}</p>
              <ApiDiagnosticNote>{t.approvalNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="examples/05_human_approval.cpp">{highlightCpp(approvalExampleSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <a className="api-cite" href={`${SITE_BASE}/api/runtime.html#suspend-for-approval`} style={{ borderTop: "none", paddingTop: 0, display: "block" }}>
              AgentSession &amp; ChatClient — Pausing a whole run for a real human →
            </a>
          </RevealItem>
        </RevealGroup>

        {/* ---- ToolCallHook -------------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-hook">
              <span className="eyebrow">{t.hookEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.hookHeading}</h3>
              <p>{t.hookBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="tests/test_rt_agent_session_tool_call_hook.cpp">{highlightCpp(toolCallHookExampleSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <a className="api-cite" href={`${SITE_BASE}/api/runtime.html#tool-call-hook`} style={{ borderTop: "none", paddingTop: 0, display: "block" }}>
              AgentSession &amp; ChatClient — ToolCallHook →
            </a>
          </RevealItem>
        </RevealGroup>

        {/* ---- agent.ask ------------------------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-codeact-ask">
              <span className="eyebrow">{t.codeactEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.codeactHeading}</h3>
              <p>{t.codeactBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="agent_ask_codegen.hpp + agent_session.hpp">{highlightCpp(agentAskHitlSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <a className="api-cite" href={`${SITE_BASE}/api/codeact.html#codeact-agent-ask`} style={{ borderTop: "none", paddingTop: 0, display: "block" }}>
              CodeAct — agent.ask →
            </a>
          </RevealItem>
        </RevealGroup>

        {/* ---- request_port ------------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-workflow">
              <span className="eyebrow">{t.workflowEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.workflowHeading}</h3>
              <p>{t.workflowBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="examples/23_handoff_mesh.cpp">{highlightCpp(requestPortRawSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <a className="api-cite" href={`${SITE_BASE}/api/workflow.html#workflow-hitl`} style={{ borderTop: "none", paddingTop: 0, display: "block" }}>
              Workflow &amp; Orchestration — request_port →
            </a>
          </RevealItem>
        </RevealGroup>

        {/* ---- WorkflowChatClient ------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-chat-client">
              <span className="eyebrow">{t.chatClientEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.chatClientHeading}</h3>
              <p>{t.chatClientBody}</p>
              <ApiDiagnosticNote kind="see also">{t.chatClientNote}</ApiDiagnosticNote>
              <ApiDiagnosticNote status="stub" kind="gap">{t.chatClientGapNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="rt/workflow_as_chat_client.hpp">{highlightCpp(workflowChatClientHitlSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <div style={{ display: "flex", gap: 16, flexWrap: "wrap" }}>
              <a className="api-cite" href={`${SITE_BASE}/api/workflow.html#workflow-hitl-chat-client`} style={{ borderTop: "none", paddingTop: 0 }}>
                Workflow &amp; Orchestration — HITL over chat_client →
              </a>
              <a className="api-cite" href={`${SITE_BASE}/api/streaming.html#session-streaming`} style={{ borderTop: "none", paddingTop: 0 }}>
                Streaming — session streaming →
              </a>
            </div>
          </RevealItem>
        </RevealGroup>

        {/* ---- WorkflowChatClient driven as a real session loop --------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-chat-client-session-loop">
              <span className="eyebrow">{t.sessionLoopEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.sessionLoopHeading}</h3>
              <p>{t.sessionLoopBody}</p>
              <ApiDiagnosticNote>{t.sessionLoopNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="examples/31_workflow_chat_client_session_loop.cpp">
              {highlightCpp(workflowChatClientSessionLoopSnippet)}
            </CodePanel>
          </RevealItem>
          <RevealItem>
            <a className="api-cite" href={gh("examples/31_workflow_chat_client_session_loop.cpp")} target="_blank" rel="noreferrer" style={{ borderTop: "none", paddingTop: 0, display: "block" }}>
              examples/31_workflow_chat_client_session_loop.cpp →
            </a>
          </RevealItem>
        </RevealGroup>

        {/* ---- Magentic plan sign-off ----------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-magentic">
              <span className="eyebrow">{t.magenticEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.magenticHeading}</h3>
              <p>{t.magenticBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="workflow/magentic.hpp">{highlightCpp(magenticPlanSignoffSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <a className="api-cite" href={`${SITE_BASE}/api/builder.html#magentic-workflow-builder`} style={{ borderTop: "none", paddingTop: 0, display: "block" }}>
              Builder API — MagenticWorkflowBuilder →
            </a>
          </RevealItem>
        </RevealGroup>

        {/* ---- Durability -------------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-durability">
              <span className="eyebrow">{t.durabilityEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.durabilityHeading}</h3>
              <p>{t.durabilityBody}</p>
              <ApiDiagnosticNote>{t.durabilityNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="examples/20_workflow_checkpoint_resume.cpp">{highlightCpp(workflowCheckpointResumeSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <a className="api-cite" href={`${SITE_BASE}/api/durability.html#du-interactions`} style={{ borderTop: "none", paddingTop: 0, display: "block" }}>
              Durability — Interactions across a restart →
            </a>
          </RevealItem>
        </RevealGroup>

        {/* ---- Protocols --------------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-protocols">
              <span className="eyebrow">{t.protocolsEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.protocolsHeading}</h3>
              <p>{t.protocolsBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <p style={{ fontWeight: 600, marginBottom: 8 }}>AG-UI — three reasons, one wire shape</p>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="tests/test_rt_agui_projection.cpp">{highlightCpp(aguiInterruptMultiCaseSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <p style={{ fontWeight: 600, margin: "20px 0 8px" }}>A2A — a real, non-terminal state instead</p>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="tests/test_a2a_streaming.cpp">{highlightCpp(a2aStreamProjectorInterruptSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <a className="api-cite" href={`${SITE_BASE}/api/protocols.html#protocol-wire-projection`} style={{ borderTop: "none", paddingTop: 0, display: "block" }}>
              Protocol surfaces — wire projection →
            </a>
          </RevealItem>
        </RevealGroup>

        {/* ---- Skill & Sandbox: honest absence ------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-skill-sandbox">
              <span className="eyebrow">{t.skillSandboxEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.skillSandboxHeading}</h3>
              <p>{t.skillSandboxBody}</p>
              <ApiDiagnosticNote>{t.skillSandboxNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>
          <RevealItem>
            <p style={{ color: "var(--text-dim)", lineHeight: 1.65 }}>{t.skillSandboxBody2}</p>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="cli_chat.cpp + read_sandbox_file.hpp">{highlightCpp(skillSandboxNoGateSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <div style={{ display: "flex", gap: 16, flexWrap: "wrap", marginTop: 8 }}>
              <a className="api-cite" href={gh("tools/cli_chat.cpp")} target="_blank" rel="noreferrer" style={{ borderTop: "none", paddingTop: 0 }}>
                tools/cli_chat.cpp
              </a>
              <a className="api-cite" href={gh("include/agentengine/tools/read_sandbox_file.hpp")} target="_blank" rel="noreferrer" style={{ borderTop: "none", paddingTop: 0 }}>
                include/agentengine/tools/read_sandbox_file.hpp
              </a>
              <a className="api-cite" href={`${SITE_BASE}/api/skill.html`} style={{ borderTop: "none", paddingTop: 0 }}>
                Skill →
              </a>
              <a className="api-cite" href={`${SITE_BASE}/api/trust-sandbox.html`} style={{ borderTop: "none", paddingTop: 0 }}>
                Capabilities &amp; Sandbox →
              </a>
            </div>
          </RevealItem>
        </RevealGroup>

        {/* ---- Real examples ------------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-examples">
              <span className="eyebrow">{t.examplesEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.examplesHeading}</h3>
              <p>{t.examplesBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.exampleCols]}
              templateColumns="1.6fr 2.2fr 1fr"
              rows={hitlExampleRows[lang].map((r) => [
                <Cite key="f" path={r.what} />,
                r.mechanism,
                r.status,
              ])}
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- Choosing ---------------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="hitl-choosing">
              <span className="eyebrow">{t.choosingEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.choosingHeading}</h3>
            </div>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 8, lineHeight: 1.75 }}>{t.choosingBody}</p>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
