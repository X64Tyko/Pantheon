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

// Activity page "Now Playing" — see hephaestus/src/api/ActivityRouter.cpp.
export interface ActivitySession {
  id:              string
  kind:            'channel' | 'vod'
  title:           string
  file_path:       string
  hw_accel:        string
  decode_hw_accel: string
  started_at_ms:   number
  direct_play?:    boolean
}

export async function getActivitySessions(): Promise<ActivitySession[]> {
  const res = await fetch('/stream/activity/sessions', { headers: authHeaders() })
  if (!res.ok) throw new Error(`activity sessions: ${res.statusText}`)
  return res.json()
}

// Polled, not a live stream — the shared log buffer isn't partitioned per
// session, so this is a filtered snapshot of the most recent matching lines,
// not a tail -f. Call on an interval from the UI.
export async function getSessionLogs(sessionId: string, lines = 300): Promise<string[]> {
  const res = await fetch(`/stream/activity/sessions/${sessionId}/logs?lines=${lines}`, { headers: authHeaders() })
  if (!res.ok) throw new Error(`session logs: ${res.statusText}`)
  return res.json()
}
