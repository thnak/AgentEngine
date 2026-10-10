import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import HowItWorksDetailApp from './HowItWorksDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <HowItWorksDetailApp />
  </StrictMode>,
)
