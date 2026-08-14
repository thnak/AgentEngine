import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import WorkflowGroupChatDetailApp from './WorkflowGroupChatDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <WorkflowGroupChatDetailApp />
  </StrictMode>,
)
