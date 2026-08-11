import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import WorkflowDetailApp from './WorkflowDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <WorkflowDetailApp />
  </StrictMode>,
)
