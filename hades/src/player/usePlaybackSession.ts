import { useEffect, useRef, useState, useCallback } from 'react'
import { startVodPlayback, stopVodPlayback, liveChannelManifestUrl } from './playbackApi'
import type { VodTracks } from './playbackApi'
import { api } from '../api/client'

export type PlaybackTarget =
  | { kind: 'movie';   id: string }
  | { kind: 'episode'; id: string }
  | { kind: 'channel'; id: string }

export interface PlaybackSession {
  loading:       boolean
  error:         string | null
  manifestUrl:   string | null
  subtitleUrl:   string | null
  isLive:        boolean
  directPlay:    boolean | null // null: n/a (live)
  title:         string
  durationMs:    number
  tracks:        VodTracks | null // null: loading, or n/a (live)
  audioTrack:    number
  subtitleTrack: number
  // Where the *current* manifest/video element should start playback from.
  // Hephaestus's VOD manifest now describes the WHOLE file's real absolute
  // timeline from segment 0, for the session's entire life (a plain seek
  // stays on the same session/manifest — see VideoPlayer.tsx/PlayerPage.tsx)
  // — this is no longer an addend recovering true position from a
  // manifest-relative one (see git history for the old basePositionMs if
  // that's ever needed), just the one-time seek target for a freshly
  // (re)loaded manifest (initial mount, or a NEW session from a track
  // switch — reload() is still the only thing that changes this).
  startPositionMs: number
  // VOD only — restarts the session (a genuinely new encode/manifest) at
  // the given position/track selection. Used for track switches, which
  // still need a different ffmpeg -map selection; a plain seek does NOT
  // call this anymore (see PlayerPage.tsx's handleSeek — it just seeks the
  // existing persistent manifest directly).
  reload: (opts: { positionMs?: number; audioTrack?: number; subtitleTrack?: number }) => void
}

// Position pings land on the *previous* session id, since stop() races with
// the reload — this ref lets callers stop the right one on unmount too.
export function usePlaybackSession(
  target: PlaybackTarget,
  initialPositionMs = 0,
  // Seeds the very first /stream/vod/start call — used by the Cast receiver
  // (see CastReceiverProvider's LOAD interceptor) to open a fresh session
  // with whatever the sender had selected, instead of always defaulting to
  // audio-auto/subtitles-off. -1 means "unset"/"off", same as reload()'s own
  // convention.
  initialAudioTrack = -1,
  initialSubtitleTrack = -1,
): PlaybackSession {
  const [loading,       setLoading]       = useState(true)
  const [error,         setError]         = useState<string | null>(null)
  const [manifestUrl,   setManifestUrl]   = useState<string | null>(null)
  const [subtitleUrl,   setSubtitleUrl]   = useState<string | null>(null)
  const [directPlay,    setDirectPlay]    = useState<boolean | null>(null)
  const [title,         setTitle]         = useState('')
  const [durationMs,    setDurationMs]    = useState(0)
  const [tracks,        setTracks]        = useState<VodTracks | null>(null)
  const [audioTrack,    setAudioTrack]    = useState(-1)
  const [subtitleTrack, setSubtitleTrack] = useState(-1)
  const [startPositionMs, setStartPositionMs] = useState(initialPositionMs)

  const sessionIdRef = useRef<string | null>(null)
  const genRef        = useRef(0) // guards against a stale reload's response landing after a newer one starts

  const isLive = target.kind === 'channel'

  const load = useCallback((positionMs: number, aTrack: number, sTrack: number) => {
    const gen = ++genRef.current
    const prevSession = sessionIdRef.current
    sessionIdRef.current = null

    if (prevSession) stopVodPlayback(prevSession)

    // Set synchronously (not inside the .then() below) so it's already
    // correct by the time VideoPlayer's manifestUrl effect (re)creates its
    // Hls instance and seeds startPosition from it, rather than racing it.
    setStartPositionMs(positionMs)

    if (isLive) {
      setLoading(true)
      setError(null)
      api.checkChannelAccess(target.id).then(res => {
        if (genRef.current !== gen) return // superseded while in flight
        if (!res.allowed) {
          setError('This channel is restricted on your account.')
          setLoading(false)
          return
        }
        setManifestUrl(liveChannelManifestUrl(target.id))
        setSubtitleUrl(null)
        setDirectPlay(null)
        setTracks(null)
        setLoading(false)
      }).catch(err => {
        if (genRef.current !== gen) return
        setError(err instanceof Error ? err.message : 'Failed to load channel')
        setLoading(false)
      })
      return
    }

    setLoading(true)
    setError(null)
    startVodPlayback({
      content_type:    target.kind,
      content_id:      target.id,
      audio_track:     aTrack >= 0 ? aTrack : undefined,
      // -1 is "off"; external subtitle tracks are negative (<= -2, see
      // playbackApi.ts's VodTrackSubtitle doc) so "any negative means off"
      // would wrongly collapse a real external-track selection to none.
      subtitle_track:  sTrack !== -1 ? sTrack : undefined,
      position_ms:     positionMs,
    }).then(res => {
      if (genRef.current !== gen) { stopVodPlayback(res.session_id); return } // superseded while in flight
      sessionIdRef.current = res.session_id
      setManifestUrl(res.manifest_url)
      setSubtitleUrl(res.subtitle_url ?? null)
      setDirectPlay(res.direct_play)
      setTitle(res.title)
      setDurationMs(res.duration_ms)
      setTracks(res.tracks)
      setAudioTrack(aTrack >= 0 ? aTrack : (res.tracks.audio[0]?.index ?? 0))
      setSubtitleTrack(sTrack)
      setLoading(false)
    }).catch(err => {
      if (genRef.current !== gen) return
      setError(err instanceof Error ? err.message : 'Failed to start playback')
      setLoading(false)
    })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [target.kind, target.id, isLive])

  useEffect(() => {
    load(initialPositionMs, initialAudioTrack, initialSubtitleTrack)
    return () => {
      genRef.current++
      if (sessionIdRef.current) stopVodPlayback(sessionIdRef.current)
    }
    // Only re-run when the target itself changes — initialPositionMs/
    // initialAudioTrack/initialSubtitleTrack are mount-time seeds, not
    // reactive dependencies (reload() owns track/position state after that).
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [target.kind, target.id])

  const reload: PlaybackSession['reload'] = useCallback(opts => {
    load(opts.positionMs ?? 0, opts.audioTrack ?? audioTrack, opts.subtitleTrack ?? subtitleTrack)
  }, [load, audioTrack, subtitleTrack])

  return {
    loading, error, manifestUrl, subtitleUrl, isLive, directPlay,
    title, durationMs, tracks, audioTrack, subtitleTrack, reload, startPositionMs,
  }
}
