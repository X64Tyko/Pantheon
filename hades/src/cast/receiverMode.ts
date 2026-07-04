import { TOKEN_KEY } from '../api/client'

// Set once, as early as possible — see main.tsx. A Cast receiver page is
// loaded fresh with ?castToken= in the URL exactly once (via
// pantheon-relay's redirect, see /ChimeraShare/pantheon-relay); after that,
// client-side route navigation (React Router) never reloads the page, so a
// plain module-level flag is enough to remember "this tab is a Cast
// receiver" for the rest of its lifetime.
let receiverMode = false

// Must run before AuthProvider's own init effect (App.tsx) ever checks
// localStorage — that's what forces this to live in main.tsx rather than,
// say, TvShell: by the time TvShell would mount, ProtectedRoute has already
// decided (based on whatever was in localStorage at AuthProvider's mount)
// whether to redirect to /login, which would happen before TvShell ever got
// a chance to extract the token from the URL.
export function initReceiverModeFromUrl() {
  const params = new URLSearchParams(window.location.search)
  const castToken = params.get('castToken')
  if (!castToken) return

  localStorage.setItem(TOKEN_KEY, castToken)
  receiverMode = true

  params.delete('castToken')
  const qs = params.toString()
  const url = window.location.pathname + (qs ? `?${qs}` : '') + window.location.hash
  window.history.replaceState(null, '', url)
}

export function isCastReceiverMode(): boolean {
  return receiverMode
}
