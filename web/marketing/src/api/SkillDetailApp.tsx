import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiSkillReference } from "../components/ApiSkillReference";

const sections = [
  { id: "skills", label: "Overview" },
  { id: "skill-source", label: "SkillSource" },
  { id: "inline-skill-source-detail", label: "InlineSkillSource, in full" },
  { id: "skill-loading", label: "How a skill loads" },
  { id: "skill-mounting", label: "Mounting — SkillsProvider" },
  { id: "skill-tool-scoping", label: "Tool scoping" },
  { id: "skill-on-demand", label: "On-demand mounting" },
  { id: "skill-composition", label: "Wiring into AgentSession" },
  { id: "skill-packaging", label: "Packaging" },
  { id: "skill-builtin", label: "Built-in skills" },
  { id: "skill-status", label: "Status" },
];

function SkillDetailApp() {
  return (
    <ApiDetailLayout active="skill" sections={sections}>
      <ApiSkillReference />
    </ApiDetailLayout>
  );
}

export default SkillDetailApp;
