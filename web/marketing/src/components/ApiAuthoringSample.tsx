import { agentCppSnippet } from "../data/content";
import { gh } from "../data/apiContent";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

export function ApiAuthoringSample() {
  return (
    <section className="section" style={{ paddingTop: 0 }}>
      <div className="container">
        <RevealGroup>
          <RevealItem>
            <CodePanel filename="researcher_agent.cpp">{highlightCpp(agentCppSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>
              Verified end to end, not just in the RFC's prose:{" "}
              <a href={gh("tests/test_agent_registry.cpp")} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                tests/test_agent_registry.cpp
              </a>{" "}
              really does reject a capability-ceiling violation or a tool-name collision, each with
              its own specific error code.
            </p>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
