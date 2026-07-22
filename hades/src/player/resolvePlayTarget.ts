import { api } from '../api/client'
import type { PlaylistItem } from '../api/types'
import { type QueueItem, shuffleArray, storeQueue } from './playQueue'

export interface PlayTarget {
  kind:       'movie' | 'episode'
  id:         string
  positionMs: number
}

// Movie: play directly. Show: resume the most recently touched episode if
// it's still in progress; if it was completed (finished naturally or via
// skip/up-next — see PlayerPage's handleAdvanceToNext), continue at the next
// episode after it instead of falling back to episode 1, which is what a
// plain "most recent watch_progress row" lookup would do once that episode's
// row no longer reads as in-progress. Falls back to episode 1 if the show has
// no watch state at all, or if the completed episode was the last one.
// Shared by any "Play" affordance that only knows a show/movie id and needs
// a concrete playable target — either to build a /player/... route (below)
// or to hand off to a session-based player like Hades' own or a cast device.
//
// The actual branch lives server-side now (Kairos's GET /:id/resolve-play-target,
// PlaybackService.cpp) — this is a thin wrapper, not the algorithm. Kept as its
// own endpoint/call rather than folded into the movie case so every "Play"
// affordance still resolves once, up front, to a concrete target before
// navigating/starting a session — same route-stability/reload/cast-handoff
// guarantees as before, just one round-trip instead of 2-3.
export async function resolvePlayTarget(contentType: 'show' | 'movie', id: string): Promise<PlayTarget | null> {
  if (contentType === 'movie') return { kind: 'movie', id, positionMs: 0 }

  const target = await api.getResolvedPlayTarget(id)
  return target ? { kind: target.kind, id: target.id, positionMs: target.position_ms } : null
}

export async function resolvePlayPath(contentType: 'show' | 'movie', id: string): Promise<string | null> {
  const target = await resolvePlayTarget(contentType, id)
  if (!target) return null
  return target.positionMs > 0
    ? `/player/${target.kind}/${target.id}?t=${target.positionMs}`
    : `/player/${target.kind}/${target.id}`
}

function playlistQueueItems(items: PlaylistItem[]): QueueItem[] {
  return items.map(it => ({
    kind: it.item_type, id: it.item_id, title: it.title,
    season: it.season, episode: it.episode, duration_ms: it.duration_ms,
    thumb: it.item_type === 'episode' ? '1' : undefined,
  }))
}

// Playlist "Play" button — resolves the resume point (server-side, same
// fresh position-vs-duration logic as the show endpoint — see
// PlaylistPlayTargetHelper.cpp) and stashes the playlist's full ordered item
// list as a client-side queue (see playQueue.ts) so PlayerPage can advance
// through it without a bespoke "playlist session" API. `mode: 'shuffle'`
// keeps the resume item first (so resuming a shuffled playlist doesn't jump
// somewhere random) and shuffles only the remaining order after it.
// Doubles as "Continue" — there's no separate label/branch for it, resuming
// mid-playlist vs starting fresh from the top is the same resolved target.
export async function resolvePlaylistPlayPath(playlistId: string, items: PlaylistItem[], mode: string): Promise<string | null> {
  const target = await api.getPlaylistPlayTarget(playlistId)
  if (!target || items.length === 0) return null

  let queue = playlistQueueItems(items)
  if (mode === 'shuffle') {
    const idx = queue.findIndex(q => q.kind === target.kind && q.id === target.id)
    const current = idx >= 0 ? queue[idx] : queue[0]
    const rest = queue.filter((_, i) => i !== idx)
    queue = [current, ...shuffleArray(rest)]
  }
  const token = storeQueue(queue)
  return target.position_ms > 0
    ? `/player/${target.kind}/${target.id}?queue=${token}&t=${target.position_ms}`
    : `/player/${target.kind}/${target.id}?queue=${token}`
}

// "Restart" — ignores saved progress entirely and starts at the first item,
// in the playlist's own stored/native order (still shuffled if mode is
// 'shuffle', same as a fresh resume would be, just no resume-point lookup —
// no need to hit resolve-play-target at all for this one).
export function restartPlaylistPath(items: PlaylistItem[], mode: string): string | null {
  if (items.length === 0) return null
  let queue = playlistQueueItems(items)
  if (mode === 'shuffle') queue = shuffleArray(queue)
  const token = storeQueue(queue)
  return `/player/${queue[0].kind}/${queue[0].id}?queue=${token}`
}

// "Shuffle" — an explicit ad-hoc shuffle-play-now, regardless of the
// playlist's own configured mode (unlike Restart, which only shuffles if the
// playlist is already set to shuffle). Same "always start a fresh random
// run" philosophy as the show/season shuffle buttons (EpisodeShelf.tsx) —
// never resume-aware, no saved-position lookup.
export function shufflePlaylistPath(items: PlaylistItem[]): string | null {
  if (items.length === 0) return null
  const queue = shuffleArray(playlistQueueItems(items))
  const token = storeQueue(queue)
  return `/player/${queue[0].kind}/${queue[0].id}?queue=${token}`
}

export type PlaylistPlayAction = 'play' | 'restart' | 'shuffle'

// Shared by every playlist Play/Restart/Shuffle affordance (Library page's
// tiles and its "Viewing: <title>" chip — see PlaylistTile.tsx/LibraryPage.tsx)
// — fetches the ordered item list via the any-user GET /api/playlists/:id/items
// (not the admin-only detail route a viewer can't call) and resolves to
// whichever of the 3 paths above applies.
export async function runPlaylistPlayAction(playlistId: string, mode: string, action: PlaylistPlayAction): Promise<string | null> {
  const { items } = await api.getPlaylistItems(playlistId)
  if (action === 'play')    return resolvePlaylistPlayPath(playlistId, items, mode)
  if (action === 'restart') return restartPlaylistPath(items, mode)
  return shufflePlaylistPath(items)
}
