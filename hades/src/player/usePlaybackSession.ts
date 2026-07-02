import { useEffect, useRef, useState, useCallback } from 'react'
import { startVodPlayback, stopVodPlayback, liveChannelManifestUrl } from './playbackApi'
import type { VodTracks } from './playbackApi'

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
  // VOD only — restarts the session at the given position/track selection.
  // Both share one code path: a track switch is "seek to current position
  // with a different track," a seek is "same tracks, different position."
  reload: (opts: { positionMs?: number; audioTrack?: number; subtitleTrack?: number }) => void
}

// Position pings land on the *previous* session id, since stop() races with
// the reload — this ref lets callers stop the right one on unmount too.
export function usePlaybackSession(target: PlaybackTarget, initialPositionMs = 0): PlaybackSession {
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

  const sessionIdRef = useRef<string | null>(null)
  const genRef        = useRef(0) // guards against a stale reload's response landing after a newer one starts

  const isLive = target.kind === 'channel'

  const load = useCallback((positionMs: number, aTrack: number, sTrack: number) => {
    const gen = ++genRef.current
    const prevSession = sessionIdRef.current
    sessionIdRef.current = null

    if (prevSession) stopVodPlayback(prevSession)

    if (isLive) {
      setLoading(false)
      setManifestUrl(liveChannelManifestUrl(target.id))
      setSubtitleUrl(null)
      setDirectPlay(null)
      setTracks(null)
      return
    }

    setLoading(true)
    setError(null)
    startVodPlayback({
      content_type:    target.kind,
      content_id:      target.id,
      audio_track:     aTrack >= 0 ? aTrack : undefined,
      subtitle_track:  sTrack >= 0 ? sTrack : undefined,
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
    load(initialPositionMs, -1, -1)
    return () => {
      genRef.current++
      if (sessionIdRef.current) stopVodPlayback(sessionIdRef.current)
    }
    // Only re-run when the target itself changes — initialPositionMs is a
    // mount-time seed, not a reactive dependency (reload() owns position after that).
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [target.kind, target.id])

  const reload: PlaybackSession['reload'] = useCallback(opts => {
    load(opts.positionMs ?? 0, opts.audioTrack ?? audioTrack, opts.subtitleTrack ?? subtitleTrack)
  }, [load, audioTrack, subtitleTrack])

  return {
    loading, error, manifestUrl, subtitleUrl, isLive, directPlay,
    title, durationMs, tracks, audioTrack, subtitleTrack, reload,
  }
}
