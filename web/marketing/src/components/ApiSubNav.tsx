import { SITE_BASE } from "../data/content";
import { apiPages } from "../data/apiContent";

/** The API section's own page strip — shown on the hub and every detail page. */
export function ApiSubNav({ active }: { active?: string }) {
  return (
    <div className="api-subnav">
      <div className="container api-subnav-inner">
        <a
          className="api-subnav-link"
          href={`${SITE_BASE}/api.html`}
          aria-current={active === undefined ? "page" : undefined}
        >
          Overview
        </a>
        {apiPages.map((p) => (
          <a
            key={p.id}
            className="api-subnav-link"
            href={p.href}
            aria-current={active === p.id ? "page" : undefined}
          >
            {p.label}
          </a>
        ))}
      </div>
    </div>
  );
}
