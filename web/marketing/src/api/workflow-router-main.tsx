import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import WorkflowRouterDetailApp from './WorkflowRouterDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <WorkflowRouterDetailApp />
  </StrictMode>,
)
