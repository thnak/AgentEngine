import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import UseCasesDetailApp from './UseCasesDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <UseCasesDetailApp />
  </StrictMode>,
)
