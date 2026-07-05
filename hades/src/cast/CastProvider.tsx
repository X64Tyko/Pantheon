import { useEffect, useState } from 'react'
import { api } from '../api/client'
import { useAuth } from '../auth/AuthContext'

// Must match pantheon-relay/receiver/src/main.ts's own NAMESPACE constant —
// the two repos can't share a literal, so this is duplicated by hand on
// both sides of the handoff.
const RELAY_NAMESPACE = 'urn:x-cast:com.pantheon.relay'

declare global {
  interface Window {
    __pantheonCastReady?: boolean
    __pantheonCastReadyListener?: (ready: boolean) => void
  }
}

let readyListeners: Array<(ready: boolean) => void> = []
// index.html defines window.__onGCastApiAvailable as a plain inline script,
// executed before the gstatic <script> tag — the SDK's call to it is a race
// against this module even loading (this is a JS module, deferred until
// after the page parses), so the actual capture has to live in the HTML
// itself. __pantheonCastReady catches an early call that already landed;
// __pantheonCastReadyListener (registered below) catches a later one.
let cachedReady = !!window.__pantheonCastReady

window.__pantheonCastReadyListener = (available: boolean) => {
  cachedReady = available
  readyListeners.forEach(fn => fn(available))
}

export function useCastApiReady(): boolean {
  const [ready, setReady] = useState(cachedReady)
  useEffect(() => {
    if (cachedReady) { setReady(true); return }
    readyListeners.push(setReady)
    return () => { readyListeners = readyListeners.filter(fn => fn !== setReady) }
  }, [])
  return ready
}

// Separate from useCastApiReady (SDK script loaded): also requires
// cast_app_id to be configured, matching CastProvider's own "no App ID means
// no registered receiver, so casting stays off" comment below. Without this,
// useCastSession's `available` tracked SDK-load alone, so the Cast button
// showed as clickable even with no App ID set — requestSession() against a
// CastContext that was never given a receiverApplicationId doesn't open the
// app's own device picker; it falls through to Chrome's native tab-mirroring
// flow, which is a plausible source of an unrelated Chrome dialog (its
// native cast picker surfaces a "Live Caption" toggle) instead of casting.
let cachedAppIdConfigured = false
let configuredListeners: Array<(configured: boolean) => void> = []

function setCastConfigured(configured: boolean) {
  if (cachedAppIdConfigured === configured) return
  cachedAppIdConfigured = configured
  configuredListeners.forEach(fn => fn(configured))
}

export function useCastAvailable(): boolean {
  const [available, setAvailable] = useState(cachedReady && cachedAppIdConfigured)
  useEffect(() => {
    const recompute = () => setAvailable(cachedReady && cachedAppIdConfigured)
    recompute()
    readyListeners.push(recompute)
    configuredListeners.push(recompute)
    return () => {
      readyListeners = readyListeners.filter(fn => fn !== recompute)
      configuredListeners = configuredListeners.filter(fn => fn !== recompute)
    }
  }, [])
  return available
}

// Mounted once in App.tsx. Renders nothing — just wires up
// CastContext.setOptions() as soon as both the SDK and the App ID (a
// Kairos-backed runtime setting, not a Vite build-time env var — see
// SettingsPage's Chromecast section) are available. No App ID means no
// registered receiver to launch, so casting stays off rather than
// initializing against an empty string.
export function CastProvider() {
  const ready = useCastApiReady()
  const { user } = useAuth()
  const [appId, setAppId] = useState<string | null>(null)

  useEffect(() => {
    // CastProvider is mounted at the App root, outside ProtectedRoute — with
    // no auth check this fired (and got a 401) on the login page too. Keyed
    // on `user` rather than a one-time mount check so it also fires right
    // after login, without requiring a page refresh.
    if (!user) return
    api.getPublicSettings().then(s => setAppId(s.cast_app_id || null)).catch(() => {})
  }, [user])

  useEffect(() => {
    if (!ready || !appId) { setCastConfigured(false); return }
    cast.framework.CastContext.getInstance().setOptions({
      receiverApplicationId: appId,
      autoJoinPolicy: chrome.cast.AutoJoinPolicy.ORIGIN_SCOPED,
    })
    setCastConfigured(true)
  }, [ready, appId])

  // First cast to a device (pantheon-relay's bootstrap, not yet handed off
  // to this Hades server's own /tv): mint a dedicated, revocable,
  // viewer-scoped session (Kairos forces role=viewer server-side for
  // purpose='cast' tokens, regardless of this account's real role — see
  // AuthStore::validate) and hand it to the receiver over Cast's custom
  // message channel. Only on SESSION_STARTED, not SESSION_RESUMED — a
  // resumed session means the receiver already has a working token in its
  // own localStorage from its first pairing (or has already taken over as
  // its own CastReceiverContext, in which case nothing is listening on
  // RELAY_NAMESPACE anymore and this would be a silent no-op anyway).
  useEffect(() => {
    if (!ready) return
    const context = cast.framework.CastContext.getInstance()
    const onSessionStateChanged = (event: cast.framework.SessionStateEventData) => {
      if (event.sessionState !== cast.framework.SessionState.SESSION_STARTED) return
      api.mintCastToken().then(({ token }) => {
        event.session.sendMessage(RELAY_NAMESPACE, {
          hadesOrigin: window.location.origin,
          castToken:   token,
        }).catch(() => {}) // no-op against a receiver that isn't pantheon-relay's bootstrap (e.g. Google's default receiver during dev)
      }).catch(() => {})
    }
    context.addEventListener(cast.framework.CastContextEventType.SESSION_STATE_CHANGED, onSessionStateChanged)
    return () => context.removeEventListener(cast.framework.CastContextEventType.SESSION_STATE_CHANGED, onSessionStateChanged)
  }, [ready])

  return null
}

export function castAvailable(): boolean {
  return cachedReady && cachedAppIdConfigured
}
