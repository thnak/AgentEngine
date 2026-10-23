import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import FirstAppDetailApp from './FirstAppDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <FirstAppDetailApp />
  </StrictMode>,
)
