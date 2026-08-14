import { motion } from "framer-motion";
import { layers } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { RevealGroup, RevealItem } from "./Reveal";

const copy = {
  en: {
    eyebrow: "Layering",
    headingPrefix: "Five layers,",
    headingHighlight: "one direction",
    headingSuffix: "of dependency",
    body:
      "A layer may depend only on layers below it, and only through the seam that layer publishes. L4 has no privileged access to L0 — L1 is the only layer that may hold an OS capability; everything above it holds handles.",
    rule:
      "AgentEngine owns its own runtime substrate — a thread pool, a single-executor guard, a session/append-log durability seam, a circuit breaker, and a channel/stream backend — no distributed cluster, no actor mailbox.",
    dependsArrow: "may depend on",
    noUpward: "no upward access — L4 never reaches L0 directly",
    capBadge: "sole holder of an OS capability",
  },
  vi: {
    eyebrow: "Phân lớp",
    headingPrefix: "Năm lớp,",
    headingHighlight: "một chiều",
    headingSuffix: "phụ thuộc duy nhất",
    body:
      "Một lớp chỉ được phép phụ thuộc vào các lớp bên dưới nó, và chỉ thông qua ranh giới (seam) mà lớp đó công bố. L4 không có quyền truy cập đặc quyền vào L0 — L1 là lớp duy nhất được phép nắm giữ một capability của hệ điều hành; mọi lớp bên trên chỉ nắm giữ handle.",
    rule:
      "AgentEngine tự sở hữu nền runtime của riêng mình — một thread pool, một cơ chế bảo vệ single-executor, một ranh giới bền vững session/append-log, một circuit breaker, và một backend channel/stream — không có cluster phân tán, không có actor mailbox.",
    dependsArrow: "được phép phụ thuộc vào",
    noUpward: "không truy cập ngược lên — L4 không bao giờ chạm trực tiếp vào L0",
    capBadge: "lớp duy nhất nắm giữ capability của hệ điều hành",
  },
} as const;

export function Architecture() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" id="architecture">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span> {t.headingSuffix}
          </h2>
          <p>{t.body}</p>
        </div>

        <RevealGroup>
          <RevealItem>
            <div className="arch-diagram">
              <div className="arch-noaccess" aria-hidden="true">
                <span className="arch-noaccess-line is-top" />
                <span className="arch-noaccess-label">{t.noUpward}</span>
                <span className="arch-noaccess-line" />
              </div>
              <div className="arch-stack">
                {layers[lang].map((l, i) => (
                  <div key={l.id}>
                    <motion.div
                      className={`arch-layer glass is-${l.id}`}
                      whileHover={{ x: 6, borderColor: "rgba(255,255,255,0.22)" }}
                      transition={{ type: "spring", stiffness: 260, damping: 20 }}
                    >
                      <span className="arch-layer-badge">{l.level}</span>
                      <span className="arch-layer-body">
                        <span className="arch-layer-name">{l.name}</span>
                        <span className="arch-layer-detail">{l.detail}</span>
                      </span>
                      {l.id === "l1" ? <span className="arch-cap-badge">{t.capBadge}</span> : null}
                    </motion.div>
                    {i < layers[lang].length - 1 ? (
                      <div className="arch-connector">{t.dependsArrow}</div>
                    ) : null}
                  </div>
                ))}
              </div>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <p className="layer-rule">{t.rule}</p>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
