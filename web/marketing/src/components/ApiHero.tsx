import { motion } from "framer-motion";
import { ROADMAP_URL, SPEC_URL } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";

const copy = {
  en: {
    badge: "Milestones 0–6 complete · Milestone 7 (protocol conformance) in progress",
    headingPrefix: "The API surface,",
    headingHighlight: "honestly labeled",
    lede:
      "AgentEngine is pre-v1 and spec-driven — this page separates what compiles and has a passing test today from what a Reviewed RFC describes but nothing implements yet. Every entry below cites the header, source file, or test it comes from — nothing here is aspirational unless its badge says so.",
    ctaRoadmap: "Implementation roadmap",
    ctaSpec: "Read the spec",
  },
  vi: {
    badge: "Milestone 0–6 đã hoàn tất · Milestone 7 (tuân thủ giao thức) đang thực hiện",
    headingPrefix: "Bề mặt API,",
    headingHighlight: "được nêu nhãn trung thực",
    lede:
      "AgentEngine là pre-v1 và hướng đặc tả — trang này tách bạch những gì biên dịch được và đã có test qua hôm nay khỏi những gì một RFC Reviewed mô tả nhưng chưa có gì triển khai. Mỗi mục bên dưới đều dẫn nguồn tới header, file mã nguồn, hoặc test mà nó đến từ đó — không có gì ở đây mang tính kỳ vọng trừ khi badge của nó nói vậy.",
    ctaRoadmap: "Lộ trình triển khai",
    ctaSpec: "Đọc đặc tả",
  },
} as const;

export function ApiHero() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  return (
    <section className="hero api-hero" id="top">
      <div className="container">
        <motion.div
          initial={{ opacity: 0, y: 24 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ duration: 0.5, ease: "easeOut" }}
        >
          <div className="hero-badge glass">
            <span className="dot" aria-hidden="true" />
            {t.badge}
          </div>

          <h1>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h1>

          <p className="lede">{t.lede}</p>

          <div className="api-legend">
            <span className="status-badge status-real">{tu.statusRealTested}</span>
            <span className="status-badge status-design">{tu.statusDesignedNotBuilt}</span>
          </div>

          <div className="hero-ctas" style={{ marginTop: 28 }}>
            <motion.a
              className="btn btn-primary"
              href={ROADMAP_URL}
              target="_blank"
              rel="noreferrer"
              whileHover={{ scale: 1.035 }}
              whileTap={{ scale: 0.97 }}
            >
              {t.ctaRoadmap}
            </motion.a>
            <motion.a
              className="btn btn-secondary"
              href={SPEC_URL}
              target="_blank"
              rel="noreferrer"
              whileHover={{ scale: 1.035 }}
              whileTap={{ scale: 0.97 }}
            >
              {t.ctaSpec}
            </motion.a>
          </div>
        </motion.div>
      </div>
    </section>
  );
}
