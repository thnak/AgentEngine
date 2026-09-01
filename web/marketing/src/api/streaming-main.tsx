import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import StreamingDetailApp from './StreamingDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <StreamingDetailApp />
  </StrictMode>,
)
