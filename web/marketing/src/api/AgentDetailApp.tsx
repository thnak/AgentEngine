import { ApiAuthoringSample } from "../components/ApiAuthoringSample";
import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiSection } from "../components/ApiSection";
import { authoringEntries } from "../data/apiContent";

function AgentDetailApp() {
  return (
    <ApiDetailLayout
      active="agent"
      sections={[
        ...authoringEntries.map((e) => ({ id: e.id, label: e.tag })),
        { id: "authoring-sample", label: "Worked example" },
      ]}
    >
      <ApiSection
        id="agent"
        eyebrow="C++ CRTP authoring surface — 002"
        heading={
          <>
            Agents are <span className="grad-text">compile-time policy sets</span>
          </>
        }
        description="No runtime configuration object, no virtual dispatch — an agent's whole policy set is template parameters, compiled and validated once by register_agent<A>()."
        entries={authoringEntries}
      />
      <ApiAuthoringSample />
    </ApiDetailLayout>
  );
}

export default AgentDetailApp;
