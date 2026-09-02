import {
  pluginEntries,
  pluginHostLoadingCallSnippet,
  pluginHostSteps,
  pluginManifestSnippet,
  pluginStubTrapSnippet,
  pluginWitWorldSnippet,
} from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiDiagnosticNote } from "./ApiDiagnosticNote";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";
import type { Lang } from "../i18n/LanguageContext";

function entryById(lang: Lang, id: string) {
  const e = pluginEntries[lang].find((entry) => entry.id === id);
  if (!e) throw new Error(`missing plugin entry: ${id}`);
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
    eyebrow: "Plugin ABI — D2",
    headingPrefix: "Every plugin is a",
    headingHighlight: "signed WASM component",
    intro: "A plugin is not a shared library or a subprocess — it's a WASI 0.3 Component Model binary that exports exactly two functions and imports only what a manifest declares and an operator grants. One artifact runs bit-identically across the whole target platform set, capability-based by construction rather than by convention.",
    s1Eyebrow: "wit/ae-tool.wit — package ae:tool@1.0.0",
    s1Heading: "One ungated interface, five gated ones, two guest exports",
    s1Body: (
      <>
        <code>base</code> (logging/metrics) is always linked; it needs no capability.{" "}
        <code>fs</code>, <code>http</code>, <code>secrets</code>, <code>clock</code>, and{" "}
        <code>random</code> each require a <code>capability-handle</code> — a WIT{" "}
        <code>resource</code>, not a string or integer. This is I2's ABI-level shape: a guest
        can't fabricate a capability, only receive one the host constructed and transferred in.
      </>
    ),
    s1Note: (
      <>
        A component can't link against what its world doesn't export. <code>blob</code> and{" "}
        <code>tool-call</code> are declared in this same file, but the <code>tool</code> world
        doesn't import them. A component built expecting them fails Wasmtime's own
        component-type check before this host's manifest logic ever runs — a mechanical
        failure, not a promise kept by a runtime check further downstream.
      </>
    ),
    s1CiteNote: (
      <>
        009 §5 lists <code>blob</code> and <code>tool-call</code> as things a plugin may
        eventually reach for — contract completeness, not something the <code>tool</code>{" "}
        world grants today.
      </>
    ),
    s2Eyebrow: "src/backends/wasm/wasm_backend.hpp — the real host",
    s2Heading: "Four calls, and the manifest is checked before a component ever runs",
    s2Body: (
      <>
        Step 2 below enforces that mechanically: the manifest declares, the operator grants.
        The check runs against the component's actual compiled imports, not just what its
        manifest claims.
      </>
    ),
    s2BodyNote: <>009 §3 — "the manifest declares, the operator grants"</>,
    s2Note: (
      <>
        Every capability a component receives at step 4 is bound to exactly one{" "}
        <code>invoke</code> call. It's revoked unconditionally before that call returns,
        success or failure alike. No pooled instance, no capability that outlives its call.
      </>
    ),
    s2LoopCiteNote: <>Same per-call bind/revoke discipline as native tool calls — 006 §3 step 10</>,
    s2CallIntro: (
      <>
        The four calls above, as a real host issues them — against a compiled Rust fixture
        component, not a mock:
      </>
    ),
    s3Eyebrow: "Status — proven against a real compiled component",
    s3Heading: "What's real today, versus still a not-implemented trap",
    realLabel: "Real",
    real: (
      <>
        The whole load/verify/invoke pipeline above is proven against a compiled Rust
        component, including a capability-mismatch case denied at <code>load_component()</code>{" "}
        and a wall-clock-kill case where the component overruns its{" "}
        <code>wall_ms_limit</code>. <code>http-request</code> works today too.
      </>
    ),
    realCiteNote: <>http-request — ADR-011</>,
    stubbedLabel: "Still stubbed",
    stubbed: (
      <>
        <code>fs-read</code>, <code>fs-write</code>, and <code>resolve-secret</code> still
        trap as not-implemented inside the current minimal host. Each one is declared in the
        WIT world and gated correctly — the host-side implementation behind it isn't built
        yet.
      </>
    ),
    stubCiteNote: (
      <>
        Gated correctly in <code>ae-tool.wit</code>; the implementation behind the gate is
        still missing from <code>src/backends/wasm/wasm_backend.hpp</code>.
      </>
    ),
    stubProofIntro: (
      <>
        This isn't a design footnote — it's the exact assertion. The same probe helper this
        test file uses to prove the working gated callbacks also proves which ones don't work
        yet:
      </>
    ),
  },
  vi: {
    eyebrow: "Plugin ABI — D2",
    headingPrefix: "Mỗi plugin đều là một",
    headingHighlight: "WASM component đã ký",
    intro: "Một plugin không phải một shared library hay một subprocess — đó là một binary theo WASI 0.3 Component Model, export đúng hai hàm và chỉ import những gì một manifest khai báo và một operator cấp phát. Một artifact chạy giống hệt nhau từng bit trên toàn bộ tập nền tảng mục tiêu, dựa trên capability ngay từ cấu trúc chứ không phải theo quy ước.",
    s1Eyebrow: "wit/ae-tool.wit — package ae:tool@1.0.0",
    s1Heading: "Một interface không bị kiểm soát, năm interface bị kiểm soát, hai export của guest",
    s1Body: (
      <>
        <code>base</code> (logging/metrics) luôn được liên kết; nó không cần capability nào.{" "}
        <code>fs</code>, <code>http</code>, <code>secrets</code>, <code>clock</code>, và{" "}
        <code>random</code> mỗi cái đều đòi hỏi một <code>capability-handle</code> — một{" "}
        <code>resource</code> của WIT, không phải một chuỗi hay số nguyên. Đây chính là hình
        dạng ở cấp ABI của I2: một guest không thể tự tạo ra một capability, chỉ có thể nhận
        một capability mà host đã dựng và chuyển vào.
      </>
    ),
    s1Note: (
      <>
        Một component không thể liên kết với những gì world của nó không export.{" "}
        <code>blob</code> và <code>tool-call</code> được khai báo trong chính file này, nhưng
        world <code>tool</code> không import chúng. Một component được build với kỳ vọng có
        chúng sẽ thất bại ngay ở kiểm tra component-type của chính Wasmtime, trước cả khi
        logic manifest của host này chạy — một thất bại máy móc, không phải một lời hứa được
        giữ bởi một kiểm tra runtime ở tầng sâu hơn.
      </>
    ),
    s1CiteNote: (
      <>
        009 §5 liệt kê <code>blob</code> và <code>tool-call</code> như những thứ một plugin có
        thể vươn tới sau này — tính đầy đủ của hợp đồng, không phải thứ mà world{" "}
        <code>tool</code> cấp phát hôm nay.
      </>
    ),
    s2Eyebrow: "src/backends/wasm/wasm_backend.hpp — host thật",
    s2Heading: "Bốn lệnh gọi, và manifest được kiểm tra trước khi một component từng chạy",
    s2Body: (
      <>
        Bước 2 bên dưới thực thi điều đó một cách máy móc: manifest khai báo, operator cấp
        phát. Việc kiểm tra dựa trên các import thực tế đã biên dịch của component, không chỉ
        dựa trên những gì manifest của nó tuyên bố.
      </>
    ),
    s2BodyNote: <>009 §3 — "manifest khai báo, operator cấp phát"</>,
    s2Note: (
      <>
        Mỗi capability mà một component nhận được ở bước 4 chỉ được ràng buộc cho đúng một lệnh
        gọi <code>invoke</code>. Nó bị thu hồi vô điều kiện trước khi lệnh gọi đó trả về, dù
        thành công hay thất bại. Không có instance dùng chung theo pool, không có capability
        nào sống lâu hơn lệnh gọi của nó.
      </>
    ),
    s2LoopCiteNote: <>Cùng kỷ luật ràng buộc/thu hồi theo từng lệnh gọi như mọi lệnh gọi tool gốc — bước 10 của 006 §3</>,
    s2CallIntro: (
      <>
        Bốn lệnh gọi ở trên, đúng như cách một host thật phát ra — trên một component Rust
        fixture đã biên dịch, không phải một mock:
      </>
    ),
    s3Eyebrow: "Trạng thái — đã chứng minh trên một component biên dịch thật",
    s3Heading: "Cái gì thật ngay hôm nay, so với cái vẫn còn là bẫy not-implemented",
    realLabel: "Đã có thật",
    real: (
      <>
        Toàn bộ pipeline load/verify/invoke ở trên đã được chứng minh trên một component Rust
        biên dịch, bao gồm cả trường hợp không khớp capability bị từ chối tại{" "}
        <code>load_component()</code> và trường hợp bị kill vì vượt wall-clock, khi component
        vượt quá <code>wall_ms_limit</code> của nó. <code>http-request</code> cũng đã hoạt
        động hôm nay.
      </>
    ),
    realCiteNote: <>http-request — ADR-011</>,
    stubbedLabel: "Vẫn còn là stub",
    stubbed: (
      <>
        <code>fs-read</code>, <code>fs-write</code>, và <code>resolve-secret</code> vẫn còn
        trap là not-implemented bên trong host tối giản hiện tại. Mỗi cái đều được khai báo
        trong WIT world và được kiểm soát đúng cách — phần triển khai phía host đứng sau
        capability đó vẫn chưa được xây dựng.
      </>
    ),
    stubCiteNote: (
      <>
        Được kiểm soát đúng cách trong <code>ae-tool.wit</code>; phần triển khai đứng sau lớp
        kiểm soát đó vẫn còn thiếu trong <code>src/backends/wasm/wasm_backend.hpp</code>.
      </>
    ),
    stubProofIntro: (
      <>
        Đây không phải một chú thích thiết kế — mà là chính khẳng định đó. Cùng một helper
        probe mà file test này dùng để chứng minh các gated callback hoạt động cũng chứng minh
        những cái nào chưa hoạt động:
      </>
    ),
  },
} as const;

export function ApiPluginsReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  return (
    <section className="section" id="plugins">
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

        {/* ---- 1. The WIT world ------------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="wasm-plugin-abi" style={{ marginBottom: 22 }}>
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="ae-tool.wit">{highlightCpp(pluginWitWorldSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s1Note}</p>
            <ApiDiagnosticNote status="design">{t.s1CiteNote}</ApiDiagnosticNote>
          </RevealItem>

          <RevealItem>
            <CiteLink id="wasm-plugin-abi" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. The host lifecycle -------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="plugin-host-lifecycle" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
              <ApiDiagnosticNote>{t.s2BodyNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {pluginHostSteps[lang].map((s) => (
                <div className={`ladder-step${s.index === "2" ? " is-current" : ""}`} key={s.index}>
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
            <CodePanel filename="plugin.hpp">{highlightCpp(pluginManifestSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <div className="flow-loop-note" style={{ marginTop: 14 }}>{t.s2Note}</div>
            <ApiDiagnosticNote>{t.s2LoopCiteNote}</ApiDiagnosticNote>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 20, color: "var(--text-dim)", lineHeight: 1.65 }}>{t.s2CallIntro}</p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="tests/test_wasm_backend.cpp">
              {highlightCpp(pluginHostLoadingCallSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Status ---------------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="plugin-status" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.realLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.real}</p>
                <ApiDiagnosticNote status="real">{t.realCiteNote}</ApiDiagnosticNote>
              </div>
              <div className="compare-col is-before">
                <div className="compare-col-label">{t.stubbedLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.stubbed}</p>
                <ApiDiagnosticNote status="stub">{t.stubCiteNote}</ApiDiagnosticNote>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 20, color: "var(--text-dim)", lineHeight: 1.65 }}>{t.stubProofIntro}</p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="tests/test_wasm_backend.cpp">
              {highlightCpp(pluginStubTrapSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
