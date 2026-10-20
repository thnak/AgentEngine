// "How AgentEngine works" — the orientation page for the API docs section. Every other page in
// this reference documents ONE part field-by-field; this one explains how the parts compose: the
// lifecycle of a run, how authority reaches an effect, what enforces each of the eight core
// invariants, the layer rule, and who owns what between the engine and its host.
//
// Sourcing rule (same as apiContent.ts): every claim here is grounded in a real header, source
// file, RFC section, or test in this repo, cited with a link — never a shape these sources don't
// state. Citations deliberately point at long-standing files (spec, RFCs, capability/effect/tool
// headers, tests) rather than at files that only exist on an unmerged branch.
//
// Staleness note: a large async conversion (ADR-237) is landing on the integration branch, which
// changes extension-point signatures. Where a shape is in flux, this page describes the MODEL
// (session / run / turn / effect / capability) rather than pinning a signature, so it stays true
// across that change — the "What is real today" section says so explicitly.
//
// Bilingual (EN/VI) — same convention as every other page: code, identifiers, RFC/ADR numbers and
// file paths stay identical; only prose is translated.

import { ApiDiagnosticNote } from "./ApiDiagnosticNote";
import { ApiTable } from "./ApiTable";
import { RevealGroup, RevealItem } from "./Reveal";
import { gh } from "../data/apiContent";
import { SITE_BASE } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";

/** A file/RFC citation rendered as the site's citation channel, same idiom as ApiStreamingReference. */
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
    eyebrow: "AgentEngineSpecification.md §3–§4 · CONVENTIONS.md",
    headingPrefix: "How AgentEngine",
    headingHighlight: "works",
    intro: (
      <>
        Every other page in this reference documents one part. This page explains how the parts
        compose: what happens between a caller asking a session to do something and an effect
        actually landing, which layer is allowed to do what, and what the engine guarantees versus
        what the host supplies. If you are reading this reference for the first time, read this
        page first.
      </>
    ),

    // ---- 1. Lifecycle ------------------------------------------------------------------------
    s1Eyebrow: "001 §1/§5 — Execution Model",
    s1Heading: "A run, end to end",
    s1Body: (
      <>
        The engine's unit of work is a <strong>run on a session</strong>, not a function call. A
        session owns history and state; a run owns one ordering of turns; a turn owns one round of
        model call plus whatever tool calls that round produced. The order below is the same every
        time, and that ordering is what makes attribution (I4) and replay (I5) possible at all.
      </>
    ),
    steps: [
      {
        index: "01",
        title: "Session",
        body: "One session, one executor (I1). History and state sit behind a runtime-checked guard, so a second entry point cannot mutate them concurrently. The host creates the session and hands it a provider, a sandbox and its capabilities.",
      },
      {
        index: "02",
        title: "Run",
        body: "start_run — or the C++ Builder's equivalent — takes that guard for the whole run, mints a run_id and fixes turn order. Every effect that follows carries that identity.",
      },
      {
        index: "03",
        title: "Turn",
        body: "Context is assembled from the session's context providers — history, skills, memory, retrieval — into one ChatRequest. The turn index advances. This is where the engine decides how much of the past is in scope.",
      },
      {
        index: "04",
        title: "Model call",
        body: "One call through the configured ChatClient. The seam is streaming-shaped; the whole-response form is a convenience layered on top. A model call is nondeterministic, so it crosses a recording seam (I5) if the run is to be replayable.",
      },
      {
        index: "05",
        title: "Tool calls — authorized first",
        body: "Model output is data, never authority (I3). A declared call carries its provenance; the tool pipeline checks that provenance, the tool's own declared capability ceiling and the approval policy before anything executes.",
      },
      {
        index: "06",
        title: "Effect",
        body: "The tool, sandbox or shell runs with an explicitly passed EffectContext — never an ambient thread-local (I2). The effect is attributed and audited at the point of effect, not reconstructed afterwards (I4).",
      },
      {
        index: "07",
        title: "Commit",
        body: "Results land in the session's append-log and the worktree. A checkpoint records the durable state, which is what makes resume, time travel and replay real capabilities rather than aspirations.",
      },
    ],
    loopNote:
      "The loop is bounded, not open-ended: MaxTurns<N>, token budgets and per-call timeouts are enforced by the engine (I8). A budget here is a gate, not a comment.",
    s1ProgramNote: (
      <>
        This page is the model, and it deliberately carries no code. If you would rather read the same
        lifecycle as a program you can compile and run — includes, <code>main()</code>, and the build
        file that produces it — that is{" "}
        <a href={`${SITE_BASE}/api/first-app.html`}>Your first program</a>. Every step in the ladder
        above appears there in about sixty lines. For the same steps <em>acting together over time</em>
        , follow the <a href={`${SITE_BASE}/api/pizza-order.html`}>worked example</a> instead.
      </>
    ),

    // ---- 2. Authority ------------------------------------------------------------------------
    s2Eyebrow: "007 — Capability and Trust Model · 008 — Sandbox and Isolation",
    s2Heading: "How authority reaches an effect",
    s2Body: (
      <>
        The security story is one sentence long: <strong>nothing is reachable without a capability
        that was explicitly passed in, and nothing a model says can create one.</strong> This is the
        path a single call takes.
      </>
    ),
    authNodes: [
      {
        title: "Capability declaration",
        sub: "A tool declares what it needs at compile time — Capabilities<cap::decl::RunCommand> and its siblings. The declaration is a ceiling, not a hint.",
      },
      {
        title: "CapabilitySet held",
        sub: "Empty by default. There is no constructor that grants everything and no allow-by-default tier, so forgetting to grant is failing closed.",
      },
      {
        title: "EffectContext",
        sub: "A required parameter on the effect path, carrying run/session/agent identity, the principal, the capability set and trace context. Not a global, not a thread-local.",
      },
      {
        title: "Approval and policy decision",
        sub: "Consulted only for a call that actually needs it, answered by host-supplied deciders against the call's canonical arguments — before execution, never after.",
      },
      {
        title: "Sandbox / backend",
        sub: "The only layer permitted to hold an OS capability. Worktree, resource limits and network policy are enforced here, not above.",
      },
    ],
    authArrowGrant: "the host grants a subset",
    authArrowPass: "passed explicitly, per call",
    authArrowCheck: "checked before the effect",
    authArrowNarrow: "narrowed, never widened",
    authTaint: (
      <>
        <strong>Taint is the other half of I3.</strong> Text that originated in a model response is
        marked, cannot implicitly convert to a trusted string, and must be explicitly declassified —
        a compile-time gate, not a runtime convention. The same rule is what stops a model-authored
        tool call from being treated as a vendor-structured one.
      </>
    ),

    // ---- 3. Invariants -----------------------------------------------------------------------
    s3Eyebrow: "AgentEngineSpecification.md §4 — Core invariants",
    s3Heading: "The eight invariants, and what actually enforces each",
    s3Body: (
      <>
        The specification calls these "the load-bearing statements of the whole system" and insists
        each one is a testable gate rather than a slogan. This table pairs each with the mechanism
        that enforces it and somewhere to audit that claim.
      </>
    ),
    invColumns: ["Invariant", "What enforces it", "Where to audit it"],
    invRows: [
      [
        "I1 — One session, one executor",
        "A runtime-checked guard held for the whole of every public async entry point; turn order is FIFO.",
        "tests/rt/test_rt_lock_chain.cpp",
      ],
      [
        "I2 — No ambient authority",
        "Capabilities are required parameters; a sandbox starts from the empty set; there is no allow-by-default tier.",
        "tests/sandbox/test_sandbox_capability_authorization.cpp",
      ],
      [
        "I3 — Model output is data, never authority",
        "Declared-call provenance is checked before approval; approval is host policy decided before the effect; taint is enforced by the type system.",
        "tests/core/chat/test_system_channel_taint_fence.cpp",
      ],
      [
        "I4 — Every effect is attributable",
        "EffectContext is a required parameter at the point of effect, so identity travels with the call instead of being reconstructed.",
        "include/agentengine/core/effect_context.hpp",
      ],
      [
        "I5 — Nondeterminism crosses a recorded seam",
        "Model responses, clocks, RNG, sandbox results and remote calls each pass through a seam that can record and replay.",
        "tests/core/chat/test_replay_chat_client.cpp",
      ],
      [
        "I6 — Declarative and native are equivalent",
        "The YAML/JSON definition and the CRTP definition compile to the same metadata table and run through the same validator.",
        "tests/core/agent/test_agent_yaml_compiler.cpp",
      ],
      [
        "I7 — Protocol conformance is a gate",
        "Each protocol RFC names an executable suite and a protocol revision; \"supports MCP\" without a passing suite at a named revision is not a claim this project makes.",
        "rfcs/011-MCP-Conformance.md",
      ],
      [
        "I8 — Budgets are enforced, not documented",
        "Turn latency, sandbox cold start, per-session footprint and engine overhead have numeric budgets checked by a gate.",
        "tests/rt/agent_session/test_rt_agent_session_token_budget.cpp",
      ],
    ],
    invCaveat: (
      <>
        <strong>I7 is the one with a real gap today.</strong> The MCP client-role suite reports a
        genuine number at a named revision, but the server-role suite needs a test-fixture listener
        that does not exist yet, so 011 §10 G1 and 012 §8 G1 stay open. The{" "}
        <a href={`${SITE_BASE}/api/protocols.html`}>protocols page</a> carries the current status
        rather than a rounded-up claim.
      </>
    ),

    // ---- 4. Layers ---------------------------------------------------------------------------
    s4Eyebrow: "001 §3 — Layering · CONVENTIONS.md",
    s4Heading: "Five layers, one direction",
    s4Body: (
      <>
        A layer may depend only on layers below it, and only through the seam that layer publishes.
        L4 has no privileged access to L0. L1 is the only layer that may hold an OS capability;
        everything above it holds handles. This is not a diagram convention — a lint checks every{" "}
        <code>#include</code> in <code>include/</code> and <code>src/</code> against the layer map,
        fails on any new upward edge, and fails again if a previously-known edge disappears without
        its baseline entry being deleted, so the baseline can only shrink.
      </>
    ),
    layerRows: [
      ["L4", "Protocol surfaces", "MCP · A2A · AG-UI · OpenAI-compatible HTTP"],
      ["L3", "Orchestration", "workflow graph, handoff, group chat, checkpoint / resume / time-travel"],
      ["L2", "Agent core", "Agent · AgentSession · tool plane · ChatClient plane · middleware"],
      ["L1", "Trust & isolation", "capability model · sandbox seam · WASM component plugin ABI"],
      ["L0", "Runtime substrate", "agentengine::rt::: tasks, AsyncMutex, channels/streams, session & append-log stores, the platform PAL"],
      ["V", "Shared vocabulary", "error model, JSON/YAML values, the content model, the taint marker, run events — every layer may include it"],
    ],
    layerColumns: ["Layer", "What lives there", "Examples"],

    // ---- 5. Ownership ------------------------------------------------------------------------
    s5Eyebrow: "CONVENTIONS.md — dependency posture · ADR-061",
    s5Heading: "Who owns what: engine vs host",
    s5Body: (
      <>
        AgentEngine is not a loop you write, and it is not a server you deploy. It owns the session
        lifecycle, the turn loop, the dispatch pipeline, the durability seam and the isolation
        boundary. You supply the parts it calls — and you keep the socket, the credentials and the
        policy.
      </>
    ),
    ownsLabel: "The engine owns",
    owns: [
      "the session lifecycle and the turn loop",
      "the tool dispatch pipeline, including the order authorization happens in",
      "the runtime substrate: tasks, guards, channels, pools",
      "the durability seam: append-log, checkpoints, replay",
      "the sandbox contract — the shape a backend must satisfy, not its implementation",
    ],
    supplyLabel: "You supply",
    supply: [
      "ChatClient — a model provider, or one of the shipped Anthropic / OpenAI clients",
      "SandboxBackend — an isolation implementation behind the seam",
      "SessionStore / AppendLogStore — where durability actually lands",
      "ApprovalDecider / PolicyDecider — your approval and policy answers",
      "Tool<> conformers and a MemoryProvider — your tools and your memory",
      "the inbound socket, TLS, auth and principal derivation",
    ],
    s5Note: (
      <>
        <strong>The transport boundary is deliberate.</strong> The engine does not bind a port: host
        code owns the socket, terminates TLS and authentication, and hands the engine already-parsed
        requests. That is why there is no built-in HTTP listener to configure, and why
        principal derivation is a host responsibility rather than an engine setting.
      </>
    ),

    // ---- 6. Status ---------------------------------------------------------------------------
    s6Eyebrow: "docs/planning/v1-implementation-roadmap.md",
    s6Heading: "What is real today, and what is in flight",
    s6Body: (
      <>
        Every entry in this reference is labelled <strong>real &amp; tested</strong> or{" "}
        <strong>designed, not built</strong>. The honest summary of the whole engine is three
        sentences long.
      </>
    ),
    statusRows: [
      [
        "Built and tested",
        "Milestones 0–6: the runtime substrate, tools / capabilities / sandbox, worktree + interpreter + CodeAct, sessions / durability / memory, real providers / identity / secrets, and multi-agent orchestration.",
      ],
      [
        "In progress",
        "Milestone 7, protocol conformance. The client-role suites are real and report numbers at named revisions; the server-role suites need a test-fixture listener that has not been built, so several gates stay open.",
      ],
      [
        "Landing now",
        "A conversion making every extension point asynchronous — a real I/O reactor underneath tools, skills, sandbox and shell, so a component can do network I/O without holding a thread. Until it completes, the synchronous paths remain the ones production callers use.",
      ],
    ],
    statusColumns: ["State", "What it covers"],
    s6Caveat: (
      <>
        Because that conversion is in flight, a signature you read here or on a neighbouring page
        may be changing underneath you. Every page cites the header it was written from, so{" "}
        <strong>the header is the tiebreaker</strong> — and where a shape is in flux, this page
        describes the model rather than the signature.
      </>
    ),
  },

  vi: {
    eyebrow: "AgentEngineSpecification.md §3–§4 · CONVENTIONS.md",
    headingPrefix: "AgentEngine",
    headingHighlight: "vận hành thế nào",
    intro: (
      <>
        Mọi trang khác trong tài liệu tham chiếu này mô tả một phần. Trang này giải thích cách các
        phần ghép lại với nhau: điều gì xảy ra từ lúc người gọi yêu cầu một phiên làm việc cho tới
        khi một hiệu ứng thực sự được thực thi, lớp nào được phép làm gì, và đâu là bảo đảm của
        engine so với phần mà host phải cung cấp. Nếu bạn đọc tài liệu tham chiếu này lần đầu, hãy
        đọc trang này trước.
      </>
    ),

    s1Eyebrow: "001 §1/§5 — Execution Model",
    s1Heading: "Một run, từ đầu đến cuối",
    s1Body: (
      <>
        Đơn vị công việc của engine là <strong>một run trên một phiên</strong>, không phải một lời
        gọi hàm. Phiên sở hữu lịch sử và trạng thái; run sở hữu một thứ tự các lượt; một lượt sở
        hữu một vòng gọi model cùng những lời gọi tool mà vòng đó sinh ra. Thứ tự dưới đây lặp lại
        như nhau mỗi lần, và chính thứ tự đó làm cho việc quy kết nguồn (I4) và replay (I5) trở
        nên khả thi.
      </>
    ),
    steps: [
      {
        index: "01",
        title: "Phiên (Session)",
        body: "Một phiên, một executor (I1). Lịch sử và trạng thái nằm sau một guard được kiểm tra lúc chạy, nên một điểm vào thứ hai không thể sửa chúng đồng thời. Host tạo phiên và trao cho nó một provider, một sandbox và tập capability của nó.",
      },
      {
        index: "02",
        title: "Run",
        body: "start_run — hoặc API tương đương của Builder C++ — giữ guard đó cho toàn bộ run, cấp một run_id và cố định thứ tự lượt. Mọi hiệu ứng về sau đều mang danh tính đó.",
      },
      {
        index: "03",
        title: "Lượt (Turn)",
        body: "Ngữ cảnh được lắp từ các context provider của phiên — lịch sử, skill, memory, truy hồi — thành một ChatRequest. Chỉ số lượt tăng lên. Đây là nơi engine quyết định bao nhiêu phần quá khứ nằm trong phạm vi.",
      },
      {
        index: "04",
        title: "Gọi model",
        body: "Một lời gọi qua ChatClient đã cấu hình. Ranh giới này có dạng streaming; dạng trả về toàn bộ chỉ là tiện ích xếp trên đó. Lời gọi model mang tính bất định, nên nó đi qua một ranh giới ghi lại (I5) nếu run cần replay được.",
      },
      {
        index: "05",
        title: "Lời gọi tool — phê duyệt trước",
        body: "Đầu ra của model là dữ liệu, không bao giờ là quyền hạn (I3). Một lời gọi được khai báo mang theo nguồn gốc của nó; pipeline tool kiểm tra nguồn gốc đó, mức trần capability mà tool tự khai báo, và chính sách phê duyệt — trước khi bất cứ thứ gì chạy.",
      },
      {
        index: "06",
        title: "Hiệu ứng",
        body: "Tool, sandbox hoặc shell chạy với một EffectContext được truyền tường minh — không bao giờ là thread-local ẩn (I2). Hiệu ứng được quy kết nguồn và ghi audit ngay tại điểm phát sinh, không phải dựng lại về sau (I4).",
      },
      {
        index: "07",
        title: "Ghi nhận",
        body: "Kết quả được ghi vào append-log của phiên và vào worktree. Một checkpoint ghi lại trạng thái bền vững — đó là thứ khiến resume, time travel và replay trở thành năng lực thật thay vì kỳ vọng.",
      },
    ],
    loopNote:
      "Vòng lặp có biên, không mở: MaxTurns<N>, ngân sách token và timeout từng lời gọi đều do engine thực thi (I8). Ngân sách ở đây là một cổng chặn, không phải một dòng chú thích.",
    s1ProgramNote: (
      <>
        Trang này là mô hình, và nó cố ý không mang mã nguồn. Nếu bạn muốn đọc cùng vòng đời đó dưới
        dạng một chương trình có thể biên dịch và chạy — kể cả các dòng include, hàm{" "}
        <code>main()</code>, và tệp build tạo ra nó — thì đó là{" "}
        <a href={`${SITE_BASE}/api/first-app.html`}>Chương trình đầu tiên của bạn</a>. Mọi bước trong
        thang ở trên đều xuất hiện ở đó trong khoảng sáu mươi dòng. Còn muốn thấy chính các bước đó{" "}
        <em>phối hợp với nhau theo thời gian</em>, hãy theo{" "}
        <a href={`${SITE_BASE}/api/pizza-order.html`}>ví dụ xuyên suốt</a>.
      </>
    ),

    s2Eyebrow: "007 — Capability and Trust Model · 008 — Sandbox and Isolation",
    s2Heading: "Quyền hạn đi tới một hiệu ứng như thế nào",
    s2Body: (
      <>
        Câu chuyện bảo mật gói trong một câu: <strong>không gì tiếp cận được nếu không có một
        capability được truyền vào tường minh, và không gì model nói ra có thể tạo ra một
        capability.</strong> Đây là đường đi của một lời gọi.
      </>
    ),
    authNodes: [
      {
        title: "Khai báo capability",
        sub: "Tool khai báo thứ nó cần ngay lúc biên dịch — Capabilities<cap::decl::RunCommand> và các khai báo anh em. Khai báo là mức trần, không phải một gợi ý.",
      },
      {
        title: "CapabilitySet được nắm giữ",
        sub: "Mặc định là rỗng. Không có constructor nào cấp phát tất cả và không có tầng \"mặc định cho phép\", nên quên cấp phát đồng nghĩa với đóng lại an toàn.",
      },
      {
        title: "EffectContext",
        sub: "Tham số bắt buộc trên đường đi của hiệu ứng, mang danh tính run/session/agent, principal, tập capability và trace context. Không phải biến toàn cục, không phải thread-local.",
      },
      {
        title: "Quyết định phê duyệt và chính sách",
        sub: "Chỉ được hỏi với lời gọi thực sự cần, do các decider phía host trả lời dựa trên tham số chuẩn tắc của lời gọi — trước khi thực thi, không bao giờ sau đó.",
      },
      {
        title: "Sandbox / backend",
        sub: "Lớp duy nhất được phép nắm giữ capability của hệ điều hành. Worktree, giới hạn tài nguyên và chính sách mạng được thực thi ở đây, không phải ở trên.",
      },
    ],
    authArrowGrant: "host cấp một tập con",
    authArrowPass: "truyền tường minh, theo từng lời gọi",
    authArrowCheck: "kiểm tra trước khi phát sinh hiệu ứng",
    authArrowNarrow: "chỉ thu hẹp, không bao giờ mở rộng",
    authTaint: (
      <>
        <strong>Taint là nửa còn lại của I3.</strong> Văn bản bắt nguồn từ phản hồi của model bị
        đánh dấu, không thể chuyển đổi ngầm sang một chuỗi tin cậy, và phải được giải mật tường
        minh — đây là một cổng chặn lúc biên dịch, không phải một quy ước lúc chạy. Cùng quy tắc đó
        là thứ ngăn một lời gọi tool do model tạo ra bị đối xử như một lời gọi do vendor trả về.
      </>
    ),

    s3Eyebrow: "AgentEngineSpecification.md §4 — Core invariants",
    s3Heading: "Tám bất biến, và cái gì thực sự thực thi từng cái",
    s3Body: (
      <>
        Đặc tả gọi đây là "những phát biểu chịu lực của toàn hệ thống" và khẳng định mỗi cái là một
        cổng chặn kiểm chứng được, không phải khẩu hiệu. Bảng này ghép mỗi bất biến với cơ chế thực
        thi nó và một nơi để kiểm chứng lại tuyên bố đó.
      </>
    ),
    invColumns: ["Bất biến", "Cơ chế thực thi", "Nơi kiểm chứng"],
    invRows: [
      [
        "I1 — Một phiên, một executor",
        "Một guard kiểm tra lúc chạy được giữ suốt mọi điểm vào async công khai; thứ tự lượt là FIFO.",
        "tests/rt/test_rt_lock_chain.cpp",
      ],
      [
        "I2 — Không có quyền hạn ẩn",
        "Capability là tham số bắt buộc; sandbox khởi đầu với tập rỗng; không có tầng mặc định cho phép.",
        "tests/sandbox/test_sandbox_capability_authorization.cpp",
      ],
      [
        "I3 — Đầu ra model là dữ liệu, không phải quyền hạn",
        "Nguồn gốc của lời gọi được khai báo được kiểm tra trước khi phê duyệt; phê duyệt là chính sách của host, quyết định trước hiệu ứng; taint được thực thi bởi hệ thống kiểu.",
        "tests/core/chat/test_system_channel_taint_fence.cpp",
      ],
      [
        "I4 — Mọi hiệu ứng đều quy kết được",
        "EffectContext là tham số bắt buộc tại điểm phát sinh hiệu ứng, nên danh tính đi cùng lời gọi thay vì được dựng lại.",
        "include/agentengine/core/effect_context.hpp",
      ],
      [
        "I5 — Tính bất định đi qua một ranh giới được ghi lại",
        "Phản hồi model, đồng hồ, RNG, kết quả sandbox và lời gọi từ xa đều đi qua một ranh giới có thể ghi và phát lại.",
        "tests/core/chat/test_replay_chat_client.cpp",
      ],
      [
        "I6 — Bề mặt khai báo và native tương đương",
        "Định nghĩa YAML/JSON và định nghĩa CRTP biên dịch ra cùng một bảng metadata và đi qua cùng một validator.",
        "tests/core/agent/test_agent_yaml_compiler.cpp",
      ],
      [
        "I7 — Tuân thủ giao thức là một cổng chặn",
        "Mỗi RFC giao thức nêu một bộ kiểm thử chạy được và một phiên bản giao thức; \"hỗ trợ MCP\" mà không có bộ kiểm thử đạt ở phiên bản được nêu tên thì không phải là tuyên bố dự án này đưa ra.",
        "rfcs/011-MCP-Conformance.md",
      ],
      [
        "I8 — Ngân sách được thực thi, không chỉ ghi lại",
        "Độ trễ lượt, thời gian khởi động nguội của sandbox, dấu chân mỗi phiên và chi phí nền của engine đều có ngân sách số học được kiểm bởi một cổng chặn.",
        "tests/rt/agent_session/test_rt_agent_session_token_budget.cpp",
      ],
    ],
    invCaveat: (
      <>
        <strong>I7 là bất biến còn lỗ hổng thật hôm nay.</strong> Bộ kiểm thử phía client của MCP
        báo một con số thật ở một phiên bản được nêu tên, nhưng bộ kiểm thử phía server cần một
        listener làm fixture mà chưa được dựng, nên 011 §10 G1 và 012 §8 G1 vẫn còn mở.{" "}
        <a href={`${SITE_BASE}/api/protocols.html`}>Trang protocols</a> ghi trạng thái hiện tại thay
        vì một tuyên bố được làm tròn lên.
      </>
    ),

    s4Eyebrow: "001 §3 — Layering · CONVENTIONS.md",
    s4Heading: "Năm lớp, một chiều phụ thuộc",
    s4Body: (
      <>
        Một lớp chỉ được phụ thuộc vào các lớp bên dưới nó, và chỉ qua ranh giới mà lớp đó công bố.
        L4 không có quyền truy cập đặc quyền vào L0. L1 là lớp duy nhất được phép nắm giữ capability
        của hệ điều hành; mọi lớp trên nó chỉ nắm handle. Đây không phải quy ước vẽ sơ đồ — một
        lint kiểm tra mọi <code>#include</code> trong <code>include/</code> và <code>src/</code>{" "}
        đối chiếu với bản đồ lớp, fail khi có cạnh đi lên mới, và fail lần nữa nếu một cạnh đã biết
        biến mất mà dòng baseline của nó không được xóa — nên baseline chỉ có thể thu hẹp.
      </>
    ),
    layerRows: [
      ["L4", "Bề mặt giao thức", "MCP · A2A · AG-UI · HTTP tương thích OpenAI"],
      ["L3", "Điều phối", "đồ thị workflow, handoff, group chat, checkpoint / resume / time-travel"],
      ["L2", "Lõi agent", "Agent · AgentSession · tool plane · ChatClient plane · middleware"],
      ["L1", "Tin cậy & cô lập", "mô hình capability · ranh giới sandbox · ABI plugin WASM Component"],
      ["L0", "Nền runtime", "agentengine::rt::: task, AsyncMutex, channel/stream, session & append-log store, PAL nền tảng"],
      ["V", "Từ vựng dùng chung", "mô hình lỗi, giá trị JSON/YAML, mô hình nội dung, dấu taint, run event — mọi lớp đều được phép include"],
    ],
    layerColumns: ["Lớp", "Nơi đây có gì", "Ví dụ"],

    s5Eyebrow: "CONVENTIONS.md — dependency posture · ADR-061",
    s5Heading: "Ai sở hữu cái gì: engine và host",
    s5Body: (
      <>
        AgentEngine không phải vòng lặp bạn tự viết, và cũng không phải server bạn tự triển khai. Nó
        sở hữu vòng đời phiên, vòng lặp lượt, pipeline điều phối, ranh giới bền vững và ranh giới cô
        lập. Bạn cung cấp những phần nó gọi tới — và bạn giữ socket, thông tin xác thực và chính
        sách.
      </>
    ),
    ownsLabel: "Engine sở hữu",
    owns: [
      "vòng đời phiên và vòng lặp lượt",
      "pipeline điều phối tool, kể cả thứ tự các bước kiểm quyền",
      "nền runtime: task, guard, channel, pool",
      "ranh giới bền vững: append-log, checkpoint, replay",
      "hợp đồng sandbox — hình dạng một backend phải thỏa, không phải phần triển khai của nó",
    ],
    supplyLabel: "Bạn cung cấp",
    supply: [
      "ChatClient — một nhà cung cấp model, hoặc một trong các client Anthropic / OpenAI đã có sẵn",
      "SandboxBackend — một bản triển khai cô lập phía sau ranh giới",
      "SessionStore / AppendLogStore — nơi độ bền vững thực sự hạ cánh",
      "ApprovalDecider / PolicyDecider — câu trả lời phê duyệt và chính sách của bạn",
      "Các conformer Tool<> và một MemoryProvider — tool và memory của bạn",
      "socket đầu vào, TLS, xác thực và việc suy ra principal",
    ],
    s5Note: (
      <>
        <strong>Ranh giới vận chuyển là chủ ý.</strong> Engine không tự mở cổng: mã host sở hữu
        socket, kết thúc TLS và xác thực, rồi trao cho engine các request đã được phân tích cú
        pháp. Đó là lý do không có listener HTTP sẵn để cấu hình, và cũng là lý do việc suy ra
        principal thuộc trách nhiệm của host chứ không phải một thiết lập của engine.
      </>
    ),

    s6Eyebrow: "docs/planning/v1-implementation-roadmap.md",
    s6Heading: "Hôm nay đã có gì thật, và cái gì đang trên đường tới",
    s6Body: (
      <>
        Mọi mục trong tài liệu tham chiếu này đều được gắn nhãn{" "}
        <strong>đã có thật &amp; đã kiểm thử</strong> hoặc{" "}
        <strong>đã thiết kế, chưa xây</strong>. Tóm tắt trung thực về toàn bộ engine gói trong ba
        câu.
      </>
    ),
    statusRows: [
      [
        "Đã xây và đã kiểm thử",
        "Milestone 0–6: nền runtime, tool / capability / sandbox, worktree + interpreter + CodeAct, phiên / độ bền vững / memory, provider thật / danh tính / bí mật, và điều phối đa agent.",
      ],
      [
        "Đang thực hiện",
        "Milestone 7, tuân thủ giao thức. Các bộ kiểm thử phía client là thật và báo con số ở những phiên bản được nêu tên; các bộ phía server cần một listener làm fixture chưa được dựng, nên một số cổng chặn vẫn còn mở.",
      ],
      [
        "Đang hạ cánh",
        "Một cuộc chuyển đổi đưa mọi extension point sang bất đồng bộ — một I/O reactor thật nằm dưới tool, skill, sandbox và shell, để một thành phần có thể làm I/O mạng mà không cần giữ một thread. Cho tới khi hoàn tất, các đường đồng bộ vẫn là đường mà mã production đang dùng.",
      ],
    ],
    statusColumns: ["Trạng thái", "Bao gồm những gì"],
    s6Caveat: (
      <>
        Vì cuộc chuyển đổi đó đang diễn ra, một chữ ký bạn đọc ở đây hoặc ở trang bên cạnh có thể
        đang thay đổi ngay dưới chân bạn. Mỗi trang đều dẫn header mà nó được viết từ đó, nên{" "}
        <strong>header là thứ phân xử cuối cùng</strong> — và ở nơi hình dạng còn đang biến động,
        trang này mô tả mô hình thay vì mô tả chữ ký.
      </>
    ),
  },
};

function Ladder({ steps }: { steps: readonly { index: string; title: string; body: string }[] }) {
  return (
    <div className="ladder glass" style={{ padding: "6px 20px" }}>
      {steps.map((s) => (
        <div className="ladder-step" key={s.index}>
          <span className="ladder-index">{s.index}</span>
          <div>
            <h4>{s.title}</h4>
            <p>{s.body}</p>
          </div>
        </div>
      ))}
    </div>
  );
}

export function ApiHowItWorksReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  return (
    <section className="section" id="how-it-works">
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

        {/* ---- 1. The lifecycle ---------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="lifecycle" style={{ marginBottom: 22 }}>
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <Ladder steps={t.steps} />
            <div className="flow-loop-note">{t.loopNote}</div>
            <p className="gs-note" style={{ marginTop: 16 }}>{t.s1ProgramNote}</p>
          </RevealItem>
          <RevealItem>
            <Cite path="AgentEngineSpecification.md" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. Authority -------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="authority" style={{ marginTop: 54, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <div className="flow glass">
              <div className="flow-node is-purple">
                <div className="flow-node-title">{t.authNodes[0].title}</div>
                <div className="flow-node-sub">{t.authNodes[0].sub}</div>
              </div>
              <div className="flow-arrow">{t.authArrowGrant}</div>
              <div className="flow-node">
                <div className="flow-node-title">{t.authNodes[1].title}</div>
                <div className="flow-node-sub">{t.authNodes[1].sub}</div>
              </div>
              <div className="flow-arrow">{t.authArrowPass}</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">{t.authNodes[2].title}</div>
                <div className="flow-node-sub">{t.authNodes[2].sub}</div>
              </div>
              <div className="flow-arrow">{t.authArrowCheck}</div>
              <div className="flow-node">
                <div className="flow-node-title">{t.authNodes[3].title}</div>
                <div className="flow-node-sub">{t.authNodes[3].sub}</div>
              </div>
              <div className="flow-arrow">{t.authArrowNarrow}</div>
              <div className="flow-node is-pink">
                <div className="flow-node-title">{t.authNodes[4].title}</div>
                <div className="flow-node-sub">{t.authNodes[4].sub}</div>
              </div>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiDiagnosticNote>{t.authTaint}</ApiDiagnosticNote>
            <Cite
              path="include/agentengine/trust/capability.hpp"
              label="include/agentengine/trust/capability.hpp · include/agentengine/core/effect_context.hpp · include/agentengine/core/tool_pipeline.hpp"
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Invariants ------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="invariants" style={{ marginTop: 54, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={t.invColumns}
              rows={t.invRows.map((r) => [...r])}
              templateColumns="0.9fr 1.6fr 0.9fr"
            />
          </RevealItem>
          <RevealItem>
            <ApiDiagnosticNote status="stub" kind="caveat">
              {t.invCaveat}
            </ApiDiagnosticNote>
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. Layers ----------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="layers" style={{ marginTop: 54, marginBottom: 22 }}>
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={t.layerColumns}
              rows={t.layerRows.map((r) => [...r])}
              templateColumns="0.18fr 0.7fr 2fr"
            />
          </RevealItem>
          <RevealItem>
            <Cite
              path="tools/layers.toml"
              label="tools/layers.toml · tools/layering_lint.py · CONVENTIONS.md"
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- 5. Engine vs host --------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="ownership" style={{ marginTop: 54, marginBottom: 22 }}>
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <div className="flow-branch">
              <div className="flow-node is-teal">
                <div className="flow-branch-label is-yes">{t.ownsLabel}</div>
                <ul style={{ margin: "6px 0 0", paddingLeft: 18 }}>
                  {t.owns.map((x) => (
                    <li key={x} className="flow-node-sub">
                      {x}
                    </li>
                  ))}
                </ul>
              </div>
              <div className="flow-node is-purple">
                <div className="flow-branch-label">{t.supplyLabel}</div>
                <ul style={{ margin: "6px 0 0", paddingLeft: 18 }}>
                  {t.supply.map((x) => (
                    <li key={x} className="flow-node-sub">
                      {x}
                    </li>
                  ))}
                </ul>
              </div>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiDiagnosticNote>{t.s5Note}</ApiDiagnosticNote>
            <Cite path="decisions/archive/ADR-061-host-provided-inbound-transport.md" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 6. Status ----------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="status" style={{ marginTop: 54, marginBottom: 22 }}>
              <span className="eyebrow">{t.s6Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
              <p>{t.s6Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={t.statusColumns}
              rows={t.statusRows.map((r) => [...r])}
              templateColumns="0.5fr 2fr"
            />
          </RevealItem>
          <RevealItem>
            <ApiDiagnosticNote status="stub" kind="caveat">
              {t.s6Caveat}
            </ApiDiagnosticNote>
            <Cite
              path="docs/planning/v1-implementation-roadmap.md"
              label="docs/planning/v1-implementation-roadmap.md · decisions/archive/ADR-237-async-extension-points-and-io-reactor.md"
            />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
