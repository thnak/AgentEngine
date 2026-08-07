import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import ToolDetailApp from './ToolDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <ToolDetailApp />
  </StrictMode>,
)
