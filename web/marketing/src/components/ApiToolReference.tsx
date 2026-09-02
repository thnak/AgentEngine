import {
  capabilityGatedToolDeclSnippet,
  describedFieldSchemaSnippet,
  gh,
  jsonSchemaTypeMapping,
  minimalToolSnippet,
  multiToolRegistrationSnippet,
  toolDescriptorSnippet,
  toolSchemaOutput,
  toolSchemaSnippet,
  toolStaticMembers,
} from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiDiagnosticNote } from "./ApiDiagnosticNote";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

const copy = {
  en: {
    eyebrow: "006 — Tool and Function Plane",
    headingPrefix: "A tool's",
    headingHighlight: "full shape",
    headingSuffix: ", field by field",
    intro: (
      <>
        <code>Tool&lt;Derived, Policies...&gt;</code> fixes one declaration surface: a
        schema-typed name, description, an argument/reply pair, and an <code>invoke</code> the
        host reaches through a fixed pipeline. Every row below is a static member{" "}
        <code>Derived</code> must or may provide.
      </>
    ),
    introNote: <>pipeline walked through in 006 §3 and on the Capabilities &amp; Sandbox page</>,
    recoLabel: "Recommended default",
    recoBody: (
      <>
        Only five members are required — <code>name</code>, <code>description</code>,{" "}
        <code>Args</code>, <code>Reply</code>, and <code>invoke</code>. The three optional
        accessors below are already provided by <code>Tool&lt;Derived&gt;</code> itself with
        their conservative defaults (empty capabilities, <code>never_require</code> approval,{" "}
        <code>at_most_once</code> effect class) — a minimal tool doesn't write them at all,
        it only adds a <code>Capabilities&lt;...&gt;</code>/<code>Approval&lt;...&gt;</code>/
        <code>EffectClass&lt;...&gt;</code> policy the day it needs to override one:
      </>
    ),
    flowSchemaSub: "field names + C++ types only — no description/title/enum (see below)",
    flowSchemaLabel: "JSON Schema 2020-12 text",
    flowRegisterArrow: "register_agent<A>() compiles every declared tool",
    flowDescriptorSub: "one entry in the immutable per-run ToolTable (006 §6)",
    flowSentArrow: "sent to the model, shaped per backend",
    tableColumns: ["Member", "Type", "Required", "Notes"],
    noteTitle: "What a field can't carry: a description.",
    noteBody: (
      <>
        {" "}C++23 has no compile-time reflection (P2996 lands in C++26), so{" "}
        <code>AE_JSON_SCHEMA(Type, field1, field2, ...)</code> can only see the bare field{" "}
        <em>names</em> you list — there is no macro parameter, no doc-comment extraction, and no
        per-field <code>"description"</code>, <code>"title"</code>, or <code>enum</code> value
        list in the generated schema. A C++ enum flattens to a plain <code>"integer"</code>. A
        parameter that needs more explanation than its name and type carries that explanation in
        the tool's own top-level <code>description</code> string instead.
      </>
    ),
    typesEyebrow: "C++ type → JSON Schema type",
    typesHeading: (
      <>
        What <code>AE_JSON_SCHEMA</code> actually emits
      </>
    ),
    typesTableColumns: ["C++ field type", "JSON Schema", "Note"],
    exampleIntro: "A real, tested fixture — nested types, an array of objects, and an optional field all in one worked example:",
    exampleOutputIntro: (
      <>
        <code>json_schema_of&lt;SearchReply&gt;()</code> — the richest of the three, showing
        array-of-object nesting and the required/optional split at once:
      </>
    ),
    descriptorEyebrow: "Where the schema goes",
    descriptorHeading: (
      <>
        <code>ToolDescriptor</code> — one entry in the per-run tool table
      </>
    ),
    descriptorBody: (
      <>
        <code>register_agent&lt;A&gt;()</code> compiles every declared tool into this shape. It's
        a real external serialization path, not internal-only validation: Anthropic's client
        parses <code>args_schema_json</code> into <code>{"{name, description, input_schema}"}</code>{" "}
        for <code>/v1/messages</code>; OpenAI's parses it into{" "}
        <code>{'{type:"function", function:{name, description, parameters}}'}</code> for{" "}
        <code>/v1/chat/completions</code>. The exact JSON your tool's field types produced is
        what the model receives.
      </>
    ),
    descriptorNote: <>006 §6 — "resolved at run start into an immutable per-run tool table"</>,
    multiToolEyebrow: "tests/test_rt_agent_session_live_multitool_e2e.cpp",
    multiToolHeading: "Four real tools, one ToolTable, one real model",
    multiToolBody: (
      <>
        <code>from_tools&lt;Ts...&gt;</code> takes as many <code>Tool&lt;...&gt;</code> types as you
        give it — no arity limit in the API. This live-e2e fixture (runs when{" "}
        <code>AGENTENGINE_OPENROUTER_API_KEY</code> is set, skips otherwise) registers four tools
        into one table and makes two of them (<code>get_time</code>, <code>stock_price</code>)
        deliberate distractors — the model must never call them for this prompt. That's tool{" "}
        <em>selection</em> discipline, not just declaration.
      </>
    ),
    describedEyebrow: 'tests/test_json_schema_described.cpp — Described<T, "...">',
    describedHeading: (
      <>
        A second channel to carry <code>"description"</code>: <code>Described&lt;T, "..."&gt;</code>
      </>
    ),
    describedBody: (
      <>
        <code>Described&lt;T, "..."&gt;</code> (<code>core/json_schema.hpp</code>) is a separate
        channel: the description lives on the field's own type, not on the{" "}
        <code>AE_JSON_SCHEMA</code> macro, so the macro's bare-name limit above doesn't apply to
        it. The description survives both Anthropic's and OpenAI's <code>translate_tool</code>{" "}
        byte-for-byte, from a real <code>ToolDescriptor</code>.
      </>
    ),
    capabilityGatedEyebrow: "examples/06_capabilities_and_denial.cpp — 007 §9, I2",
    capabilityGatedHeading: (
      <>
        Declaring a capability ceiling: <code>Capabilities&lt;cap::decl::FsWrite&lt;"work"&gt;&gt;</code>
      </>
    ),
    capabilityGatedBody: (
      <>
        This is a declared ceiling — what the tool might need — not a grant. What actually
        authorizes a call is the <code>CapabilitySet</code>{" "}
        <code>session.set_capabilities()</code> installs, checked fresh on every call. An empty{" "}
        <code>CapabilitySet</code> denies the call before <code>invoke()</code> runs; granting{" "}
        <code>FsWrite{'{"work"}'}</code> lets the same call through the same pipeline.
      </>
    ),
    capabilityGatedNote: (
      <>I2 — full two-session proof on the Capabilities &amp; Sandbox page</>
    ),
  },
  vi: {
    eyebrow: "006 — Tool and Function Plane",
    headingPrefix: "Hình dạng",
    headingHighlight: "đầy đủ",
    headingSuffix: "của một tool, theo từng trường",
    intro: (
      <>
        <code>Tool&lt;Derived, Policies...&gt;</code> chốt một bề mặt khai báo duy nhất: một
        tên định kiểu theo schema, description, một cặp argument/reply, và một <code>invoke</code>{" "}
        mà host chạm tới qua một pipeline cố định. Mỗi hàng bên dưới là một thành viên static
        mà <code>Derived</code> phải hoặc có thể cung cấp.
      </>
    ),
    introNote: <>pipeline được trình bày chi tiết ở 006 §3 và trang Capability &amp; Sandbox</>,
    recoLabel: "Mặc định khuyến nghị",
    recoBody: (
      <>
        Chỉ năm thành viên là bắt buộc — <code>name</code>, <code>description</code>,{" "}
        <code>Args</code>, <code>Reply</code>, và <code>invoke</code>. Ba accessor tùy chọn bên
        dưới đã được chính <code>Tool&lt;Derived&gt;</code> cung cấp sẵn với giá trị mặc định
        thận trọng (capabilities rỗng, phê duyệt <code>never_require</code>, effect class{" "}
        <code>at_most_once</code>) — một tool tối giản không cần viết chúng ra, chỉ thêm policy{" "}
        <code>Capabilities&lt;...&gt;</code>/<code>Approval&lt;...&gt;</code>/
        <code>EffectClass&lt;...&gt;</code> vào ngày nào đó cần ghi đè một trong số chúng:
      </>
    ),
    flowSchemaSub: "chỉ có tên trường + kiểu C++ — không có description/title/enum (xem bên dưới)",
    flowSchemaLabel: "Văn bản JSON Schema 2020-12",
    flowRegisterArrow: "register_agent<A>() biên dịch mọi tool đã khai báo",
    flowDescriptorSub: "một mục trong ToolTable bất biến theo từng lần chạy (006 §6)",
    flowSentArrow: "gửi tới model, định hình theo từng backend",
    tableColumns: ["Thành viên", "Kiểu", "Bắt buộc", "Ghi chú"],
    noteTitle: "Một trường không thể mang theo: một mô tả (description).",
    noteBody: (
      <>
        {" "}C++23 chưa có reflection tại thời điểm biên dịch (P2996 sẽ có ở C++26), nên{" "}
        <code>AE_JSON_SCHEMA(Type, field1, field2, ...)</code> chỉ nhìn thấy được các{" "}
        <em>tên</em> trường trần trụi mà bạn liệt kê — không có tham số macro nào, không có
        trích xuất doc-comment nào, và không có danh sách giá trị <code>"description"</code>,{" "}
        <code>"title"</code>, hay <code>enum</code> theo từng trường trong schema được sinh ra.
        Một enum trong C++ được làm phẳng thành một <code>"integer"</code> đơn thuần. Một
        tham số cần giải thích nhiều hơn tên và kiểu của nó thì lời giải thích đó nằm trong
        chuỗi <code>description</code> ở cấp cao nhất của chính tool.
      </>
    ),
    typesEyebrow: "Kiểu C++ → Kiểu JSON Schema",
    typesHeading: (
      <>
        <code>AE_JSON_SCHEMA</code> thực sự phát ra những gì
      </>
    ),
    typesTableColumns: ["Kiểu trường C++", "JSON Schema", "Ghi chú"],
    exampleIntro: "Một fixture thật, đã kiểm thử — kiểu lồng nhau, một mảng đối tượng, và một trường tùy chọn, tất cả trong một ví dụ minh họa:",
    exampleOutputIntro: (
      <>
        <code>json_schema_of&lt;SearchReply&gt;()</code> — phong phú nhất trong ba ví dụ, cho
        thấy đồng thời việc lồng mảng-đối-tượng và sự phân tách required/optional:
      </>
    ),
    descriptorEyebrow: "Schema đi về đâu",
    descriptorHeading: (
      <>
        <code>ToolDescriptor</code> — một mục trong bảng tool theo từng lần chạy
      </>
    ),
    descriptorBody: (
      <>
        <code>register_agent&lt;A&gt;()</code> biên dịch mọi tool đã khai báo thành hình dạng
        này. Đây là một đường tuần tự hóa (serialization) hướng ra bên ngoài thật, không
        chỉ là xác thực nội bộ: client của Anthropic phân tích{" "}
        <code>args_schema_json</code> thành <code>{"{name, description, input_schema}"}</code>{" "}
        cho <code>/v1/messages</code>; client của OpenAI phân tích nó thành{" "}
        <code>{'{type:"function", function:{name, description, parameters}}'}</code> cho{" "}
        <code>/v1/chat/completions</code>. Đúng JSON mà các kiểu trường của tool bạn tạo ra
        chính là những gì model nhận được.
      </>
    ),
    descriptorNote: (
      <>006 §6 — "được phân giải khi bắt đầu chạy thành một bảng tool bất biến theo từng lần chạy"</>
    ),
    multiToolEyebrow: "tests/test_rt_agent_session_live_multitool_e2e.cpp",
    multiToolHeading: "Bốn tool thật, một ToolTable, một model thật",
    multiToolBody: (
      <>
        <code>from_tools&lt;Ts...&gt;</code> nhận bao nhiêu kiểu <code>Tool&lt;...&gt;</code> cũng
        được — không có giới hạn nào ở API. Fixture live-e2e này (chạy khi có{" "}
        <code>AGENTENGINE_OPENROUTER_API_KEY</code>, bỏ qua nếu không) đăng ký bốn tool vào một
        bảng và đặt hai trong số đó (<code>get_time</code>, <code>stock_price</code>) làm những
        "phần gây nhiễu" có chủ đích — model không bao giờ được gọi chúng cho prompt này. Đó là
        kỷ luật <em>chọn</em> tool thật sự, không chỉ đơn thuần là khai báo.
      </>
    ),
    describedEyebrow: 'tests/test_json_schema_described.cpp — Described<T, "...">',
    describedHeading: (
      <>
        Một kênh khác để mang <code>"description"</code>: <code>Described&lt;T, "..."&gt;</code>
      </>
    ),
    describedBody: (
      <>
        <code>Described&lt;T, "..."&gt;</code> (<code>core/json_schema.hpp</code>) là một kênh
        tách biệt: mô tả nằm ngay trên kiểu của trường, không phải trên macro{" "}
        <code>AE_JSON_SCHEMA</code>, nên giới hạn tên-trần-trụi của macro ở trên không áp dụng
        cho nó. Mô tả sống sót qua <code>translate_tool</code> của cả Anthropic lẫn OpenAI,
        byte-for-byte, từ một <code>ToolDescriptor</code> thật.
      </>
    ),
    capabilityGatedEyebrow: "examples/06_capabilities_and_denial.cpp — 007 §9, I2",
    capabilityGatedHeading: (
      <>
        Khai báo một trần capability: <code>Capabilities&lt;cap::decl::FsWrite&lt;"work"&gt;&gt;</code>
      </>
    ),
    capabilityGatedBody: (
      <>
        Đây là một trần đã khai báo — thứ tool có thể cần — không phải một cấp phát. Điều thực
        sự cấp quyền cho một lệnh gọi là <code>CapabilitySet</code> mà{" "}
        <code>session.set_capabilities()</code> đã cài đặt, được kiểm tra lại mới mỗi lệnh gọi.
        Một <code>CapabilitySet</code> rỗng từ chối lệnh gọi trước khi <code>invoke()</code> chạy;
        cấp <code>FsWrite{'{"work"}'}</code> cho phép chính lệnh gọi đó chạy qua chính pipeline đó.
      </>
    ),
    capabilityGatedNote: (
      <>I2 — chứng minh hai-session đầy đủ ở trang Capability &amp; Sandbox</>
    ),
  },
} as const;

export function ApiToolReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  return (
    <section className="section" id="tool-reference">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
            {t.headingSuffix}
          </h2>
          <p>{t.intro}</p>
          <ApiDiagnosticNote>{t.introNote}</ApiDiagnosticNote>
        </div>

        <RevealGroup>
          <RevealItem>
            <div className="gs-recommend">
              <span className="gs-recommend-label">{t.recoLabel}</span>
              <p>{t.recoBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="web_search_tool.hpp">{highlightCpp(minimalToolSnippet)}</CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="flow glass" style={{ marginBottom: 48 }}>
              <div className="flow-node is-purple">
                <div className="flow-node-title">Tool&lt;Derived, Policies...&gt;</div>
                <div className="flow-node-sub">Args/Reply nested types + AE_JSON_SCHEMA(Type, field1, field2, ...)</div>
              </div>
              <div className="flow-arrow">args_schema() / reply_schema() — json_schema_of&lt;T&gt;()</div>
              <div className="flow-node">
                <div className="flow-node-title">{t.flowSchemaLabel}</div>
                <div className="flow-node-sub">{t.flowSchemaSub}</div>
              </div>
              <div className="flow-arrow">{t.flowRegisterArrow}</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">ToolDescriptor</div>
                <div className="flow-node-sub">{t.flowDescriptorSub}</div>
              </div>
              <div className="flow-arrow">{t.flowSentArrow}</div>
              <div className="flow-row">
                <div className="flow-node">
                  <div className="flow-node-title">Anthropic</div>
                  <div className="flow-node-sub">{"{name, description, input_schema}"}</div>
                </div>
                <div className="flow-node">
                  <div className="flow-node-title">OpenAI</div>
                  <div className="flow-node-sub">{'{type:"function", function:{name, description, parameters}}'}</div>
                </div>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <ApiTable
              columns={[...t.tableColumns]}
              templateColumns="1.3fr 1.6fr 0.7fr 2.4fr"
              rows={toolStaticMembers[lang].map((f) => [
                <code key="name">{f.name}</code>,
                <code key="type">{f.type}</code>,
                f.required ? tu.required : tu.optional,
                f.notes,
              ])}
            />
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="gs-note" style={{ marginTop: 28, borderLeftColor: "var(--accent-pink)" }}>
              <strong>{t.noteTitle}</strong>
              {t.noteBody}
              <div style={{ marginTop: 8 }}>
                <a href={gh("include/agentengine/core/json_schema.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/core/json_schema.hpp:5-19
                </a>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="tool-described-field">
              <span className="eyebrow">{t.describedEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.describedHeading}</h3>
              <p>{t.describedBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="test_json_schema_described.cpp">{highlightCpp(describedFieldSchemaSnippet)}</CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="tool-schema-types">
              <span className="eyebrow">{t.typesEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.typesHeading}</h3>
            </div>
            <ApiTable
              columns={[...t.typesTableColumns]}
              templateColumns="1.1fr 1.6fr 2.3fr"
              rows={jsonSchemaTypeMapping[lang].map((tt) => [
                <code key="cpp">{tt.cpp}</code>,
                <code key="json">{tt.json}</code>,
                tt.note,
              ])}
            />
          </RevealItem>
        </RevealGroup>

        <RevealGroup className="spec-layout anchor-target" style={{ marginTop: 48 }} id="tool-schema-example">
          <RevealGroup>
            <RevealItem>
              <p style={{ color: "var(--text-dim)", lineHeight: 1.65, marginBottom: 16 }}>
                {t.exampleIntro}
              </p>
            </RevealItem>
            <RevealItem>
              <CodePanel filename="test_tool_json_schema.cpp">{highlightCpp(toolSchemaSnippet)}</CodePanel>
            </RevealItem>
          </RevealGroup>

          <RevealGroup>
            <RevealItem>
              <p style={{ color: "var(--text-dim)", lineHeight: 1.65, marginBottom: 16 }}>
                {t.exampleOutputIntro}
              </p>
            </RevealItem>
            <RevealItem>
              <CodePanel filename="SearchReply.schema.json">{toolSchemaOutput}</CodePanel>
            </RevealItem>
          </RevealGroup>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="tool-descriptor">
              <span className="eyebrow">{t.descriptorEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.descriptorHeading}</h3>
              <p>{t.descriptorBody}</p>
              <ApiDiagnosticNote>{t.descriptorNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="tool_pipeline.hpp">{highlightCpp(toolDescriptorSnippet)}</CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="tool-multi-registration">
              <span className="eyebrow">{t.multiToolEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.multiToolHeading}</h3>
              <p>{t.multiToolBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="test_rt_agent_session_live_multitool_e2e.cpp">
              {highlightCpp(multiToolRegistrationSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="tool-capability-gated">
              <span className="eyebrow">{t.capabilityGatedEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.capabilityGatedHeading}</h3>
              <p>{t.capabilityGatedBody}</p>
              <ApiDiagnosticNote>{t.capabilityGatedNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="examples/06_capabilities_and_denial.cpp">
              {highlightCpp(capabilityGatedToolDeclSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
