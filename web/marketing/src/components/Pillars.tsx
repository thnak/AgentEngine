import { motion } from "framer-motion";
import { pillars } from "../data/content";
import { RevealGroup, RevealItem } from "./Reveal";

export function Pillars() {
  return (
    <section className="section" id="pillars">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">What makes it different</span>
          <h2>
            Isolation is the <span className="grad-text">substrate</span>, not a bolt-on
          </h2>
          <p>
            Six of the design decisions that follow directly from AgentEngine's threat model and
            its three locked architecture decisions.
          </p>
        </div>

        <RevealGroup className="pillar-grid">
          {pillars.map((p) => (
            <RevealItem key={p.id}>
              <motion.article
                className="pillar-card glass"
                whileHover={{ y: -6, borderColor: "rgba(255,255,255,0.22)" }}
                whileTap={{ scale: 0.98 }}
                transition={{ type: "spring", stiffness: 300, damping: 22 }}
              >
                <span className="pillar-tag">{p.tag}</span>
                <h3>{p.title}</h3>
                <p>{p.body}</p>
              </motion.article>
            </RevealItem>
          ))}
        </RevealGroup>
      </div>
    </section>
  );
}
