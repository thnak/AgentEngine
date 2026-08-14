import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import WorkflowMapReduceDetailApp from './WorkflowMapReduceDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <WorkflowMapReduceDetailApp />
  </StrictMode>,
)
