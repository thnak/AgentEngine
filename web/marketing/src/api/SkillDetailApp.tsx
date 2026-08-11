import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiSkillReference } from "../components/ApiSkillReference";

const sections = [
  { id: "skills", label: "Overview" },
  { id: "skill-loading", label: "How a skill loads" },
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
