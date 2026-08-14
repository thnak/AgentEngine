import { motion } from "framer-motion";
import { pillars } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { RevealGroup, RevealItem } from "./Reveal";

const copy = {
  en: {
    eyebrow: "What makes it different",
    headingPrefix: "Isolation is the",
    headingHighlight: "substrate",
    headingSuffix: ", not a bolt-on",
    body:
      "Six of the design decisions that follow directly from AgentEngine's threat model and its three locked architecture decisions.",
  },
  vi: {
    eyebrow: "Điều làm nên sự khác biệt",
    headingPrefix: "Cách ly là",
    headingHighlight: "nền tảng",
    headingSuffix: ", không phải phần gắn thêm",
    body:
      "Sáu trong số các quyết định thiết kế bắt nguồn trực tiếp từ mô hình mối đe dọa của AgentEngine và ba quyết định kiến trúc đã được chốt.",
  },
} as const;

export function Pillars() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" id="pillars">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
            {t.headingSuffix}
          </h2>
          <p>{t.body}</p>
        </div>

        <RevealGroup className="pillar-grid">
          {pillars[lang].map((p) => (
            <RevealItem key={p.id}>
              <motion.article
                className="pillar-card glass"
                whileHover={{ y: -6, borderColor: "rgba(255,255,255,0.22)" }}
                whileTap={{ scale: 0.98 }}
                transition={{ type: "spring", stiffness: 300, damping: 22 }}
              >
                <span className="pillar-tag">{p.tag}</span>
                <h3>{p.title}</h3>
                <p>{p.body}</p>
              </motion.article>
            </RevealItem>
          ))}
        </RevealGroup>
      </div>
    </section>
  );
}
