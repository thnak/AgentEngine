import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiSkillReference } from "../components/ApiSkillReference";

function SkillDetailApp() {
  return (
    <ApiDetailLayout active="skill">
      <ApiSkillReference />
    </ApiDetailLayout>
  );
}

export default SkillDetailApp;
