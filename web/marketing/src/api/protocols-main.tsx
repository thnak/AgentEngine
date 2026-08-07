import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import ProtocolsDetailApp from './ProtocolsDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <ProtocolsDetailApp />
  </StrictMode>,
)
