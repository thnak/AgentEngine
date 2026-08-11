import { EXAMPLES_URL, ROADMAP_URL, TESTS_URL } from "../data/content";
import { apiRfcLinks } from "../data/apiContent";
import { RevealGroup, RevealItem } from "./Reveal";

export function ApiNextSteps() {
  return (
    <section className="section" style={{ paddingTop: 0 }}>
      <div className="container">
        <RevealGroup>
          <RevealItem>
            <div className="gs-next glass">
              <span className="eyebrow">The RFCs behind this page</span>
              <div className="rfc-grid">
                {apiRfcLinks.map((l) => (
                  <a key={l.href} className="rfc-chip" href={l.href} target="_blank" rel="noreferrer">
                    <span className={`status-badge status-${l.status}`}>
                      {l.status === "real" ? "real" : "design"}
                    </span>
                    <span>{l.label}</span>
                  </a>
                ))}
              </div>
              <p className="gs-note" style={{ marginTop: 24 }}>
                Want the ground truth instead of a summary? Read the{" "}
                <a href={ROADMAP_URL} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                  implementation roadmap
                </a>{" "}
                for milestone status, browse{" "}
                <a href={TESTS_URL} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                  tests/
                </a>{" "}
                — every "real & tested" claim on this page cites one — or run{" "}
                <a href={EXAMPLES_URL} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                  examples/
                </a>{" "}
                for small, offline, single-file programs that build and pass, one concept each.
              </p>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
