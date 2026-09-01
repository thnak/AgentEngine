import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import EventsDetailApp from './EventsDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <EventsDetailApp />
  </StrictMode>,
)
