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
    direct_stream: boolean
  duration_ms:   number
  title:         string
  tracks:        VodTracks
  subtitle_burned_in: boolean
  // The actually-resolved selection (may differ from what was requested —
  // -1/"unset" resolved to a saved preference or a default track). Nothing
  // else says which master_url rendition is already active, so
  // usePlaybackSession uses these to start its own selection state in sync
  // with what's really playing instead of guessing.
  audio_track:    number
  subtitle_track: number
  // Multi-part movies (GitHub #3) — present only when the movie is
  // multi-part; duration_ms above is always THIS part's duration.
  is_multi_part?:     boolean
  part_num?:          number
  total_parts?:       number
  movie_duration_ms?: number
}

export interface VodStartParams {
  content_type: 'movie' | 'episode'
  content_id:   string
  audio_track?:    number
  subtitle_track?: number
  position_ms?:    number
  // Requests a specific part of a multi-part movie directly — see
  // resolvePlayTarget.ts's PlayTarget.partNum.
  part_num?:       number
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
  if (typeof window !== 'undefined' && (window.location.protocol === 'https:' || window.location.hostname !== 'localhost')) {
    if (data.manifest_url) data.manifest_url = window.location.origin + data.manifest_url
    if (data.subtitle_url) data.subtitle_url = window.location.origin + data.subtitle_url
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

// Capability-bucketed live channel HLS (Hephaestus's ChannelViewerRegistry) —
// an opt-in per-viewer session on top of the legacy liveChannelManifestUrl
// above, which stays exactly as-is as the fallback for a failed/declined
// start call. Resolution is entirely server-side (against whatever this
// caller's token most recently declared via declareClientCapabilities
// below), no request body needed.
export interface ChannelViewerStartResponse {
    viewer_session_id: string
    manifest_url: string
    direct_stream: boolean
}

export async function startChannelViewer(channelId: string): Promise<ChannelViewerStartResponse> {
    const res = await fetch(`/stream/channel/${channelId}/start`, {
        method: 'POST',
        headers: {'Content-Type': 'application/json', ...authHeaders()},
    })
    if (!res.ok) {
        const body = await res.json().catch(() => ({error: res.statusText}))
        throw new Error((body as { error?: string }).error ?? res.statusText)
    }
    const data = await res.json()
    if (typeof window !== 'undefined' && (window.location.protocol === 'https:' || window.location.hostname !== 'localhost')) {
        if (data.manifest_url) data.manifest_url = window.location.origin + data.manifest_url
    }
    return data
}

// Fire-and-forget, same reasoning as stopVodPlayback above — the session
// self-reaps on its own idle timeout if this doesn't land.
export function stopChannelViewer(viewerSessionId: string) {
    fetch(`/stream/channel/viewer/${viewerSessionId}/stop`, {method: 'POST', headers: authHeaders()}).catch(() => {
    })
}

export interface DeclaredClientCapabilities {
    video_codecs: string[]
    audio_codecs: string[]
    max_height?: number
}

// Re-declare on every app load, not just first-ever login — Hephaestus's
// cache is wiped on every restart (frequent — redeployed on every push), so
// a stale/never-declared token silently falls back to a conservative
// h264/aac allowlist server-side rather than this client's real support.
export function declareClientCapabilities(caps: DeclaredClientCapabilities) {
    return fetch('/stream/client-capabilities', {
        method: 'POST',
        headers: {'Content-Type': 'application/json', ...authHeaders()},
        body: JSON.stringify(caps),
    }).catch(() => {
    })
}

export function forgetClientCapabilities() {
    return fetch('/stream/client-capabilities', {method: 'DELETE', headers: authHeaders()}).catch(() => {
    })
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
    direct_stream?: boolean
  // channel sessions only — a VOD session is always exactly one viewer.
    // bucket is 'default' (transcode) or 'native' (direct-stream); a channel
    // can have both active at once as two separate sessions, previously
    // indistinguishable in this list. client_count is exact (native MPEG-TS/
    // DVR clients). hls_viewer_count is exact for capability-bucketed HLS
    // viewers on this session's own bucket (ChannelViewerRegistry); it can
    // still be 0 while a viewer is present on the legacy, non-bucketed HLS
    // URL, which has no per-viewer identity to count at all — hls_viewer_active
    // is the only signal for that remaining case.
    bucket?: string
  client_count?:      number
  hls_viewer_active?: boolean
    hls_viewer_count?: number
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
