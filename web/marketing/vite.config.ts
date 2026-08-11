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
        apiTool: fileURLToPath(new URL('./api/tool.html', import.meta.url)),
        apiCodeact: fileURLToPath(new URL('./api/codeact.html', import.meta.url)),
        apiSkill: fileURLToPath(new URL('./api/skill.html', import.meta.url)),
        apiTrustSandbox: fileURLToPath(new URL('./api/trust-sandbox.html', import.meta.url)),
        apiRuntime: fileURLToPath(new URL('./api/runtime.html', import.meta.url)),
        apiWorkflow: fileURLToPath(new URL('./api/workflow.html', import.meta.url)),
        apiPlugins: fileURLToPath(new URL('./api/plugins.html', import.meta.url)),
        apiProtocols: fileURLToPath(new URL('./api/protocols.html', import.meta.url)),
      },
    },
  },
})
