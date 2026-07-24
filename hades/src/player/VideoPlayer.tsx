import { useCallback, useEffect, useRef, type RefObject } from 'react'
import Hls from 'hls.js'
import { registerReceiverVideoElement } from '../cast/CastReceiverProvider'
import styles from './VideoPlayer.module.css'

interface VideoPlayerProps {
  videoRef:        RefObject<HTMLVideoElement>
  manifestUrl:     string | null
  isLive:          boolean
  // Which subtitle track should be showing, in Hephaestus's own index scheme
  // (embedded relative_index >= 0, external sidecar <= -2, -1 = off — same
  // as VodTracks/TrackMenu's convention). VOD only; ignored for live. Now a
  // controlled selection against the master manifest's own SUBTITLES group
  // (see hephaestus's VodSession::buildMasterPlaylist) rather than a
  // separately sideloaded <track> — switching this no longer touches
  // manifestUrl/the video or audio at all.
  subtitleTrack?:  number
  // The selected track's language (BCP-47/ISO 639-2, e.g. "eng") — only
  // needed for the Safari native-HLS fallback below, which has no URL to
  // key off the way hls.js's own subtitleTracks[] does.
  subtitleLanguage?: string | null
  // Where to seek to once this (freshly loaded) manifest is ready — VOD only.
  // Hephaestus's VOD manifest now describes the whole file's real absolute
  // timeline from segment 0 for the session's entire life, so this is just
  // the one-time seek target for THIS load (initial mount, or a new session
  // from an audio track switch), not something forced to 0 the way it used to be.
  startPositionSec?: number
  autoPlay?:    boolean
  controls?:    boolean // native scrub bar — used by the admin Chapters review panel, not the full player (which has its own PlayerControls)
  onTimeUpdate: (currentMs: number, durationMs: number) => void
  onEnded:      () => void
  onError:      (message: string) => void
}

export function VideoPlayer({ videoRef, manifestUrl, isLive, subtitleTrack = -1, subtitleLanguage = null, startPositionSec, autoPlay = true, controls = false, onTimeUpdate, onEnded, onError }: VideoPlayerProps) {
  const hlsRef = useRef<Hls | null>(null)

  // Maps our subtitleTrack index onto whichever of hls.js's own
  // subtitleTracks[] actually corresponds to it — hls.js assigns its own
  // opaque sequential ids that don't otherwise carry our index, so this
  // matches by URL instead: every /subtitles/{n}/playlist.m3u8 route
  // Hephaestus generates embeds our index literally (see
  // buildMasterPlaylist), so it round-trips exactly. -1 (off) just disables
  // the active track. Safe to call before hls.js has parsed the manifest —
  // subtitleTracks is empty until SUBTITLE_TRACKS_UPDATED, so this is a
  // silent no-op until the listener below re-invokes it.
  const applySubtitleTrack = useCallback(() => {
    const hls = hlsRef.current
    if (!hls) return
    if (subtitleTrack < 0) { hls.subtitleTrack = -1; return }
    const idx = hls.subtitleTracks.findIndex(t => t.url.includes(`/subtitles/${subtitleTrack}/playlist.m3u8`))
    if (idx !== -1) hls.subtitleTrack = idx
  }, [subtitleTrack])

  // Safari (native HLS, no hls.js — see the else-if branch below) parses
  // SUBTITLES groups into the <video> element's own native textTracks
  // itself; matched by language since TextTrack has no URL to key off the
  // way hls.js's MediaPlaylist does. Imprecise if two tracks share a
  // language (picks the first) — the old single-<track> sideload had the
  // same limitation for Safari (only ever offered the one server-resolved
  // track regardless of duplicates).
  //
  // Guarded on !hlsRef.current — hls.js renders subtitles natively too (its
  // own textTracks entries on this same <video>), and setting .mode here
  // unconditionally fought its internal track-mode management: it worked
  // once on initial load (before any manual switch ever ran this with
  // tracks already populated) but broke on every switch after, since this
  // was forcibly disabling the track hls.js had just enabled and only
  // sometimes matching it back on by language.
  const applyNativeSubtitleTrack = useCallback(() => {
    if (hlsRef.current) return
    const video = videoRef.current
    if (!video) return
    for (const t of Array.from(video.textTracks)) t.mode = 'disabled'
    if (subtitleTrack < 0 || !subtitleLanguage) return
    const match = Array.from(video.textTracks).find(t => t.language === subtitleLanguage)
    if (match) match.mode = 'showing'
  }, [subtitleTrack, subtitleLanguage])

  // Re-applies whenever the caller changes which track should show — the
  // only trigger now, since a subtitle switch no longer reloads manifestUrl.
  useEffect(() => { applySubtitleTrack() }, [applySubtitleTrack])
  useEffect(() => { applyNativeSubtitleTrack() }, [applyNativeSubtitleTrack])

  useEffect(() => {
    const video = videoRef.current
    if (!video || !manifestUrl) return

    let hls: Hls | null = null

    if (Hls.isSupported()) {
      // VOD sessions get a complete, #EXT-X-ENDLIST-terminated playlist from
      // Hephaestus immediately (the whole file's segment list is known and
      // declared upfront — see VodSession.cpp's sliding-window engine), so
      // hls.js correctly treats it as on-demand rather than live from the
      // very first fetch. startPosition seeds where in that (now-absolute,
      // whole-file) timeline THIS load should begin — true live channels
      // keep hls.js's own default live-edge sync instead (that playlist is a
      // genuinely rolling/deleting window, not an append-only one).
      hls = new Hls(isLive ? {} : { startPosition: startPositionSec ?? 0 })
      hlsRef.current = hls
      // Fires once hls.js has parsed the manifest's SUBTITLES groups —
      // applySubtitleTrack no-ops until then (subtitleTracks is empty), so
      // this is what actually applies the initial selection.
      hls.on(Hls.Events.SUBTITLE_TRACKS_UPDATED, applySubtitleTrack)
      hls.loadSource(manifestUrl)
      hls.attachMedia(video)
      registerReceiverVideoElement(video)

      // A "fatal" hls.js error is fatal to the current internal load state,
      // not necessarily to the stream — a channel's backing ffmpeg process
      // can keep transcoding and appending fresh segments right through a
      // transient network blip (a slow/aborted playlist fetch, a segment
      // request racing hls_flags=delete_segments off the rolling window) or
      // a decoder hiccup, and hls.js's own docs call for attempting
      // recovery before giving up: startLoad() re-polls the (very much
      // still-live) playlist for NETWORK_ERROR, recoverMediaError() for
      // MEDIA_ERROR. Previously this surfaced onError on the very first
      // fatal event with no recovery attempt at all, which read as "the
      // stream stopped playing" to anyone watching a 24/7 live channel even
      // though the encode never actually stopped. Retries are capped and
      // per-error-type so a genuinely dead stream still surfaces onError
      // rather than looping forever, but the counters reset on the next
      // successfully loaded fragment so a channel that recovers cleanly
      // isn't left with an exhausted budget for the next unrelated blip
      // hours later.
      let networkRetries = 0
      let mediaRetries   = 0
      const MAX_RETRIES  = 3
      hls.on(Hls.Events.FRAG_LOADED, () => { networkRetries = 0; mediaRetries = 0 })
      hls.on(Hls.Events.ERROR, (_evt, data) => {
        if (!data.fatal) return
        switch (data.type) {
          case Hls.ErrorTypes.NETWORK_ERROR:
            if (networkRetries++ < MAX_RETRIES) hls!.startLoad()
            else onError('Network error — the stream stopped responding.')
            break
          case Hls.ErrorTypes.MEDIA_ERROR:
            if (mediaRetries++ < MAX_RETRIES) hls!.recoverMediaError()
            else onError('Playback error — the stream could not be decoded.')
            break
          default:
            onError('Playback failed.')
        }
      })
      if (autoPlay) video.play().catch(() => {})
    } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
      // Safari: native HLS, no hls.js needed. Unlike hls.js there's no
      // startPosition option — the manifest's own absolute timeline means
      // native playback would otherwise just start at 0, so seek explicitly
      // once metadata (duration/seekable range) is actually available.
      console.log('[player] Using native HLS (Safari/iOS)')
      video.src = manifestUrl
      registerReceiverVideoElement(video)
      // Safari populates video.textTracks from the manifest's SUBTITLES
      // group only once metadata is actually loaded — same event the seek
      // below already waits on.
      video.addEventListener('loadedmetadata', applyNativeSubtitleTrack)
      if (!isLive && startPositionSec) {
        const seekOnce = () => { video.currentTime = startPositionSec; video.removeEventListener('loadedmetadata', seekOnce) }
        video.addEventListener('loadedmetadata', seekOnce)
      }
      if (autoPlay) video.play().catch(() => {})
    } else {
      onError('This browser cannot play HLS video.')
    }

    return () => {
      video.removeEventListener('loadedmetadata', applyNativeSubtitleTrack)
      hlsRef.current = null
      hls?.destroy()
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [manifestUrl])

  return (
    <video
      ref={videoRef}
      onTimeUpdate={e => onTimeUpdate(e.currentTarget.currentTime * 1000, e.currentTarget.duration * 1000)}
      onEnded={onEnded}
      className={styles.video}
      playsInline
      controls={controls}
    />
  )
}
