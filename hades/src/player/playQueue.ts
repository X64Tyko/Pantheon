// Client-side "what's playing after this" mechanism shared by playlist
// playback, and shuffle-play of a show or a single season (see PlayerPage's
// queue-aware next/advance logic). Deliberately not a backend concept: the
// full ordered item list (with title/season/episode/duration already in
// hand from the playlist detail or episode list the caller already fetched)
// is stashed in sessionStorage under a short token, and PlayerPage reads it
// back via that token in the ?queue= route param. This avoids a bespoke
// "playlist session" API surface and keeps shuffle order private to this
// tab's viewing session (never persisted server-side, never resumed into
// a stale shuffle order).
export interface QueueItem {
  kind:        'movie' | 'episode'
  id:          string
  title:       string
  season?:     number
  episode?:    number
  duration_ms?: number
  /** Truthy-only flag mirroring Episode.thumb — actual image src is always built from the id. */
  thumb?:      string
}

const KEY_PREFIX = 'hds-queue-'

export function shuffleArray<T>(arr: T[]): T[] {
  const a = [...arr]
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1))
    ;[a[i], a[j]] = [a[j], a[i]]
  }
  return a
}

export function storeQueue(items: QueueItem[]): string {
  const token = Math.random().toString(36).slice(2, 10)
  sessionStorage.setItem(KEY_PREFIX + token, JSON.stringify(items))
  return token
}

export function loadQueue(token: string): QueueItem[] | null {
  const raw = sessionStorage.getItem(KEY_PREFIX + token)
  if (!raw) return null
  try {
    const items = JSON.parse(raw)
    return Array.isArray(items) ? items : null
  } catch { return null }
}

// The item immediately after `kind`/`id` in the stored queue, or null if the
// queue is gone, the current item isn't in it, or it's already the last one.
export function nextInQueue(token: string, kind: 'movie' | 'episode', id: string): QueueItem | null {
  const items = loadQueue(token)
  if (!items) return null
  const idx = items.findIndex(it => it.kind === kind && it.id === id)
  if (idx === -1 || idx + 1 >= items.length) return null
  return items[idx + 1]
}
