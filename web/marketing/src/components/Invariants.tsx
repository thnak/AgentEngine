import { invariants } from "../data/content";
import { RevealGroup, RevealItem } from "./Reveal";

export function Invariants() {
  return (
    <section className="section" style={{ paddingTop: 0 }}>
      <div className="container">
        <div className="section-head" style={{ marginBottom: 28 }}>
          <span className="eyebrow">The load-bearing statements</span>
          <h2>Eight invariants, restated by every RFC that touches them</h2>
          <p>Every one is a testable gate, not a slogan.</p>
        </div>

        <RevealGroup className="invariant-strip">
          {invariants.map((inv) => (
            <RevealItem key={inv.id}>
              <div className="invariant-chip glass">
                <span className="id">{inv.id}</span>
                <span className="label">{inv.label}</span>
              </div>
            </RevealItem>
          ))}
        </RevealGroup>
      </div>
    </section>
  );
}
