import { authHeaders } from '../api/client'

// These hit Hermes's /stream/* routes directly (Hephaestus's stream engine),
// not Kairos's /api/* — a separate surface from api/client.ts's request().

export interface VodTrackVideo { codec: string; width: number; height: number }
export interface VodTrackAudio { index: number; codec: string; language: string; title: string; channels: number }
export interface VodTrackSubtitle { index: number; codec: string; language: string; title: string; extractable: boolean }
export interface VodTracks { video: VodTrackVideo[]; audio: VodTrackAudio[]; subtitles: VodTrackSubtitle[] }

export interface VodStartResponse {
  session_id:    string
  manifest_url:  string
  subtitle_url?: string
  direct_play:   boolean
  duration_ms:   number
  title:         string
  tracks:        VodTracks
}

export interface VodStartParams {
  content_type: 'movie' | 'episode'
  content_id:   string
  audio_track?:    number
  subtitle_track?: number
  position_ms?:    number
}

export async function startVodPlayback(params: VodStartParams): Promise<VodStartResponse> {
  const res = await fetch('/stream/vod/start', {
    method:  'POST',
    headers: { 'Content-Type': 'application/json', ...authHeaders() },
    body:    JSON.stringify(params),
  })
  if (!res.ok) {
    const body = await res.json().catch(() => ({ error: res.statusText }))
    throw new Error((body as { error?: string }).error ?? res.statusText)
  }
  return res.json()
}

// Fire-and-forget — called on unmount/seek/track-switch to tear down the
// superseded session. Never throws; the session will also self-reap on its
// own idle timeout if this doesn't land.
export function stopVodPlayback(sessionId: string) {
  fetch(`/stream/vod/${sessionId}/stop`, { method: 'POST', headers: authHeaders() }).catch(() => {})
}

export function liveChannelManifestUrl(channelId: string): string {
  return `/stream/hls/channels/${channelId}/playlist.m3u8`
}
