import { useEffect, type RefObject } from 'react'
import Hls from 'hls.js'

interface VideoPlayerProps {
  videoRef:     RefObject<HTMLVideoElement>
  manifestUrl:  string | null
  subtitleUrl:  string | null
  autoPlay?:    boolean
  onTimeUpdate: (currentMs: number, durationMs: number) => void
  onEnded:      () => void
  onError:      (message: string) => void
}

export function VideoPlayer({ videoRef, manifestUrl, subtitleUrl, autoPlay = true, onTimeUpdate, onEnded, onError }: VideoPlayerProps) {
  useEffect(() => {
    const video = videoRef.current
    if (!video || !manifestUrl) return

    let hls: Hls | null = null

    if (Hls.isSupported()) {
      hls = new Hls()
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
