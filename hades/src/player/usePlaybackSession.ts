import { useEffect, useRef, useState, useCallback } from 'react'
import {
    startVodPlayback,
    stopVodPlayback,
    liveChannelManifestUrl,
    startChannelViewer,
    stopChannelViewer,
    getChannelViewerStatus
} from './playbackApi'
import type { VodTracks } from './playbackApi'
import { api } from '../api/client'
import {playbackDebugLog} from './playbackDebugLog'

export type PlaybackTarget =
  | { kind: 'movie';   id: string }
  | { kind: 'episode'; id: string }
  | { kind: 'channel'; id: string }

export interface PlaybackSession {
  loading:       boolean
  error:         string | null
  manifestUrl:   string | null
  isLive:        boolean
    directStream: boolean | null // null: n/a (live)
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
  // VOD only — restarts the session (a genuinely new encode/manifest) at the
  // given position. Now only needed for a burn-in subtitle switch (a bitmap
  // subtitle is composited into the video itself, VodSession's subtitleBurnIn
  // path, so selecting one is fundamentally a different video variant, not a
  // manifest-level toggle — see PlayerPage.tsx's handleSelectSubtitle) or a
  // genuine seek-driven restart; a plain seek does NOT call this (handleSeek
  // just seeks the existing persistent manifest directly). Audio no longer
  // goes through here — see selectAudioTrack below; the audioTrack option
  // still exists since a burn-in reload needs to preserve whatever audio
  // track was already selected across the new session.
  reload: (opts: { positionMs?: number; audioTrack?: number; subtitleTrack?: number }) => void
  // Pure client-side selection against the master manifest's own AUDIO group
  // (VodSession::buildMasterPlaylist) — VideoPlayer.tsx matches this index to
  // hls.js's own audioTracks[] by the X-PANTHEON-INDEX attribute and switches
  // there directly. No network call to Hephaestus's /stream/vod/start, no new
  // session: audio has been its own independent VodEncodeStream server-side
  // since the video/audio split landed, so a track switch only ever needs
  // hls.js to point at a different already-declared AUDIO rendition — the
  // existing /stream/vod/{id}/audio/{n}/playlist.m3u8 route Hephaestus
  // already serves for exactly this. Previously this went through reload()
  // (a full new session/manifest), which tore down and reloaded the whole
  // player — visible as playback stopping and a loading spinner on every
  // audio switch — for a change that no longer needs any of that.
  selectAudioTrack: (index: number) => void
  // Pure client-side selection against the master manifest's own SUBTITLES
  // group (VodSession::buildMasterPlaylist) — VideoPlayer.tsx matches this
  // index to hls.js's own subtitleTracks[] by URL and switches there
  // directly. No network call, no session/encoder restart: unlike audio,
  // subtitle extraction has been fully decoupled from the main encoder
  // since the on-demand /subtitles/{n} pipe route landed. Text/extractable
  // tracks only — burn-in never reaches this (see reload's own comment).
  selectSubtitleTrack: (index: number) => void
  // Multi-part movies (GitHub #3) — isMultiPart false/partNum 0 for the
  // common single-file case. movieDurationMs is the summed total across all
  // parts (durationMs above stays THIS part's own duration, same meaning it
  // always had).
  isMultiPart:     boolean
  partNum:         number
  totalParts:      number
  movieDurationMs: number
  // Starts a fresh session for partNum+1 of the same movie, at position 0 —
  // the "no Up Next interstitial" continuation PlayerPage's handleVideoEnded
  // calls on HLS end-of-stream for a non-final part. A no-op if already on
  // the last part.
  advanceToNextPart: () => void
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
  // Multi-part movies (GitHub #3) — seeds the very first /stream/vod/start
  // call's part_num, same "mount-time seed only" convention as
  // initialPositionMs/initialAudioTrack/initialSubtitleTrack.
  initialPartNum = 0,
): PlaybackSession {
  const [loading,       setLoading]       = useState(true)
  const [error,         setError]         = useState<string | null>(null)
  const [manifestUrl,   setManifestUrl]   = useState<string | null>(null)
    const [directStream, setDirectStream] = useState<boolean | null>(null)
  const [title,         setTitle]         = useState('')
  const [durationMs,    setDurationMs]    = useState(0)
  const [tracks,        setTracks]        = useState<VodTracks | null>(null)
  const [audioTrack,    setAudioTrack]    = useState(-1)
  const [subtitleTrack, setSubtitleTrack] = useState(-1)
  const [startPositionMs, setStartPositionMs] = useState(initialPositionMs)
  const [isMultiPart,     setIsMultiPart]     = useState(false)
  const [partNum,         setPartNum]         = useState(initialPartNum)
  const [totalParts,      setTotalParts]      = useState(0)
  const [movieDurationMs, setMovieDurationMs] = useState(0)

  const sessionIdRef = useRef<string | null>(null)
    // Capability-bucketed channel viewer identity — separate from sessionIdRef
    // (VOD's session_id) since the two endpoints/id-spaces are unrelated; only
    // one is ever populated for a given target.kind.
    const viewerSessionIdRef = useRef<string | null>(null)
  const genRef        = useRef(0) // guards against a stale reload's response landing after a newer one starts

  const isLive = target.kind === 'channel'

  const load = useCallback((positionMs: number, aTrack: number, sTrack: number, partNumArg = 0) => {
    const gen = ++genRef.current
    const prevSession = sessionIdRef.current
    sessionIdRef.current = null
      const prevViewerSession = viewerSessionIdRef.current
      viewerSessionIdRef.current = null

      if (isLive) {
          playbackDebugLog('session',
              `load() gen=${gen} target=${target.kind}:${target.id} positionMs=${positionMs} ` +
              `prevViewerSession=${prevViewerSession ?? 'none'}`)
      }

    if (prevSession) stopVodPlayback(prevSession)
      if (prevViewerSession) stopChannelViewer(prevViewerSession)

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
          // Capability-bucketed viewer session (may get a stream-copied
          // "native" bucket instead of the universal transcode — see
          // playbackApi.ts's startChannelViewer). Purely an optimization on
          // top of a path that already works: any failure here silently falls
          // back to the legacy anonymous manifest URL rather than surfacing
          // an error, since the fallback below is exactly today's behavior.
          startChannelViewer(target.id).then(viewerRes => {
              if (genRef.current !== gen) {
                  playbackDebugLog('session', `startChannelViewer gen=${gen} superseded (current gen=${genRef.current}) — stopping viewer=${viewerRes.viewer_session_id}`)
                  stopChannelViewer(viewerRes.viewer_session_id);
                  return
              }
              playbackDebugLog('session',
                  `startChannelViewer gen=${gen} ok viewer=${viewerRes.viewer_session_id} directStream=${viewerRes.direct_stream}`)
              viewerSessionIdRef.current = viewerRes.viewer_session_id
              setManifestUrl(viewerRes.manifest_url)
              setDirectStream(viewerRes.direct_stream)
              setTracks(null)
              setLoading(false)
          }).catch(err => {
              if (genRef.current !== gen) return
              // Capability-bucketed viewer setup failed — falls back to the
              // legacy anonymous manifest URL silently (see the comment above
              // startChannelViewer's call). That fallback is otherwise
              // invisible; worth knowing about since it means this viewer
              // isn't on the pinned-bucket path the reconnect logic below
              // manages.
              playbackDebugLog('session',
                  `startChannelViewer gen=${gen} FAILED, falling back to legacy manifest: ${err instanceof Error ? err.message : String(err)}`)
              setManifestUrl(liveChannelManifestUrl(target.id))
          setDirectStream(null)
              setTracks(null)
              setLoading(false)
          })
      }).catch(err => {
        if (genRef.current !== gen) return
          playbackDebugLog('session', `checkChannelAccess gen=${gen} FAILED: ${err instanceof Error ? err.message : String(err)}`)
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
      part_num:        partNumArg > 0 ? partNumArg : undefined,
    }).then(res => {
      if (genRef.current !== gen) { stopVodPlayback(res.session_id); return } // superseded while in flight
      sessionIdRef.current = res.session_id
      setManifestUrl(res.manifest_url)
        setDirectStream(res.direct_stream)
      setTitle(res.title)
      setDurationMs(res.duration_ms)
      setTracks(res.tracks)
      // The server's own resolved selection (VodStartResponse's doc comment)
      // — not just echoing back what was requested — since -1/"unset" can
      // resolve to a saved preference or default track the client has no
      // other way to learn, and the master manifest needs this to know
      // which rendition is already DEFAULT="YES" (see VideoPlayer.tsx).
      setAudioTrack(res.audio_track)
      setSubtitleTrack(res.subtitle_track)
      setIsMultiPart(res.is_multi_part ?? false)
      setPartNum(res.part_num ?? 0)
      setTotalParts(res.total_parts ?? 0)
      setMovieDurationMs(res.movie_duration_ms ?? 0)
      setLoading(false)
    }).catch(err => {
      if (genRef.current !== gen) return
      setError(err instanceof Error ? err.message : 'Failed to start playback')
      setLoading(false)
    })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [target.kind, target.id, isLive])

  useEffect(() => {
    load(initialPositionMs, initialAudioTrack, initialSubtitleTrack, initialPartNum)
    return () => {
      genRef.current++
      if (sessionIdRef.current) stopVodPlayback(sessionIdRef.current)
        if (viewerSessionIdRef.current) stopChannelViewer(viewerSessionIdRef.current)
    }
    // Only re-run when the target itself changes — initialPositionMs/
    // initialAudioTrack/initialSubtitleTrack are mount-time seeds, not
    // reactive dependencies (reload() owns track/position state after that).
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [target.kind, target.id])

  const reload: PlaybackSession['reload'] = useCallback(opts => {
    // audioTrack defaults to the current one as a request hint when the
    // caller doesn't pass it — so a burn-in-driven restart resolves the SAME
    // audio track rather than falling back to whatever the fresh session
    // would auto-pick on its own. Stays on the same part.
    load(opts.positionMs ?? 0, opts.audioTrack ?? audioTrack, opts.subtitleTrack ?? subtitleTrack, partNum)
  }, [load, audioTrack, subtitleTrack, partNum])

    // Polls Hephaestus for a recommended bucket switch (see
    // playbackApi.ts's getChannelViewerStatus doc comment) and reconnects
    // (a real fresh viewer session, not a silent in-place swap) when one's
    // recommended. viewerSessionIdRef is read fresh on every tick rather than
    // captured, so a reconnect this same poll triggered doesn't get raced by
    // the next tick still holding the old (now-stopped) id.
    useEffect(() => {
        if (!isLive) return
        const interval = setInterval(() => {
            const vid = viewerSessionIdRef.current
            if (!vid) return // no viewer session yet, or mid-reconnect
            getChannelViewerStatus(vid).then(st => {
                if (viewerSessionIdRef.current !== vid) return // superseded while in flight
                if (st.reconnect_recommended) {
                    playbackDebugLog('session', `getChannelViewerStatus viewer=${vid} reconnect_recommended — reloading`)
                    reload({})
                }
            }).catch(() => {
            }) // best-effort — just tries again next tick
        }, 5000)
        return () => clearInterval(interval)
    }, [isLive, reload])

  const advanceToNextPart: PlaybackSession['advanceToNextPart'] = useCallback(() => {
    if (!isMultiPart || partNum >= totalParts) return
    load(0, audioTrack, subtitleTrack, partNum + 1)
  }, [load, isMultiPart, partNum, totalParts, audioTrack, subtitleTrack])

  const selectAudioTrack: PlaybackSession['selectAudioTrack'] = useCallback(index => {
    console.log('[player] selectAudioTrack called with index=', index)
      if (isLive) playbackDebugLog('track', `selectAudioTrack -> index=${index}`)
    setAudioTrack(index)
  }, [isLive])

  const selectSubtitleTrack: PlaybackSession['selectSubtitleTrack'] = useCallback(index => {
    console.log('[player] selectSubtitleTrack called with index=', index)
      if (isLive) playbackDebugLog('track', `selectSubtitleTrack -> index=${index}`)
    setSubtitleTrack(index)
  }, [isLive])

  return {
      loading, error, manifestUrl, isLive, directStream,
    title, durationMs, tracks, audioTrack, subtitleTrack, reload, selectAudioTrack, selectSubtitleTrack, startPositionMs,
    isMultiPart, partNum, totalParts, movieDurationMs, advanceToNextPart,
  }
}
