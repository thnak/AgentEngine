import { gh } from "../data/apiContent";
import { SITE_BASE } from "../data/content";
import {
  bundleAskSnippet,
  bundleAskStreamSignatureSnippet,
  builderGapRows,
  magenticBuilderMethodRows,
  magenticBuilderShapeSnippet,
  magenticPlanSignoffSnippet,
  magenticWorkedExampleSnippet,
  quickstartAliasesSnippet,
  quickstartWorkedExampleSnippet,
  sessionBuilderMethodRows,
  workflowBuilderMethodRows,
  workflowBuilderWorkedExampleSnippet,
  workflowConnectStaticAssertSnippet,
} from "../data/builderContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
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
    eyebrow: "Quickstart & native authoring · session_builder.hpp · graph.hpp:590 · magentic.hpp:73",
    headingPrefix: "The surface app code",
    headingHighlight: "actually reaches for",
    intro: (
      <>
        Every other page under <code>/api/</code> documents a piece of the engine's own authoring
        surface — <a href={`${SITE_BASE}/api/agent.html`}>the CRTP <code>Agent&lt;&gt;</code> an
        agent type is written against</a>, a raw <code>AgentSession</code>, a hand-built{" "}
        <a href={`${SITE_BASE}/api/workflow.html`}><code>Workflow</code> graph</a>. This page
        documents something different: the two convenience builders an application reaches for
        when it just wants to use an agent or run a graph, without first learning that lower-level
        vocabulary. <strong>If you're building an app that uses agents, start here.</strong> If
        you're authoring a new agent type or hand-wiring a graph with fine-grained control, those
        other pages are where you actually want to be.
      </>
    ),
    introNote: (
      <>
        Two unrelated builders share this page because they solve the identical problem — chained
        calls instead of ceremony — for two different engine surfaces: sessions
        (<code>QuickstartSessionBuilder</code>) and workflow graphs (<code>WorkflowBuilder</code>/
        <code>MagenticWorkflowBuilder</code>). Neither wraps the other.
      </>
    ),

    s1Eyebrow: "core/session_builder.hpp — promoted from prototype 2026-08-22, three red-team rounds",
    s1Heading: "QuickstartSessionBuilder<Provider>: a session in a few chained calls",
    s1Body: (
      <>
        <code>QuickstartSessionBuilder&lt;Provider&gt;</code> is a compile-time-templated facade
        over <code>AgentSession</code>'s own wiring. <code>Provider</code> — a backend-vendor
        selector — is chosen once, as a template argument, not a runtime toggle:{" "}
        <code>AgentSession&lt;ChatClientT&gt;</code> is already templated on backend choice, and a
        runtime switch between two different C++ types has no single, clean <code>build()</code>{" "}
        return type. <code>OpenAiSessionBuilder</code>/<code>AnthropicSessionBuilder</code> are the
        two ready-made aliases most callers actually use.
      </>
    ),
    s1MethodCols: ["Method", "What it does"],

    s2Eyebrow: "examples/30_session_builder_quickstart_live.cpp — new, compiles, runs",
    s2Heading: "Worked example: the whole quickstart flow, end to end",
    s2Body: (
      <>
        This is the real <code>main()</code> body — trimmed only at the top (the
        skip-if-no-API-key guard) and bottom (the final pass/fail print). It skips cleanly with
        exit 0 when <code>AGENTENGINE_OPENROUTER_API_KEY</code> is unset, and succeeds against a
        real OpenRouter-hosted model when one is set. Run it via{" "}
        <code>tools/run-live-provider-tests.ps1</code>. No <code>AgentSession</code>,{" "}
        <code>ChatClientT</code>, or <code>HistoryProvider</code> vocabulary appears anywhere in
        it.
      </>
    ),
    s2ComposedNote: (
      <>
        <strong>An escape hatch exists for more advanced composition.</strong>{" "}
        <code>ComposedQuickstartSessionBuilder&lt;Provider, Store, Ms...&gt;</code> wires extra
        context/history providers into a session — a{" "}
        <code>ComposedContextProvider&lt;Ms...&gt;</code>, engaged with real, host-constructed
        provider values through a single <code>.providers(tuple, budgets)</code> call.{" "}
        <code>HistoryProviderT</code> is fixed at declaration, the same way <code>Provider</code>{" "}
        is. The class duplicates the base builder's fluent setters rather than sharing them
        through a common base — a deliberate simplification, named in the header's own top
        comment. This page gives it one paragraph; the design draft and the test file below cover
        it in full.
      </>
    ),

    s3Eyebrow: "core/session_builder.hpp:442-643 — Bundle",
    s3Heading: "Bundle::ask() and .ask_stream(): the one-shot round trip",
    s3Body: (
      <>
        <code>.build()</code> hands back a <code>result&lt;Bundle&gt;</code>. <code>Bundle</code>{" "}
        owns everything the session's <code>ChatClientT</code> and <code>CapabilitySet const*</code>{" "}
        reference for as long as it's used, and exposes <code>.session()</code> as a raw{" "}
        <code>AgentSession&amp;</code> escape hatch. <code>.ask(text)</code> is the synchronous
        one-shot: it serializes every call against itself through an internal mutex, then drives
        with a bounded, single-<code>resume()</code> loop. The plain examples use the naive{" "}
        <code>while(!done()) resume()</code> idiom instead; that idiom only stays safe when
        nothing else can contend the session's mutex, a guarantee a reusable <code>Bundle</code>{" "}
        doesn't make.
      </>
    ),
    s3StreamNote: (
      <>
        <code>.ask_stream(text)</code> is the streaming counterpart, with the same bounded-
        <code>resume()</code> contract. It runs on a background <code>std::jthread</code> pair
        instead — an outer driver plus a relay draining the session's event stream — so the caller
        can consume text live rather than waiting for the whole reply. See{" "}
        <a href={`${SITE_BASE}/api/streaming.html`}>the streaming page</a> for{" "}
        <code>stream&lt;T&gt;</code> itself; it isn't re-explained here.
      </>
    ),

    s4Eyebrow: "include/agentengine/workflow/graph.hpp:590",
    s4Heading: "WorkflowBuilder: a graph in a fluent chain, not hand-built data",
    s4Body: (
      <>
        A <code>Workflow</code> is normally authored as plain data.{" "}
        <code>examples/04_first_workflow.cpp</code>, AgentEngine's own "first workflow" walkthrough,
        constructs a two-executor <code>Workflow{"{"}...{"}"}</code> aggregate and drives it
        directly through <code>rt::WorkflowSupervisor</code> — exactly the layer{" "}
        <a href={`${SITE_BASE}/api/workflow.html`}>the Workflow reference page</a> documents.{" "}
        <code>WorkflowBuilder</code> is the shorter path to the identical graph shape: chained{" "}
        <code>.add()</code>/<code>.connect()</code> calls instead of hand-populated{" "}
        <code>executors</code>/<code>edges</code> vectors.
      </>
    ),
    s4MethodCols: ["Method", "What it does"],
    s4StaticAssertHeading: "The genuinely nice part: a compile-time type check on every edge",
    s4StaticAssertBody: (
      <>
        <code>TypedExecutor&lt;In, Out&gt;</code> carries the real C++ message types, so{" "}
        <code>.connect()</code> can reject a mismatched edge with a <code>static_assert</code> — a
        compile error naming the exact source/target type mismatch, instead of a runtime{" "}
        <code>result</code> error discovered only once the graph actually runs.
      </>
    ),
    s4CompileFailNote: (
      <>
        <code>tests/compile_fail/workflow_edge_type_mismatch.cpp</code> is a file that must not
        compile — the build itself checks that it doesn't.
      </>
    ),
    s4ValidatorNote: (
      <>
        The compile-time check is not the only one. <code>.build()</code> runs the same shared{" "}
        <code>validate_workflow()</code> the declarative YAML/JSON loader uses — a graph that
        satisfies every <code>static_assert</code> and still declares no bound is rejected too,
        with the identical <code>workflow.unbounded</code> error the declarative form would raise.
        The C++ form is never exempt from the validator just because it type-checks (I6).
      </>
    ),

    s5Eyebrow: "include/agentengine/workflow/magentic.hpp:73 — ADR-149",
    s5Heading: "MagenticWorkflowBuilder<TaskMsg, ReportMsg>: the Planner pattern, pre-wired",
    s5Body: (
      <>
        <code>MagenticWorkflowBuilder</code> is pure sugar over <code>WorkflowBuilder</code>, not a
        new engine primitive: it synthesizes the exact manager/participant cyclic graph shape{" "}
        <a href={`${SITE_BASE}/api/workflow-planner.html`}>the Planner (Magentic) pattern</a> already
        hand-builds, with <code>switch_case</code> routing out of the manager and <code>direct</code>{" "}
        edges back in. It takes two type parameters, not one. The manager reads a{" "}
        <code>ReportMsg</code> and emits a <code>TaskMsg</code>; every participant reads that{" "}
        <code>TaskMsg</code> and emits a <code>ReportMsg</code> back — reversed relative to the
        manager. That reversal is exactly what makes both directions satisfy{" "}
        <code>connect()</code>'s type-equality rule.
      </>
    ),
    s5MethodCols: ["Method", "What it does"],
    s5PlanSignoffIntro: (
      <>
        <code>.require_plan_signoff(port_id)</code> works end to end, not just wired in. The
        manager sends a <em>typed</em> request (<code>MagenticPlanSignoffRequest{"{plan}"}</code>),
        never a free-text message a reviewer has to parse. A host resumes with a typed{" "}
        <code>MagenticPlanSignoffResponse{"{approved, feedback}"}</code>, through the exact same{" "}
        <code>resume_workflow()</code> path <a href="./workflow.html#workflow-hitl">every other
        request_port</a> already uses — no new engine mechanism.
      </>
    ),
    s5PlanSignoffProofNote: (
      <>
        Proven end to end by P4 in <code>test_workflow_magentic_plan_signoff.cpp</code>.
      </>
    ),
    s5PlanSignoffNote: (
      <>
        <strong>This is a revise loop, not a fixed gate.</strong> <code>approved == false</code> is
        not a dead end: the response lands as the manager's very next input, with{" "}
        <code>feedback</code> attached, so an ordinary manager body can route straight back into
        another planning round instead of failing the whole run.
      </>
    ),

    s6Eyebrow: "examples/19_magentic_builder_live.cpp — live, against a real model",
    s6Heading: "Worked example: three executors, ten lines",
    s6Body: (
      <>
        <code>examples/17_planner_live.cpp</code> hand-builds the moderator/researcher/writer
        cycle; this example rebuilds it with <code>MagenticWorkflowBuilder</code>, proving the
        convenience layer produces a genuinely runnable graph, not just one that "looks right" as
        data. It skips cleanly without a live API key, the same as every other live example in
        this repo.
      </>
    ),

    s7Eyebrow: "Named, not hidden",
    s7Heading: "What's still rough",
    s7Body:
      "These are the disclosed gaps and footguns — named in the source itself, not silently left off this page.",
    gapCols: ["What", "Where it's named", "State"],
  },

  vi: {
    eyebrow: "Quickstart & authoring gốc · session_builder.hpp · graph.hpp:590 · magentic.hpp:73",
    headingPrefix: "Bề mặt mà mã ứng dụng",
    headingHighlight: "thực sự tìm đến",
    intro: (
      <>
        Mọi trang khác dưới <code>/api/</code> đều tài liệu hóa một phần bề mặt authoring của chính
        engine — <a href={`${SITE_BASE}/api/agent.html`}>CRTP <code>Agent&lt;&gt;</code> mà một
        kiểu agent được viết dựa trên</a>, một <code>AgentSession</code> thô, một{" "}
        <a href={`${SITE_BASE}/api/workflow.html`}>đồ thị <code>Workflow</code></a> dựng tay. Trang
        này tài liệu hóa một thứ khác: hai builder tiện dụng mà một ứng dụng tìm đến khi nó chỉ
        muốn sử dụng một agent hoặc chạy một đồ thị, mà không cần học trước lớp từ vựng cấp thấp
        đó. <strong>Nếu bạn đang xây một ứng dụng dùng agent, hãy bắt đầu ở đây.</strong> Nếu bạn
        đang viết một kiểu agent mới hay tự nối tay một đồ thị với kiểm soát chi tiết, những trang
        kia mới là nơi bạn thực sự cần tới.
      </>
    ),
    introNote: (
      <>
        Hai builder không liên quan nhau cùng nằm trên trang này vì chúng giải quyết cùng một vấn
        đề — chuỗi lệnh gọi thay vì nghi thức — cho hai bề mặt engine khác nhau: session
        (<code>QuickstartSessionBuilder</code>) và đồ thị workflow (<code>WorkflowBuilder</code>/
        <code>MagenticWorkflowBuilder</code>). Không cái nào bọc cái kia.
      </>
    ),

    s1Eyebrow: "core/session_builder.hpp — được đôn từ nguyên mẫu ngày 2026-08-22, ba vòng red-team",
    s1Heading: "QuickstartSessionBuilder<Provider>: một session trong vài lệnh gọi nối chuỗi",
    s1Body: (
      <>
        <code>QuickstartSessionBuilder&lt;Provider&gt;</code> là một facade có template tại thời
        điểm biên dịch, phủ lên chính hệ dây của <code>AgentSession</code>. <code>Provider</code>{" "}
        — bộ chọn nhà cung cấp backend — được chọn một lần, như một đối số template, không phải
        một công tắc thời gian chạy: <code>AgentSession&lt;ChatClientT&gt;</code> vốn đã có
        template theo lựa chọn backend, và một công tắc thời gian chạy giữa hai kiểu C++ khác nhau
        thì không có một kiểu trả về <code>build()</code> duy nhất, gọn gàng nào cả.{" "}
        <code>OpenAiSessionBuilder</code>/<code>AnthropicSessionBuilder</code> là hai alias dựng sẵn
        mà hầu hết bên gọi thực sự dùng.
      </>
    ),
    s1MethodCols: ["Phương thức", "Làm gì"],

    s2Eyebrow: "examples/30_session_builder_quickstart_live.cpp — mới, biên dịch được, chạy được",
    s2Heading: "Ví dụ minh họa: toàn bộ luồng quickstart, từ đầu tới cuối",
    s2Body: (
      <>
        Đây là chính thân <code>main()</code> thật — chỉ được cắt gọn ở đầu (điều kiện bỏ qua khi
        không có API key) và cuối (dòng in pass/fail cuối cùng). Nó bỏ qua sạch sẽ với exit 0 khi{" "}
        <code>AGENTENGINE_OPENROUTER_API_KEY</code> chưa được đặt, và chạy thành công với một model
        thật chạy qua OpenRouter khi biến đó được đặt. Chạy nó qua{" "}
        <code>tools/run-live-provider-tests.ps1</code>. Không một từ vựng <code>AgentSession</code>,{" "}
        <code>ChatClientT</code>, hay <code>HistoryProvider</code> nào xuất hiện ở bất kỳ đâu trong
        đó.
      </>
    ),
    s2ComposedNote: (
      <>
        <strong>Một lối thoát tồn tại cho kiểu kết hợp nâng cao hơn.</strong>{" "}
        <code>ComposedQuickstartSessionBuilder&lt;Provider, Store, Ms...&gt;</code> nối thêm các
        provider ngữ cảnh/lịch sử vào một session — một{" "}
        <code>ComposedContextProvider&lt;Ms...&gt;</code>, được kích hoạt với các giá trị provider
        thật, do host dựng, qua một lệnh gọi <code>.providers(tuple, budgets)</code> duy nhất.{" "}
        <code>HistoryProviderT</code> bị cố định ngay tại khai báo, giống hệt{" "}
        <code>Provider</code>. Lớp này nhân bản các setter kiểu chuỗi của builder gốc thay vì chia
        sẻ chúng qua một lớp cơ sở chung — một đơn giản hóa có chủ ý, được nêu tên ngay trong chú
        thích đầu file của header. Trang này chỉ dành cho nó một đoạn văn; design draft và file
        test bên dưới trình bày đầy đủ hơn.
      </>
    ),

    s3Eyebrow: "core/session_builder.hpp:442-643 — Bundle",
    s3Heading: "Bundle::ask() và .ask_stream(): vòng khứ hồi một-lần",
    s3Body: (
      <>
        <code>.build()</code> trả về một <code>result&lt;Bundle&gt;</code>. <code>Bundle</code> sở
        hữu mọi thứ mà <code>ChatClientT</code> và <code>CapabilitySet const*</code> của session
        tham chiếu tới trong suốt thời gian nó được dùng, và phơi ra <code>.session()</code> như
        một lối thoát <code>AgentSession&amp;</code> thô. <code>.ask(text)</code> là phiên bản
        đồng bộ một-lần: nó tuần tự hóa mọi lệnh gọi với chính nó qua một mutex nội bộ, rồi lái
        bằng một vòng lặp bị chặn, chỉ một <code>resume()</code>. Các ví dụ đơn giản dùng thành
        ngữ ngây thơ <code>while(!done()) resume()</code> thay vào đó; thành ngữ đó chỉ an toàn
        khi không gì khác có thể tranh chấp mutex của session — một đảm bảo mà một{" "}
        <code>Bundle</code> tái sử dụng được không có.
      </>
    ),
    s3StreamNote: (
      <>
        <code>.ask_stream(text)</code> là phiên bản streaming tương ứng, với cùng hợp đồng{" "}
        <code>resume()</code> bị chặn. Nó chạy trên một cặp <code>std::jthread</code> nền thay vào
        đó — một driver ngoài cộng một relay rút cạn luồng sự kiện của session — để bên gọi có thể
        tiêu thụ văn bản trực tiếp thay vì đợi toàn bộ câu trả lời. Xem{" "}
        <a href={`${SITE_BASE}/api/streaming.html`}>trang streaming</a> để biết về chính{" "}
        <code>stream&lt;T&gt;</code>; trang này không giải thích lại.
      </>
    ),

    s4Eyebrow: "include/agentengine/workflow/graph.hpp:590",
    s4Heading: "WorkflowBuilder: một đồ thị trong một chuỗi lệnh gọi, không phải dữ liệu dựng tay",
    s4Body: (
      <>
        Một <code>Workflow</code> bình thường được viết như dữ liệu trần.{" "}
        <code>examples/04_first_workflow.cpp</code>, bài "workflow đầu tiên" của chính AgentEngine,
        dựng một aggregate <code>Workflow{"{"}...{"}"}</code> hai-executor rồi lái nó trực tiếp
        qua <code>rt::WorkflowSupervisor</code> — đúng lớp mà{" "}
        <a href={`${SITE_BASE}/api/workflow.html`}>trang tham chiếu Workflow</a> tài liệu hóa.{" "}
        <code>WorkflowBuilder</code> là con đường ngắn hơn tới đúng hình dạng đồ thị đó: các lệnh
        gọi <code>.add()</code>/<code>.connect()</code> nối chuỗi thay vì các vector{" "}
        <code>executors</code>/<code>edges</code> điền tay.
      </>
    ),
    s4MethodCols: ["Phương thức", "Làm gì"],
    s4StaticAssertHeading: "Phần thực sự hay: kiểm tra kiểu tại thời điểm biên dịch trên mỗi cạnh",
    s4StaticAssertBody: (
      <>
        <code>TypedExecutor&lt;In, Out&gt;</code> mang kiểu thông điệp C++ thật, nên{" "}
        <code>.connect()</code> có thể từ chối một cạnh sai kiểu bằng một <code>static_assert</code>{" "}
        — một lỗi biên dịch nêu đúng tên chỗ lệch kiểu nguồn/đích, thay vì một lỗi{" "}
        <code>result</code> thời gian chạy chỉ phát hiện được khi đồ thị thực sự chạy.
      </>
    ),
    s4CompileFailNote: (
      <>
        <code>tests/compile_fail/workflow_edge_type_mismatch.cpp</code> là một file bắt buộc không
        được biên dịch — chính bản build kiểm tra điều đó.
      </>
    ),
    s4ValidatorNote: (
      <>
        Kiểm tra tại thời điểm biên dịch không phải là kiểm tra duy nhất.{" "}
        <code>.build()</code> chạy cùng <code>validate_workflow()</code> dùng chung mà bộ nạp
        YAML/JSON khai báo dùng — một đồ thị thỏa mọi <code>static_assert</code> mà vẫn không khai
        báo bound thì cũng bị từ chối, với đúng cùng lỗi <code>workflow.unbounded</code> mà dạng
        khai báo sẽ gặp. Dạng C++ không bao giờ được miễn trừ khỏi validator chỉ vì nó đúng kiểu
        (I6).
      </>
    ),

    s5Eyebrow: "include/agentengine/workflow/magentic.hpp:73 — ADR-149",
    s5Heading: "MagenticWorkflowBuilder<TaskMsg, ReportMsg>: mẫu hình Planner, nối sẵn",
    s5Body: (
      <>
        <code>MagenticWorkflowBuilder</code> thuần túy là sugar phủ trên{" "}
        <code>WorkflowBuilder</code>, không phải một nguyên thủy engine mới: nó tổng hợp đúng hình
        dạng đồ thị theo chu trình manager/participant mà{" "}
        <a href={`${SITE_BASE}/api/workflow-planner.html`}>mẫu hình Planner (Magentic)</a> vốn đã
        dựng tay, với định tuyến <code>switch_case</code> ra khỏi manager và các cạnh{" "}
        <code>direct</code> quay lại. Nó nhận hai tham số kiểu, không phải một. Manager đọc một{" "}
        <code>ReportMsg</code> và phát ra một <code>TaskMsg</code>; mỗi participant đọc{" "}
        <code>TaskMsg</code> đó và phát lại một <code>ReportMsg</code> — đảo ngược so với manager.
        Chính sự đảo ngược đó khiến cả hai chiều thỏa quy tắc so khớp kiểu của <code>connect()</code>.
      </>
    ),
    s5MethodCols: ["Phương thức", "Làm gì"],
    s5PlanSignoffIntro: (
      <>
        <code>.require_plan_signoff(port_id)</code> hoạt động từ đầu đến cuối, không chỉ được nối
        dây. Manager gửi một yêu cầu <em>có kiểu</em> (
        <code>MagenticPlanSignoffRequest{"{plan}"}</code>), không bao giờ là một message văn bản
        tự do mà người xem xét phải tự phân tích. Một host khôi phục bằng một{" "}
        <code>MagenticPlanSignoffResponse{"{approved, feedback}"}</code> có kiểu, qua đúng con
        đường <code>resume_workflow()</code> mà{" "}
        <a href="./workflow.html#workflow-hitl">mọi request_port khác</a> đã dùng — không có cơ chế
        engine mới nào.
      </>
    ),
    s5PlanSignoffProofNote: (
      <>
        Đã được chứng minh từ đầu đến cuối bởi P4 trong{" "}
        <code>test_workflow_magentic_plan_signoff.cpp</code>.
      </>
    ),
    s5PlanSignoffNote: (
      <>
        <strong>Đây là một vòng lặp sửa lại, không phải một cổng cố định.</strong>{" "}
        <code>approved == false</code> không phải là ngõ cụt: phản hồi đến ngay như đầu vào kế
        tiếp của manager, kèm theo <code>feedback</code>, để một manager body bình thường có thể
        định tuyến thẳng vào một vòng lập kế hoạch khác thay vì làm hỏng toàn bộ run.
      </>
    ),

    s6Eyebrow: "examples/19_magentic_builder_live.cpp — chạy thật, với một model thật",
    s6Heading: "Ví dụ minh họa: ba executor, mười dòng",
    s6Body: (
      <>
        <code>examples/17_planner_live.cpp</code> dựng tay chu trình moderator/researcher/writer;
        ví dụ này dựng lại nó bằng <code>MagenticWorkflowBuilder</code>, chứng minh lớp tiện dụng
        này tạo ra một đồ thị thực sự chạy được, không chỉ "trông có vẻ đúng" như dữ liệu. Nó bỏ
        qua sạch sẽ khi không có API key thật, giống mọi ví dụ chạy thật khác trong repo này.
      </>
    ),

    s7Eyebrow: "Được nêu tên, không giấu",
    s7Heading: "Những gì còn thô ráp",
    s7Body:
      "Đây là những khoảng trống và cạm bẫy — được nêu tên ngay trong mã nguồn, không bị lặng lẽ bỏ qua khỏi trang này.",
    gapCols: ["Cái gì", "Được nêu ở đâu", "Trạng thái"],
  },
} as const;

export function ApiBuilderReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];

  return (
    <section className="section" id="builder">
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
            <ApiDiagnosticNote>{t.introNote}</ApiDiagnosticNote>
          </RevealItem>
        </RevealGroup>

        {/* ---- 1. QuickstartSessionBuilder ------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="quickstart-session-builder" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="core/session_builder.hpp">{highlightCpp(quickstartAliasesSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.s1MethodCols]}
              templateColumns="1.6fr 3fr"
              rows={sessionBuilderMethodRows[lang].map((r) => [
                <code key="m" className="api-tag">
                  {r.method}
                </code>,
                r.does,
              ])}
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. Worked example + Composed escape hatch ---------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/30_session_builder_quickstart_live.cpp">
              {highlightCpp(quickstartWorkedExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s2ComposedNote}</p>
          </RevealItem>

          <RevealItem>
            <Cite path="docs/planning/quickstart-session-builder-design-draft.md" label="docs/planning/quickstart-session-builder-design-draft.md — §2b · tests/test_session_builder.cpp" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Bundle::ask() / ask_stream() --------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="bundle-ask" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="core/session_builder.hpp">{highlightCpp(bundleAskSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s3StreamNote}</p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="core/session_builder.hpp">{highlightCpp(bundleAskStreamSignatureSnippet)}</CodePanel>
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. WorkflowBuilder ---------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="workflow-builder" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.s4MethodCols]}
              templateColumns="1.8fr 3fr"
              rows={workflowBuilderMethodRows[lang].map((r) => [
                <code key="m" className="api-tag">
                  {r.method}
                </code>,
                r.does,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <div style={{ marginTop: 32, marginBottom: 12 }}>
              <h4 style={{ fontSize: "1.05rem", margin: "0 0 8px" }}>{t.s4StaticAssertHeading}</h4>
              <p>{t.s4StaticAssertBody}</p>
              <ApiDiagnosticNote>{t.s4CompileFailNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="workflow/graph.hpp">{highlightCpp(workflowConnectStaticAssertSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="tests/test_workflow_graph_validation.cpp">
              {highlightCpp(workflowBuilderWorkedExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <ApiDiagnosticNote>{t.s4ValidatorNote}</ApiDiagnosticNote>
          </RevealItem>

          <RevealItem>
            <Cite path="examples/04_first_workflow.cpp" label="examples/04_first_workflow.cpp — the plain Workflow-as-data walkthrough this builder shortens" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 5. MagenticWorkflowBuilder --------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="magentic-workflow-builder" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="workflow/magentic.hpp">{highlightCpp(magenticBuilderShapeSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.s5MethodCols]}
              templateColumns="2fr 3fr"
              rows={magenticBuilderMethodRows[lang].map((r) => [
                <code key="m" className="api-tag">
                  {r.method}
                </code>,
                r.does,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 24, color: "var(--text-dim)", lineHeight: 1.65 }}>{t.s5PlanSignoffIntro}</p>
            <ApiDiagnosticNote>{t.s5PlanSignoffProofNote}</ApiDiagnosticNote>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="workflow/magentic.hpp">{highlightCpp(magenticPlanSignoffSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s5PlanSignoffNote}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 6. Magentic worked example ---------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s6Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
              <p>{t.s6Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/19_magentic_builder_live.cpp">
              {highlightCpp(magenticWorkedExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <Cite path="examples/21_workflow_as_participant.cpp" label="examples/21_workflow_as_participant.cpp — a second live MagenticWorkflowBuilder worked example, a whole Workflow used as one participant" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 7. Gaps ---------------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="builder-gaps" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s7Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s7Heading}</h3>
              <p>{t.s7Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.gapCols]}
              templateColumns="1.6fr 1.6fr 3fr"
              rows={builderGapRows[lang].map((g) => [
                <span key="w">
                  {g.what}
                  <br />
                  <span className="status-badge status-design" style={{ marginTop: 6 }}>
                    {tu.statusDesignedNotBuilt}
                  </span>
                </span>,
                <code key="c" style={{ fontSize: "0.85rem" }}>
                  {g.cite}
                </code>,
                g.state,
              ])}
            />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
