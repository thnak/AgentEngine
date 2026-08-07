import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import PluginsDetailApp from './PluginsDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <PluginsDetailApp />
  </StrictMode>,
)
