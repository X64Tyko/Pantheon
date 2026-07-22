import { describe, it, expect, vi, beforeEach } from 'vitest'
import { LibraryStore } from '@/stores/LibraryStore'
import { api } from '@/api/client'

vi.mock('@/api/client', () => ({
  api: {
    getAllLibraries: vi.fn(),
    getShows:         vi.fn(),
    getMovies:        vi.fn(),
    scraperSearch:    vi.fn(),
  },
}))

const mockApi = api as Record<
  'getAllLibraries' | 'getShows' | 'getMovies' | 'scraperSearch',
  ReturnType<typeof vi.fn>
>

const SHOW_1  = { show_id: 'sh1', title: 'Foo Show', year: 2020, audience_rating: 8.1 }
const MOVIE_1 = { movie_id: 'mv1', title: 'Bar Movie', year: 2019, audience_rating: 7.4 }

describe('LibraryStore', () => {
  let store: LibraryStore

  beforeEach(() => {
    vi.resetAllMocks()
    mockApi.getShows.mockResolvedValue({ items: [], total: 0 })
    mockApi.getMovies.mockResolvedValue({ items: [], total: 0 })
    store = new LibraryStore()
  })

  // ── initial state ────────────────────────────────────────────────────────

  it('defaults to sort=recently_added, contentType=all, page=0', () => {
    expect(store.sort).toBe('recently_added')
    expect(store.contentType).toBe('all')
    expect(store.page).toBe(0)
  })

  // ── fetch() — content type gating ───────────────────────────────────────

  describe('fetch', () => {
    it('fetches both shows and movies when contentType=all', async () => {
      await store.fetch()
      expect(mockApi.getShows).toHaveBeenCalledTimes(1)
      expect(mockApi.getMovies).toHaveBeenCalledTimes(1)
    })

    it('skips getMovies when contentType=show', async () => {
      store.setContentType('show')
      await store.fetch()
      expect(mockApi.getShows).toHaveBeenCalled()
      expect(mockApi.getMovies).not.toHaveBeenCalled()
    })

    it('skips getShows when contentType=movie', async () => {
      store.setContentType('movie')
      await store.fetch()
      expect(mockApi.getMovies).toHaveBeenCalled()
      expect(mockApi.getShows).not.toHaveBeenCalled()
    })

    it('passes sort through to both getShows and getMovies', async () => {
      store.setSort('recently_released')
      await store.fetch()
      expect(mockApi.getShows).toHaveBeenCalledWith(expect.objectContaining({ sort: 'recently_released' }))
      expect(mockApi.getMovies).toHaveBeenCalledWith(expect.objectContaining({ sort: 'recently_released' }))
    })

    it('populates shows/movies/total from the API results', async () => {
      mockApi.getShows.mockResolvedValue({ items: [SHOW_1], total: 1 })
      mockApi.getMovies.mockResolvedValue({ items: [MOVIE_1], total: 1 })
      await store.fetch()
      expect(store.shows).toEqual([SHOW_1])
      expect(store.movies).toEqual([MOVIE_1])
      expect(store.total).toBe(2)
    })

    it('clears loading even when the API call rejects', async () => {
      mockApi.getShows.mockRejectedValue(new Error('boom'))
      await store.fetch()
      expect(store.loading).toBe(false)
    })
  })

  // ── setSort / setContentType / setFilterGenre / setLibrary — mutate + refetch ──

  describe('setters', () => {
    it('setSort updates sort, resets page to 0, and refetches', async () => {
      store.setPage(3)
      store.setSort('recently_aired')
      await Promise.resolve()
      expect(store.sort).toBe('recently_aired')
      expect(store.page).toBe(0)
      expect(mockApi.getShows).toHaveBeenCalledWith(expect.objectContaining({ sort: 'recently_aired' }))
    })

    it('setContentType updates contentType and resets page', () => {
      store.setPage(2)
      store.setContentType('movie')
      expect(store.contentType).toBe('movie')
      expect(store.page).toBe(0)
    })

    it('setFilterGenre updates genre and is folded into the canon filter string', async () => {
      // filterGenre isn't sent as its own param — fetch() merges it into the
      // filter-tree-derived canon filter string (genre:Horror), same as any
      // other rule-builder clause. See LibraryStore.fetch's `filter` build.
      store.setFilterGenre('Horror')
      await Promise.resolve()
      expect(mockApi.getShows).toHaveBeenCalledWith(expect.objectContaining({ filter: 'genre:Horror' }))
    })
  })

  // ── loadMore ─────────────────────────────────────────────────────────────

  describe('loadMore', () => {
    it('does nothing if already at the end of the result set', async () => {
      mockApi.getShows.mockResolvedValue({ items: [SHOW_1], total: 1 })
      await store.fetch()
      await store.loadMore()
      // Only the initial fetch's call, no second page requested
      expect(mockApi.getShows).toHaveBeenCalledTimes(1)
    })

    it('advances the page and appends results when more are available', async () => {
      mockApi.getShows.mockResolvedValueOnce({ items: [SHOW_1], total: 2 })
      await store.fetch()
      mockApi.getShows.mockResolvedValueOnce({ items: [{ ...SHOW_1, show_id: 'sh2' }], total: 2 })
      await store.loadMore()
      expect(store.shows).toHaveLength(2)
      expect(store.page).toBe(1)
    })

    it('is a no-op while a fetch is already loading', async () => {
      let resolveFetch: (v: unknown) => void = () => {}
      mockApi.getShows.mockReturnValue(new Promise(res => { resolveFetch = res }))
      const first = store.fetch()
      await store.loadMore() // should bail immediately — store.loading is true
      expect(mockApi.getShows).toHaveBeenCalledTimes(1)
      resolveFetch({ items: [], total: 0 })
      await first
    })
  })

  // ── selection ────────────────────────────────────────────────────────────

  describe('selectItem / clearSelection', () => {
    it('selecting an item sets selectedId/selectedType', () => {
      store.selectItem('sh1', 'show')
      expect(store.selectedId).toBe('sh1')
      expect(store.selectedType).toBe('show')
    })

    it('selecting the same item again deselects it', () => {
      store.selectItem('sh1', 'show')
      store.selectItem('sh1', 'show')
      expect(store.selectedId).toBeNull()
      expect(store.selectedType).toBeNull()
    })

    it('clearSelection resets both fields', () => {
      store.selectItem('sh1', 'show')
      store.clearSelection()
      expect(store.selectedId).toBeNull()
      expect(store.selectedType).toBeNull()
    })
  })

  // ── discover mode ────────────────────────────────────────────────────────

  describe('discoverSearch', () => {
    it('does not call the API for a blank/whitespace-only query', async () => {
      store.query = '   '
      await store.discoverSearch()
      expect(mockApi.scraperSearch).not.toHaveBeenCalled()
      expect(store.discoverResults).toEqual([])
    })

    it('searches and populates discoverResults for a real query', async () => {
      mockApi.scraperSearch.mockResolvedValue({ items: [{ source: 'tmdb', external_id: '1', title: 'X' }] })
      store.query = 'batman'
      await store.discoverSearch()
      expect(mockApi.scraperSearch).toHaveBeenCalledWith('batman', undefined)
      expect(store.discoverResults).toHaveLength(1)
    })

    it('toggleDiscoverMode off triggers a normal fetch', async () => {
      store.toggleDiscoverMode() // -> true, no query, no fetch
      store.toggleDiscoverMode() // -> false, should fetch()
      await Promise.resolve()
      expect(mockApi.getShows).toHaveBeenCalled()
    })
  })

  // ── density / sidebar — persisted to localStorage ───────────────────────

  describe('density persistence', () => {
    it('setDensity persists to localStorage and a fresh store picks it up', () => {
      store.setDensity('rich')
      expect(store.density).toBe('rich')
      const fresh = new LibraryStore()
      expect(fresh.density).toBe('rich')
    })

    it('toggleSidebar flips and persists sidebarOpen', () => {
      const initial = store.sidebarOpen
      store.toggleSidebar()
      expect(store.sidebarOpen).toBe(!initial)
      const fresh = new LibraryStore()
      expect(fresh.sidebarOpen).toBe(!initial)
    })
  })
})
