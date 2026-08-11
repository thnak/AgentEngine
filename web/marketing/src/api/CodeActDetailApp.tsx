import { ApiCodeActReference } from "../components/ApiCodeActReference";
import { ApiDetailLayout } from "../components/ApiDetailLayout";

const sections = [
  { id: "codeact", label: "Overview" },
  { id: "codeact-modules", label: "The nine agent.* modules" },
  { id: "codeact-bridges", label: "The two real bridges" },
  { id: "codeact-generated", label: "What a bridged call looks like" },
  { id: "codeact-skills", label: "Can CodeAct call a skill's tools?" },
  { id: "codeact-status", label: "Status" },
];

function CodeActDetailApp() {
  return (
    <ApiDetailLayout active="codeact" sections={sections}>
      <ApiCodeActReference />
    </ApiDetailLayout>
  );
}

export default CodeActDetailApp;
