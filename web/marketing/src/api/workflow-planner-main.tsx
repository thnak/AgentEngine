import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import WorkflowPlannerDetailApp from './WorkflowPlannerDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <WorkflowPlannerDetailApp />
  </StrictMode>,
)
