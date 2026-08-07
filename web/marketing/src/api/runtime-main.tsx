import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import RuntimeDetailApp from './RuntimeDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <RuntimeDetailApp />
  </StrictMode>,
)
