import { motion } from "framer-motion";
import { REPO_URL, SITE_BASE } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";

export function Nav({ page = "home" }: { page?: "home" | "api" }) {
  // Site-root-relative, so this is correct from index.html, api.html, AND the nested
  // /api/*.html detail pages alike — no "../" bookkeeping per page depth.
  const homeHref = (hash: string) => (page === "home" ? hash : `${SITE_BASE}/index.html${hash}`);
  const { lang, toggle } = useLang();
  const t = ui[lang];

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
          aria-label={t.navHome}
        >
          <span className="brand-mark" aria-hidden="true">
            AE
          </span>
          AgentEngine
        </a>

        <nav className="nav-links nav-only-desktop" aria-label="Primary">
          <a href={homeHref("#pillars")}>{t.navPillars}</a>
          <a href={homeHref("#architecture")}>{t.navArchitecture}</a>
          <a href={homeHref("#spec-driven")}>{t.navSpecDriven}</a>
          <a href={homeHref("#getting-started")}>{t.navGettingStarted}</a>
          <a href={`${SITE_BASE}/api.html`} aria-current={page === "api" ? "page" : undefined}>
            {t.navApi}
          </a>
        </nav>

        <div className="nav-cta">
          <button
            type="button"
            className="btn btn-secondary lang-toggle"
            onClick={toggle}
            aria-label={t.langToggleLabel}
          >
            {lang === "en" ? "VI" : "EN"}
          </button>
          <a
            className="btn btn-secondary"
            href={REPO_URL}
            target="_blank"
            rel="noreferrer"
          >
            {t.navGithub}
          </a>
        </div>
      </div>
    </motion.header>
  );
}
