// Hades is always served by Hermes at the same origin (vite.config.ts proxies
// both /api and /stream to Hermes; every fetch in the app is a relative
// path) — the Chromecast device fetches media itself over the LAN, so this
// is the only "server address" plumbing casting needs.
export function toAbsoluteStreamUrl(path: string): string {
  return new URL(path, window.location.origin).toString()
}

export interface CastMetadata {
  title:    string
  subtitle?: string
  imageUrl?: string
  // TV episode metadata — renders as "Show · S1E4" on the receiver instead
  // of a bare episode title, same distinction the desktop player's title
  // bar makes.
  seriesTitle?: string
  season?:      number
  episode?:     number
}

export interface CastMediaArgs {
  manifestUrl: string   // relative, e.g. session.manifestUrl from usePlaybackSession
  isLive:      boolean
  currentMs:   number   // VOD resume position; ignored for live
  metadata:    CastMetadata
}

// Hephaestus's in-progress VOD manifests are tagged EVENT (no ENDLIST) until
// the transcode finishes (VodSession.cpp) — the same characteristic that
// already requires an explicit startPosition workaround for hls.js locally
// (VideoPlayer.tsx). Passing an explicit streamType/currentTime here avoids
// relying on the receiver's own auto-detection making the same mistake hls.js
// would without that workaround.
export function buildLoadRequest({ manifestUrl, isLive, currentMs, metadata }: CastMediaArgs): chrome.cast.media.LoadRequest {
  const mediaInfo = new chrome.cast.media.MediaInfo(toAbsoluteStreamUrl(manifestUrl), 'application/x-mpegURL')
  mediaInfo.streamType = isLive ? chrome.cast.media.StreamType.LIVE : chrome.cast.media.StreamType.BUFFERED

  const meta = metadata.seriesTitle != null
    ? Object.assign(new chrome.cast.media.TvShowMediaMetadata(), {
        seriesTitle: metadata.seriesTitle,
        title:       metadata.title,
        season:      metadata.season,
        episode:     metadata.episode,
      })
    : Object.assign(new chrome.cast.media.MovieMediaMetadata(), {
        title: metadata.title,
      })
  if (metadata.imageUrl) meta.images = [new chrome.cast.Image(toAbsoluteStreamUrl(metadata.imageUrl))]
  mediaInfo.metadata = meta

  const request = new chrome.cast.media.LoadRequest(mediaInfo)
  request.autoplay = true
  if (!isLive) request.currentTime = currentMs / 1000
  return request
}
