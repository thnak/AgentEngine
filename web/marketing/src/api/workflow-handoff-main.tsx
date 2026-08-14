import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import WorkflowHandoffDetailApp from './WorkflowHandoffDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <WorkflowHandoffDetailApp />
  </StrictMode>,
)
