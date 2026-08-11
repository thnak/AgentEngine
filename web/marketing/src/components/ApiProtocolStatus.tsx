import { protocolEntries } from "../data/apiContent";
import { RevealGroup, RevealItem } from "./Reveal";

export function ApiProtocolStatus() {
  return (
    <section className="section" id="protocols">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">L4 protocol surfaces — Milestone 7</span>
          <h2>
            Five surfaces named in the spec, <span className="grad-text">Milestone 7 in progress</span>
          </h2>
          <p>
            L4's protocol surfaces (MCP, A2A, AG-UI, OpenAI-compatible HTTP) and the declarative
            YAML/JSON authoring format are Reviewed RFCs. Milestone 6 is complete. Milestone 7
            (protocol conformance) is in progress — real, tested code exists for MCP, A2A, AG-UI,
            and the declarative compilers — and a Phase G gate audit found the milestone's own exit
            criterion not yet met, chiefly blocked on a real network listener. Milestones 8-9 have
            not started.
          </p>
        </div>

        <RevealGroup className="protocol-table">
          {protocolEntries.map((p) => (
            <RevealItem key={p.id}>
              <div className="protocol-row glass">
                <div>
                  <div className="protocol-name">{p.name}</div>
                  <a className="protocol-rfc" href={p.rfcHref} target="_blank" rel="noreferrer">
                    RFC {p.rfc}
                  </a>
                </div>
                <span className={`status-badge status-${p.status}`}>
                  {p.status === "real" ? "Real & tested" : "Designed, not built"}
                </span>
                <p className="protocol-note">{p.note}</p>
              </div>
            </RevealItem>
          ))}
        </RevealGroup>
      </div>
    </section>
  );
}
