import type { PropsWithChildren } from "react";
import { SITE_BASE } from "../data/content";
import { apiPages } from "../data/apiContent";
import { ApiNextSteps } from "./ApiNextSteps";
import { ApiSidebar } from "./ApiSidebar";
import { ApiToc, type ApiSidebarSection } from "./ApiToc";
import { Footer } from "./Footer";
import { Nav } from "./Nav";

/** Shared chrome for api.html (the hub) AND every /api/*.html detail page alike: the standard
 * three-rail docs layout (Stripe/MDN/Docusaurus) — nav, a left rail of site-wide navigation
 * (ApiSidebar), the page's own content behind a breadcrumb, and a right rail scoped to just this
 * page's own sections (ApiToc). Both rails collapse to a horizontal pill strip below the
 * three-column breakpoint. `sections` feeds the right rail's "on this page" anchor list.
 * `active` is omitted on the hub (breadcrumb reads just "API", ApiSidebar highlights "Overview"). */
export function ApiDetailLayout({
  active,
  sections = [],
  children,
}: PropsWithChildren<{ active?: string; sections?: ApiSidebarSection[] }>) {
  const page = active ? apiPages.find((p) => p.id === active) : undefined;

  return (
    <>
      <Nav page="api" />
      <div className="api-layout">
        <ApiSidebar active={active} />
        <div className="api-content">
          <nav className="api-breadcrumb" aria-label="Breadcrumb">
            {active ? (
              <>
                <a href={`${SITE_BASE}/api.html`}>API</a>
                <span aria-hidden="true">/</span>
                <span aria-current="page">{page?.label ?? active}</span>
              </>
            ) : (
              <span aria-current="page">API</span>
            )}
          </nav>
          <main>{children}</main>
        </div>
        <ApiToc sections={sections} />
      </div>
      <ApiNextSteps />
      <Footer />
    </>
  );
}
