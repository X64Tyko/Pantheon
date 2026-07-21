import { authHeaders, isHdrCapableDisplay } from '../api/client'

// These hit Hermes's /stream/* routes directly (Hephaestus's stream engine),
// not Kairos's /api/* — a separate surface from api/client.ts's request().

export interface VodTrackVideo { codec: string; width: number; height: number }
export interface VodTrackAudio { index: number; codec: string; language: string; title: string; channels: number }
// extractable: text-based (subrip/ass/webvtt/...) — delivered as a WebVTT
// sidecar (subtitle_url below). burn_in: bitmap-based (PGS/DVD/DVB) —
// composited directly into the video stream, no separate URL, can't be
// toggled off without restarting playback on a different track.
// source: 'embedded' (container stream, index is its -map 0:s:N position) or
// 'external' (a sidecar .srt/.ass/.ssa/.vtt file, index is negative starting
// at -2 — see Hephaestus's Router.cpp for where that scheme is assigned).
// forced/sdh are only ever populated for external entries today — embedded
// tracks have no such disposition data available, not a bug.
export interface VodTrackSubtitle {
  index: number; codec: string; language: string; title: string
  extractable: boolean; burn_in: boolean; source: 'embedded' | 'external'
  forced?: boolean; sdh?: boolean
}
export interface VodTracks { video: VodTrackVideo[]; audio: VodTrackAudio[]; subtitles: VodTrackSubtitle[] }

export interface VodStartResponse {
  session_id:    string
  manifest_url:  string
  subtitle_url?: string
  direct_play:   boolean
  duration_ms:   number
  title:         string
  tracks:        VodTracks
  subtitle_burned_in: boolean
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
    body:    JSON.stringify({ ...params, hdr_capable: isHdrCapableDisplay() }),
  })
  if (!res.ok) {
    const body = await res.json().catch(() => ({ error: res.statusText }))
    throw new Error((body as { error?: string }).error ?? res.statusText)
  }
  const data = await res.json()
  if (data.manifest_url && typeof window !== 'undefined' && (window.location.protocol === 'https:' || window.location.hostname !== 'localhost')) {
    data.manifest_url = window.location.origin + data.manifest_url
  }
  return data
}

// Fire-and-forget — called on unmount/seek/track-switch to tear down the
// superseded session. Never throws; the session will also self-reap on its
// own idle timeout if this doesn't land.
export function stopVodPlayback(sessionId: string) {
  fetch(`/stream/vod/${sessionId}/stop`, { method: 'POST', headers: authHeaders() }).catch(() => {})
}

export function liveChannelManifestUrl(channelId: string): string {
  const url = `/stream/hls/channels/${channelId}/playlist.m3u8`
  if (typeof window !== 'undefined' && (window.location.protocol === 'https:' || window.location.hostname !== 'localhost')) {
    return window.location.origin + url
  }
  return url
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
  // channel sessions only — a VOD session is always exactly one viewer.
  // client_count is exact (native MPEG-TS/DVR clients); hls_viewer_active is
  // a presence signal only (HLS has no persistent connection to count
  // distinct viewers against), not an addition to client_count.
  client_count?:      number
  hls_viewer_active?: boolean
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
