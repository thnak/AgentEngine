import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Served by GitHub Pages under https://thnak.github.io/AgentEngine/ (a project page, not a
// <user>.github.io root repo), so asset URLs need the repo name as a base path.
export default defineConfig({
  base: '/AgentEngine/',
  plugins: [react()],
})
