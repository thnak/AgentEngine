import { authoringEntries, minimalAgentSnippet, registerAgentSteps } from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";
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
    headingPrefix: "Agents are",
    headingHighlight: "compile-time policy sets",
    intro: (
      <>
        There is no runtime "agent config" object anywhere in this engine, and no virtual
        dispatch. An agent's whole policy set — which chat client, which tools, which
        capabilities, how many turns — is a list of template parameters, compiled and validated
        exactly once by <code>register_agent&lt;A&gt;()</code>.
      </>
    ),
    section1Eyebrow: "core/agent.hpp",
    section1Heading: (
      <>
        <code>Agent&lt;Derived, Policies...&gt;</code> — an empty tag, not a base class
      </>
    ),
    isntLabel: "What this ISN'T",
    isnt: (
      <>
        A runtime configuration object you construct and pass around. There's no{" "}
        <code>AgentConfig</code> instance, no virtual method an override participates in —{" "}
        <code>Agent&lt;...&gt;</code> itself is empty; it carries zero state.
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
        Returns <code>result&lt;AgentMetadata&gt;</code> — the SAME compiled table the
        declarative YAML compiler also targets (015, I6). Not every check below enforces
        something real today; each is labeled for what it actually is, not what it's meant
        to eventually become.
      </>
    ),
    stubsNote: (
      <>
        <strong>Three of these eight are always-pass stubs</strong> — not because the design
        is wrong, but because the machinery they'd check against (a per-deployment sandbox
        backend registry, a per-tool sandbox policy tag, 014's workflow graph wired to this
        specific check) doesn't exist yet at this call site. Named plainly rather than left to
        look enforced.
      </>
    ),
  },
  vi: {
    eyebrow: "Bề mặt viết mã CRTP trong C++ — 002",
    headingPrefix: "Agent là",
    headingHighlight: "tập policy tại thời điểm biên dịch",
    intro: (
      <>
        Không có bất kỳ đối tượng "agent config" nào lúc chạy trong toàn bộ engine này, và
        không có virtual dispatch. Toàn bộ tập policy của một agent — dùng chat client nào, tool
        nào, capability nào, bao nhiêu lượt — là một danh sách tham số template, được biên dịch
        và xác thực đúng một lần bởi <code>register_agent&lt;A&gt;()</code>.
      </>
    ),
    section1Eyebrow: "core/agent.hpp",
    section1Heading: (
      <>
        <code>Agent&lt;Derived, Policies...&gt;</code> — một tag rỗng, không phải một base class
      </>
    ),
    isntLabel: "Đây KHÔNG PHẢI là gì",
    isnt: (
      <>
        Một đối tượng cấu hình lúc chạy mà bạn tự khởi tạo và truyền đi. Không có thực thể{" "}
        <code>AgentConfig</code> nào, không có phương thức virtual nào để một override tham
        gia vào — chính <code>Agent&lt;...&gt;</code> là rỗng; nó không mang trạng thái nào cả.
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
        Trả về <code>result&lt;AgentMetadata&gt;</code> — CHÍNH bảng đã biên dịch mà trình biên
        dịch YAML khai báo cũng nhắm tới (015, I6). Không phải mọi kiểm tra bên dưới đều thực thi
        điều gì đó có thật ngay hôm nay; mỗi kiểm tra được nêu nhãn đúng bản chất của nó, không
        phải theo dự định cuối cùng nó sẽ trở thành.
      </>
    ),
    stubsNote: (
      <>
        <strong>Ba trong số tám kiểm tra này là stub luôn-pass</strong> — không phải vì thiết
        kế sai, mà vì cơ chế mà chúng cần kiểm tra dựa vào (một registry backend sandbox theo
        từng deployment, một thẻ chính sách sandbox theo từng tool, đồ thị workflow của 014 được
        đấu nối vào đúng kiểm tra này) hiện chưa tồn tại tại điểm gọi này. Được nêu tên thẳng
        thắn thay vì để trông như đã được thực thi.
      </>
    ),
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
        </div>

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
          </RevealItem>

          <RevealItem>
            <CiteLink id="register-agent" />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
