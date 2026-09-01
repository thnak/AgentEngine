import { fileURLToPath } from 'node:url'
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Served by GitHub Pages under https://thnak.github.io/AgentEngine/ (a project page, not a
// <user>.github.io root repo), so asset URLs need the repo name as a base path.
export default defineConfig({
  base: '/AgentEngine/',
  plugins: [react()],
  build: {
    rollupOptions: {
      // Multi-page build: index.html (marketing) + the API section's own page system —
      // api.html (hub) plus one detail page per part under api/.
      input: {
        main: fileURLToPath(new URL('./index.html', import.meta.url)),
        api: fileURLToPath(new URL('./api.html', import.meta.url)),
        apiAgent: fileURLToPath(new URL('./api/agent.html', import.meta.url)),
        apiBuilder: fileURLToPath(new URL('./api/builder.html', import.meta.url)),
        apiTool: fileURLToPath(new URL('./api/tool.html', import.meta.url)),
        apiCodeact: fileURLToPath(new URL('./api/codeact.html', import.meta.url)),
        apiSkill: fileURLToPath(new URL('./api/skill.html', import.meta.url)),
        apiBuiltinTools: fileURLToPath(new URL('./api/builtin-tools.html', import.meta.url)),
        apiTrustSandbox: fileURLToPath(new URL('./api/trust-sandbox.html', import.meta.url)),
        apiWorktree: fileURLToPath(new URL('./api/worktree.html', import.meta.url)),
        apiRuntime: fileURLToPath(new URL('./api/runtime.html', import.meta.url)),
        apiProviders: fileURLToPath(new URL('./api/providers.html', import.meta.url)),
        apiStreaming: fileURLToPath(new URL('./api/streaming.html', import.meta.url)),
        apiEvents: fileURLToPath(new URL('./api/events.html', import.meta.url)),
        apiMemory: fileURLToPath(new URL('./api/memory.html', import.meta.url)),
        apiDurability: fileURLToPath(new URL('./api/durability.html', import.meta.url)),
        apiWorkflow: fileURLToPath(new URL('./api/workflow.html', import.meta.url)),
        apiWorkflowSequential: fileURLToPath(new URL('./api/workflow-sequential.html', import.meta.url)),
        apiWorkflowConcurrent: fileURLToPath(new URL('./api/workflow-concurrent.html', import.meta.url)),
        apiWorkflowMapReduce: fileURLToPath(new URL('./api/workflow-map-reduce.html', import.meta.url)),
        apiWorkflowHandoff: fileURLToPath(new URL('./api/workflow-handoff.html', import.meta.url)),
        apiWorkflowRouter: fileURLToPath(new URL('./api/workflow-router.html', import.meta.url)),
        apiWorkflowGroupChat: fileURLToPath(new URL('./api/workflow-group-chat.html', import.meta.url)),
        apiWorkflowPlanner: fileURLToPath(new URL('./api/workflow-planner.html', import.meta.url)),
        apiWorkflowReflection: fileURLToPath(new URL('./api/workflow-reflection.html', import.meta.url)),
        apiPlugins: fileURLToPath(new URL('./api/plugins.html', import.meta.url)),
        apiProtocols: fileURLToPath(new URL('./api/protocols.html', import.meta.url)),
      },
    },
  },
})
