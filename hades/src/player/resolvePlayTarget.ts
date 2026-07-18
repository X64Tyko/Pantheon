import { api } from '../api/client'

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
