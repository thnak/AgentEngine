import { motion } from "framer-motion";
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

        <RevealGroup className="api-grid">
          {entries.map((e) => (
            <RevealItem key={e.id}>
              <motion.article
                className="api-card glass"
                whileHover={{ y: -4, borderColor: "rgba(255,255,255,0.22)" }}
                transition={{ type: "spring", stiffness: 300, damping: 22 }}
              >
                <div className="api-card-head">
                  <StatusBadge status={e.status} />
                </div>
                <code className="api-tag">{e.tag}</code>
                <h3>{e.title}</h3>
                <p>{e.body}</p>
                <a className="api-cite" href={e.href} target="_blank" rel="noreferrer">
                  {e.cite}
                </a>
              </motion.article>
            </RevealItem>
          ))}
        </RevealGroup>
      </div>
    </section>
  );
}
