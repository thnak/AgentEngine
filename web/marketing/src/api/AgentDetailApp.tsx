import { ApiAgentReference } from "../components/ApiAgentReference";
import { ApiAuthoringSample } from "../components/ApiAuthoringSample";
import { ApiDetailLayout } from "../components/ApiDetailLayout";

const sections = {
  en: [
    { id: "agent-crtp", label: "Agent<Derived, Policies...>" },
    { id: "register-agent", label: "register_agent<A>()" },
    { id: "authoring-sample", label: "Worked example" },
  ],
  vi: [
    { id: "agent-crtp", label: "Agent<Derived, Policies...>" },
    { id: "register-agent", label: "register_agent<A>()" },
    { id: "authoring-sample", label: "Ví dụ minh họa" },
  ],
};

function AgentDetailApp() {
  return (
    <ApiDetailLayout active="agent" sections={sections}>
      <ApiAgentReference />
      <ApiAuthoringSample />
    </ApiDetailLayout>
  );
}

export default AgentDetailApp;
