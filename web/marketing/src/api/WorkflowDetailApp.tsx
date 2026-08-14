import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiWorkflowReference } from "../components/ApiWorkflowReference";

const sections = {
  en: [
    { id: "workflow", label: "Overview" },
    { id: "workflow-edge-kinds", label: "Six edge kinds" },
    { id: "workflow-graph-shape", label: "The graph, verbatim" },
    { id: "workflow-engine", label: "Running one" },
  ],
  vi: [
    { id: "workflow", label: "Tổng quan" },
    { id: "workflow-edge-kinds", label: "Sáu loại edge" },
    { id: "workflow-graph-shape", label: "Đồ thị, nguyên văn" },
    { id: "workflow-engine", label: "Chạy một workflow" },
  ],
};

function WorkflowDetailApp() {
  return (
    <ApiDetailLayout active="workflow" sections={sections}>
      <ApiWorkflowReference />
    </ApiDetailLayout>
  );
}

export default WorkflowDetailApp;
