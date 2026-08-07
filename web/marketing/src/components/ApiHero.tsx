import { motion } from "framer-motion";
import { ROADMAP_URL, SPEC_URL } from "../data/content";

export function ApiHero() {
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
            Milestones 0–4 complete · Milestone 5 in progress (Phase J remaining)
          </div>

          <h1>
            The API surface, <span className="grad-text">honestly labeled</span>
          </h1>

          <p className="lede">
            AgentEngine is pre-v1 and spec-driven — this page separates what compiles and has a
            passing test today from what a Reviewed RFC describes but nothing implements yet.
            Every entry below cites the header, source file, or test it comes from — nothing here
            is aspirational unless its badge says so.
          </p>

          <div className="api-legend">
            <span className="status-badge status-real">Real &amp; tested</span>
            <span className="status-badge status-design">Designed, not built</span>
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
              Implementation roadmap
            </motion.a>
            <motion.a
              className="btn btn-secondary"
              href={SPEC_URL}
              target="_blank"
              rel="noreferrer"
              whileHover={{ scale: 1.035 }}
              whileTap={{ scale: 0.97 }}
            >
              Read the spec
            </motion.a>
          </div>
        </motion.div>
      </div>
    </section>
  );
}
