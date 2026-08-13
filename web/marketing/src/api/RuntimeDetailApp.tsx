import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiSection } from "../components/ApiSection";
import { runtimeEntries } from "../data/apiContent";

function RuntimeDetailApp() {
  return (
    <ApiDetailLayout active="runtime" sections={runtimeEntries.map((e) => ({ id: e.id, label: e.tag }))}>
      <ApiSection
        id="runtime"
        eyebrow="Agent core — L2"
        heading={
          <>
            A real runtime, <span className="grad-text">real provider backends</span>
          </>
        }
        description="AgentSession runs on AgentEngine's own agentengine::rt:: runtime with sixteen-plus tests behind it, not a facade — and the ChatClient it talks to is a live Anthropic or OpenAI backend, not only a mock."
        entries={runtimeEntries}
      />
    </ApiDetailLayout>
  );
}

export default RuntimeDetailApp;
