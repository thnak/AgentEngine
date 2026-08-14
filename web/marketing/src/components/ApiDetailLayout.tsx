import type { PropsWithChildren } from "react";
import { SITE_BASE } from "../data/content";
import { apiPages } from "../data/apiContent";
import { LanguageProvider, useLang, type Lang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { ApiNextSteps } from "./ApiNextSteps";
import { ApiSidebar } from "./ApiSidebar";
import { ApiToc, type ApiSidebarSection } from "./ApiToc";
import { Footer } from "./Footer";
import { Nav } from "./Nav";

function ApiDetailLayoutInner({
  active,
  sections,
  children,
}: PropsWithChildren<{ active?: string; sections: Record<Lang, ApiSidebarSection[]> }>) {
  const { lang } = useLang();
  const t = ui[lang];
  const page = active ? apiPages[lang].find((p) => p.id === active) : undefined;

  return (
    <>
      <Nav page="api" />
      <div className="api-layout">
        <ApiSidebar active={active} />
        <div className="api-content">
          <nav className="api-breadcrumb" aria-label="Breadcrumb">
            {active ? (
              <>
                <a href={`${SITE_BASE}/api.html`}>{t.breadcrumbApi}</a>
                <span aria-hidden="true">/</span>
                <span aria-current="page">{page?.label ?? active}</span>
              </>
            ) : (
              <span aria-current="page">{t.breadcrumbApi}</span>
            )}
          </nav>
          <main>{children}</main>
        </div>
        <ApiToc sections={sections[lang]} />
      </div>
      <ApiNextSteps />
      <Footer />
    </>
  );
}

/** Shared chrome for api.html (the hub) AND every /api/*.html detail page alike: the standard
 * three-rail docs layout (Stripe/MDN/Docusaurus) — nav, a left rail of site-wide navigation
 * (ApiSidebar), the page's own content behind a breadcrumb, and a right rail scoped to just this
 * page's own sections (ApiToc). Both rails collapse to a horizontal pill strip below the
 * three-column breakpoint. `sections` feeds the right rail's "on this page" anchor list.
 * `active` is omitted on the hub (breadcrumb reads just "API", ApiSidebar highlights "Overview").
 * Owns the LanguageProvider for the whole api.html / api/*.html page family — this is a static
 * multi-page site, so each page gets its own React root and its own provider instance. */
export function ApiDetailLayout({
  active,
  sections = { en: [], vi: [] },
  children,
}: PropsWithChildren<{ active?: string; sections?: Record<Lang, ApiSidebarSection[]> }>) {
  return (
    <LanguageProvider>
      <ApiDetailLayoutInner active={active} sections={sections}>
        {children}
      </ApiDetailLayoutInner>
    </LanguageProvider>
  );
}
