import { motion } from "framer-motion";
import { REPO_URL, SITE_BASE } from "../data/content";

export function Nav({ page = "home" }: { page?: "home" | "api" }) {
  // Site-root-relative, so this is correct from index.html, api.html, AND the nested
  // /api/*.html detail pages alike — no "../" bookkeeping per page depth.
  const homeHref = (hash: string) => (page === "home" ? hash : `${SITE_BASE}/index.html${hash}`);

  return (
    <motion.header
      className="nav"
      initial={{ y: -40, opacity: 0 }}
      animate={{ y: 0, opacity: 1 }}
      transition={{ duration: 0.5, ease: "easeOut" }}
    >
      <div className="container nav-inner">
        <a
          href={page === "home" ? "#top" : `${SITE_BASE}/index.html`}
          className="brand"
          aria-label="AgentEngine home"
        >
          <span className="brand-mark" aria-hidden="true">
            AE
          </span>
          AgentEngine
        </a>

        <nav className="nav-links nav-only-desktop" aria-label="Primary">
          <a href={homeHref("#pillars")}>Pillars</a>
          <a href={homeHref("#architecture")}>Architecture</a>
          <a href={homeHref("#spec-driven")}>Spec-driven</a>
          <a href={homeHref("#getting-started")}>Getting started</a>
          <a href={`${SITE_BASE}/api.html`} aria-current={page === "api" ? "page" : undefined}>
            API
          </a>
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
