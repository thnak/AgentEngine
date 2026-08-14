import {
  attenuationExampleSnippet,
  capabilityDenialExampleSnippet,
  sandboxBackends,
  sandboxProfileSnippet,
  toolPipelineSteps,
  trustEntries,
} from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";
import type { Lang } from "../i18n/LanguageContext";

function entryById(lang: Lang, id: string) {
  const e = trustEntries[lang].find((entry) => entry.id === id);
  if (!e) throw new Error(`missing trust entry: ${id}`);
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
    eyebrow: "007/008 — Trust & Isolation (L1)",
    headingPrefix: "Capabilities are the only way to",
    headingHighlight: "reach an effect",
    intro: (
      <>
        I2 — no ambient authority — enforced by the type system, not by discipline: there is no
        constructor anywhere in this codebase that grants everything, only ones that narrow.
        Below is what that actually means for one tool call, from a tool's own declaration all
        the way to the sandbox that isolates its side effects.
      </>
    ),
    s1Eyebrow: "trust/capability.hpp",
    s1Heading: (
      <>
        A tool's <code>Capabilities&lt;...&gt;</code> is a ceiling, never a grant
      </>
    ),
    s1Body: (
      <>
        <code>WriteNoteTool</code> below declares it might need{" "}
        <code>FsWrite&lt;"work"&gt;</code>. Declaring that changes nothing about whether the
        tool can actually run — the only thing that authorizes a call is what the{" "}
        <em>session</em> was explicitly handed.
      </>
    ),
    declaresLabel: "What the tool declares",
    declares: (
      <>
        <code>Capabilities&lt;cap::decl::FsWrite&lt;"work"&gt;&gt;</code> — a compile-time
        ceiling, read by the pipeline to know what to check. The tool author states what it
        needs; that statement grants nothing.
      </>
    ),
    grantedLabel: "What the session was granted",
    granted: (
      <>
        <code>CapabilitySet::grant_root(...)</code> — called by the HOST, never reachable
        from anything derived from model output (I3). This is the only thing step 4/7 of
        the pipeline actually checks against.
      </>
    ),
    outcomeNote: (
      <>
        <strong>The same tool, the same call, two different outcomes</strong> — the ONLY
        variable across the two sessions above is what <code>CapabilitySet</code> was handed
        to <code>set_capabilities()</code>. Denial is an ordinary tool error fed back to the
        model, not a run-level failure: the run still converges, it just never invoked{" "}
        <code>write_note</code>'s real body.
      </>
    ),
    s2Eyebrow: "tool_pipeline.hpp — invoke_tool()",
    s2Heading: "Every tool call, real or model-requested, crosses the same ten steps",
    s2Body: (
      <>
        006 §3's own numbering — <code>invoke_tool()</code> is the ONE function every native
        tool call goes through, whether it came from the model or a workflow node. Two of
        these ten are the actual enforcement gates; the rest are resolution, bookkeeping, or
        a documented no-op.
      </>
    ),
    handleNote: (
      <>
        A bound capability handle from step 4/7 is valid for exactly ONE call — revoked
        unconditionally at step 10, success or failure, before the result is even normalized.
        A handle from call N is unusable in call N+1 by construction, not convention (this is
        what a real regression proves, not just what a comment claims).
      </>
    ),
    s3Eyebrow: "CapabilitySet::attenuate()",
    s3Heading: "Deriving a narrower set is the only direction that compiles",
    s3Body: (
      <>
        A parent <code>CapabilitySet</code> can hand out a strictly narrower child — a
        delegate agent, a spawned sub-worktree, a plugin — but only ever narrower. Asking for
        anything the parent doesn't already cover fails the WHOLE derivation closed, not
        partially.
      </>
    ),
    s3Note: (
      <>
        <strong>No convenience "give me everything" shortcut exists.</strong>{" "}
        <code>CapabilitySet()</code> default-constructs empty; <code>grant_root()</code> is
        the ONE explicitly-named, greppable entry point host policy calls. That's not a
        comment promising a property — it's the actual thing 007 §9's own falsifiable test
        gate checks.
      </>
    ),
    s4Eyebrow: "sandbox/sandbox.hpp",
    s4Heading: (
      <>
        <code>SandboxProfile&lt;Strict&gt;</code> — the strongest backend for THIS platform
      </>
    ),
    s4Body: (
      <>
        A capability grant says WHAT an effect may reach; a sandbox profile is about HOW
        isolated the process running it is. <code>P</code> in{" "}
        <code>SandboxProfile&lt;P&gt;</code> is either a concrete backend or the{" "}
        <code>Strict</code> selector, resolved at build/startup time by ranking every backend
        that actually supports the current platform.
      </>
    ),
    strengthLabel: "strength",
    coldStartLabel: "cold start",
  },
  vi: {
    eyebrow: "007/008 — Trust & Isolation (L1)",
    headingPrefix: "Capability là cách duy nhất để",
    headingHighlight: "chạm tới một effect",
    intro: (
      <>
        I2 — không có quyền hạn mặc nhiên — được hệ thống kiểu thực thi, không dựa vào kỷ luật
        lập trình: không có constructor nào trong codebase này cấp toàn quyền, chỉ có những
        constructor thu hẹp lại. Bên dưới là ý nghĩa thực sự của điều đó đối với một lệnh gọi
        tool, từ chính khai báo của tool cho tới sandbox cách ly các side effect của nó.
      </>
    ),
    s1Eyebrow: "trust/capability.hpp",
    s1Heading: (
      <>
        <code>Capabilities&lt;...&gt;</code> của một tool là một ceiling, không bao giờ là
        một sự cấp phát
      </>
    ),
    s1Body: (
      <>
        <code>WriteNoteTool</code> bên dưới khai báo rằng nó có thể cần{" "}
        <code>FsWrite&lt;"work"&gt;</code>. Việc khai báo đó không thay đổi gì về việc tool có
        thực sự chạy được hay không — thứ duy nhất ủy quyền cho một lệnh gọi là những gì{" "}
        <em>session</em> được trao một cách tường minh.
      </>
    ),
    declaresLabel: "Những gì tool khai báo",
    declares: (
      <>
        <code>Capabilities&lt;cap::decl::FsWrite&lt;"work"&gt;&gt;</code> — một ceiling tại
        thời điểm biên dịch, được pipeline đọc để biết cần kiểm tra gì. Người viết tool nêu
        ra nó cần gì; lời khai báo đó không cấp phát bất cứ điều gì.
      </>
    ),
    grantedLabel: "Những gì session được cấp",
    granted: (
      <>
        <code>CapabilitySet::grant_root(...)</code> — được HOST gọi, không bao giờ chạm tới
        được từ bất cứ thứ gì bắt nguồn từ đầu ra của model (I3). Đây là thứ DUY NHẤT mà bước
        4/7 của pipeline thực sự kiểm tra dựa vào.
      </>
    ),
    outcomeNote: (
      <>
        <strong>Cùng một tool, cùng một lệnh gọi, hai kết quả khác nhau</strong> — biến số
        DUY NHẤT giữa hai session ở trên là <code>CapabilitySet</code> nào được trao cho{" "}
        <code>set_capabilities()</code>. Từ chối là một lỗi tool bình thường được đưa trở lại
        cho model, không phải một thất bại ở cấp run: run vẫn hội tụ, chỉ là nó không bao giờ
        gọi thực thi phần thân thật của <code>write_note</code>.
      </>
    ),
    s2Eyebrow: "tool_pipeline.hpp — invoke_tool()",
    s2Heading: "Mọi lệnh gọi tool, dù thật hay do model yêu cầu, đều đi qua đúng mười bước",
    s2Body: (
      <>
        Đúng theo cách đánh số của 006 §3 — <code>invoke_tool()</code> là hàm DUY NHẤT mà mọi
        lệnh gọi tool gốc (native) đi qua, bất kể nó đến từ model hay từ một node workflow.
        Hai trong số mười bước này là các cổng thực thi thật; phần còn lại là phân giải, sổ
        sách (bookkeeping), hoặc một no-op đã được ghi nhận.
      </>
    ),
    handleNote: (
      <>
        Một capability handle đã ràng buộc từ bước 4/7 chỉ có hiệu lực cho đúng MỘT lệnh gọi
        — bị thu hồi vô điều kiện ở bước 10, dù thành công hay thất bại, trước cả khi kết quả
        được chuẩn hóa. Một handle từ lệnh gọi N không thể dùng được ở lệnh gọi N+1, do chính
        cấu trúc quyết định chứ không phải quy ước (đây là điều một hồi quy thật chứng minh,
        không chỉ là điều một comment tuyên bố).
      </>
    ),
    s3Eyebrow: "CapabilitySet::attenuate()",
    s3Heading: "Suy ra một tập hẹp hơn là hướng duy nhất biên dịch được",
    s3Body: (
      <>
        Một <code>CapabilitySet</code> cha có thể trao ra một tập con hẹp hơn một cách
        nghiêm ngặt — cho một agent ủy quyền, một sub-worktree được sinh ra, một plugin —
        nhưng luôn chỉ hẹp hơn. Yêu cầu bất cứ điều gì mà cha chưa bao phủ sẽ khiến TOÀN BỘ
        phép suy ra thất bại đóng, không phải một phần.
      </>
    ),
    s3Note: (
      <>
        <strong>Không tồn tại lối tắt tiện lợi kiểu "cho tôi mọi thứ".</strong>{" "}
        <code>CapabilitySet()</code> khởi tạo mặc định là rỗng; <code>grant_root()</code> là
        điểm vào DUY NHẤT được nêu tên tường minh, có thể grep được mà chính sách của host
        gọi tới. Đó không phải một comment hứa hẹn một tính chất — đó là điều mà chính cổng
        kiểm định có thể-bác-bỏ (falsifiable) của 007 §9 kiểm tra thật.
      </>
    ),
    s4Eyebrow: "sandbox/sandbox.hpp",
    s4Heading: (
      <>
        <code>SandboxProfile&lt;Strict&gt;</code> — backend mạnh nhất cho NỀN TẢNG NÀY
      </>
    ),
    s4Body: (
      <>
        Một sự cấp phát capability nói CÁI GÌ một effect có thể chạm tới; một sandbox profile
        nói về việc tiến trình chạy nó bị cách ly ĐẾN MỨC NÀO. <code>P</code> trong{" "}
        <code>SandboxProfile&lt;P&gt;</code> hoặc là một backend cụ thể, hoặc là bộ chọn{" "}
        <code>Strict</code>, được phân giải tại thời điểm build/khởi động bằng cách xếp hạng
        mọi backend thực sự hỗ trợ nền tảng hiện tại.
      </>
    ),
    strengthLabel: "độ mạnh",
    coldStartLabel: "cold start",
  },
} as const;

export function ApiTrustSandboxReference() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" id="trust-sandbox">
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

        {/* ---- 1. Declaration vs grant ----------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="capabilities" style={{ marginBottom: 22 }}>
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">{t.declaresLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.declares}</p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.grantedLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.granted}</p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/06_capabilities_and_denial.cpp">
              {highlightCpp(capabilityDenialExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.outcomeNote}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="capabilities" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. The ten-step pipeline ---------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="tool-pipeline" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {toolPipelineSteps[lang].map((s) => (
                <div
                  className={`ladder-step${s.index === "4 / 7" || s.index === "5" ? " is-current" : ""}`}
                  key={s.index}
                >
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
            <div className="flow-loop-note" style={{ marginTop: 14 }}>{t.handleNote}</div>
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Attenuation only ---------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="attenuation" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="capability.hpp">{highlightCpp(attenuationExampleSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s3Note}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. Sandbox profiles ----------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="sandbox-profile" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow-row">
              {sandboxBackends[lang].map((b) => (
                <div className={`flow-node${b.strength > 0 ? " is-purple" : ""}`} key={b.name}>
                  <div className="flow-node-title">{b.name}</div>
                  <div className="flow-node-sub">
                    {t.strengthLabel} {b.strength} · {b.platforms}
                    <br />
                    {t.coldStartLabel}: {b.coldStart} · {b.note}
                  </div>
                </div>
              ))}
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="sandbox.hpp">{highlightCpp(sandboxProfileSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <CiteLink id="sandbox-profile" />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
