import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import CodeActDetailApp from './CodeActDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <CodeActDetailApp />
  </StrictMode>,
)
