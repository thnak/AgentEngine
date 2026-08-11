import type { ReactNode } from "react";
import type { ApiEntry } from "../data/apiContent";
import { RevealGroup, RevealItem } from "./Reveal";

function StatusBadge({ status }: { status: ApiEntry["status"] }) {
  return (
    <span className={`status-badge status-${status}`}>
      {status === "real" ? "Real & tested" : "Designed, not built"}
    </span>
  );
}

// A stacked reference-doc list, not a grid of showcase cards: each entry is addressable by
// its own id (the sidebar's "on this page" anchors and scroll-spy target these directly).
export function ApiSection({
  id,
  eyebrow,
  heading,
  description,
  entries,
}: {
  id: string;
  eyebrow: string;
  heading: ReactNode;
  description: ReactNode;
  entries: ApiEntry[];
}) {
  return (
    <section className="section" id={id}>
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">{eyebrow}</span>
          <h2>{heading}</h2>
          <p>{description}</p>
        </div>

        <RevealGroup className="doc-entries">
          {entries.map((e) => (
            <RevealItem key={e.id}>
              <article className="doc-entry" id={e.id}>
                <div className="doc-entry-head">
                  <code className="api-tag">{e.tag}</code>
                  <StatusBadge status={e.status} />
                </div>
                <h3>{e.title}</h3>
                <p>{e.body}</p>
                <a className="api-cite" href={e.href} target="_blank" rel="noreferrer">
                  {e.cite}
                </a>
              </article>
            </RevealItem>
          ))}
        </RevealGroup>
      </div>
    </section>
  );
}
