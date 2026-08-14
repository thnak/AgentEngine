import { SITE_BASE } from "../data/content";
import { apiPages } from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";

/** The API section's left rail: ONE unified list, "Overview" plus every part, shared by api.html
 * (the hub) and every /api/*.html detail page alike — the standard three-rail docs layout (Stripe/
 * MDN/Docusaurus). `active` is undefined on the hub (Overview highlighted) or an apiPages id on a
 * detail page. Kept separate from the current page's own "on this page" anchors, which live in the
 * right rail instead (see ApiToc). A horizontal pill strip stands in for this on narrow viewports
 * where there's no room for three columns. */
export function ApiSidebar({ active }: { active?: string }) {
  const { lang } = useLang();
  const t = ui[lang];
  const pages = apiPages[lang];
  return (
    <>
      <nav className="api-sidebar" aria-label="API reference">
        <div className="api-sidebar-group">
          <span className="api-sidebar-label">{t.sidebarApiReference}</span>
          <ul className="api-sidebar-list">
            <li>
              <a
                className="api-sidebar-link"
                href={`${SITE_BASE}/api.html`}
                aria-current={active === undefined ? "page" : undefined}
              >
                <span className="api-sidebar-dot" aria-hidden="true" />
                {t.sidebarOverview}
              </a>
            </li>
            {pages.map((p) => (
              <li key={p.id}>
                <a
                  className="api-sidebar-link"
                  href={p.href}
                  aria-current={active === p.id ? "page" : undefined}
                >
                  <span
                    className={`api-sidebar-dot${p.status === "design" ? " is-design" : ""}`}
                    aria-hidden="true"
                  />
                  {p.label}
                </a>
              </li>
            ))}
          </ul>
        </div>
      </nav>

      <div className="api-sidebar-mobile">
        <div className="api-sidebar-mobile-inner">
          <a
            className="api-sidebar-mobile-link"
            href={`${SITE_BASE}/api.html`}
            aria-current={active === undefined ? "page" : undefined}
          >
            {t.sidebarOverview}
          </a>
          {pages.map((p) => (
            <a
              key={p.id}
              className="api-sidebar-mobile-link"
              href={p.href}
              aria-current={active === p.id ? "page" : undefined}
            >
              {p.label}
            </a>
          ))}
        </div>
      </div>
    </>
  );
}
