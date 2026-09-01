import {
  builtinToolEntries,
  firstPartyCatalogExampleSnippet,
  firstPartySkillsCatalog,
  gh,
} from "../data/apiContent";
import { SITE_BASE } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiTable } from "./ApiTable";
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
    eyebrow: "009 §7/§8f — the generic tool catalog",
    headingPrefix: "What's actually shipped,",
    headingHighlight: "not just the mechanism",
    intro: (
      <>
        The <a href={`${SITE_BASE}/api/tool.html`}>Tool page</a> documents the abstract{" "}
        <code>Tool&lt;Derived, Policies...&gt;</code> conformance mechanism; the{" "}
        <a href={`${SITE_BASE}/api/skill.html`}>Skill page</a> documents the abstract{" "}
        <code>SKILL.md</code>/<code>SkillSource</code> loading mechanism. Neither one walks
        through what this engine actually SHIPS with that mechanism. This page does: the five
        real, tested <code>Tool&lt;&gt;</code> conformers under{" "}
        <code>include/agentengine/tools/</code> (009 §7's "generic tool catalog" — source-
        agnostic, capability-crossing operations an application would otherwise reinvent), and
        the six real, compiled-in skills (009 §8f) that teach an agent when and how to use them
        and the rest of this engine's surface.
      </>
    ),
    s1Eyebrow: "include/agentengine/tools/ — five real Tool<> conformers",
    s1Heading: "The tools, field by field",
    s1Body: (
      <>
        All five declare an empty static <code>Capabilities&lt;&gt;</code> ceiling — a per-call{" "}
        <code>url</code>/<code>path</code> target can't be fixed at the type level (006 §1) — and
        instead check a real, dynamic capability inside <code>invoke()</code> against{" "}
        <code>EffectContext::capabilities</code> before touching the network or filesystem. Still
        I2-compliant: authority is still gated by an explicit, pre-granted capability, checked
        before any effect — only the pipeline step that checks it moves from static to dynamic.
      </>
    ),
    predecessorNote: (
      <>
        <code>read_sandbox_file</code> (<code>tools/read_sandbox_file.hpp</code>) predates{" "}
        <code>read_content</code>'s own <code>path</code> source and proved the same sandbox-
        mount seam first — still present, not yet judged redundant, not documented separately
        here.
      </>
    ),
    s2Eyebrow: "core/builtin_skills.hpp + tools/extract_pdf_text.hpp — six real skills",
    s2Heading: "The skills, and what each one is gated behind",
    s2Body: (
      <>
        Every skill below is <code>InlineSkillSource</code>-backed — compiled into the binary as
        a raw <code>SKILL.md</code> string literal, never a loose file a deployment could omit or
        move. The first five ship via <code>make_builtin_skills_source()</code>; the sixth ships
        via <code>make_extracting_document_text_skill_source()</code> and is only buildable at
        all alongside the four PDF tools it teaches. Full mechanism detail (
        <code>InlineSkillSource</code>'s two constructors, <code>SkillsProvider</code> mounting,{" "}
        <code>allowed-tools</code> enforcement) lives on the{" "}
        <a href={`${SITE_BASE}/api/skill.html`}>Skill page</a> — this table is just the roster.
      </>
    ),
    skillsTableColumns: ["Skill", "Teaches", "Ships in"],
    s3Eyebrow: "A complete, worked example",
    s3Heading: "Declaring the tools, building the skill sources, in one place",
    s3Body: (
      <>
        Grounded in the real static members and constructors documented above and on the{" "}
        <a href={`${SITE_BASE}/api/agent.html`}>Agent</a> and{" "}
        <a href={`${SITE_BASE}/api/skill.html`}>Skill</a> pages — nothing invented. This snippet
        stops at building the tool declaration and the skill-source list, the two pieces
        specific to a first-party catalog; wiring the result into a running{" "}
        <code>AgentSession</code> is the general{" "}
        <code>ComposedContextProvider</code> pattern the{" "}
        <a href={`${SITE_BASE}/api/runtime.html`}>AgentSession &amp; ChatClient page</a> already
        covers in full, reused unchanged here.
      </>
    ),
    statusNote: (
      <>
        <strong>Status: every tool and skill on this page is real, compiled, and tested.</strong>{" "}
        The four PDF tools plus <code>extracting-document-text</code> exist only in a target
        configured with <code>-DAGENTENGINE_WITH_PDF=ON</code> (PDFium fetched as a pinned,
        checksummed prebuilt release — see <code>CMakeLists.txt</code>'s own option comment); a
        default build has <code>read_content</code> and the five generic skills only.{" "}
        <code>ADR-106</code> is the licensing decision behind the PDFium choice (poppler is GPL,
        mupdf is AGPL — both ruled out against this project's own locked MIT license).{" "}
        <code>docs/planning/pdf-text-extraction-design-draft.md</code> is the PDF tools' full
        sandboxing design, six red-team rounds. Each tool/skill's own test file is linked from its
        entry above.
      </>
    ),
  },
  vi: {
    eyebrow: "009 §7/§8f — danh mục tool chung",
    headingPrefix: "Những gì thực sự đã phát hành,",
    headingHighlight: "không chỉ là cơ chế",
    intro: (
      <>
        <a href={`${SITE_BASE}/api/tool.html`}>Trang Tool</a> tài liệu hóa cơ chế tuân theo{" "}
        <code>Tool&lt;Derived, Policies...&gt;</code> trừu tượng; <a href={`${SITE_BASE}/api/skill.html`}>trang Skill</a> tài liệu hóa cơ chế nạp{" "}
        <code>SKILL.md</code>/<code>SkillSource</code> trừu tượng. Không trang nào trong hai
        trang đó đi qua những gì engine này THỰC SỰ phát hành cùng với cơ chế đó. Trang này thì
        có: năm conformer <code>Tool&lt;&gt;</code> thật, đã kiểm thử dưới{" "}
        <code>include/agentengine/tools/</code> ("danh mục tool chung" của 009 §7 — các thao tác
        vượt ranh giới capability, độc lập nguồn, mà một ứng dụng nếu không sẽ phải tự làm lại),
        và sáu skill thật, biên dịch sẵn (009 §8f) dạy agent khi nào và cách dùng chúng cùng phần
        còn lại của bề mặt engine này.
      </>
    ),
    s1Eyebrow: "include/agentengine/tools/ — năm conformer Tool<> thật",
    s1Heading: "Các tool, theo từng trường một",
    s1Body: (
      <>
        Cả năm đều khai báo trần capability tĩnh <code>Capabilities&lt;&gt;</code> RỖNG — một
        target <code>url</code>/<code>path</code> theo từng lệnh gọi không thể cố định ở cấp kiểu
        (006 §1) — và thay vào đó kiểm tra một capability động, có thật bên trong{" "}
        <code>invoke()</code> dựa trên <code>EffectContext::capabilities</code> trước khi chạm
        vào mạng hay filesystem. Vẫn tuân thủ I2: quyền hạn vẫn bị kiểm soát bởi một capability
        tường minh, đã được cấp trước, kiểm tra trước bất kỳ effect nào — chỉ có bước trong
        pipeline thực hiện kiểm tra đó chuyển từ tĩnh sang động.
      </>
    ),
    predecessorNote: (
      <>
        <code>read_sandbox_file</code> (<code>tools/read_sandbox_file.hpp</code>) có trước nguồn{" "}
        <code>path</code> của chính <code>read_content</code> và đã chứng minh cùng một seam
        sandbox-mount trước tiên — vẫn còn tồn tại, chưa được đánh giá là dư thừa, không được tài
        liệu hóa riêng ở đây.
      </>
    ),
    s2Eyebrow: "core/builtin_skills.hpp + tools/extract_pdf_text.hpp — sáu skill thật",
    s2Heading: "Các skill, và mỗi cái bị gate đằng sau điều gì",
    s2Body: (
      <>
        Mọi skill bên dưới đều dùng <code>InlineSkillSource</code> — biên dịch thẳng vào binary
        dưới dạng string literal <code>SKILL.md</code> thô, không bao giờ là một file rời rạc mà
        một deployment có thể vô tình bỏ sót hay di chuyển. Năm skill đầu phát hành qua{" "}
        <code>make_builtin_skills_source()</code>; skill thứ sáu phát hành qua{" "}
        <code>make_extracting_document_text_skill_source()</code> và chỉ build được cùng với bốn
        tool PDF mà nó dạy. Chi tiết đầy đủ về cơ chế (hai constructor của{" "}
        <code>InlineSkillSource</code>, việc mount của <code>SkillsProvider</code>, thực thi{" "}
        <code>allowed-tools</code>) nằm ở{" "}
        <a href={`${SITE_BASE}/api/skill.html`}>trang Skill</a> — bảng này chỉ là danh sách.
      </>
    ),
    skillsTableColumns: ["Skill", "Dạy điều gì", "Có trong"],
    s3Eyebrow: "Một ví dụ minh họa hoàn chỉnh",
    s3Heading: "Khai báo tool, xây danh sách nguồn skill, trong cùng một chỗ",
    s3Body: (
      <>
        Được đặt nền trên các thành viên static và constructor thật đã tài liệu hóa ở trên và
        trên trang <a href={`${SITE_BASE}/api/agent.html`}>Agent</a> cùng{" "}
        <a href={`${SITE_BASE}/api/skill.html`}>Skill</a> — không có gì bịa ra. Đoạn mã này dừng
        lại ở việc xây khai báo tool và danh sách nguồn skill, hai phần riêng có của một danh mục
        chính chủ; đấu nối kết quả đó vào một <code>AgentSession</code> đang chạy là mẫu{" "}
        <code>ComposedContextProvider</code> tổng quát mà{" "}
        <a href={`${SITE_BASE}/api/runtime.html`}>trang AgentSession &amp; ChatClient</a> đã đi
        qua đầy đủ, được tái sử dụng nguyên vẹn ở đây.
      </>
    ),
    statusNote: (
      <>
        <strong>Trạng thái: mọi tool và skill trên trang này đều thật, đã biên dịch, và đã kiểm
        thử.</strong> Bốn tool PDF cộng <code>extracting-document-text</code> chỉ tồn tại trong
        một bản build được cấu hình với <code>-DAGENTENGINE_WITH_PDF=ON</code> (PDFium được tải
        về dưới dạng một bản phát hành dựng sẵn, đã ghim phiên bản và kiểm tra checksum — xem
        chú thích của chính option đó trong <code>CMakeLists.txt</code>); một bản build mặc định
        chỉ có <code>read_content</code> và năm skill chung. <code>ADR-106</code> là quyết định
        về giấy phép đứng sau lựa chọn PDFium (poppler là GPL, mupdf là AGPL — cả hai đều bị loại
        vì xung đột với giấy phép MIT đã khóa của dự án này).{" "}
        <code>docs/planning/pdf-text-extraction-design-draft.md</code> là thiết kế sandbox hóa
        đầy đủ của các tool PDF, sáu vòng red-team. File test riêng của mỗi tool/skill được liên
        kết từ mục tương ứng ở trên.
      </>
    ),
  },
} as const;

export function ApiBuiltinToolsReference() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" id="catalog">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {ui[lang].statusRealTested}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="catalog-tools">
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="doc-entries">
              {builtinToolEntries[lang].map((e) => (
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

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.predecessorNote}</p>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="catalog-skills">
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.skillsTableColumns]}
              templateColumns="1.5fr 2.7fr 1.1fr"
              rows={firstPartySkillsCatalog[lang].map((s) => [
                <code key="name">{s.name}</code>,
                s.teaches,
                s.gate,
              ])}
            />
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="catalog-example">
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="doc_agent.hpp">
              {highlightCpp(firstPartyCatalogExampleSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div
              className="gs-note anchor-target"
              id="catalog-status"
              style={{ marginTop: 40, borderLeftColor: "var(--accent-pink)" }}
            >
              {t.statusNote}
              <div style={{ marginTop: 8, display: "flex", gap: 16, flexWrap: "wrap" }}>
                <a href={gh("include/agentengine/tools/read_content.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/tools/read_content.hpp
                </a>
                <a href={gh("include/agentengine/core/builtin_skills.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/core/builtin_skills.hpp
                </a>
                <a href={gh("decisions/ADR-106-document-extraction-catalog-candidate-license-substitution.md")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  ADR-106
                </a>
                <a href={gh("docs/planning/pdf-text-extraction-design-draft.md")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  pdf-text-extraction-design-draft.md
                </a>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
