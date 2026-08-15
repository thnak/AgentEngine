import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import MemoryDetailApp from './MemoryDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <MemoryDetailApp />
  </StrictMode>,
)
