import { useEffect, useState } from 'react'
import { api } from '../api/client'

let readyListeners: Array<(ready: boolean) => void> = []
let cachedReady = false

// The gstatic <script> (index.html) calls this once the framework is
// actually usable — it can fire before or after CastProvider mounts (script
// load timing vs. React mount timing is a race), so both directions are
// handled: the global here catches an early call, and the listener list
// catches a late one.
window.__onGCastApiAvailable = (available: boolean) => {
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
