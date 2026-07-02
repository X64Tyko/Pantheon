import { api } from '../api/client'

// Movie: play directly. Show: resume the most recently in-progress episode
// (from watch_progress) if there is one, otherwise start from episode 1.
// Shared by any "Play" affordance that only knows a show/movie id and needs
// to land on a concrete /player/... route (detail page, homepage hero, ...).
export async function resolvePlayPath(contentType: 'show' | 'movie', id: string): Promise<string | null> {
  if (contentType === 'movie') return `/player/movie/${id}`

  const [progress, episodes] = await Promise.all([
    api.getWatchProgress().catch(() => []),
    api.getEpisodes(id),
  ])
  const inProgress = progress
    .filter(p => p.content_type === 'episode' && p.show_id === id)
    .sort((a, b) => b.updated_at - a.updated_at)[0]
  if (inProgress) return `/player/episode/${inProgress.content_id}?t=${inProgress.position_ms}`

  const first = [...episodes].sort((a, b) => a.season - b.season || a.episode - b.episode)[0]
  return first ? `/player/episode/${first.episode_id}` : null
}
