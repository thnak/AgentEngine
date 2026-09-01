import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import BuiltinToolsDetailApp from './BuiltinToolsDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <BuiltinToolsDetailApp />
  </StrictMode>,
)
