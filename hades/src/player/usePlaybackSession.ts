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
  // Absolute position (source-file ms) the *current* manifest's own t=0
  // represents. Every (re)load — initial mount, a seek outside the buffered
  // range, or a track switch — starts a brand new VOD encode/manifest whose
  // internal HLS timeline begins at 0 regardless of where in the source file
  // it actually starts (VideoPlayer forces hls.js startPosition:0 for VOD).
  // Callers must add this to the <video> element's own currentTime to get
  // the true position — see PlayerPage's onTimeUpdate.
  basePositionMs: number
  // VOD only — restarts the session at the given position/track selection.
  // Both share one code path: a track switch is "seek to current position
  // with a different track," a seek is "same tracks, different position."
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
  const [basePositionMs, setBasePositionMs] = useState(initialPositionMs)

  const sessionIdRef = useRef<string | null>(null)
  const genRef        = useRef(0) // guards against a stale reload's response landing after a newer one starts

  const isLive = target.kind === 'channel'

  const load = useCallback((positionMs: number, aTrack: number, sTrack: number) => {
    const gen = ++genRef.current
    const prevSession = sessionIdRef.current
    sessionIdRef.current = null

    if (prevSession) stopVodPlayback(prevSession)

    // Set synchronously (not inside the .then() below) so it's already
    // correct by the time the new manifest's first onTimeUpdate tick lands,
    // rather than racing it.
    setBasePositionMs(positionMs)

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
    title, durationMs, tracks, audioTrack, subtitleTrack, reload, basePositionMs,
  }
}
