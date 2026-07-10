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
export async function resolvePlayTarget(contentType: 'show' | 'movie', id: string): Promise<PlayTarget | null> {
  if (contentType === 'movie') return { kind: 'movie', id, positionMs: 0 }

  const state = await api.getShowWatchState(id).catch(() => null)
  if (state && !state.completed) return { kind: 'episode', id: state.content_id, positionMs: state.position_ms }

  if (state && state.completed) {
    const next = await api.getNextEpisode(state.content_id).catch(() => null)
    if (next) return { kind: 'episode', id: next.episode_id, positionMs: 0 }
  }

  const episodes = await api.getEpisodes(id)
  const first = [...episodes].sort((a, b) => a.season - b.season || a.episode - b.episode)[0]
  return first ? { kind: 'episode', id: first.episode_id, positionMs: 0 } : null
}

export async function resolvePlayPath(contentType: 'show' | 'movie', id: string): Promise<string | null> {
  const target = await resolvePlayTarget(contentType, id)
  if (!target) return null
  return target.positionMs > 0
    ? `/player/${target.kind}/${target.id}?t=${target.positionMs}`
    : `/player/${target.kind}/${target.id}`
}
