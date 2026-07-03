import { useEffect, useState } from 'react'
import { api } from '../api/client'

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

// Mounted once in App.tsx. Renders nothing — just wires up
// CastContext.setOptions() as soon as both the SDK and the App ID (a
// Kairos-backed runtime setting, not a Vite build-time env var — see
// SettingsPage's Chromecast section) are available. No App ID means no
// registered receiver to launch, so casting stays off rather than
// initializing against an empty string.
export function CastProvider() {
  const ready = useCastApiReady()
  const [appId, setAppId] = useState<string | null>(null)

  useEffect(() => {
    api.getSettings().then(s => setAppId(s.cast_app_id || null)).catch(() => {})
  }, [])

  useEffect(() => {
    if (!ready || !appId) return
    cast.framework.CastContext.getInstance().setOptions({
      receiverApplicationId: appId,
      autoJoinPolicy: chrome.cast.AutoJoinPolicy.ORIGIN_SCOPED,
    })
  }, [ready, appId])

  return null
}

export function castAvailable(): boolean {
  return cachedReady
}
