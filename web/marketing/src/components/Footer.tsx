import { QUARK_URL, README_URL, REPO_URL, SPEC_URL } from "../data/content";

export function Footer() {
  return (
    <footer className="footer">
      <div className="container footer-inner">
        <div>
          <a href="#top" className="brand" aria-label="AgentEngine home">
            <span className="brand-mark" aria-hidden="true">
              AE
            </span>
            AgentEngine
          </a>
          <p className="footer-fine">
            C++23 · Quark actor engine · MCP · A2A · AG-UI · OTel GenAI
          </p>
        </div>

        <nav className="footer-links" aria-label="Footer">
          <a href={REPO_URL} target="_blank" rel="noreferrer">
            Repository
          </a>
          <a href={SPEC_URL} target="_blank" rel="noreferrer">
            Specification
          </a>
          <a href={README_URL} target="_blank" rel="noreferrer">
            RFC index
          </a>
          <a href={QUARK_URL} target="_blank" rel="noreferrer">
            Quark
          </a>
        </nav>
      </div>

      <div className="container">
        <p className="footer-fine">
          Status: design phase. No implementation yet — every RFC is Draft. Licence: not yet
          decided (MIT is the working assumption, matching Quark).
        </p>
      </div>
    </footer>
  );
}
