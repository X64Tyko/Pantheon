// Shape stashed on chrome.cast.media.MediaInfo.customData by the sender
// (castMedia.ts's buildLoadRequest) and read back off
// messages.LoadRequestData.media.customData by the receiver
// (CastReceiverProvider.tsx). An HLS manifest URL alone can't be reversed
// back into a Hades route, so the sender embeds the route's own identity
// directly rather than making the receiver guess at it.
export interface CastCustomData {
  contentType: 'movie' | 'episode' | 'channel'
  contentId:   string
  positionMs?:    number
  // Carries the sender's *currently selected* tracks so the receiver's fresh
  // session (see CastReceiverProvider's LOAD interceptor, which discards the
  // sender's manifestUrl and starts its own /stream/vod/start) opens with the
  // same tracks instead of silently resetting to audio-auto/subtitles-off.
  // Mid-cast track switching is still out of scope for v1 (see castMedia.ts)
  // — this only fixes the initial handoff.
  audioTrack?:    number
  subtitleTrack?: number
}

export function isCastCustomData(data: unknown): data is CastCustomData {
  if (!data || typeof data !== 'object') return false
  const d = data as Record<string, unknown>
  return (d.contentType === 'movie' || d.contentType === 'episode' || d.contentType === 'channel')
    && typeof d.contentId === 'string'
}
