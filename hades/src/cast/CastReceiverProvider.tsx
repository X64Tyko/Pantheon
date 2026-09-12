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
        // Channels have no track selection (live-only, see TrackMenu's isLive
        // branch) — only build a query string for movie/episode targets.
        let qs = ''
        if (custom.contentType !== 'channel') {
          const params = new URLSearchParams()
          if (custom.positionMs) params.set('t', String(custom.positionMs))
          if (custom.audioTrack != null && custom.audioTrack >= 0) params.set('audio', String(custom.audioTrack))
          if (custom.subtitleTrack != null && custom.subtitleTrack !== -1) params.set('subtitle', String(custom.subtitleTrack))
          const s = params.toString()
          qs = s ? `?${s}` : ''
        }
        const path = custom.contentType === 'channel'
          ? `/player/channel/${custom.contentId}`
          : `/player/${custom.contentType}/${custom.contentId}${qs}`
        navigate(path)
        // Resolving with the original data (not null) is what lets the
        // sender's session.loadMedia() promise actually resolve instead of
        // hanging until it times out — null tells CAF "don't run your own
        // default handler," which is what we want (Hades' own VideoPlayer/
        // hls.js drives real playback once /player/* mounts), but it also
        // leaves the sender with nothing to resolve against on its own.
        return data
      })

      // A physical remote's Back/Home button on Android TV/Google TV gets
      // translated by the platform into a standard STOP media command, not a
      // DOM key event — PlayerManager's default STOP handling tears down the
      // loaded media and, with nothing left playing and no sender-driven
      // reconnect, ends the whole app session. Intercepting it and returning
      // null skips that default handling entirely; navigating to /tv (the
      // receiver's own browse screen — see TvShell) is what actually gives
      // "back" the close/minimize-the-player behavior instead of exiting.
      manager.setMessageInterceptor(cast.framework.messages.MessageType.STOP, () => {
        navigate('/tv', { replace: true })
        return null
      })

      // Two separate CAF timeouts, easy to conflate:
      //  - maxInactivity closes the *sender connection* if the receiver can't
      //    communicate with it — not what's happening here, but bumped anyway
      //    so a flaky connection doesn't compound the real problem below.
      //  - disableIdleTimeout is the one that actually matters: by default CAF
      //    closes the whole receiver app once *it* thinks active playback has
      //    stopped, which it tracks through PlayerManager's own default engine.
      //    Since the LOAD interceptor above hands playback off to Hades' own
      //    VideoPlayer/hls.js instead of letting that default engine drive it,
      //    CAF never sees confirmation that playback is ongoing and decides
      //    it's stopped — ending the session after its default grace period.
      //    Google's own docs call this out as required for exactly this
      //    "custom player, not the framework's" setup. Same fix pantheon-relay's
      //    bootstrap receiver already needed for the same reason.
      const options = new cast.framework.CastReceiverOptions()
      options.maxInactivity = 3600
      options.disableIdleTimeout = true
      context.start(options)
    }).catch(err => console.error('[cast-receiver]', err))
  }, [navigate])

  return null
}
