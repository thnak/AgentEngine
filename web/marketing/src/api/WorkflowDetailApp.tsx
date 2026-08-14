import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiWorkflowReference } from "../components/ApiWorkflowReference";

const sections = {
  en: [
    { id: "workflow", label: "Overview" },
    { id: "workflow-edge-kinds", label: "Six edge kinds" },
    { id: "workflow-graph-shape", label: "The graph, verbatim" },
    { id: "workflow-patterns-catalog", label: "Eight patterns, by use case" },
    { id: "workflow-engine", label: "Running one" },
    { id: "workflow-hitl", label: "Human-in-the-loop" },
    { id: "workflow-checkpoint", label: "Checkpoint & time-travel" },
    { id: "workflow-failure", label: "Failure handling" },
  ],
  vi: [
    { id: "workflow", label: "Tổng quan" },
    { id: "workflow-edge-kinds", label: "Sáu loại edge" },
    { id: "workflow-graph-shape", label: "Đồ thị, nguyên văn" },
    { id: "workflow-patterns-catalog", label: "Tám mẫu, theo use case" },
    { id: "workflow-engine", label: "Chạy một workflow" },
    { id: "workflow-hitl", label: "Human-in-the-loop" },
    { id: "workflow-checkpoint", label: "Checkpoint & time-travel" },
    { id: "workflow-failure", label: "Xử lý thất bại" },
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
