import { motion } from "framer-motion";
import { layers } from "../data/content";
import { RevealGroup, RevealItem } from "./Reveal";

export function Architecture() {
  return (
    <section className="section" id="architecture">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">Layering</span>
          <h2>
            Five layers, <span className="grad-text">one direction</span> of dependency
          </h2>
          <p>
            A layer may depend only on layers below it, and only through the seam that layer
            publishes. L4 has no privileged access to L0 — L1 is the only layer that may hold an OS
            capability; everything above it holds handles.
          </p>
        </div>

        <RevealGroup className="layer-stack">
          {layers.map((l) => (
            <RevealItem key={l.id}>
              <motion.div
                className="layer-row glass"
                whileHover={{ x: 6, borderColor: "rgba(255,255,255,0.22)" }}
                transition={{ type: "spring", stiffness: 260, damping: 20 }}
              >
                <span className="layer-badge">{l.level}</span>
                <span className="layer-name">{l.name}</span>
                <span className="layer-detail">{l.detail}</span>
              </motion.div>
            </RevealItem>
          ))}
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <p className="layer-rule">
              AgentEngine owns its own runtime substrate — a thread pool, a single-executor guard,
              a session/append-log durability seam, a circuit breaker, and a channel/stream backend
              — no distributed cluster, no actor mailbox.
            </p>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
