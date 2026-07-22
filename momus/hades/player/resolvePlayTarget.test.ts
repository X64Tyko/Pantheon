import { describe, it, expect, vi, beforeEach } from 'vitest'
import { resolvePlayTarget, resolvePlayPath } from '@/player/resolvePlayTarget'
import { api } from '@/api/client'

// The show/movie resume-point branching (watch-state vs next-episode vs
// earliest-episode fallback) moved server-side into Kairos's
// GET /shows/:id/resolve-play-target (see PlaybackService.cpp, tested there) —
// these two functions are now just a thin client wrapper + path-builder
// around that one endpoint, so that's all this file covers.
vi.mock('@/api/client', () => ({
  api: {
    getResolvedPlayTarget: vi.fn(),
  },
}))

const mockApi = api as Record<'getResolvedPlayTarget', ReturnType<typeof vi.fn>>

describe('resolvePlayTarget', () => {
  beforeEach(() => {
    vi.resetAllMocks()
  })

  it('movies resolve directly with no API call', async () => {
    const target = await resolvePlayTarget('movie', 'm1')
    expect(target).toEqual({ kind: 'movie', id: 'm1', positionMs: 0 })
    expect(mockApi.getResolvedPlayTarget).not.toHaveBeenCalled()
  })

  it('shows delegate to the server-resolved target', async () => {
    mockApi.getResolvedPlayTarget.mockResolvedValue({ kind: 'episode', id: 'e2', position_ms: 42_000 })
    const target = await resolvePlayTarget('show', 'sh1')
    expect(target).toEqual({ kind: 'episode', id: 'e2', positionMs: 42_000 })
    expect(mockApi.getResolvedPlayTarget).toHaveBeenCalledWith('sh1')
  })

  it('a show the server has no target for resolves to null', async () => {
    mockApi.getResolvedPlayTarget.mockResolvedValue(null)
    const target = await resolvePlayTarget('show', 'sh1')
    expect(target).toBeNull()
  })
})

describe('resolvePlayPath', () => {
  beforeEach(() => {
    vi.resetAllMocks()
  })

  it('movies resolve directly with no ?t= (position is always 0)', async () => {
    const path = await resolvePlayPath('movie', 'm1')
    expect(path).toBe('/player/movie/m1')
  })

  it('includes ?t= when the resolved target has a non-zero position', async () => {
    mockApi.getResolvedPlayTarget.mockResolvedValue({ kind: 'episode', id: 'e2', position_ms: 42_000 })
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e2?t=42000')
  })

  it('omits ?t= when the resolved target starts at position 0', async () => {
    mockApi.getResolvedPlayTarget.mockResolvedValue({ kind: 'episode', id: 'e1', position_ms: 0 })
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBe('/player/episode/e1')
  })

  it('returns null when the show has no resolvable target', async () => {
    mockApi.getResolvedPlayTarget.mockResolvedValue(null)
    const path = await resolvePlayPath('show', 'sh1')
    expect(path).toBeNull()
  })
})
