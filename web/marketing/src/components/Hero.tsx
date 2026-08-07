import { motion, type Variants } from "framer-motion";
import { heroCodeSnippet, README_URL, SPEC_URL } from "../data/content";

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

function highlightCpp(source: string) {
  // A small, dependency-free token highlighter for the one snippet we show —
  // not a general syntax highlighter, just enough to read as "real code".
  const lines = source.split("\n");
  return lines.map((line, i) => {
    const parts: Array<{ text: string; cls?: string }> = [];
    const re = /(\/\/.*$)|("[^"]*")|\b(struct|auto|static|constexpr|std)\b|\b([A-Z][A-Za-z0-9_]*)\b/g;
    let last = 0;
    let m: RegExpExecArray | null;
    while ((m = re.exec(line))) {
      if (m.index > last) parts.push({ text: line.slice(last, m.index) });
      if (m[1]) parts.push({ text: m[1], cls: "tok-com" });
      else if (m[2]) parts.push({ text: m[2], cls: "tok-str" });
      else if (m[3]) parts.push({ text: m[3], cls: "tok-kw" });
      else if (m[4]) parts.push({ text: m[4], cls: "tok-type" });
      last = re.lastIndex;
    }
    if (last < line.length) parts.push({ text: line.slice(last) });
    return (
      <div key={i}>
        {parts.map((p, j) =>
          p.cls ? (
            <span key={j} className={p.cls}>
              {p.text}
            </span>
          ) : (
            <span key={j}>{p.text}</span>
          ),
        )}
        {line.length === 0 ? " " : null}
      </div>
    );
  });
}

export function Hero() {
  return (
    <section className="hero" id="top">
      <div className="container hero-grid">
        <motion.div variants={container} initial="hidden" animate="show">
          <motion.div className="hero-badge glass" variants={item}>
            <span className="dot" aria-hidden="true" />
            Status: design phase — 30 RFCs, every one Draft
          </motion.div>

          <motion.h1 variants={item}>
            A C++23 engine for building{" "}
            <span className="grad-text">agent applications</span>
          </motion.h1>

          <motion.p className="lede" variants={item}>
            AgentEngine hosts agents, sessions, tools, and multi-agent workflows on the{" "}
            <a href="https://github.com/thnak/QuarkCpp" target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
              Quark
            </a>{" "}
            actor engine. Untrusted code isolation and a Python interpreter are built-in
            subsystems, not optional add-ons — speaking the open agent protocols of 2026: MCP, A2A,
            AG-UI, OpenTelemetry GenAI.
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
              Read the spec
            </motion.a>
            <motion.a
              className="btn btn-secondary"
              href={README_URL}
              target="_blank"
              rel="noreferrer"
              whileHover={{ scale: 1.035 }}
              whileTap={{ scale: 0.97 }}
            >
              Browse the RFCs
            </motion.a>
          </motion.div>

          <motion.div className="hero-meta" variants={item}>
            <span>
              <strong>8</strong> core invariants
            </span>
            <span>
              <strong>30</strong> RFCs
            </span>
            <span>
              <strong>MAF-shaped</strong> developer model
            </span>
            <span>
              <strong>Windows + Linux</strong> targets
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
