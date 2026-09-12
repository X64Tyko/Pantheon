import { authHeaders, TOKEN_KEY } from '../api/client'
import type {WatchTogetherSession} from '../api/types'

// These hit Hermes's /watch-together/* routes directly (live playback
// coordination — position/paused, who's host), not Kairos's /api/watch-together/*
// (identity/discovery — see api/client.ts's createWatchTogether etc). Same
// split as playbackApi.ts's own /stream/* vs /api/* separation.

// `sync` is the periodic authoritative-position tick (Hermes' own timer, see
// WatchTogetherSession.h) and also what a fresh subscriber's very first
// message always is (the current state, not a backlog). `seek`/`pause`/`play`
// are the host's own explicit actions, applied immediately rather than
// waiting for the next sync tick.
export interface WatchTogetherEvent {
  type:         'sync' | 'seek' | 'pause' | 'play'
  position_ms?: number
  paused?:      boolean
}

// EventSource can't set custom headers — same ?token= fallback
// SystemStore.connectLogs() already uses for Kairos's own /api/logs/stream,
// and that Hermes's WatchTogetherRouter.cpp explicitly accepts for this
// reason (see its resolveAuthHeader). Caller owns the returned EventSource's
// lifecycle (close it on unmount/session change) — this just wires the
// per-event-type onmessage dispatch instead of the log stream's single
// onmessage, since the wire payload here is JSON with a real `type`, not a
// single implicit line type.
export function subscribeWatchTogether(
  sessionId: string,
  onEvent: (event: WatchTogetherEvent) => void,
): EventSource {
  const token = localStorage.getItem(TOKEN_KEY)
  const url = token
    ? `/watch-together/${sessionId}/stream?token=${encodeURIComponent(token)}`
    : `/watch-together/${sessionId}/stream`
  const es = new EventSource(url)
  es.onmessage = (e: MessageEvent) => {
    try { onEvent(JSON.parse(e.data) as WatchTogetherEvent) } catch { /* malformed/ping — ignore */ }
  }
  return es
}

// Forwards to Kairos's own join (the real membership write) and merges in
// the live position/paused Kairos never tracks — one call instead of join +
// a separate snapshot fetch, so the caller can seed a joining viewer's VOD
// session at the real position (hls.js's own startPosition) instead of
// starting at 0 and relying on a later raw currentTime nudge (unreliable,
// see PlayerPage.tsx).
export async function joinWatchTogether(sessionId: string): Promise<WatchTogetherSession & {
    position_ms: number;
    paused: boolean
}> {
    const res = await fetch(`/watch-together/${sessionId}/join`, {method: 'POST', headers: authHeaders()})
    if (!res.ok) throw new Error(`watch-together join: ${res.statusText}`)
    return res.json()
}

// Host-only — the caller (PlayerPage) is responsible for only ever calling
// this when session.isHost is true; Hermes independently enforces the same
// restriction server-side regardless (403 otherwise), this is just what
// keeps a non-host's UI from ever attempting it in the first place.
export async function postWatchTogetherCommand(
  sessionId: string,
  command: { type: 'seek' | 'pause' | 'play'; position_ms?: number },
): Promise<void> {
  await fetch(`/watch-together/${sessionId}/command`, {
    method:  'POST',
    headers: { 'Content-Type': 'application/json', ...authHeaders() },
    body:    JSON.stringify(command),
  }).catch(() => {})
}

// Host-only, cheap/frequent — fire-and-forget, same spirit as
// stopVodPlayback: a dropped heartbeat just means Hermes extrapolates from a
// slightly stale baseline until the next one lands, never worth surfacing an
// error to the host over.
export async function postWatchTogetherHeartbeat(
  sessionId: string,
  positionMs: number,
  paused: boolean,
): Promise<void> {
  await fetch(`/watch-together/${sessionId}/heartbeat`, {
    method:  'POST',
    headers: { 'Content-Type': 'application/json', ...authHeaders() },
    body:    JSON.stringify({ position_ms: Math.round(positionMs), paused }),
  }).catch(() => {})
}
