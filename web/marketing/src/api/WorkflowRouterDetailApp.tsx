import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiWorkflowPatternDetail } from "../components/ApiWorkflowPatternDetail";

const sections = {
  en: [
    { id: "pattern-graph", label: "The graph" },
    { id: "pattern-code", label: "The code" },
    { id: "pattern-why", label: "Why this shape" },
    { id: "pattern-related", label: "Related patterns" },
  ],
  vi: [
    { id: "pattern-graph", label: "Đồ thị" },
    { id: "pattern-code", label: "Mã nguồn" },
    { id: "pattern-why", label: "Vì sao hình dạng này" },
    { id: "pattern-related", label: "Các mẫu liên quan" },
  ],
};

function WorkflowRouterDetailApp() {
  return (
    <ApiDetailLayout active="workflow" sections={sections}>
      <ApiWorkflowPatternDetail pattern="router" />
    </ApiDetailLayout>
  );
}

export default WorkflowRouterDetailApp;
