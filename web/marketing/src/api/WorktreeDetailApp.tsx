import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiWorktreeReference } from "../components/ApiWorktreeReference";

const sections = {
  en: [
    { id: "worktree", label: "Overview" },
    { id: "object-model", label: "Object model" },
    { id: "sub-worktrees", label: "Sub-worktrees" },
    { id: "concurrency-merge", label: "Concurrency & merge" },
    { id: "mounts-capabilities", label: "Mounts & capabilities" },
    { id: "persistence-lifecycle", label: "Persistence & lifecycle" },
    { id: "agent-view", label: "What the agent sees" },
    { id: "workflow-integration", label: "Workflow executors" },
    { id: "worktree-status", label: "Status" },
  ],
  vi: [
    { id: "worktree", label: "Tổng quan" },
    { id: "object-model", label: "Mô hình đối tượng" },
    { id: "sub-worktrees", label: "Sub-worktree" },
    { id: "concurrency-merge", label: "Tương tranh & merge" },
    { id: "mounts-capabilities", label: "Mount & capability" },
    { id: "persistence-lifecycle", label: "Bền vững & vòng đời" },
    { id: "agent-view", label: "Agent nhìn thấy gì" },
    { id: "workflow-integration", label: "Executor của workflow" },
    { id: "worktree-status", label: "Trạng thái" },
  ],
};

function WorktreeDetailApp() {
  return (
    <ApiDetailLayout active="worktree" sections={sections}>
      <ApiWorktreeReference />
    </ApiDetailLayout>
  );
}

export default WorktreeDetailApp;
