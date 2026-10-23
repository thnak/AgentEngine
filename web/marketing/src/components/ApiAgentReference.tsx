import {
  authoringEntries,
  helloAgentRunSnippet,
  minimalAgentSnippet,
  multiTurnSnippet,
  registerAgentSteps,
} from "../data/apiContent";
import { SITE_BASE } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiDiagnosticNote } from "./ApiDiagnosticNote";
import { ApiIncludes } from "./ApiIncludes";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

// Verified by devsim/verify-includes: every path here is compiled by the gate.
const AGENT_HEADERS = [
  "agentengine/core/agent.hpp",
  "agentengine/core/agent_session_bridge.hpp",
  "agentengine/core/chat_client.hpp",
  "agentengine/core/tool.hpp",
  "agentengine/core/content.hpp",
];
import type { Lang } from "../i18n/LanguageContext";

function entryById(lang: Lang, id: string) {
  const e = authoringEntries[lang].find((entry) => entry.id === id);
  if (!e) throw new Error(`missing authoring entry: ${id}`);
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
    eyebrow: "C++ CRTP authoring surface — 002",
    includesNote: (
      <>
        Five headers. <code>core/agent.hpp</code> declares the <code>Agent&lt;Derived, Policies...&gt;</code>{" "}
        tag and every policy type it takes; <code>core/agent_session_bridge.hpp</code> is{" "}
        <code>make_agent_session&lt;A&gt;()</code> — the call that turns that declaration into a
        running session (ADR-226); <code>core/chat_client.hpp</code> is the concept your client must
        satisfy; <code>core/tool.hpp</code> is what a <code>Tools&lt;...&gt;</code> entry must be;
        and <code>core/content.hpp</code> gives you <code>Message</code> and <code>Text</code>.
      </>
    ),
    headingPrefix: "Agents are",
    headingHighlight: "compile-time policy sets",
    intro: (
      <>
        <code>Agent&lt;Derived, Policies...&gt;</code> declares an agent's whole policy set as a
        list of template parameters — which chat client, which tools, which capabilities, how many
        turns. <code>register_agent&lt;A&gt;()</code> compiles and validates that list exactly
        once. There is no runtime "agent config" object anywhere in this engine, and no virtual
        dispatch.
      </>
    ),
    builderNote: (
      <>
        Building an application that uses agents, rather than authoring a new agent type? The rest
        of this page is a compile-time authoring surface: declare a policy set, and{" "}
        <code>register_agent&lt;A&gt;()</code> validates it. For a running session in app code,{" "}
        <code>QuickstartSessionBuilder</code> (see the{" "}
        <a href={`${SITE_BASE}/api/builder.html`}>Builder API page</a>) reaches a running{" "}
        <code>AgentSession</code> in a few chained calls instead of the full CRTP declaration below.
      </>
    ),
    section1Eyebrow: "core/agent.hpp",
    section1Heading: (
      <>
        <code>Agent&lt;Derived, Policies...&gt;</code> — an empty tag, not a base class
      </>
    ),
    isntLabel: "What this isn't",
    isnt: (
      <>
        A runtime configuration object you construct and pass around. There's no{" "}
        <code>AgentConfig</code> instance, and no virtual method for an override to participate
        in. <code>Agent&lt;...&gt;</code> itself is empty; it carries zero state.
      </>
    ),
    isLabel: "What it actually is",
    is: (
      <>
        A list of template parameters — <code>ChatClientId&lt;Id&gt;</code>,{" "}
        <code>Tools&lt;Ts...&gt;</code>, <code>Capabilities&lt;Cs...&gt;</code>,{" "}
        <code>SandboxProfile&lt;P&gt;</code>, <code>MaxTurns&lt;N&gt;</code>,{" "}
        <code>TokenBudget&lt;N&gt;</code> — resolved entirely at compile time, read once by{" "}
        <code>register_agent&lt;A&gt;()</code>.
      </>
    ),
    s1RecoLabel: "Recommended default",
    s1RecoBody: (
      <>
        Only <code>ChatClientId&lt;Id&gt;</code> is required — it has no default, and an agent
        that omits it fails <code>register_agent&lt;A&gt;()</code> before anything else runs.
        Everything else in the list to the left already has a sane default: <code>MaxTurns</code>{" "}
        is 16, <code>TokenBudget</code> is unbounded, <code>SandboxProfile</code> resolves to{" "}
        <code>Strict</code>, and <code>Capabilities</code> is empty. Most agents write only{" "}
        <code>ChatClientId&lt;Id&gt;</code> plus <code>Tools&lt;Ts...&gt;</code> and leave the
        rest unstated:
      </>
    ),
    section2Eyebrow: "core/agent_registry.hpp — compiler::run()",
    section2Heading: (
      <>
        <code>register_agent&lt;A&gt;()</code> — eight checks, told honestly
      </>
    ),
    section2Body: (
      <>
        <code>register_agent&lt;A&gt;()</code> returns <code>result&lt;AgentMetadata&gt;</code> —
        the same compiled table the declarative YAML compiler also targets. Not every check below
        enforces something real today; each check is labeled for what it actually is, not what
        it's meant to eventually become.
      </>
    ),
    section2Note: <>015, I6</>,
    stubsNote: (
      <>
        <strong>Three of these eight checks are always-pass stubs.</strong> That isn't because the
        design is wrong — the machinery they'd check against doesn't exist yet at this call site:
        a per-deployment sandbox backend registry, a per-tool sandbox policy tag, and a workflow
        graph wired to this specific check. Each stub is named plainly rather than left to look
        enforced.
      </>
    ),
    stubsNoteCite: <>014 — the workflow graph this check will wire to</>,
    section3Eyebrow: "examples/01_hello_agent.cpp, examples/03_multi_turn.cpp",
    section3Heading: "Running a turn, for real",
    section3Body: (
      <>
        The two surfaces described above are wired together, and have been since{" "}
        <code>ADR-226</code>: <code>make_agent_session&lt;A&gt;()</code> runs{" "}
        <code>register_agent&lt;A&gt;()</code>'s validation and binds the compiled policy set to a
        real <code>rt::AgentSession</code> — so the declaration <strong>is</strong> what runs. The
        first example below takes that road. The second takes the other one, constructing{" "}
        <code>agentengine::rt::AgentSession&lt;ChatClientT&gt;</code> directly from a{" "}
        <code>ChatClient</code> type, which is what you do when the type-level authoring surface is
        more machinery than you need. The fake <code>JokerChatClient</code> always answers the same
        pirate joke, so both examples build and run completely offline: no API key, no network.
      </>
    ),
    section3aEyebrow: "The smallest possible turn",
    section3bEyebrow: "The same session, across two turns",
    section3bBody: (
      <>
        There is no separate "session" object threaded through calls. The{" "}
        <code>rt::AgentSession</code> itself is the conversation: every <code>start_run()</code>{" "}
        against the same session appends to its own durable <code>history_</code>, and that
        accumulated history is what the next turn's <code>ChatClient</code> sees.
      </>
    ),
    section3bNote: <>005 §3</>,
  },
  vi: {
    eyebrow: "Bề mặt viết mã CRTP trong C++ — 002",
    includesNote: (
      <>
        Năm header. <code>core/agent.hpp</code> khai báo tag{" "}
        <code>Agent&lt;Derived, Policies...&gt;</code> cùng mọi kiểu policy mà nó nhận;{" "}
        <code>core/agent_session_bridge.hpp</code> là <code>make_agent_session&lt;A&gt;()</code> —
        lệnh gọi biến khai báo đó thành một session đang chạy (ADR-226);{" "}
        <code>core/chat_client.hpp</code> là concept mà client của bạn phải thỏa mãn;{" "}
        <code>core/tool.hpp</code> là thứ một mục <code>Tools&lt;...&gt;</code> phải là; và{" "}
        <code>core/content.hpp</code> cho bạn <code>Message</code> cùng <code>Text</code>.
      </>
    ),
    headingPrefix: "Agent là",
    headingHighlight: "tập policy tại thời điểm biên dịch",
    intro: (
      <>
        <code>Agent&lt;Derived, Policies...&gt;</code> khai báo toàn bộ tập policy của một agent
        dưới dạng một danh sách tham số template — dùng chat client nào, tool nào, capability
        nào, bao nhiêu lượt. <code>register_agent&lt;A&gt;()</code> biên dịch và xác thực danh
        sách đó đúng một lần. Không có bất kỳ đối tượng "agent config" nào lúc chạy trong toàn bộ
        engine này, và không có virtual dispatch.
      </>
    ),
    builderNote: (
      <>
        Đang xây một ứng dụng sử dụng agent, thay vì viết ra một kiểu agent mới? Phần còn lại của
        trang này là một bề mặt viết mã tại thời điểm biên dịch: khai báo một tập policy, rồi{" "}
        <code>register_agent&lt;A&gt;()</code> xác thực nó. Nếu bạn chỉ cần một session đang chạy
        trong code ứng dụng, <code>QuickstartSessionBuilder</code> (xem{" "}
        <a href={`${SITE_BASE}/api/builder.html`}>trang Builder API</a>) đưa bạn tới một{" "}
        <code>AgentSession</code> đang chạy chỉ bằng vài lệnh gọi nối chuỗi, thay vì toàn bộ khai
        báo CRTP bên dưới.
      </>
    ),
    section1Eyebrow: "core/agent.hpp",
    section1Heading: (
      <>
        <code>Agent&lt;Derived, Policies...&gt;</code> — một tag rỗng, không phải một base class
      </>
    ),
    isntLabel: "Đây không phải là gì",
    isnt: (
      <>
        Một đối tượng cấu hình lúc chạy mà bạn tự khởi tạo và truyền đi. Không có thực thể{" "}
        <code>AgentConfig</code> nào, và không có phương thức virtual nào để một override tham
        gia vào. Chính <code>Agent&lt;...&gt;</code> là rỗng; nó không mang trạng thái nào cả.
      </>
    ),
    isLabel: "Nó thực sự là gì",
    is: (
      <>
        Một danh sách tham số template — <code>ChatClientId&lt;Id&gt;</code>,{" "}
        <code>Tools&lt;Ts...&gt;</code>, <code>Capabilities&lt;Cs...&gt;</code>,{" "}
        <code>SandboxProfile&lt;P&gt;</code>, <code>MaxTurns&lt;N&gt;</code>,{" "}
        <code>TokenBudget&lt;N&gt;</code> — được phân giải hoàn toàn tại thời điểm biên dịch, chỉ
        được đọc một lần bởi <code>register_agent&lt;A&gt;()</code>.
      </>
    ),
    s1RecoLabel: "Mặc định khuyến nghị",
    s1RecoBody: (
      <>
        Chỉ <code>ChatClientId&lt;Id&gt;</code> là bắt buộc — nó không có giá trị mặc định, và
        một agent bỏ qua nó sẽ khiến <code>register_agent&lt;A&gt;()</code> thất bại trước khi
        bất kỳ điều gì khác chạy. Mọi thứ còn lại trong danh sách bên trái đều đã có một giá trị
        mặc định hợp lý: <code>MaxTurns</code> là 16, <code>TokenBudget</code> không giới hạn,{" "}
        <code>SandboxProfile</code> phân giải về <code>Strict</code>, và{" "}
        <code>Capabilities</code> rỗng. Hầu hết agent chỉ cần viết{" "}
        <code>ChatClientId&lt;Id&gt;</code> cùng <code>Tools&lt;Ts...&gt;</code> và để phần còn
        lại ở trạng thái mặc định:
      </>
    ),
    section2Eyebrow: "core/agent_registry.hpp — compiler::run()",
    section2Heading: (
      <>
        <code>register_agent&lt;A&gt;()</code> — tám kiểm tra, được nói thẳng
      </>
    ),
    section2Body: (
      <>
        <code>register_agent&lt;A&gt;()</code> trả về <code>result&lt;AgentMetadata&gt;</code> —
        chính bảng đã biên dịch mà trình biên dịch YAML khai báo cũng nhắm tới. Không phải mọi
        kiểm tra bên dưới đều thực thi điều gì đó có thật ngay hôm nay; mỗi kiểm tra được nêu nhãn
        đúng bản chất của nó, không phải theo dự định cuối cùng nó sẽ trở thành.
      </>
    ),
    section2Note: <>015, I6</>,
    stubsNote: (
      <>
        <strong>Ba trong số tám kiểm tra này là stub luôn-pass.</strong> Đó không phải vì thiết kế
        sai, mà vì cơ chế mà chúng cần kiểm tra dựa vào chưa tồn tại tại điểm gọi này: một registry
        backend sandbox theo từng deployment, một thẻ chính sách sandbox theo từng tool, và một đồ
        thị workflow được đấu nối vào đúng kiểm tra này. Mỗi stub được nêu tên thẳng thắn thay vì
        để trông như đã được thực thi.
      </>
    ),
    stubsNoteCite: <>014 — đồ thị workflow mà kiểm tra này sẽ được đấu nối tới</>,
    section3Eyebrow: "examples/01_hello_agent.cpp, examples/03_multi_turn.cpp",
    section3Heading: "Chạy một lượt, thật sự",
    section3Body: (
      <>
        Hai bề mặt ở trên đã được đấu nối với nhau, từ <code>ADR-226</code>:{" "}
        <code>make_agent_session&lt;A&gt;()</code> chạy phần xác thực của{" "}
        <code>register_agent&lt;A&gt;()</code> rồi gắn tập policy đã biên dịch vào một{" "}
        <code>rt::AgentSession</code> thật — nên chính khai báo <strong>là</strong> thứ chạy. Ví
        dụ đầu tiên bên dưới đi theo con đường đó. Ví dụ thứ hai đi con đường còn lại: dựng{" "}
        <code>agentengine::rt::AgentSession&lt;ChatClientT&gt;</code> trực tiếp từ một kiểu{" "}
        <code>ChatClient</code> — cách bạn chọn khi bề mặt viết mã ở mức kiểu là quá nhiều máy móc
        so với nhu cầu. Client giả <code>JokerChatClient</code> luôn trả lời cùng một câu đùa về
        cướp biển, nên cả hai ví dụ đều build và chạy hoàn toàn offline: không cần API key, không
        cần mạng.
      </>
    ),
    section3aEyebrow: "Lượt chạy nhỏ nhất có thể",
    section3bEyebrow: "Cùng một session, qua hai lượt",
    section3bBody: (
      <>
        Không có một đối tượng "session" riêng nào được truyền qua các lệnh gọi. Chính{" "}
        <code>rt::AgentSession</code> là cuộc hội thoại: mỗi lần <code>start_run()</code> trên
        cùng một session sẽ nối thêm vào <code>history_</code> bền vững của chính nó, và toàn bộ
        lịch sử tích lũy đó chính là những gì <code>ChatClient</code> ở lượt kế tiếp nhìn thấy.
      </>
    ),
    section3bNote: <>005 §3</>,
  },
} as const;

export function ApiAgentReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  const agentCrtp = entryById(lang, "agent-crtp");
  return (
    <section className="section" id="agent" style={{ paddingBottom: 0 }}>
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
          <p className="gs-note" style={{ marginTop: 16 }}>{t.builderNote}</p>
        </div>

        {/* ---- What to include ------------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <ApiIncludes headers={AGENT_HEADERS}>{t.includesNote}</ApiIncludes>
          </RevealItem>
        </RevealGroup>

        {/* ---- 1. A compile-time tag ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="agent-crtp" style={{ marginBottom: 22 }}>
              <span className="eyebrow">{t.section1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.section1Heading}</h3>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="gs-recommend">
              <span className="gs-recommend-label">{t.s1RecoLabel}</span>
              <p>{t.s1RecoBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="core/agent.hpp">{highlightCpp(minimalAgentSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">{t.isntLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.isnt}</p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.isLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.is}</p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>
              {agentCrtp.body}
            </p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="agent-crtp" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. register_agent<A>() ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="register-agent" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.section2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.section2Heading}</h3>
              <p>{t.section2Body}</p>
              <ApiDiagnosticNote>{t.section2Note}</ApiDiagnosticNote>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {registerAgentSteps[lang].map((s) => (
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
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>
              {t.stubsNote}
            </p>
            <ApiDiagnosticNote status="stub">{t.stubsNoteCite}</ApiDiagnosticNote>
          </RevealItem>

          <RevealItem>
            <CiteLink id="register-agent" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Running a turn, for real ------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="running-a-turn" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.section3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.section3Heading}</h3>
              <p>{t.section3Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <span className="eyebrow" style={{ display: "block", marginBottom: 10 }}>{t.section3aEyebrow}</span>
            <CodePanel filename="examples/01_hello_agent.cpp">{highlightCpp(helloAgentRunSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <span className="eyebrow" style={{ display: "block", margin: "28px 0 10px" }}>{t.section3bEyebrow}</span>
            <p style={{ color: "var(--text-dim)", lineHeight: 1.65, marginBottom: 16 }}>{t.section3bBody}</p>
            <ApiDiagnosticNote>{t.section3bNote}</ApiDiagnosticNote>
            <CodePanel filename="examples/03_multi_turn.cpp">{highlightCpp(multiTurnSnippet)}</CodePanel>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
