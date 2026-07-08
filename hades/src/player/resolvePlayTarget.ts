import { api } from '../api/client'

export interface PlayTarget {
  kind:       'movie' | 'episode'
  id:         string
  positionMs: number
}

// Movie: play directly. Show: resume the most recently in-progress episode
// (from watch_progress) if there is one, otherwise start from episode 1.
// Shared by any "Play" affordance that only knows a show/movie id and needs
// a concrete playable target — either to build a /player/... route (below)
// or to hand off to a session-based player like Hades' own or a cast device.
export async function resolvePlayTarget(contentType: 'show' | 'movie', id: string): Promise<PlayTarget | null> {
  if (contentType === 'movie') return { kind: 'movie', id, positionMs: 0 }

  const [progress, episodes] = await Promise.all([
    api.getWatchProgress().catch(() => []),
    api.getEpisodes(id),
  ])
  const inProgress = progress
    .filter(p => p.content_type === 'episode' && p.show_id === id)
    .sort((a, b) => b.updated_at - a.updated_at)[0]
  if (inProgress) return { kind: 'episode', id: inProgress.content_id, positionMs: inProgress.position_ms }

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
