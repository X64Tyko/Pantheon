import { useEffect, useRef, useState } from 'react'
import { useCastApiReady } from './CastProvider'
import { buildLoadRequest, type CastMediaArgs } from './castMedia'

export interface CastSessionState {
  available:  boolean // SDK loaded + a receiver App ID has been registered (CastProvider)
  connected:  boolean
  deviceName: string | null
  currentMs:  number
  durationMs: number
  paused:     boolean
  load:       (args: CastMediaArgs) => Promise<void>
  togglePlay: () => void
  seek:       (ms: number) => void
  endSession: () => void
}

// One RemotePlayer/RemotePlayerController pair per mount — the framework
// itself tracks which (if any) session they're bound to internally, so these
// don't need to be recreated on session start/end, only re-read via the
// ANY_CHANGE listener below.
export function useCastSession(): CastSessionState {
  const ready = useCastApiReady()
  const [, forceUpdate] = useState(0)
  const playerRef     = useRef<cast.framework.RemotePlayer | null>(null)
  const controllerRef = useRef<cast.framework.RemotePlayerController | null>(null)

  useEffect(() => {
    if (!ready) return
    const player     = new cast.framework.RemotePlayer()
    const controller = new cast.framework.RemotePlayerController(player)
    playerRef.current     = player
    controllerRef.current = controller

    const onChange = () => forceUpdate(n => n + 1)
    controller.addEventListener(cast.framework.RemotePlayerEventType.ANY_CHANGE, onChange)
    return () => controller.removeEventListener(cast.framework.RemotePlayerEventType.ANY_CHANGE, onChange)
  }, [ready])

  const player = playerRef.current
  const session = ready ? cast.framework.CastContext.getInstance().getCurrentSession() : null

  const load = async (args: CastMediaArgs) => {
    if (!session) return
    await session.loadMedia(buildLoadRequest(args))
  }

  return {
    available:  ready,
    connected:  !!player?.isConnected,
    deviceName: session?.getCastDevice().friendlyName ?? null,
    currentMs:  (player?.currentTime ?? 0) * 1000,
    durationMs: (player?.duration ?? 0) * 1000,
    paused:     player?.isPaused ?? false,
    load,
    togglePlay: () => controllerRef.current?.playOrPause(),
    seek: ms => {
      if (!player || !controllerRef.current) return
      player.currentTime = ms / 1000
      controllerRef.current.seek()
    },
    endSession: () => cast.framework.CastContext.getInstance().endCurrentSession(true),
  }
}
