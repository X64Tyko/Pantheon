import { useEffect, useState } from 'react'
import { api } from '../api/client'
import { useAuth } from '../auth/AuthContext'

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
    api.getSettings().then(s => setAppId(s.cast_app_id || null)).catch(() => {})
  }, [user])

  useEffect(() => {
    if (!ready || !appId) { setCastConfigured(false); return }
    cast.framework.CastContext.getInstance().setOptions({
      receiverApplicationId: appId,
      autoJoinPolicy: chrome.cast.AutoJoinPolicy.ORIGIN_SCOPED,
    })
    setCastConfigured(true)
  }, [ready, appId])

  return null
}

export function castAvailable(): boolean {
  return cachedReady && cachedAppIdConfigured
}
