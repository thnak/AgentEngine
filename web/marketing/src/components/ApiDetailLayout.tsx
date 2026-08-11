import type { PropsWithChildren } from "react";
import { ApiNextSteps } from "./ApiNextSteps";
import { ApiSidebar, type ApiSidebarSection } from "./ApiSidebar";
import { Footer } from "./Footer";
import { Nav } from "./Nav";

/** Shared chrome for every /api/*.html detail page: nav, the API section's own left sidebar
 * (a horizontal pill strip on narrow viewports), the page's own content, the "RFCs behind this
 * page" block, and the footer. `sections` feeds the sidebar's "on this page" anchor list. */
export function ApiDetailLayout({
  active,
  sections,
  children,
}: PropsWithChildren<{ active: string; sections?: ApiSidebarSection[] }>) {
  return (
    <>
      <Nav page="api" />
      <div className="api-layout">
        <ApiSidebar active={active} sections={sections} />
        <div className="api-content">
          <main>{children}</main>
        </div>
      </div>
      <ApiNextSteps />
      <Footer />
    </>
  );
}
