import { motion } from "framer-motion";
import { CONVENTIONS_URL, DECISIONS_URL, maturityLadder } from "../data/content";
import { RevealGroup, RevealItem } from "./Reveal";

const flowSteps = ["RFC", "red-team", "prove", "judge", "ADR"];

export function SpecDriven() {
  return (
    <section className="section" id="spec-driven">
      <div className="container spec-layout">
        <RevealGroup>
          <RevealItem>
            <span className="eyebrow">Engineering discipline</span>
          </RevealItem>
          <RevealItem>
            <h2 style={{ margin: "12px 0 22px", fontSize: "clamp(1.8rem, 3vw, 2.4rem)" }}>
              Spec-driven, <span className="grad-text">not spec-decorated</span>
            </h2>
          </RevealItem>

          <RevealItem>
            <div className="spec-flow" aria-label="Design process for contested or security-critical changes">
              {flowSteps.map((step, i) => (
                <motion.span
                  key={step}
                  className="spec-flow-step"
                  whileHover={{ y: -3, borderColor: "rgba(255,255,255,0.3)" }}
                >
                  {step}
                  {i < flowSteps.length - 1 ? (
                    <span className="spec-flow-arrow" aria-hidden="true">
                      {" "}
                      →
                    </span>
                  ) : null}
                </motion.span>
              ))}
            </div>
          </RevealItem>

          <RevealItem>
            <p className="spec-quote">
              When code and a spec disagree, the spec wins.
              <br />
              <span>If the spec is wrong, fix the spec first — with an ADR — then the code.</span>
            </p>
          </RevealItem>

          <RevealItem>
            <p style={{ color: "var(--text-dim)", lineHeight: 1.65, fontSize: "0.98rem" }}>
              24 RFC documents are the authoritative design; contested, hot-path, or
              security-critical decisions go through a <code>design → red-team → prove → judge</code>{" "}
              loop and land as an{" "}
              <a href={DECISIONS_URL} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                ADR
              </a>{" "}
              in <code>decisions/</code> — an executed proof, not an ad-hoc change. The binding
              contract for how code gets written once implementation starts lives in{" "}
              <a href={CONVENTIONS_URL} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                CONVENTIONS.md
              </a>
              .
            </p>
          </RevealItem>

          <RevealItem>
            <div className="stat-row">
              <div className="stat">
                <div className="num">30</div>
                <div className="label">RFCs, numbered</div>
              </div>
              <div className="stat">
                <div className="num">8</div>
                <div className="label">Core invariants</div>
              </div>
              <div className="stat">
                <div className="num">0</div>
                <div className="label">RFCs above Draft</div>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="glass" style={{ padding: "8px 8px", borderRadius: "var(--radius-lg)" }}>
              <div className="ladder" style={{ padding: "10px 18px" }}>
                {maturityLadder.map((stage, i) => (
                  <div className={`ladder-step${i === 0 ? " is-current" : ""}`} key={stage.id}>
                    <span className="ladder-index">{String(i + 1).padStart(2, "0")}</span>
                    <div>
                      <h4>{stage.name}</h4>
                      <p>{stage.detail}</p>
                    </div>
                  </div>
                ))}
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
