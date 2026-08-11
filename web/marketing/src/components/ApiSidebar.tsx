import { useEffect, useState } from "react";
import { SITE_BASE } from "../data/content";
import { apiPages } from "../data/apiContent";

export interface ApiSidebarSection {
  id: string;
  label: string;
}

/** Highlights whichever of `ids` is currently nearest the top of the viewport, so the
 * "on this page" sublist tracks scroll position the way a docs site sidebar does. */
function useScrollSpy(ids: string[]): string | undefined {
  const [activeId, setActiveId] = useState<string | undefined>(ids[0]);

  useEffect(() => {
    const elements = ids
      .map((id) => document.getElementById(id))
      .filter((el): el is HTMLElement => el !== null);
    if (elements.length === 0) return;

    const observer = new IntersectionObserver(
      (entries) => {
        const visible = entries.filter((entry) => entry.isIntersecting);
        if (visible.length === 0) return;
        const topmost = visible.reduce((a, b) =>
          a.boundingClientRect.top <= b.boundingClientRect.top ? a : b,
        );
        setActiveId(topmost.target.id);
      },
      { rootMargin: "-90px 0px -65% 0px", threshold: [0, 1] },
    );

    elements.forEach((el) => observer.observe(el));
    return () => observer.disconnect();
  }, [ids.join("|")]);

  return activeId;
}

/** The API section's left sidebar: the seven parts, plus (for the active part) an in-page
 * anchor list into that page's own sections. A horizontal pill strip stands in for it on
 * narrow viewports — see .api-sidebar-mobile in index.css. */
export function ApiSidebar({
  active,
  sections = [],
}: {
  active: string;
  sections?: ApiSidebarSection[];
}) {
  const sectionIds = sections.map((s) => s.id);
  const activeSectionId = useScrollSpy(sectionIds);

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
                {active === p.id && sections.length > 0 && (
                  <ul className="api-sidebar-sublist">
                    {sections.map((s) => (
                      <li key={s.id}>
                        <a
                          href={`#${s.id}`}
                          className="api-sidebar-sublink"
                          aria-current={activeSectionId === s.id ? "true" : undefined}
                        >
                          {s.label}
                        </a>
                      </li>
                    ))}
                  </ul>
                )}
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
