import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { BrowserRouter } from 'react-router-dom'
import './index.css'
import App from './App'
import { api } from './api/client'
import { setConcurrency } from './channel/imageQueue'

// Apply server-side KAIROS_SYNC_THREADS to the image prefetch queue concurrency.
api.getSettings().then(s => setConcurrency(s.sync_threads)).catch(() => {})

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <BrowserRouter>
      <App />
    </BrowserRouter>
  </StrictMode>
)
