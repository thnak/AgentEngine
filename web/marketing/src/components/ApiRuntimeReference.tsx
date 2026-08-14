import {
  approvalExampleSnippet,
  chatClientSwapSnippet,
  composedProviderExampleSnippet,
  middlewareExampleSnippet,
  runtimeConvergeStep,
  runtimeEntries,
  runtimeToolRoundStep,
  runtimeTurnLoopSteps,
  statefulToolExampleSnippet,
} from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";
import type { Lang } from "../i18n/LanguageContext";

function entryById(lang: Lang, id: string) {
  const e = runtimeEntries[lang].find((entry) => entry.id === id);
  if (!e) throw new Error(`missing runtime entry: ${id}`);
  return e;
}

function CiteLink({ id }: { id: string }) {
  const { lang } = useLang();
  const e = entryById(lang, id);
  return (
    <a
      className="api-cite"
      href={e.href}
      target="_blank"
      rel="noreferrer"
      style={{ borderTop: "none", paddingTop: 0, marginTop: 18, display: "block" }}
    >
      {e.cite}
    </a>
  );
}

const copy = {
  en: {
    eyebrow: "Agent core — L2",
    headingPrefix: "AgentSession,",
    headingHighlight: "walked through end to end",
    intro: (
      <>
        <code>AgentSession&lt;ChatClientT, StateT, HistoryProviderT&gt;</code> is the object a
        conversation actually lives on — one <code>start_run()</code> call resolves a whole
        multi-round tool conversation internally, runs on AgentEngine's own{" "}
        <code>agentengine::rt::</code> runtime, and talks to a live Anthropic or OpenAI backend
        (or a deterministic offline replay) through the exact same interface. Everything below
        walks through what actually happens, in order, with the real code shape at each step —
        not just what each piece is called.
      </>
    ),
    s1Eyebrow: "agent_session.hpp — run_rounds()",
    s1Heading: (
      <>
        What one <code>start_run()</code> call actually does
      </>
    ),
    s1Body: "A caller never drives a loop of their own — they send one message and get one answer back. Internally, AgentSession may call the model several times before it answers: once per round of tool use, until the model stops asking for tools.",
    noToolCalls: "No tool calls",
    toolCallsPresent: "Tool calls present",
    loopNote: '↻ the “tool calls present” branch loops back to step 03 — build the request again (now with tool results in history), call the model again, ask step 05 again. This is the whole loop; there is no separate “round” object, just this same cycle repeating.',
    s2Eyebrow: "context_assembly.hpp — step 03, in detail",
    s2Heading: "Context providers: everything that builds the request",
    s2Body: (
      <>
        Step 03 above isn't one function — it's an ordered list of{" "}
        <code>ContextProvider</code> conformers (history, mounted skills, memory, …), each
        contributing its own <code>{"{instructions, messages, tools}"}</code>, merged
        deterministically into one <code>ChatRequest</code>. This is AgentEngine's answer to
        Microsoft Agent Framework's <code>AIContextProvider</code>, with one deliberate
        divergence explained below.
      </>
    ),
    flowSessionCtxTitle: "SessionContext{session_id, principal, history}",
    flowSessionCtxSub: "Built once per turn, handed to every provider below — identically, not threaded through one another.",
    flowFanOut: "fan-out — every provider sees the SAME input, independently",
    flowHistorySub: "Recent conversation, oldest dropped first over budget.",
    flowSkillsSub: "One advertisement message per mounted skill.",
    flowMemorySub: "Recalled notes + a real recall(query) tool.",
    flowAssemble: "assemble_context() — declared order, per-provider token budget, drops recorded",
    flowCombinedTitle: "Combined ContextContribution",
    flowCombinedSub: "instructions concatenated · messages concatenated in provider order · tools unioned",
    flowFolds: "folds directly into",
    flowChatReqSub: "→ sent to the ChatClient (step 04)",
    s2LoopNote: "↩ after the model responds, on_turn_end(TurnView) fires on every provider with exactly this turn's new messages — the seam MemoryProvider's write path uses to extract and persist a memory item.",
    s2Note: (
      <>
        <strong>Fan-out, not a pipeline — a deliberate divergence from MAF.</strong> MAF's{" "}
        <code>AIContextProvider</code> hands provider N the ALREADY-MERGED output of provider
        N−1, so a later provider can react to an earlier one. AgentEngine's{" "}
        <code>assemble_context()</code> never does this: every provider sees only{" "}
        <code>SessionContext</code>, because this codebase has no per-message provenance stamp
        for a later provider to tell what it's reacting to — adding one was designed,
        red-teamed, and rejected (<code>OpenQuestions.md</code> OQ-18). Need a provider to react
        to another's output anyway? Write a purpose-built composite (like{" "}
        <code>HistoryAndSkillsProvider</code>) that calls its sub-providers directly — it knows
        exactly what each one produced, by construction.
      </>
    ),
    s3Eyebrow: "tool_pipeline.hpp — ADR-030",
    s3Heading: "A tool that remembers things — per session, not per process",
    s3Body: (
      <>
        An ordinary <code>Tool&lt;...&gt;</code>'s <code>invoke()</code> is a static
        function — it has no path back to the specific <code>AgentSession</code> instance
        that's calling it. <code>make_tool_descriptor_with_invoke&lt;ToolT&gt;()</code>{" "}
        fixes that: the invoke logic is a callable that closes over its own provider's
        member state.
      </>
    ),
    beforeLabel: "Before — process-wide static",
    before: (
      <>
        <code>invoke()</code> reaches a <code>static int counter</code>. Every session in
        the same process shares ONE counter — session A's tool call changes what session
        B sees.
      </>
    ),
    afterLabel: "After — session-scoped",
    after: (
      <>
        <code>invoke()</code> is a lambda capturing <code>this</code> — the provider
        instance living inside ONE <code>AgentSession</code>. Two sessions never see each
        other's counter.
      </>
    ),
    s3Note: (
      <>
        <strong>The one enforced guard:</strong> a state-capturing tool can never also be{" "}
        <code>Backgroundable</code> — <code>background_task()</code> rejects the combination
        outright. A detached background thread holding a reference into session state isn't
        synchronized against <code>fork_from()</code>/<code>clear_in_process_state()</code> —
        a real dangling-reference hazard, closed structurally rather than left as a
        documented-only rule.
      </>
    ),
    s4Eyebrow: "ADR-029",
    s4Heading: "Pausing a whole run for a real human, not a synchronous callback",
    s4Body: (
      <>
        A tool declared <code>Approval&lt;approval_mode::always_require&gt;</code> normally
        needs a synchronous decider configured on the session. Without one, and with{" "}
        <code>suspend_for_approval_</code> set, the run genuinely stops — it does not hang,
        and it does not fabricate an answer.
      </>
    ),
    step01Title: "start_run() reaches the gated tool call",
    step01Body: "Approval is always_require, and no synchronous decider is configured.",
    step02Title: "The whole ask suspends — genuinely",
    step02Body: (
      <>
        It never resolves. A real <code>Interaction</code> opens
        (<code>interaction_reason::approval</code>); an{" "}
        <code>approval_requested</code> event fires on the run's event stream.
      </>
    ),
    step03Title: "Time passes, out of band",
    step03Body: "A human actually looks at it — a CLI prompt, a web console, whatever surface a real deployment uses.",
    step04Title: 'resolve_interaction(ResolveInteraction{"{id, approved}"})',
    step04Body: (
      <>
        Resumes the SAME run — never a new <code>run_id</code> (I4).{" "}
        <code>approved=true</code> invokes the pending call for real, through the
        ordinary capability-checked pipeline; <code>approved=false</code> folds an
        ordinary tool-error denial into history.
      </>
    ),
    s5Eyebrow: "middleware.hpp — ADR-033/ADR-036",
    s5Heading: "Middleware: a real before/after chain around the model call",
    s5Body: (
      <>
        <code>Ms...</code>, in registration order, wrap step 04 above — position 0 is the
        OUTERMOST layer, matching a real nested decorator: its <code>before_model</code>{" "}
        runs first, its <code>after_model</code> runs last.
      </>
    ),
    onionM0Before: "M0.before_model — can rewrite the request, or short-circuit",
    onionM2After: "M2.after_model — sees the settled response",
    onionM1After: "M1.after_model",
    onionM0After: "M0.after_model",
    onionCore: "real backend call()",
    s5Note1: (
      <>
        <strong>A short-circuit still unwinds honestly.</strong> If M1's{" "}
        <code>before_model</code> settles the call (a synthetic response, or a denial), the
        real backend and M2 are never reached — but M0 and M1 still each get their own{" "}
        <code>after_model</code> turn on the way back out, exactly like a real{" "}
        <code>{"if (!short_circuited) inner->call(); after();"}</code> decorator would.
      </>
    ),
    s5Note2: (
      <>
        <strong>The fatal finding this mechanism had to close on its own:</strong> a
        content-rewriting middleware could forge or mutate a trusted <code>ToolCall</code>,
        bypassing ADR-023's confused-deputy gate — content-rewrite reaches the same outcome as
        capability-widening if left unchecked. <code>enforce_backend_tool_call_provenance()</code>{" "}
        forces any <code>ToolCall</code> that didn't come verbatim from the real backend down
        to <code>call_provenance::text_derived</code> before this wrapper ever returns it — a
        middleware can change what the model is asked or told, never what a tool call is
        trusted to have come from.
      </>
    ),
    s6Eyebrow: "One interface, three interchangeable backends",
    s6Heading: "AnthropicChatClient · OpenAIChatClient · ReplayChatClient",
    s6Body: (
      <>
        Every backend below satisfies the exact same <code>ChatClient</code> concept —{" "}
        <code>capabilities()</code> + <code>chat_stream()</code> — the interface every tool
        and agent is actually written against. Swap one for another, or wrap either in the
        retry/middleware layering from the previous section, and nothing else changes.
      </>
    ),
    anthropicSub: "POSTs /v1/messages · real streaming · prompt-cache TTL",
    openaiSub: "POSTs /v1/chat/completions · streams via a detached worker",
    replaySub: "Replays a recorded run deterministically, offline — the I5 seam",
  },
  vi: {
    eyebrow: "Lõi Agent — L2",
    headingPrefix: "AgentSession,",
    headingHighlight: "đi từng bước từ đầu tới cuối",
    intro: (
      <>
        <code>AgentSession&lt;ChatClientT, StateT, HistoryProviderT&gt;</code> là đối tượng mà
        một cuộc hội thoại thực sự sống trên đó — một lệnh gọi <code>start_run()</code> tự
        giải quyết một cuộc hội thoại nhiều vòng gọi tool ở bên trong, chạy trên chính runtime{" "}
        <code>agentengine::rt::</code> của AgentEngine, và nói chuyện với một backend
        Anthropic hoặc OpenAI thật (hoặc một lần phát lại ngoại tuyến tất định) thông qua đúng
        cùng một interface. Mọi thứ bên dưới đi qua chính xác những gì thực sự xảy ra, theo
        thứ tự, với hình dạng mã thật ở mỗi bước — không chỉ là tên gọi của từng phần.
      </>
    ),
    s1Eyebrow: "agent_session.hpp — run_rounds()",
    s1Heading: (
      <>
        Một lệnh gọi <code>start_run()</code> thực sự làm gì
      </>
    ),
    s1Body: "Một caller không bao giờ tự điều khiển một vòng lặp của riêng mình — họ gửi một thông điệp và nhận lại một câu trả lời. Bên trong, AgentSession có thể gọi model nhiều lần trước khi trả lời: mỗi lần cho một vòng dùng tool, cho tới khi model dừng yêu cầu tool.",
    noToolCalls: "Không có lệnh gọi tool",
    toolCallsPresent: "Có lệnh gọi tool",
    loopNote: "↻ nhánh “có lệnh gọi tool” quay lại bước 03 — xây dựng request lần nữa (giờ có thêm kết quả tool trong history), gọi model lần nữa, hỏi lại bước 05. Đây là toàn bộ vòng lặp; không có đối tượng “round” riêng biệt nào cả, chỉ là chính chu trình này lặp lại.",
    s2Eyebrow: "context_assembly.hpp — bước 03, chi tiết",
    s2Heading: "Context provider: mọi thứ xây dựng nên request",
    s2Body: (
      <>
        Bước 03 ở trên không phải một hàm duy nhất — đó là một danh sách có thứ tự các kiểu
        tuân theo <code>ContextProvider</code> (history, skill đã mount, memory, …), mỗi cái
        đóng góp <code>{"{instructions, messages, tools}"}</code> riêng của mình, được gộp
        lại một cách tất định thành một <code>ChatRequest</code> duy nhất. Đây là câu trả lời
        của AgentEngine cho <code>AIContextProvider</code> của Microsoft Agent Framework, với
        một điểm khác biệt cố ý được giải thích bên dưới.
      </>
    ),
    flowSessionCtxTitle: "SessionContext{session_id, principal, history}",
    flowSessionCtxSub: "Được xây dựng một lần mỗi lượt, trao cho mọi provider bên dưới — giống hệt nhau, không xâu chuỗi qua nhau.",
    flowFanOut: "fan-out — mọi provider nhìn thấy CÙNG một đầu vào, độc lập với nhau",
    flowHistorySub: "Cuộc hội thoại gần đây, thông điệp cũ nhất bị loại bỏ trước khi vượt ngân sách.",
    flowSkillsSub: "Một thông điệp quảng cáo cho mỗi skill đã mount.",
    flowMemorySub: "Ghi chú đã gợi nhớ + một tool recall(query) thật.",
    flowAssemble: "assemble_context() — theo thứ tự khai báo, ngân sách token theo từng provider, mọi lần loại bỏ đều được ghi lại",
    flowCombinedTitle: "ContextContribution kết hợp",
    flowCombinedSub: "instructions được nối lại · messages được nối theo thứ tự provider · tools được hợp lại",
    flowFolds: "gộp thẳng vào",
    flowChatReqSub: "→ gửi tới ChatClient (bước 04)",
    s2LoopNote: "↩ sau khi model phản hồi, on_turn_end(TurnView) được kích hoạt trên mọi provider với đúng các thông điệp mới của lượt này — đây là ranh giới mà đường ghi của MemoryProvider dùng để trích xuất và lưu lại một mục bộ nhớ.",
    s2Note: (
      <>
        <strong>Fan-out, không phải một pipeline — một khác biệt cố ý so với MAF.</strong>{" "}
        <code>AIContextProvider</code> của MAF trao cho provider N đầu ra ĐÃ-ĐƯỢC-GỘP của
        provider N−1, để một provider sau có thể phản ứng lại provider trước. {" "}
        <code>assemble_context()</code> của AgentEngine không bao giờ làm vậy: mọi provider chỉ
        nhìn thấy <code>SessionContext</code>, vì codebase này không có dấu vết nguồn gốc theo
        từng thông điệp để một provider sau biết nó đang phản ứng lại điều gì — việc thêm một
        cơ chế như vậy đã được thiết kế, red-team, và bị bác bỏ (<code>OpenQuestions.md</code>{" "}
        OQ-18). Vẫn cần một provider phản ứng lại đầu ra của provider khác? Hãy viết một
        composite chuyên biệt (như <code>HistoryAndSkillsProvider</code>) gọi trực tiếp các
        sub-provider của nó — nó biết chính xác mỗi cái tạo ra gì, ngay từ cấu trúc.
      </>
    ),
    s3Eyebrow: "tool_pipeline.hpp — ADR-030",
    s3Heading: "Một tool biết ghi nhớ — theo từng session, không theo từng tiến trình",
    s3Body: (
      <>
        <code>invoke()</code> của một <code>Tool&lt;...&gt;</code> bình thường là một hàm
        static — nó không có đường nào quay lại thực thể <code>AgentSession</code> cụ thể
        đang gọi nó. <code>make_tool_descriptor_with_invoke&lt;ToolT&gt;()</code> khắc phục
        điều đó: logic invoke là một callable đóng gói trạng thái thành viên của chính
        provider của nó.
      </>
    ),
    beforeLabel: "Trước — static toàn tiến trình",
    before: (
      <>
        <code>invoke()</code> chạm tới một <code>static int counter</code>. Mọi session
        trong cùng tiến trình dùng chung MỘT counter — lệnh gọi tool của session A thay đổi
        những gì session B nhìn thấy.
      </>
    ),
    afterLabel: "Sau — theo phạm vi session",
    after: (
      <>
        <code>invoke()</code> là một lambda capture <code>this</code> — thực thể provider
        sống bên trong MỘT <code>AgentSession</code>. Hai session không bao giờ nhìn thấy
        counter của nhau.
      </>
    ),
    s3Note: (
      <>
        <strong>Điểm bảo vệ duy nhất được thực thi:</strong> một tool có capture trạng thái
        không bao giờ được đồng thời là <code>Backgroundable</code> —{" "}
        <code>background_task()</code> thẳng thừng từ chối tổ hợp này. Một luồng nền tách
        rời giữ một tham chiếu vào trạng thái session mà không được đồng bộ hóa với{" "}
        <code>fork_from()</code>/<code>clear_in_process_state()</code> — một nguy cơ
        dangling-reference có thật, được đóng lại về mặt cấu trúc thay vì chỉ là một quy tắc
        chỉ tồn tại trong tài liệu.
      </>
    ),
    s4Eyebrow: "ADR-029",
    s4Heading: "Tạm dừng cả một run để chờ một con người thật, không phải một callback đồng bộ",
    s4Body: (
      <>
        Một tool được khai báo <code>Approval&lt;approval_mode::always_require&gt;</code>{" "}
        thông thường cần một decider đồng bộ được cấu hình trên session. Nếu không có, và với{" "}
        <code>suspend_for_approval_</code> được thiết lập, run thực sự dừng lại — nó không bị
        treo, và nó không bịa ra một câu trả lời.
      </>
    ),
    step01Title: "start_run() chạm tới lệnh gọi tool bị kiểm soát",
    step01Body: "Approval là always_require, và không có decider đồng bộ nào được cấu hình.",
    step02Title: "Toàn bộ yêu cầu tạm dừng — thực sự",
    step02Body: (
      <>
        Nó không bao giờ được giải quyết. Một <code>Interaction</code> thật được mở ra
        (<code>interaction_reason::approval</code>); một sự kiện{" "}
        <code>approval_requested</code> được kích hoạt trên luồng sự kiện của run.
      </>
    ),
    step03Title: "Thời gian trôi qua, ngoài luồng chính",
    step03Body: "Một con người thực sự xem xét nó — một prompt CLI, một web console, bất kỳ bề mặt nào một deployment thật sử dụng.",
    step04Title: 'resolve_interaction(ResolveInteraction{"{id, approved}"})',
    step04Body: (
      <>
        Khôi phục lại CHÍNH run đó — không bao giờ tạo <code>run_id</code> mới (I4).{" "}
        <code>approved=true</code> gọi thực thi thật lệnh gọi đang chờ, qua pipeline kiểm
        tra capability thông thường; <code>approved=false</code> gộp một sự từ chối như một
        lỗi tool bình thường vào history.
      </>
    ),
    s5Eyebrow: "middleware.hpp — ADR-033/ADR-036",
    s5Heading: "Middleware: một chuỗi before/after thật bao quanh lệnh gọi model",
    s5Body: (
      <>
        <code>Ms...</code>, theo thứ tự đăng ký, bọc quanh bước 04 ở trên — vị trí 0 là lớp
        NGOÀI CÙNG, giống một decorator lồng nhau thật sự: <code>before_model</code> của nó
        chạy trước tiên, <code>after_model</code> của nó chạy sau cùng.
      </>
    ),
    onionM0Before: "M0.before_model — có thể viết lại request, hoặc short-circuit",
    onionM2After: "M2.after_model — nhìn thấy phản hồi đã ổn định",
    onionM1After: "M1.after_model",
    onionM0After: "M0.after_model",
    onionCore: "lệnh gọi call() thật tới backend",
    s5Note1: (
      <>
        <strong>Một short-circuit vẫn thoát ra một cách trung thực.</strong> Nếu{" "}
        <code>before_model</code> của M1 tự giải quyết lệnh gọi (một phản hồi tổng hợp, hoặc
        một sự từ chối), backend thật và M2 không bao giờ được chạm tới — nhưng M0 và M1 vẫn
        mỗi cái đều nhận được lượt <code>after_model</code> riêng của mình trên đường quay
        ra, y hệt như một decorator{" "}
        <code>{"if (!short_circuited) inner->call(); after();"}</code> thật sẽ làm.
      </>
    ),
    s5Note2: (
      <>
        <strong>Phát hiện chí mạng mà cơ chế này phải tự đóng lại:</strong> một middleware
        viết lại nội dung có thể giả mạo hoặc thay đổi một <code>ToolCall</code> đáng tin
        cậy, vượt qua cổng confused-deputy của ADR-023 — viết lại nội dung dẫn tới cùng hậu
        quả như mở rộng capability nếu không được kiểm soát.{" "}
        <code>enforce_backend_tool_call_provenance()</code> buộc mọi{" "}
        <code>ToolCall</code> không đến nguyên văn từ backend thật phải hạ xuống thành{" "}
        <code>call_provenance::text_derived</code> trước khi wrapper này trả nó về — một
        middleware có thể thay đổi những gì model được hỏi hay được nói, nhưng không bao
        giờ thay đổi được việc một lệnh gọi tool được tin là đến từ đâu.
      </>
    ),
    s6Eyebrow: "Một interface, ba backend có thể hoán đổi cho nhau",
    s6Heading: "AnthropicChatClient · OpenAIChatClient · ReplayChatClient",
    s6Body: (
      <>
        Mỗi backend bên dưới đều thỏa mãn đúng cùng một concept <code>ChatClient</code> —{" "}
        <code>capabilities()</code> + <code>chat_stream()</code> — interface mà mọi tool và
        agent thực sự được viết dựa vào. Hoán đổi cái này sang cái khác, hoặc bọc bất kỳ cái
        nào trong lớp retry/middleware ở phần trước, và không có gì khác thay đổi.
      </>
    ),
    anthropicSub: "Gửi POST tới /v1/messages · streaming thật · hỗ trợ prompt-cache TTL",
    openaiSub: "Gửi POST tới /v1/chat/completions · streaming qua một worker tách rời",
    replaySub: "Phát lại một run đã ghi một cách tất định, ngoại tuyến — ranh giới của I5",
  },
} as const;

export function ApiRuntimeReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  return (
    <section className="section" id="runtime">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {tu.statusRealTested}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        {/* ---- 1. The turn loop --------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="agent-session" style={{ marginBottom: 22 }}>
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {runtimeTurnLoopSteps[lang].map((s) => (
                <div className="ladder-step" key={s.index}>
                  <span className="ladder-index">{s.index}</span>
                  <div>
                    <h4>{s.title}</h4>
                    <p>{s.body}</p>
                  </div>
                </div>
              ))}
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow-branch" style={{ marginTop: 18 }}>
              <div className="flow-node is-pink">
                <div className="flow-branch-label is-no">{t.noToolCalls}</div>
                <div className="flow-node-title">{runtimeConvergeStep[lang].title}</div>
                <div className="flow-node-sub">{runtimeConvergeStep[lang].body}</div>
              </div>
              <div className="flow-node is-teal">
                <div className="flow-branch-label is-yes">{t.toolCallsPresent}</div>
                <div className="flow-node-title">{runtimeToolRoundStep[lang].title}</div>
                <div className="flow-node-sub">{runtimeToolRoundStep[lang].body}</div>
              </div>
            </div>
            <div className="flow-loop-note">{t.loopNote}</div>
          </RevealItem>

          <RevealItem>
            <CiteLink id="agent-session" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. Context providers ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="context-providers" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow glass">
              <div className="flow-node is-purple">
                <div className="flow-node-title">{t.flowSessionCtxTitle}</div>
                <div className="flow-node-sub">{t.flowSessionCtxSub}</div>
              </div>
              <div className="flow-arrow">{t.flowFanOut}</div>
              <div className="flow-row">
                <div className="flow-node">
                  <div className="flow-node-title">HistoryProvider&lt;Window&lt;N&gt;&gt;</div>
                  <div className="flow-node-sub">{t.flowHistorySub}</div>
                </div>
                <div className="flow-node">
                  <div className="flow-node-title">SkillsProvider</div>
                  <div className="flow-node-sub">{t.flowSkillsSub}</div>
                </div>
                <div className="flow-node">
                  <div className="flow-node-title">MemoryProvider</div>
                  <div className="flow-node-sub">{t.flowMemorySub}</div>
                </div>
              </div>
              <div className="flow-arrow">{t.flowAssemble}</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">{t.flowCombinedTitle}</div>
                <div className="flow-node-sub">{t.flowCombinedSub}</div>
              </div>
              <div className="flow-arrow">{t.flowFolds}</div>
              <div className="flow-node is-purple">
                <div className="flow-node-title">ChatRequest{"{messages, tools}"}</div>
                <div className="flow-node-sub">{t.flowChatReqSub}</div>
              </div>
            </div>
            <div className="flow-loop-note" style={{ marginTop: 14 }}>{t.s2LoopNote}</div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="composed_context_provider.hpp">
              {highlightCpp(composedProviderExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s2Note}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="context-providers" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Session-scoped stateful tools ------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="session-scoped-stateful-tools" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">{t.beforeLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.before}</p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.afterLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.after}</p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="tool_pipeline.hpp">{highlightCpp(statefulToolExampleSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s3Note}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="session-scoped-stateful-tools" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. Suspend for approval ----------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="suspend-for-approval" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              <div className="ladder-step">
                <span className="ladder-index">01</span>
                <div>
                  <h4>{t.step01Title}</h4>
                  <p>{t.step01Body}</p>
                </div>
              </div>
              <div className="ladder-step is-current">
                <span className="ladder-index">02</span>
                <div>
                  <h4>{t.step02Title}</h4>
                  <p>{t.step02Body}</p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">03</span>
                <div>
                  <h4>{t.step03Title}</h4>
                  <p>{t.step03Body}</p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">04</span>
                <div>
                  <h4>{t.step04Title}</h4>
                  <p>{t.step04Body}</p>
                </div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/05_human_approval.cpp">
              {highlightCpp(approvalExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <CiteLink id="suspend-for-approval" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 5. Middleware chain --------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="middleware-chain" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="onion-layer glass">
              <div className="onion-label">{t.onionM0Before}</div>
              <div className="onion-layer">
                <div className="onion-label">M1.before_model</div>
                <div className="onion-layer">
                  <div className="onion-label">M2.before_model</div>
                  <div className="onion-core">{t.onionCore}</div>
                  <div className="onion-label after">{t.onionM2After}</div>
                </div>
                <div className="onion-label after">{t.onionM1After}</div>
              </div>
              <div className="onion-label after">{t.onionM0After}</div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s5Note1}</p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="model_call_gateway.hpp">{highlightCpp(middlewareExampleSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s5Note2}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="middleware-chain" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 6. Chat clients --------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="chat-clients" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s6Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
              <p>{t.s6Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow-row">
              <div className="flow-node is-purple">
                <div className="flow-node-title">AnthropicChatClient&lt;Store&gt;</div>
                <div className="flow-node-sub">{t.anthropicSub}</div>
              </div>
              <div className="flow-node is-purple">
                <div className="flow-node-title">OpenAIChatClient&lt;Store&gt;</div>
                <div className="flow-node-sub">{t.openaiSub}</div>
              </div>
              <div className="flow-node is-purple">
                <div className="flow-node-title">ReplayChatClient</div>
                <div className="flow-node-sub">{t.replaySub}</div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="agent_session.hpp">{highlightCpp(chatClientSwapSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <CiteLink id="chat-clients" />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
