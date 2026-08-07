import { motion } from "framer-motion";
import { REPO_URL } from "../data/content";

export function Nav() {
  return (
    <motion.header
      className="nav"
      initial={{ y: -40, opacity: 0 }}
      animate={{ y: 0, opacity: 1 }}
      transition={{ duration: 0.5, ease: "easeOut" }}
    >
      <div className="container nav-inner">
        <a href="#top" className="brand" aria-label="AgentEngine home">
          <span className="brand-mark" aria-hidden="true">
            AE
          </span>
          AgentEngine
        </a>

        <nav className="nav-links nav-only-desktop" aria-label="Primary">
          <a href="#pillars">Pillars</a>
          <a href="#architecture">Architecture</a>
          <a href="#spec-driven">Spec-driven</a>
        </nav>

        <div className="nav-cta">
          <a
            className="btn btn-secondary"
            href={REPO_URL}
            target="_blank"
            rel="noreferrer"
          >
            GitHub
          </a>
        </div>
      </div>
    </motion.header>
  );
}
