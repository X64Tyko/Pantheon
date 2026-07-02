import { useEffect, type RefObject } from 'react'
import Hls from 'hls.js'

interface VideoPlayerProps {
  videoRef:     RefObject<HTMLVideoElement>
  manifestUrl:  string | null
  subtitleUrl:  string | null
  isLive:       boolean
  autoPlay?:    boolean
  onTimeUpdate: (currentMs: number, durationMs: number) => void
  onEnded:      () => void
  onError:      (message: string) => void
}

export function VideoPlayer({ videoRef, manifestUrl, subtitleUrl, isLive, autoPlay = true, onTimeUpdate, onEnded, onError }: VideoPlayerProps) {
  useEffect(() => {
    const video = videoRef.current
    if (!video || !manifestUrl) return

    let hls: Hls | null = null

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
      hls.on(Hls.Events.ERROR, (_evt, data) => {
        if (!data.fatal) return
        switch (data.type) {
          case Hls.ErrorTypes.NETWORK_ERROR:
            onError('Network error — the stream stopped responding.')
            break
          case Hls.ErrorTypes.MEDIA_ERROR:
            onError('Playback error — the stream could not be decoded.')
            break
          default:
            onError('Playback failed.')
        }
      })
      if (autoPlay) video.play().catch(() => {})
    } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
      // Safari: native HLS, no hls.js needed.
      video.src = manifestUrl
      if (autoPlay) video.play().catch(() => {})
    } else {
      onError('This browser cannot play HLS video.')
    }

    return () => { hls?.destroy() }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [manifestUrl])

  return (
    <video
      ref={videoRef}
      onTimeUpdate={e => onTimeUpdate(e.currentTarget.currentTime * 1000, e.currentTarget.duration * 1000)}
      onEnded={onEnded}
      style={{ width: '100%', height: '100%', background: '#000' }}
      playsInline
    >
      {subtitleUrl && <track kind="subtitles" src={subtitleUrl} default label="Subtitles" />}
    </video>
  )
}
