import { useEffect, useRef } from 'react'
import { useNavigate } from 'react-router-dom'
import { isCastReceiverMode } from './receiverMode'
import { isCastCustomData } from './castCustomData'

const RECEIVER_SDK_URL = 'https://www.gstatic.com/cast/sdk/libs/caf_receiver/v3/cast_receiver_framework.js'

let scriptPromise: Promise<void> | null = null
function loadReceiverSdk(): Promise<void> {
  if (scriptPromise) return scriptPromise
  scriptPromise = new Promise((resolve, reject) => {
    const script = document.createElement('script')
    script.src = RECEIVER_SDK_URL
    script.onload = () => resolve()
    script.onerror = () => reject(new Error('Failed to load Cast receiver SDK'))
    document.head.appendChild(script)
  })
  return scriptPromise
}

let playerManager: cast.framework.PlayerManager | null = null
let pendingVideo: HTMLVideoElement | null = null

// Called from VideoPlayer.tsx once its <video> element is attached (hls.js
// or Safari-native) — a no-op outside receiver mode, and safe to call before
// the receiver SDK has finished loading (the pending element is applied as
// soon as the PlayerManager becomes available).
export function registerReceiverVideoElement(video: HTMLVideoElement | null) {
  if (!isCastReceiverMode() || !video) return
  if (playerManager) playerManager.setMediaElement(video)
  else pendingVideo = video
}

// Mounted once at the app root (App.tsx), alongside CastProvider (the
// sender-side counterpart) — must live above the /tv <-> /player/* route
// boundary, not inside TvShell, because CastReceiverContext.start() and the
// PlayerManager it returns need to persist across navigation between those
// sibling routes, not be torn down and recreated on every route change.
export function CastReceiverProvider() {
  const navigate = useNavigate()
  const startedRef = useRef(false)

  useEffect(() => {
    if (!isCastReceiverMode() || startedRef.current) return
    startedRef.current = true

    loadReceiverSdk().then(() => {
      const context = cast.framework.CastReceiverContext.getInstance()
      const manager = context.getPlayerManager()
      playerManager = manager

      if (pendingVideo) {
        manager.setMediaElement(pendingVideo)
        pendingVideo = null
      }

      manager.setMessageInterceptor(cast.framework.messages.MessageType.LOAD, data => {
        const custom = data.media?.customData
        if (!isCastCustomData(custom)) {
          return new cast.framework.messages.ErrorData(cast.framework.messages.ErrorType.LOAD_FAILED)
        }
        const path = custom.contentType === 'channel'
          ? `/player/channel/${custom.contentId}`
          : `/player/${custom.contentType}/${custom.contentId}${custom.positionMs ? `?t=${custom.positionMs}` : ''}`
        navigate(path)
        // Resolving with the original data (not null) is what lets the
        // sender's session.loadMedia() promise actually resolve instead of
        // hanging until it times out — null tells CAF "don't run your own
        // default handler," which is what we want (Hades' own VideoPlayer/
        // hls.js drives real playback once /player/* mounts), but it also
        // leaves the sender with nothing to resolve against on its own.
        return data
      })

      // Default inactivity handling assumes PlayerManager's own engine is
      // driving playback and reports activity through it — since we bypass
      // that (see interceptor above), the framework never sees any activity
      // and kills the app as "idle" after its default timeout, ending the
      // whole cast session. Same fix pantheon-relay's bootstrap receiver
      // already needed for the same reason.
      const options = new cast.framework.CastReceiverOptions()
      options.maxInactivity = 3600
      context.start(options)
    }).catch(err => console.error('[cast-receiver]', err))
  }, [navigate])

  return null
}
