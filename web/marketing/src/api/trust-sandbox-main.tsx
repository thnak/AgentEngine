import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import TrustSandboxDetailApp from './TrustSandboxDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <TrustSandboxDetailApp />
  </StrictMode>,
)
