import type { ReactNode } from "react";
import { useLang } from "../i18n/LanguageContext";
import { CodePanel } from "./CodePanel";

/** The `#include` block a page's subject actually needs. Every reference page's snippets are
 * excerpts, so none of them shows how to reach the types it names — and a reader who cannot guess
 * a header is stuck before they can try anything. This renders the real, verified list once per
 * page, above the first code panel.
 *
 * Every list placed with this component has been compiled: see devsim/verify-includes, which
 * includes each block and fails the gate if a path stops existing. */
const labels = {
  en: { filename: "what to include" },
  vi: { filename: "cần include những gì" },
} as const;

export function ApiIncludes({ headers, children }: { headers: string[]; children?: ReactNode }) {
  const { lang } = useLang();
  const t = labels[lang];
  const snippet = headers.map((h) => `#include "${h}"`).join("\n");
  return (
    <>
      <CodePanel filename={t.filename}>{snippet}</CodePanel>
      {children ? (
        <p className="gs-note" style={{ marginTop: 14 }}>
          {children}
        </p>
      ) : null}
    </>
  );
}
