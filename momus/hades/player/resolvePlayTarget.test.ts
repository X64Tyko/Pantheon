import { describe, it, expect, vi, beforeEach } from 'vitest'
import { resolvePlayPath } from '@/player/resolvePlayTarget'
import { api } from '@/api/client'

vi.mock('@/api/client', () => ({
  api: {
    getWatchProgress: vi.fn(),
    getEpisodes:      vi.fn(),
  },
}))

const mockApi = api as Record<'getWatchProgress' | 'getEpisodes', ReturnType<typeof vi.fn>>

function ep(episode_id: string, season: number, episode: number) {
  return { episode_id, season, episode, title: '', duration_ms: 0 }
}

function progress(overrides: Partial<{
  content_type: 'episode' | 'movie'; content_id: string; show_id: string
  position_ms: number; updated_at: number
}> = {}) {
  return {
    content_type: 'episode' as const, content_id: 'e1', show_id: 'sh1',
    title: '', duration_ms: 100_000, position_ms: 1000, updated_at: 1,
    ...overrides,
  }
}

describe('resolvePlayPath', () => {
  beforeEach(() => {
    vi.resetAllMocks()
    mockApi.getWatchProgress.mockResolvedValue([])
    mockApi.getEpisodes.mockResolvedValue([])
  })

  it('movies resolve directly with no API calls', async () => {
    const path = await resolvePlayPath('movie', 'm1')
    expect(path).toBe('/player/movie/m1')
    expect(mockApi.getWatchProgress).not.toHaveBeenCalled()
    expect(mockApi.getEpisodes).not.toHaveBeenCalled()
  })

  it('a show with no progress and no episodes resolves to null', async () => {
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBeNull()
  })

  it('a show with episodes but no progress starts at the earliest episode', async () => {
    mockApi.getEpisodes.mockResolvedValue([ep('e3', 2, 1), ep('e1', 1, 1), ep('e2', 1, 2)])
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e1')
  })

  it('resumes the most recently updated in-progress episode for this show', async () => {
    mockApi.getEpisodes.mockResolvedValue([ep('e1', 1, 1)])
    mockApi.getWatchProgress.mockResolvedValue([
      progress({ content_id: 'e-old', updated_at: 1, position_ms: 5000 }),
      progress({ content_id: 'e-new', updated_at: 99, position_ms: 42_000 }),
    ])
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e-new?t=42000')
  })

  it('ignores progress entries for other shows', async () => {
    mockApi.getEpisodes.mockResolvedValue([ep('e1', 1, 1)])
    mockApi.getWatchProgress.mockResolvedValue([
      progress({ show_id: 'other-show', content_id: 'e-other', updated_at: 99 }),
    ])
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e1') // falls through to first-episode logic
  })

  it('ignores non-episode progress entries (movies)', async () => {
    mockApi.getEpisodes.mockResolvedValue([ep('e1', 1, 1)])
    mockApi.getWatchProgress.mockResolvedValue([
      progress({ content_type: 'movie', content_id: 'm1', show_id: 'sh1', updated_at: 99 }),
    ])
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e1')
  })

  it('treats a getWatchProgress failure as no progress rather than rejecting', async () => {
    mockApi.getEpisodes.mockResolvedValue([ep('e1', 1, 1)])
    mockApi.getWatchProgress.mockRejectedValue(new Error('network error'))
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e1')
  })
})
