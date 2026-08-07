import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import AgentDetailApp from './AgentDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <AgentDetailApp />
  </StrictMode>,
)
