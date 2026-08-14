import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import WorkflowConcurrentDetailApp from './WorkflowConcurrentDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <WorkflowConcurrentDetailApp />
  </StrictMode>,
)
