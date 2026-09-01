import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import BuilderDetailApp from './BuilderDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <BuilderDetailApp />
  </StrictMode>,
)
