import { useCallback, useEffect, useRef, type RefObject } from 'react'
import Hls from 'hls.js'
import { registerReceiverVideoElement } from '../cast/CastReceiverProvider'
import styles from './VideoPlayer.module.css'

interface VideoPlayerProps {
  videoRef:     RefObject<HTMLVideoElement>
  manifestUrl:  string | null
  subtitleUrl:  string | null
  isLive:       boolean
  autoPlay?:    boolean
  controls?:    boolean // native scrub bar — used by the admin Chapters review panel, not the full player (which has its own PlayerControls)
  onTimeUpdate: (currentMs: number, durationMs: number) => void
  onEnded:      () => void
  onError:      (message: string) => void
}

export function VideoPlayer({ videoRef, manifestUrl, subtitleUrl, isLive, autoPlay = true, controls = false, onTimeUpdate, onEnded, onError }: VideoPlayerProps) {
  const trackRef = useRef<HTMLTrackElement>(null)

  // <track default> alone doesn't reliably show the track here: the element
  // is only added to the DOM once subtitleUrl resolves from the (async)
  // session-start response, well after the <video> itself has mounted and
  // started playing. Browsers only honor the `default` content attribute
  // reliably for tracks present at initial parse — one added later needs its
  // TextTrack.mode set explicitly, same role ExoPlayer's
  // SELECTION_FLAG_DEFAULT plays for the Android client's sideloaded track.
  const activateSubtitleTrack = useCallback(() => {
    const track = trackRef.current?.track
    if (track) track.mode = 'showing'
  }, [])

  useEffect(() => {
    if (!subtitleUrl) return
    activateSubtitleTrack()
  }, [subtitleUrl, activateSubtitleTrack])

  useEffect(() => {
    const video = videoRef.current
    if (!video || !manifestUrl) return

    let hls: Hls | null = null

    // manifestUrl and subtitleUrl always change together — every track
    // switch/seek-outside-buffer is a brand new session (usePlaybackSession's
    // reload()), which tears down and recreates the Hls instance below in the
    // same render that updates subtitleUrl. Re-attaching media triggers the
    // browser's own resource-load algorithm, which can reset/repopulate the
    // <video>'s text tracks — racing the effect above if it happened to run
    // first. Re-asserting here, on the video's own 'loadedmetadata' (fired
    // once the new resource is actually attached and tracks are settled),
    // guarantees this always wins regardless of hook/effect ordering.
    video.addEventListener('loadedmetadata', activateSubtitleTrack)

    if (Hls.isSupported()) {
      // VOD sessions (movies/episodes) are served as a growing HLS "event"
      // playlist while Hephaestus is still transcoding (VodSession.cpp) —
      // no #EXT-X-ENDLIST until the whole file finishes. hls.js decides
      // "is this live" purely from ENDLIST absence, not the EVENT/live
      // distinction the HLS spec itself makes, so without this it defaults
      // to live-edge start behavior (liveSyncDurationCount segments back
      // from whatever's newest). That's invisible for a fast transcode
      // where segments arrive well ahead of playback, but for a slow one
      // the player keeps chasing a moving target it can never quite catch
      // and stalls indefinitely with no fatal error — "stuck on the
      // throbber." Forcing startPosition 0 makes VOD always start from the
      // beginning regardless of how far transcoding has progressed. True
      // live channels keep hls.js's default (live-edge sync is correct
      // there — that playlist is a genuinely rolling/deleting window, not
      // an append-only one).
      hls = new Hls(isLive ? {} : { startPosition: 0 })
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
      // Safari: native HLS, no hls.js needed.
      console.log('[player] Using native HLS (Safari/iOS)')
      video.src = manifestUrl
      registerReceiverVideoElement(video)
      if (autoPlay) video.play().catch(() => {})
    } else {
      onError('This browser cannot play HLS video.')
    }

    return () => {
      video.removeEventListener('loadedmetadata', activateSubtitleTrack)
      hls?.destroy()
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [manifestUrl, activateSubtitleTrack])

  return (
    <video
      ref={videoRef}
      onTimeUpdate={e => onTimeUpdate(e.currentTarget.currentTime * 1000, e.currentTarget.duration * 1000)}
      onEnded={onEnded}
      className={styles.video}
      playsInline
      controls={controls}
    >
      {subtitleUrl && <track ref={trackRef} kind="subtitles" src={subtitleUrl} default label="Subtitles" />}
    </video>
  )
}
