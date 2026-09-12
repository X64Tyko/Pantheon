import { describe, it, expect, vi, beforeEach } from 'vitest'
import { resolvePlayTarget, resolvePlayPath } from '@/player/resolvePlayTarget'
import { api } from '@/api/client'

// The show/movie resume-point branching (watch-state vs next-episode vs
// earliest-episode fallback) moved server-side into Kairos's
// GET /shows|movies/:id/resolve-play-target (see PlaybackService.cpp, tested
// there) — these two functions are now just a thin client wrapper +
// path-builder around those two endpoints, so that's all this file covers.
// Movies get their own endpoint (not the shows one) because multi-part
// movies (GitHub #3) need to translate a summed-across-parts position into
// a part_num + in-part offset — see PlaybackService.cpp's own comment.
vi.mock('@/api/client', () => ({
  api: {
    getResolvedPlayTarget: vi.fn(),
      getMovieResolvedPlayTarget: vi.fn(),
  },
}))

const mockApi = api as Record<'getResolvedPlayTarget' | 'getMovieResolvedPlayTarget', ReturnType<typeof vi.fn>>

describe('resolvePlayTarget', () => {
  beforeEach(() => {
    vi.resetAllMocks()
  })

    it('movies delegate to the server-resolved target', async () => {
        mockApi.getMovieResolvedPlayTarget.mockResolvedValue({kind: 'movie', id: 'm1', position_ms: 30_000})
        const target = await resolvePlayTarget('movie', 'm1')
        expect(target).toEqual({kind: 'movie', id: 'm1', positionMs: 30_000, partNum: undefined, totalParts: undefined})
        expect(mockApi.getMovieResolvedPlayTarget).toHaveBeenCalledWith('m1')
    })

    it('a movie with no saved progress resolves to position 0', async () => {
        mockApi.getMovieResolvedPlayTarget.mockResolvedValue(null)
    const target = await resolvePlayTarget('movie', 'm1')
    expect(target).toEqual({ kind: 'movie', id: 'm1', positionMs: 0 })
    })

    it('a multi-part movie resolves with part_num/total_parts', async () => {
        mockApi.getMovieResolvedPlayTarget.mockResolvedValue({
            kind: 'movie', id: 'm1', position_ms: 10_000, part_num: 2, total_parts: 3,
        })
        const target = await resolvePlayTarget('movie', 'm1')
        expect(target).toEqual({kind: 'movie', id: 'm1', positionMs: 10_000, partNum: 2, totalParts: 3})
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

    it('movie with no saved progress has no ?t=', async () => {
        mockApi.getMovieResolvedPlayTarget.mockResolvedValue(null)
    const path = await resolvePlayPath('movie', 'm1')
    expect(path).toBe('/player/movie/m1')
    })

    it('movie mid-playback includes ?t=', async () => {
        mockApi.getMovieResolvedPlayTarget.mockResolvedValue({kind: 'movie', id: 'm1', position_ms: 30_000})
        const path = await resolvePlayPath('movie', 'm1')
        expect(path).toBe('/player/movie/m1?t=30000')
    })

    it('multi-part movie includes ?t= and ?part=', async () => {
        mockApi.getMovieResolvedPlayTarget.mockResolvedValue({
            kind: 'movie', id: 'm1', position_ms: 5_000, part_num: 2, total_parts: 3,
        })
        const path = await resolvePlayPath('movie', 'm1')
        expect(path).toBe('/player/movie/m1?t=5000&part=2')
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
