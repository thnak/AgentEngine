import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiWorkflowReference } from "../components/ApiWorkflowReference";

const sections = [
  { id: "workflow", label: "Overview" },
  { id: "workflow-edge-kinds", label: "Six edge kinds" },
  { id: "workflow-graph-shape", label: "The graph, verbatim" },
  { id: "workflow-engine", label: "Running one" },
];

function WorkflowDetailApp() {
  return (
    <ApiDetailLayout active="workflow" sections={sections}>
      <ApiWorkflowReference />
    </ApiDetailLayout>
  );
}

export default WorkflowDetailApp;
