import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import SkillDetailApp from './SkillDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <SkillDetailApp />
  </StrictMode>,
)
