import { SITE_BASE } from "../data/content";
import { apiPages } from "../data/apiContent";

/** The API section's left rail: site-wide navigation only (the API's parts, all of apiPages) —
 * the standard three-rail docs layout (Stripe/MDN/Docusaurus) keeps this separate from the current page's own
 * "on this page" anchors, which live in the right rail instead (see ApiToc). A horizontal pill
 * strip stands in for this on narrow viewports where there's no room for three columns. */
export function ApiSidebar({ active }: { active: string }) {
  return (
    <>
      <nav className="api-sidebar" aria-label="API reference">
        <a className="api-sidebar-back" href={`${SITE_BASE}/api.html`}>
          &larr; API overview
        </a>

        <div className="api-sidebar-group">
          <span className="api-sidebar-label">Parts</span>
          <ul className="api-sidebar-list">
            {apiPages.map((p) => (
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
          <a className="api-sidebar-mobile-link" href={`${SITE_BASE}/api.html`}>
            Overview
          </a>
          {apiPages.map((p) => (
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
