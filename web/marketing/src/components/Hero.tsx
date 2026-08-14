import { motion, type Variants } from "framer-motion";
import { heroCodeSnippet, README_URL, SPEC_URL } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { highlightCpp } from "../lib/highlightCpp";

const copy = {
  en: {
    badge: "Status: Milestones 0–6 complete · Milestone 7 (protocol conformance) in progress",
    titlePrefix: "A C++23 engine for building",
    titleHighlight: "agent applications",
    lede:
      "AgentEngine hosts agents, sessions, tools, and multi-agent workflows on its own self-contained C++23 runtime. Untrusted code isolation and a Python interpreter are built-in subsystems, not optional add-ons — speaking the open agent protocols of 2026: MCP, A2A, AG-UI, OpenTelemetry GenAI.",
    ctaSpec: "Read the spec",
    ctaRfcs: "Browse the RFCs",
    metaInvariants: "core invariants",
    metaRfcs: "RFCs",
    metaMaf: "developer model",
    metaPlatforms: "targets",
  },
  vi: {
    badge: "Trạng thái: Milestone 0–6 đã hoàn tất · Milestone 7 (tuân thủ giao thức) đang thực hiện",
    titlePrefix: "Một engine C++23 để xây dựng",
    titleHighlight: "ứng dụng agent",
    lede:
      "AgentEngine vận hành agent, session, tool, và các workflow đa agent trên chính runtime C++23 tự chứa của mình. Cách ly mã không tin cậy và một trình thông dịch Python là các phân hệ tích hợp sẵn, không phải tùy chọn thêm — nói được các giao thức agent mở của năm 2026: MCP, A2A, AG-UI, OpenTelemetry GenAI.",
    ctaSpec: "Đọc đặc tả",
    ctaRfcs: "Xem các RFC",
    metaInvariants: "invariant cốt lõi",
    metaRfcs: "RFC",
    metaMaf: "mô hình phát triển",
    metaPlatforms: "nền tảng mục tiêu",
  },
} as const;

const container: Variants = {
  hidden: {},
  show: {
    transition: { staggerChildren: 0.11, delayChildren: 0.1 },
  },
};

const item: Variants = {
  hidden: { opacity: 0, y: 22 },
  show: { opacity: 1, y: 0, transition: { duration: 0.55, ease: "easeOut" } },
};

export function Hero() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="hero" id="top">
      <div className="container hero-grid">
        <motion.div variants={container} initial="hidden" animate="show">
          <motion.div className="hero-badge glass" variants={item}>
            <span className="dot" aria-hidden="true" />
            {t.badge}
          </motion.div>

          <motion.h1 variants={item}>
            {t.titlePrefix} <span className="grad-text">{t.titleHighlight}</span>
          </motion.h1>

          <motion.p className="lede" variants={item}>
            {t.lede}
          </motion.p>

          <motion.div className="hero-ctas" variants={item}>
            <motion.a
              className="btn btn-primary"
              href={SPEC_URL}
              target="_blank"
              rel="noreferrer"
              whileHover={{ scale: 1.035 }}
              whileTap={{ scale: 0.97 }}
            >
              {t.ctaSpec}
            </motion.a>
            <motion.a
              className="btn btn-secondary"
              href={README_URL}
              target="_blank"
              rel="noreferrer"
              whileHover={{ scale: 1.035 }}
              whileTap={{ scale: 0.97 }}
            >
              {t.ctaRfcs}
            </motion.a>
          </motion.div>

          <motion.div className="hero-meta" variants={item}>
            <span>
              <strong>8</strong> {t.metaInvariants}
            </span>
            <span>
              <strong>30</strong> {t.metaRfcs}
            </span>
            <span>
              <strong>MAF-shaped</strong> {t.metaMaf}
            </span>
            <span>
              <strong>Windows + Linux</strong> {t.metaPlatforms}
            </span>
          </motion.div>
        </motion.div>

        <motion.div
          className="code-panel glass"
          initial={{ opacity: 0, scale: 0.94, rotate: -1 }}
          animate={{ opacity: 1, scale: 1, rotate: 0 }}
          transition={{ duration: 0.7, delay: 0.35, ease: "easeOut" }}
          whileHover={{ rotate: 0.3, scale: 1.01 }}
        >
          <div className="code-panel-head">
            <span style={{ background: "#ff6459" }} />
            <span style={{ background: "#ffbd2e" }} />
            <span style={{ background: "#28c840" }} />
            <span className="filename">researcher.hpp</span>
          </div>
          <pre>
            <code>{highlightCpp(heroCodeSnippet)}</code>
          </pre>
        </motion.div>
      </div>
    </section>
  );
}
