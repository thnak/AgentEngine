import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiSkillReference } from "../components/ApiSkillReference";

const sections = {
  en: [
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
  ],
  vi: [
    { id: "skills", label: "Tổng quan" },
    { id: "skill-source", label: "SkillSource" },
    { id: "inline-skill-source-detail", label: "InlineSkillSource, đầy đủ" },
    { id: "skill-loading", label: "Một skill được nạp như thế nào" },
    { id: "skill-mounting", label: "Mounting — SkillsProvider" },
    { id: "skill-tool-scoping", label: "Giới hạn phạm vi tool" },
    { id: "skill-on-demand", label: "Mount theo yêu cầu" },
    { id: "skill-composition", label: "Đấu nối vào AgentSession" },
    { id: "skill-packaging", label: "Đóng gói" },
    { id: "skill-builtin", label: "Skill tích hợp sẵn" },
    { id: "skill-status", label: "Trạng thái" },
  ],
};

function SkillDetailApp() {
  return (
    <ApiDetailLayout active="skill" sections={sections}>
      <ApiSkillReference />
    </ApiDetailLayout>
  );
}

export default SkillDetailApp;
