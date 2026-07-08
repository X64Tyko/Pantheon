import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { BrowserRouter } from 'react-router-dom'
import './index.css'
import App from './App'
import { api, TOKEN_KEY } from './api/client'
import { setConcurrency } from './channel/imageQueue'
import { initReceiverModeFromUrl } from './cast/receiverMode'
import { initRemoteLogging } from './remoteLog'

initRemoteLogging()
// localStorage — a Cast receiver's first load carries ?castToken= instead of
// an already-stored token, and extracting it any later (e.g. inside TvShell's
// own mount effect) would be too late: ProtectedRoute would already have
// redirected to /login by the time TvShell got a chance to run.
initReceiverModeFromUrl()

// Apply server-side KAIROS_SYNC_THREADS to the image prefetch queue concurrency.
// This runs at module load, before AuthContext gets a chance to check the
// session — with no token guard it fired (and got a 401) on every single page
// load, including the login page itself, spamming the server log.
if (localStorage.getItem(TOKEN_KEY)) {
  api.getSettings().then(s => setConcurrency(s.sync_threads)).catch(() => {})
}

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <BrowserRouter>
      <App />
    </BrowserRouter>
  </StrictMode>
)
