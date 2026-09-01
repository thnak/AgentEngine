import {
  a2aStreamProjectorInterruptSnippet,
  aguiProjectorMessageSnippet,
  aguiSseFrameSnippet,
  declarativeCompilerSnippet,
  mcpDispatchSnippet,
  protocolEntries,
} from "../data/apiContent";
import { SITE_BASE } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

const copy = {
  en: {
    eyebrow: "L4 protocol surfaces — Milestone 7",
    headingPrefix: "Five surfaces named in the spec,",
    headingHighlight: "Milestone 7 in progress",
    intro: (
      <>
        L4's protocol surfaces (MCP, A2A, AG-UI, OpenAI-compatible HTTP) and the declarative
        YAML/JSON authoring format are Reviewed RFCs. Milestone 6 is complete. Milestone 7
        (protocol conformance) is in progress — real, tested code exists for MCP, A2A, AG-UI,
        and the declarative compilers — and a Phase G gate audit found the milestone's own exit
        criterion not yet met, chiefly blocked on a real network listener. Milestones 8-9 have
        not started.
      </>
    ),
    s1Eyebrow: "CONVENTIONS.md — protocol code rules",
    s1Heading: (
      <>
        A protocol type never reaches <code>agentengine::core</code>
      </>
    ),
    s1Body: (
      <>
        Every surface below is a translation layer, not a second copy of the engine: it maps
        wire vocabulary onto the exact same <code>AgentSession</code>/<code>ToolTable</code>{" "}
        the C++ authoring surface already drives, at the boundary, one direction only.
      </>
    ),
    flowWireSub: "wire vocabulary — JSON-RPC methods, SSE events, HTTP verbs",
    flowTranslateArrow: "translate at the boundary — L4 only, never leaks inward",
    flowCoreSub: "AgentSession, ToolTable, ContextProvider — no mcp:: or a2a:: type anywhere in here",
    s2Eyebrow: "The one gap MCP, A2A, and AG-UI all share",
    s2Heading: "Real request/response logic — no socket underneath it yet",
    s2Body: "Three of the five surfaces below are the identical shape: a real, tested translator that takes a real request value and returns a real response value — with nothing yet putting those values on an actual wire.",
    s2Note: (
      <>
        <strong>Same shape, three surfaces.</strong> <code>McpServer::dispatch()</code> above
        takes a <code>JsonRpcRequest</code>, returns a <code>JsonRpcResponse</code>.{" "}
        <code>A2aServer</code> takes a message, returns a task. <code>RunEventProjector</code>{" "}
        takes the real internal run-event stream, returns AG-UI wire events. All three are
        real and tested against real values — what's missing in every case is the same thing:
        a listener that reads bytes off a socket and calls the translator, per the status
        table below.
      </>
    ),
    s2bEyebrow: "protocol/agui/sse.hpp · protocol/agui/projection.hpp · protocol/a2a/streaming.hpp",
    s2bHeading: (
      <>
        One internal <code>RunEvent</code>, projected twice — AG-UI ends the run, A2A pauses the task
      </>
    ),
    s2bBody: (
      <>
        The SSE framing itself is proven, not just described: <code>to_sse_frame()</code> is
        exactly <code>"data: " + json::dump(...) + "\n\n"</code>, and{" "}
        <code>tests/test_agui_sse.cpp:34-46</code> checks the <code>data: </code> prefix, the{" "}
        <code>\n\n</code> terminator, and that the bytes between them parse as the real projected
        event — never a hand-built JSON literal.
      </>
    ),
    s2bCaption1: "The framing, proven end to end",
    s2bCaption2: (
      <>
        Every other <code>RunEvent</code> kind maps context-free (projection.hpp's own file-top
        comment); the ONE exception is the <code>messageId</code> <code>RunEventProjector</code>{" "}
        mints on <code>model_call_started</code> and erases on <code>model_call_finished</code>,
        bracketing <code>TEXT_MESSAGE_START</code>/<code>CONTENT</code>/<code>END</code> around one
        model turn's streamed text — proven end to end by{" "}
        <code>tests/test_rt_agui_projection.cpp:188-206</code>.
      </>
    ),
    s2bCaption3: (
      <>
        <code>input_required</code> forces AG-UI to END the run with{" "}
        <code>RunFinishedInterrupt</code> — it has no pause event (013 §2.2). A2A's{" "}
        <code>task_state</code> enum does: <code>A2aStreamProjector</code> projects the identical{" "}
        <code>input_required</code> <code>RunEvent</code> onto{" "}
        <code>TASK_STATE_INPUT_REQUIRED</code>, a real, non-terminal state{" "}
        <code>tests/test_a2a_streaming.cpp:70-78</code> confirms with{" "}
        <code>is_terminal()</code> — the task genuinely continues.
      </>
    ),
    s2bNote: (
      <>
        <strong>Same source, two shapes.</strong> <code>RunEventProjector</code> and{" "}
        <code>A2aStreamProjector</code> both consume the exact same internal{" "}
        <code>RunEvent</code>/<code>WorkflowEvent</code> stream — this page's job stops at the
        wire-format projection above it. The event model itself (every <code>RunEvent</code> kind,{" "}
        <code>enable_event_stream()</code> on a session or a workflow, what the superstep engine
        actually emits) is documented in full on the{" "}
        <a href={`${SITE_BASE}/api/events.html`}>Events page</a>.
      </>
    ),
    s3Eyebrow: "015 — Declarative Agent Format",
    s3Heading: (
      <>
        YAML compiles to the exact same <code>AgentMetadata</code> C++ produces
      </>
    ),
    s3Body: (
      <>
        I6 ("declarative and native surfaces are equivalent") isn't a design promise here —{" "}
        <code>compile_agent_document()</code> targets the literal same struct{" "}
        <code>register_agent&lt;A&gt;()</code> does, field for field.
      </>
    ),
    s4Eyebrow: "All five, field by field",
    s4Heading: "Status, by surface",
    rfcLabel: "RFC",
  },
  vi: {
    eyebrow: "Bề mặt giao thức L4 — Milestone 7",
    headingPrefix: "Năm bề mặt được nêu tên trong đặc tả,",
    headingHighlight: "Milestone 7 đang thực hiện",
    intro: (
      <>
        Các bề mặt giao thức của L4 (MCP, A2A, AG-UI, HTTP tương thích OpenAI) và định dạng
        khai báo YAML/JSON đều là các RFC ở trạng thái Reviewed. Milestone 6 đã hoàn tất.
        Milestone 7 (tuân thủ giao thức) đang được thực hiện — mã thật, đã kiểm thử tồn tại
        cho MCP, A2A, AG-UI, và các trình biên dịch khai báo — và một đợt kiểm toán cổng Phase
        G phát hiện tiêu chí thoát riêng của milestone này vẫn chưa đạt được, chủ yếu bị chặn
        bởi việc chưa có một network listener thật. Milestone 8-9 chưa bắt đầu.
      </>
    ),
    s1Eyebrow: "CONVENTIONS.md — quy tắc mã giao thức",
    s1Heading: (
      <>
        Một kiểu giao thức không bao giờ chạm tới <code>agentengine::core</code>
      </>
    ),
    s1Body: (
      <>
        Mỗi bề mặt bên dưới là một lớp phiên dịch (translation layer), không phải một bản sao
        thứ hai của engine: nó ánh xạ từ vựng trên dây (wire vocabulary) sang đúng cùng{" "}
        <code>AgentSession</code>/<code>ToolTable</code> mà bề mặt viết mã C++ đã dùng, tại
        ranh giới, chỉ theo một chiều.
      </>
    ),
    flowWireSub: "từ vựng trên dây — phương thức JSON-RPC, sự kiện SSE, động từ HTTP",
    flowTranslateArrow: "phiên dịch tại ranh giới — chỉ ở L4, không bao giờ rò rỉ vào trong",
    flowCoreSub: "AgentSession, ToolTable, ContextProvider — không có kiểu mcp:: hay a2a:: nào ở đây",
    s2Eyebrow: "Khoảng trống duy nhất mà MCP, A2A, và AG-UI đều có chung",
    s2Heading: "Logic request/response thật — chưa có socket nào bên dưới nó",
    s2Body: "Ba trong năm bề mặt bên dưới có cùng một hình dạng: một trình dịch thật, đã kiểm thử, nhận một giá trị request thật và trả về một giá trị response thật — nhưng chưa có gì đưa những giá trị đó lên một dây thật.",
    s2Note: (
      <>
        <strong>Cùng hình dạng, ba bề mặt.</strong> <code>McpServer::dispatch()</code> ở trên
        nhận một <code>JsonRpcRequest</code>, trả về một <code>JsonRpcResponse</code>.{" "}
        <code>A2aServer</code> nhận một message, trả về một task. <code>RunEventProjector</code>{" "}
        nhận luồng sự kiện run nội bộ thật, trả về các sự kiện wire của AG-UI. Cả ba đều có
        thật và đã kiểm thử trên các giá trị thật — điều còn thiếu trong mọi trường hợp đều
        giống nhau: một listener đọc byte từ socket và gọi trình dịch, theo đúng bảng trạng
        thái bên dưới.
      </>
    ),
    s2bEyebrow: "protocol/agui/sse.hpp · protocol/agui/projection.hpp · protocol/a2a/streaming.hpp",
    s2bHeading: (
      <>
        Một <code>RunEvent</code> nội bộ duy nhất, được chiếu (project) hai lần — AG-UI kết thúc
        run, A2A tạm dừng task
      </>
    ),
    s2bBody: (
      <>
        Bản thân việc đóng khung SSE đã được chứng minh, không chỉ mô tả suông:{" "}
        <code>to_sse_frame()</code> chính xác là <code>"data: " + json::dump(...) + "\n\n"</code>,
        và <code>tests/test_agui_sse.cpp:34-46</code> kiểm tra tiền tố <code>data: </code>, dấu
        kết thúc <code>\n\n</code>, và rằng các byte nằm giữa chúng phân tích được thành đúng sự
        kiện đã được chiếu thật — không bao giờ là một literal JSON viết tay.
      </>
    ),
    s2bCaption1: "Việc đóng khung, được chứng minh đầu-cuối",
    s2bCaption2: (
      <>
        Mọi loại <code>RunEvent</code> khác đều ánh xạ không phụ thuộc ngữ cảnh (đúng như comment
        đầu file của projection.hpp); ngoại lệ DUY NHẤT là <code>messageId</code> mà{" "}
        <code>RunEventProjector</code> tạo ra khi <code>model_call_started</code> và xóa đi khi{" "}
        <code>model_call_finished</code>, đóng khung <code>TEXT_MESSAGE_START</code>/
        <code>CONTENT</code>/<code>END</code> quanh phần văn bản được stream của một lượt model —
        được chứng minh đầu-cuối bởi <code>tests/test_rt_agui_projection.cpp:188-206</code>.
      </>
    ),
    s2bCaption3: (
      <>
        <code>input_required</code> buộc AG-UI phải KẾT THÚC run bằng{" "}
        <code>RunFinishedInterrupt</code> — nó không có sự kiện tạm dừng (013 §2.2). Enum{" "}
        <code>task_state</code> của A2A thì có: <code>A2aStreamProjector</code> chiếu đúng cùng một{" "}
        <code>RunEvent</code> <code>input_required</code> đó thành{" "}
        <code>TASK_STATE_INPUT_REQUIRED</code>, một trạng thái thật, KHÔNG kết thúc, được{" "}
        <code>tests/test_a2a_streaming.cpp:70-78</code> xác nhận bằng <code>is_terminal()</code> —
        task thực sự vẫn tiếp tục.
      </>
    ),
    s2bNote: (
      <>
        <strong>Cùng một nguồn, hai hình dạng.</strong> <code>RunEventProjector</code> và{" "}
        <code>A2aStreamProjector</code> đều tiêu thụ đúng cùng một luồng{" "}
        <code>RunEvent</code>/<code>WorkflowEvent</code> nội bộ — công việc của trang này dừng lại
        ở phép chiếu sang định dạng wire phía trên. Bản thân mô hình sự kiện (mọi loại{" "}
        <code>RunEvent</code>, <code>enable_event_stream()</code> trên một session hay một
        workflow, những gì superstep engine thực sự phát ra) được tài liệu hóa đầy đủ trên{" "}
        <a href={`${SITE_BASE}/api/events.html`}>trang Events</a>.
      </>
    ),
    s3Eyebrow: "015 — Declarative Agent Format",
    s3Heading: (
      <>
        YAML biên dịch thành đúng <code>AgentMetadata</code> mà C++ tạo ra
      </>
    ),
    s3Body: (
      <>
        I6 ("bề mặt khai báo và bề mặt gốc tương đương nhau") không phải một lời hứa thiết kế
        ở đây — <code>compile_agent_document()</code> nhắm tới đúng cùng một struct mà{" "}
        <code>register_agent&lt;A&gt;()</code> tạo ra, từng trường một.
      </>
    ),
    s4Eyebrow: "Cả năm bề mặt, theo từng trường",
    s4Heading: "Trạng thái, theo từng bề mặt",
    rfcLabel: "RFC",
  },
} as const;

export function ApiProtocolStatus() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  return (
    <section className="section" id="protocols">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <p>{t.intro}</p>
        </div>

        {/* ---- Where L4 sits ---------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="protocol-layering" style={{ marginBottom: 22 }}>
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow glass">
              <div className="flow-row">
                <div className="flow-node is-purple">
                  <div className="flow-node-title">MCP · A2A · AG-UI · OpenAI-HTTP</div>
                  <div className="flow-node-sub">{t.flowWireSub}</div>
                </div>
              </div>
              <div className="flow-arrow">{t.flowTranslateArrow}</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">agentengine::core</div>
                <div className="flow-node-sub">{t.flowCoreSub}</div>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>

        {/* ---- The repeated shape: real logic, no wire transport ---------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="protocol-transport-gap" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="mcp/server.hpp">{highlightCpp(mcpDispatchSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s2Note}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- AG-UI/A2A wire projection, in detail ------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="protocol-wire-projection" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2bEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2bHeading}</h3>
              <p>{t.s2bBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginBottom: 12 }}>{t.s2bCaption1}</p>
            <CodePanel filename="test_agui_sse.cpp">{highlightCpp(aguiSseFrameSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24, marginBottom: 12 }}>{t.s2bCaption2}</p>
            <CodePanel filename="test_rt_agui_projection.cpp">
              {highlightCpp(aguiProjectorMessageSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24, marginBottom: 12 }}>{t.s2bCaption3}</p>
            <CodePanel filename="test_a2a_streaming.cpp">
              {highlightCpp(a2aStreamProjectorInterruptSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s2bNote}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- Declarative compiler ---------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="declarative-compiler" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="agent_yaml_compiler.hpp">
              {highlightCpp(declarativeCompilerSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        {/* ---- Status table -------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="protocol-status-table" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup className="protocol-table">
          {protocolEntries[lang].map((p) => (
            <RevealItem key={p.id}>
              <div className="protocol-row glass">
                <div>
                  <div className="protocol-name">{p.name}</div>
                  <a className="protocol-rfc" href={p.rfcHref} target="_blank" rel="noreferrer">
                    {t.rfcLabel} {p.rfc}
                  </a>
                </div>
                <span className={`status-badge status-${p.status}`}>
                  {p.status === "real" ? tu.statusRealTested : tu.statusDesignedNotBuilt}
                </span>
                <p className="protocol-note">{p.note}</p>
              </div>
            </RevealItem>
          ))}
        </RevealGroup>
      </div>
    </section>
  );
}
