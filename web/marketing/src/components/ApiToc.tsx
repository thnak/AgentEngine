import { useEffect, useState } from "react";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";

export interface ApiSidebarSection {
  id: string;
  label: string;
}

/** Highlights whichever of `ids` is currently nearest the top of the viewport, so the "on this
 * page" list tracks scroll position the way a docs site's right-rail TOC does. */
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

/** The API section's right rail: an "on this page" table of contents scoped to the CURRENT
 * page's own sections, separate from ApiSidebar's site-wide left nav — the standard split every
 * major docs site (Stripe, MDN, Docusaurus, Next.js) uses, because the two answer different
 * questions ("where else can I go" vs. "where am I on this page") and don't share a scroll
 * position. Hidden below the three-rail breakpoint; there's no substitute UI on narrow viewports
 * — the browser's own in-page find/scroll takes over, same as those reference sites. */
export function ApiToc({ sections }: { sections: ApiSidebarSection[] }) {
  const { lang } = useLang();
  const t = ui[lang];
  const ids = sections.map((s) => s.id);
  const activeId = useScrollSpy(ids);

  if (sections.length === 0) return null;

  return (
    <nav className="api-toc" aria-label={t.tocOnThisPage}>
      <span className="api-sidebar-label">{t.tocOnThisPage}</span>
      <ul className="api-toc-list">
        {sections.map((s) => (
          <li key={s.id}>
            <a
              href={`#${s.id}`}
              className="api-toc-link"
              aria-current={activeId === s.id ? "true" : undefined}
            >
              {s.label}
            </a>
          </li>
        ))}
      </ul>
    </nav>
  );
}
