import { describe, it, expect, vi, beforeEach } from 'vitest'
import { resolvePlayPath } from '@/player/resolvePlayTarget'
import { api } from '@/api/client'

vi.mock('@/api/client', () => ({
  api: {
    getShowWatchState: vi.fn(),
    getNextEpisode:    vi.fn(),
    getEpisodes:       vi.fn(),
  },
}))

const mockApi = api as Record<'getShowWatchState' | 'getNextEpisode' | 'getEpisodes', ReturnType<typeof vi.fn>>

function ep(episode_id: string, season: number, episode: number) {
  return { episode_id, season, episode, title: '', duration_ms: 0 }
}

function nextEp(episode_id: string, season = 1, episode = 2) {
  return { episode_id, season, episode, title: '', duration_ms: 0, overview: '', air_date: '', thumb: '' }
}

function watchState(overrides: Partial<{
  content_id: string; position_ms: number; duration_ms: number; completed: boolean; updated_at: number
}> = {}) {
  return {
    content_id: 'e1', position_ms: 1000, duration_ms: 100_000, completed: false, updated_at: 1,
    ...overrides,
  }
}

describe('resolvePlayPath', () => {
  beforeEach(() => {
    vi.resetAllMocks()
    mockApi.getShowWatchState.mockResolvedValue(null)
    mockApi.getNextEpisode.mockResolvedValue(null)
    mockApi.getEpisodes.mockResolvedValue([])
  })

  it('movies resolve directly with no API calls', async () => {
    const path = await resolvePlayPath('movie', 'm1')
    expect(path).toBe('/player/movie/m1')
    expect(mockApi.getShowWatchState).not.toHaveBeenCalled()
    expect(mockApi.getNextEpisode).not.toHaveBeenCalled()
    expect(mockApi.getEpisodes).not.toHaveBeenCalled()
  })

  it('a show with no watch state and no episodes resolves to null', async () => {
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBeNull()
  })

  it('a show with episodes but no watch state starts at the earliest episode', async () => {
    mockApi.getEpisodes.mockResolvedValue([ep('e3', 2, 1), ep('e1', 1, 1), ep('e2', 1, 2)])
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e1')
  })

  it('resumes an in-progress episode at its saved position', async () => {
    mockApi.getShowWatchState.mockResolvedValue(watchState({ content_id: 'e2', position_ms: 42_000, completed: false }))
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e2?t=42000')
    expect(mockApi.getNextEpisode).not.toHaveBeenCalled()
  })

  it('continues at the next episode once the last-watched one was completed', async () => {
    mockApi.getShowWatchState.mockResolvedValue(watchState({ content_id: 'e1', completed: true }))
    mockApi.getNextEpisode.mockResolvedValue(nextEp('e2'))
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e2')
    expect(mockApi.getNextEpisode).toHaveBeenCalledWith('e1')
  })

  it('falls back to the earliest episode when the completed episode was the last one', async () => {
    mockApi.getEpisodes.mockResolvedValue([ep('e1', 1, 1)])
    mockApi.getShowWatchState.mockResolvedValue(watchState({ content_id: 'e1', completed: true }))
    mockApi.getNextEpisode.mockResolvedValue(null)
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e1')
  })

  it('treats a getShowWatchState failure as no watch state rather than rejecting', async () => {
    mockApi.getEpisodes.mockResolvedValue([ep('e1', 1, 1)])
    mockApi.getShowWatchState.mockRejectedValue(new Error('network error'))
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e1')
  })

  it('treats a getNextEpisode failure as no next episode rather than rejecting', async () => {
    mockApi.getEpisodes.mockResolvedValue([ep('e1', 1, 1)])
    mockApi.getShowWatchState.mockResolvedValue(watchState({ content_id: 'e1', completed: true }))
    mockApi.getNextEpisode.mockRejectedValue(new Error('network error'))
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e1')
  })
})
