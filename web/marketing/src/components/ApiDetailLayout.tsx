import type { PropsWithChildren } from "react";
import { ApiNextSteps } from "./ApiNextSteps";
import { ApiSubNav } from "./ApiSubNav";
import { Footer } from "./Footer";
import { Nav } from "./Nav";

/** Shared chrome for every /api/*.html detail page: nav, the API section's own sub-nav, the
 * page's own content, the "RFCs behind this page" block, and the footer. */
export function ApiDetailLayout({ active, children }: PropsWithChildren<{ active: string }>) {
  return (
    <>
      <Nav page="api" />
      <ApiSubNav active={active} />
      <main>{children}</main>
      <ApiNextSteps />
      <Footer />
    </>
  );
}
