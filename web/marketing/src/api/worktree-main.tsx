import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import WorktreeDetailApp from './WorktreeDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <WorktreeDetailApp />
  </StrictMode>,
)
