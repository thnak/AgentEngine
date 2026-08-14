import { motion } from "framer-motion";
import { CONVENTIONS_URL, DECISIONS_URL, maturityLadder } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { RevealGroup, RevealItem } from "./Reveal";

const flowSteps = ["RFC", "red-team", "prove", "judge", "ADR"];

const copy = {
  en: {
    eyebrow: "Engineering discipline",
    headingPrefix: "Spec-driven,",
    headingHighlight: "not spec-decorated",
    flowAriaLabel: "Design process for contested or security-critical changes",
    quoteMain: "When code and a spec disagree, the spec wins.",
    quoteSub: "If the spec is wrong, fix the spec first — with an ADR — then the code.",
    body1: "24 RFC documents are the authoritative design; contested, hot-path, or security-critical decisions go through a",
    body2: "loop and land as an",
    adrLabel: "ADR",
    body3: "in",
    body4: "— an executed proof, not an ad-hoc change. The binding contract for how code gets written once implementation starts lives in",
    statRfcsLabel: "RFCs, numbered",
    statInvariantsLabel: "Core invariants",
    statAboveDraftLabel: "RFCs above Draft",
  },
  vi: {
    eyebrow: "Kỷ luật kỹ thuật",
    headingPrefix: "Hướng đặc tả,",
    headingHighlight: "không phải đặc tả trang trí",
    flowAriaLabel: "Quy trình thiết kế cho các thay đổi gây tranh cãi hoặc trọng yếu về bảo mật",
    quoteMain: "Khi mã và đặc tả mâu thuẫn nhau, đặc tả luôn thắng.",
    quoteSub: "Nếu đặc tả sai, hãy sửa đặc tả trước — bằng một ADR — rồi mới sửa mã.",
    body1: "24 tài liệu RFC là thiết kế có giá trị quyết định; các quyết định gây tranh cãi, nằm trên hot-path, hoặc trọng yếu về bảo mật đều phải đi qua vòng lặp",
    body2: "và trở thành một",
    adrLabel: "ADR",
    body3: "trong",
    body4: "— một minh chứng đã được thực thi, không phải một thay đổi tùy tiện. Hợp đồng ràng buộc về cách viết mã một khi việc triển khai bắt đầu nằm trong",
    statRfcsLabel: "RFC, đánh số",
    statInvariantsLabel: "Invariant cốt lõi",
    statAboveDraftLabel: "RFC vượt qua Draft",
  },
} as const;

export function SpecDriven() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" id="spec-driven">
      <div className="container spec-layout">
        <RevealGroup>
          <RevealItem>
            <span className="eyebrow">{t.eyebrow}</span>
          </RevealItem>
          <RevealItem>
            <h2 style={{ margin: "12px 0 22px", fontSize: "clamp(1.8rem, 3vw, 2.4rem)" }}>
              {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
            </h2>
          </RevealItem>

          <RevealItem>
            <div className="spec-flow" aria-label={t.flowAriaLabel}>
              {flowSteps.map((step, i) => (
                <motion.span
                  key={step}
                  className="spec-flow-step"
                  whileHover={{ y: -3, borderColor: "rgba(255,255,255,0.3)" }}
                >
                  {step}
                  {i < flowSteps.length - 1 ? (
                    <span className="spec-flow-arrow" aria-hidden="true">
                      {" "}
                      →
                    </span>
                  ) : null}
                </motion.span>
              ))}
            </div>
          </RevealItem>

          <RevealItem>
            <p className="spec-quote">
              {t.quoteMain}
              <br />
              <span>{t.quoteSub}</span>
            </p>
          </RevealItem>

          <RevealItem>
            <p style={{ color: "var(--text-dim)", lineHeight: 1.65, fontSize: "0.98rem" }}>
              {t.body1} <code>design → red-team → prove → judge</code> {t.body2}{" "}
              <a href={DECISIONS_URL} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                {t.adrLabel}
              </a>{" "}
              {t.body3} <code>decisions/</code> {t.body4}{" "}
              <a href={CONVENTIONS_URL} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                CONVENTIONS.md
              </a>
              .
            </p>
          </RevealItem>

          <RevealItem>
            <div className="stat-row">
              <div className="stat">
                <div className="num">30</div>
                <div className="label">{t.statRfcsLabel}</div>
              </div>
              <div className="stat">
                <div className="num">8</div>
                <div className="label">{t.statInvariantsLabel}</div>
              </div>
              <div className="stat">
                <div className="num">0</div>
                <div className="label">{t.statAboveDraftLabel}</div>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="glass" style={{ padding: "8px 8px", borderRadius: "var(--radius-lg)" }}>
              <div className="ladder" style={{ padding: "10px 18px" }}>
                {maturityLadder[lang].map((stage, i) => (
                  <div className={`ladder-step${i === 0 ? " is-current" : ""}`} key={stage.id}>
                    <span className="ladder-index">{String(i + 1).padStart(2, "0")}</span>
                    <div>
                      <h4>{stage.name}</h4>
                      <p>{stage.detail}</p>
                    </div>
                  </div>
                ))}
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
