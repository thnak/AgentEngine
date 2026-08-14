import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import WorkflowSequentialDetailApp from './WorkflowSequentialDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <WorkflowSequentialDetailApp />
  </StrictMode>,
)
