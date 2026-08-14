import { motion } from "framer-motion";
import { agentCppSnippet, buildSteps, nextLinks } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { highlightCpp } from "../lib/highlightCpp";
import { RevealGroup, RevealItem } from "./Reveal";

// Shell-command highlighter for the terminal code block below — same
// dependency-free approach as the shared highlightCpp, not a general
// syntax highlighter.

function highlightShell(source: string) {
  const lines = source.split("\n");
  return lines.map((line, i) => {
    if (line.startsWith("#")) {
      return (
        <div key={i}>
          <span className="tok-com">{line}</span>
        </div>
      );
    }
    if (line.trim().length === 0) {
      return <div key={i}> </div>;
    }
    const [cmd, ...rest] = line.split(" ");
    return (
      <div key={i}>
        <span className="tok-prompt">$ </span>
        <span className="tok-kw">{cmd}</span>
        {rest.length ? " " + rest.join(" ") : ""}
      </div>
    );
  });
}

const copy = {
  en: {
    eyebrow: "Build it today",
    headingPrefix: "Getting",
    headingHighlight: "started",
    intro:
      "AgentEngine is pre-v1, spec-driven software under active development — there is no versioned package to install. Milestones 0–6 of the implementation roadmap (core substrate, sandboxed tools, the interpreter and worktree, durable sessions and memory, real provider backends, multi-agent workflow orchestration) are complete; Milestone 7 (protocol conformance — MCP, A2A, AG-UI) is in progress. What follows is the real developer-facing surface as of today, verified against the actual headers and tests — not a stable, permanent API reference.",
    authoringEyebrow: "The authoring surface",
    authoringHeadingPrefix: "The Agent CRTP pattern, as it compiles",
    authoringHeadingHighlight: "right now",
    authoringBody1: "Tools and agents are declared as compile-time policy sets, not runtime configuration objects.",
    authoringBody2:
      "compiles that declaration into a read-only",
    authoringBody3: "table and rejects a defective one — a tool-name collision or an uncovered capability ceiling fails to register, with a specific diagnostic — exactly as exercised in",
    flowRegisterSub: "compile-time CRTP policy set → read-only AgentMetadata table",
    flowDrivesArrow: "drives",
    flowStartRunSub: "resolves a whole multi-round tool conversation internally: extract → capability/approval-check → invoke → feed results back → converge on a final answer — not just one model call",
    flowBackedArrow: "backed by any",
    flowLiveSub: "real, live backends",
    flowReplaySub: "deterministic, offline testing — no network or API key required",
    note7: "See",
    note7b: "through",
    note8: "for small, runnable programs built on exactly this shape.",
    nextEyebrow: "Where to go next",
  },
  vi: {
    eyebrow: "Xây dựng ngay hôm nay",
    headingPrefix: "Bắt",
    headingHighlight: "đầu",
    intro:
      "AgentEngine là phần mềm pre-v1, hướng đặc tả (spec-driven) đang được phát triển tích cực — chưa có gói phần mềm đóng phiên bản nào để cài đặt. Milestone 0–6 của lộ trình triển khai (nền runtime lõi, tool chạy trong sandbox, trình thông dịch và worktree, session và bộ nhớ bền vững, backend provider thật, điều phối workflow đa agent) đã hoàn tất; Milestone 7 (tuân thủ giao thức — MCP, A2A, AG-UI) đang được thực hiện. Nội dung dưới đây là bề mặt thực tế dành cho lập trình viên tính đến hôm nay, đã được đối chiếu với header và test thật — không phải một tài liệu tham chiếu API ổn định, cố định.",
    authoringEyebrow: "Bề mặt viết mã (authoring surface)",
    authoringHeadingPrefix: "Mẫu CRTP của Agent, đúng như nó biên dịch",
    authoringHeadingHighlight: "ngay lúc này",
    authoringBody1: "Tool và agent được khai báo dưới dạng tập policy tại thời điểm biên dịch, không phải đối tượng cấu hình lúc chạy.",
    authoringBody2: "biên dịch khai báo đó thành một bảng chỉ-đọc",
    authoringBody3: "và từ chối một khai báo lỗi — một xung đột tên tool hay một capability ceiling chưa được bao phủ sẽ đăng ký thất bại, kèm chẩn đoán cụ thể — đúng như được kiểm chứng trong",
    flowRegisterSub: "tập CRTP policy tại thời điểm biên dịch → bảng AgentMetadata chỉ-đọc",
    flowDrivesArrow: "dẫn dắt",
    flowStartRunSub: "tự giải quyết toàn bộ một cuộc hội thoại nhiều vòng gọi tool ở bên trong: trích xuất → kiểm tra capability/approval → gọi tool → đưa kết quả trở lại → hội tụ về câu trả lời cuối cùng — chứ không chỉ một lần gọi model",
    flowBackedArrow: "được hậu thuẫn bởi bất kỳ",
    flowLiveSub: "backend thật, đang chạy",
    flowReplaySub: "kiểm thử ngoại tuyến, tất định — không cần mạng hay API key",
    note7: "Xem từ",
    note7b: "đến",
    note8: "để có các chương trình nhỏ, chạy được, xây trên đúng hình dạng này.",
    nextEyebrow: "Đi tiếp đến đâu",
  },
} as const;

export function GettingStarted() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" id="getting-started">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <p>{t.intro}</p>
        </div>

        <RevealGroup className="spec-layout">
          <RevealGroup>
            <RevealItem>
              <div className="glass" style={{ padding: "8px 8px", borderRadius: "var(--radius-lg)" }}>
                <div className="ladder" style={{ padding: "10px 18px" }}>
                  {buildSteps[lang].map((s) => (
                    <div className="ladder-step" key={s.id}>
                      <span className="ladder-index">{s.index}</span>
                      <div>
                        <h4>{s.title}</h4>
                        <p>{s.detail}</p>
                        <code className="gs-cmd">{s.command}</code>
                      </div>
                    </div>
                  ))}
                </div>
              </div>
            </RevealItem>
          </RevealGroup>

          <RevealGroup>
            <RevealItem>
              <motion.div
                className="code-panel glass"
                whileHover={{ rotate: 0.2, scale: 1.01 }}
                transition={{ type: "spring", stiffness: 260, damping: 22 }}
              >
                <div className="code-panel-head">
                  <span style={{ background: "#ff6459" }} />
                  <span style={{ background: "#ffbd2e" }} />
                  <span style={{ background: "#28c840" }} />
                  <span className="filename">terminal</span>
                </div>
                <pre>
                  <code>
                    {highlightShell(buildSteps[lang].map((s) => `# ${s.index}. ${s.title}\n${s.command}`).join("\n\n"))}
                  </code>
                </pre>
              </motion.div>
            </RevealItem>
          </RevealGroup>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head" style={{ marginTop: 56 }}>
              <span className="eyebrow">{t.authoringEyebrow}</span>
              <h3 style={{ fontSize: "clamp(1.4rem, 2.4vw, 1.8rem)", margin: "12px 0" }}>
                {t.authoringHeadingPrefix} <span className="grad-text">{t.authoringHeadingHighlight}</span>
              </h3>
              <p>
                {t.authoringBody1} <code>register_agent&lt;A&gt;()</code> {t.authoringBody2}{" "}
                <code>AgentMetadata</code> {t.authoringBody3}{" "}
                <code>tests/test_agent_registry.cpp</code>.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <motion.div
              className="code-panel glass"
              whileHover={{ rotate: -0.2, scale: 1.005 }}
              transition={{ type: "spring", stiffness: 260, damping: 22 }}
            >
              <div className="code-panel-head">
                <span style={{ background: "#ff6459" }} />
                <span style={{ background: "#ffbd2e" }} />
                <span style={{ background: "#28c840" }} />
                <span className="filename">researcher_agent.cpp</span>
              </div>
              <pre>
                <code>{highlightCpp(agentCppSnippet)}</code>
              </pre>
            </motion.div>
          </RevealItem>

          <RevealItem>
            <div className="flow glass">
              <div className="flow-node is-purple">
                <div className="flow-node-title">register_agent&lt;A&gt;()</div>
                <div className="flow-node-sub">{t.flowRegisterSub}</div>
              </div>
              <div className="flow-arrow">{t.flowDrivesArrow}</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">
                  AgentSession&lt;ChatClientT, StateT, HistoryProviderT&gt;::StartRun
                </div>
                <div className="flow-node-sub">
                  <code>agentengine::rt::</code> {t.flowStartRunSub}
                </div>
              </div>
              <div className="flow-arrow">{t.flowBackedArrow} ChatClientT</div>
              <div className="flow-row">
                <div className="flow-node is-pink">
                  <div className="flow-node-title">Anthropic / OpenAI</div>
                  <div className="flow-node-sub">{t.flowLiveSub}</div>
                </div>
                <div className="flow-node is-pink">
                  <div className="flow-node-title">ReplayChatClient</div>
                  <div className="flow-node-sub">{t.flowReplaySub}</div>
                </div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note">
              {t.note7} <code>examples/01_hello_agent.cpp</code> {t.note7b}{" "}
              <code>examples/06_capabilities_and_denial.cpp</code> {t.note8}
            </p>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="gs-next glass">
              <span className="eyebrow">{t.nextEyebrow}</span>
              <div className="gs-next-links">
                {nextLinks[lang].map((l) => (
                  <a key={l.href} className="btn btn-secondary" href={l.href} target="_blank" rel="noreferrer">
                    {l.label}
                  </a>
                ))}
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
