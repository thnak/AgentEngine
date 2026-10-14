import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import PizzaOrderDetailApp from './PizzaOrderDetailApp.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <PizzaOrderDetailApp />
  </StrictMode>,
)
