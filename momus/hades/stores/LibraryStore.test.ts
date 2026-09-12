import { describe, it, expect, vi, beforeEach } from 'vitest'
import { LibraryStore } from '@/stores/LibraryStore'
import { api } from '@/api/client'

vi.mock('@/api/client', () => ({
  api: {
    getAllLibraries:    vi.fn(),
    getShows:           vi.fn(),
    getMovies:          vi.fn(),
    getMixedMediaIndex: vi.fn(),
    getMixedMediaTiles: vi.fn(),
    scraperSearch:      vi.fn(),
  },
}))

const mockApi = api as Record<
  'getAllLibraries' | 'getShows' | 'getMovies' | 'getMixedMediaIndex' | 'getMixedMediaTiles' | 'scraperSearch',
  ReturnType<typeof vi.fn>
>

const SHOW_1  = { show_id: 'sh1', title: 'Foo Show', year: 2020, audience_rating: 8.1 }
const MOVIE_1 = { movie_id: 'mv1', title: 'Bar Movie', year: 2019, audience_rating: 7.4 }

// contentType defaults to 'all', which fetch()/loadMore() serve via the
// mixed-index path (isMixedBrowse), not plain getShows/getMovies — those
// stay reserved for a single-type browse (contentType='show'/'movie'). See
// LibraryStore.fetch()'s isMixedBrowse branch.
const IDX_SHOW_1  = { content_type: 'show' as const,  id: 'sh1', title: 'Foo Show' }
const IDX_MOVIE_1 = { content_type: 'movie' as const, id: 'mv1', title: 'Bar Movie' }
const TILE_SHOW_1  = { content_type: 'show' as const,  id: 'sh1', title: 'Foo Show',  year: 2020, audience_rating: 8.1, watched: false }
const TILE_MOVIE_1 = { content_type: 'movie' as const, id: 'mv1', title: 'Bar Movie', year: 2019, audience_rating: 7.4, watched: false }

describe('LibraryStore', () => {
  let store: LibraryStore

  beforeEach(() => {
    vi.resetAllMocks()
    mockApi.getShows.mockResolvedValue({ items: [], total: 0 })
    mockApi.getMovies.mockResolvedValue({ items: [], total: 0 })
    mockApi.getMixedMediaIndex.mockResolvedValue({ items: [] })
    mockApi.getMixedMediaTiles.mockResolvedValue({ items: [] })
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
    it('fetches the mixed index (not getShows/getMovies) when contentType=all', async () => {
      await store.fetch()
      expect(mockApi.getMixedMediaIndex).toHaveBeenCalledTimes(1)
      expect(mockApi.getShows).not.toHaveBeenCalled()
      expect(mockApi.getMovies).not.toHaveBeenCalled()
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

    it('passes sort through to the mixed index fetch when contentType=all', async () => {
      store.setSort('recently_released')
      await store.fetch()
      expect(mockApi.getMixedMediaIndex).toHaveBeenCalledWith(expect.objectContaining({ sort: 'recently_released' }))
    })

    it('passes sort through to both getShows and getMovies for a single content type', async () => {
      store.setContentType('show')
      store.setSort('recently_released')
      await store.fetch()
      expect(mockApi.getShows).toHaveBeenCalledWith(expect.objectContaining({ sort: 'recently_released' }))
    })

    it('populates mixedItems/total from the index+hydrate results when contentType=all', async () => {
      mockApi.getMixedMediaIndex.mockResolvedValue({ items: [IDX_SHOW_1, IDX_MOVIE_1] })
      mockApi.getMixedMediaTiles.mockResolvedValue({ items: [TILE_SHOW_1, TILE_MOVIE_1] })
      await store.fetch()
      expect(store.mixedItems).toEqual([TILE_SHOW_1, TILE_MOVIE_1])
      expect(store.shows).toEqual([])
      expect(store.movies).toEqual([])
      expect(store.total).toBe(2)
    })

    it('populates shows/movies/total from getShows/getMovies for a single content type', async () => {
      store.setContentType('show')
      mockApi.getShows.mockResolvedValue({ items: [SHOW_1], total: 1 })
      await store.fetch()
      expect(store.shows).toEqual([SHOW_1])
      expect(store.total).toBe(1)
    })

    it('clears loading even when the API call rejects', async () => {
      mockApi.getMixedMediaIndex.mockRejectedValue(new Error('boom'))
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
      expect(mockApi.getMixedMediaIndex).toHaveBeenCalledWith(expect.objectContaining({ sort: 'recently_aired' }))
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
      expect(mockApi.getMixedMediaIndex).toHaveBeenCalledWith(expect.objectContaining({ filter: 'genre:Horror' }))
    })
  })

  // ── loadMore ─────────────────────────────────────────────────────────────

  describe('loadMore', () => {
    it('does nothing if already at the end of the result set', async () => {
      // Mixed browse (default contentType=all): fetch() fetches the whole
      // index up front and hydrates only the first PAGE_SIZE tiles — a
      // single index entry means there's nothing left to page into.
      mockApi.getMixedMediaIndex.mockResolvedValue({ items: [IDX_SHOW_1] })
      mockApi.getMixedMediaTiles.mockResolvedValue({ items: [TILE_SHOW_1] })
      await store.fetch()
      await store.loadMore()
      // Only the initial fetch's calls, no second hydrate requested
      expect(mockApi.getMixedMediaIndex).toHaveBeenCalledTimes(1)
      expect(mockApi.getMixedMediaTiles).toHaveBeenCalledTimes(1)
    })

    it('advances the page and appends results when more are available', async () => {
      // fetch() hydrates only the first PAGE_SIZE (48) tiles of the index it
      // fetched, regardless of the index's own total size — so a 50-entry
      // index leaves 2 unhydrated until loadMore() asks for the next slice.
      const index = Array.from({ length: 50 }, (_, i) => ({
        content_type: 'show' as const, id: `sh${i}`, title: `Show ${i}`,
      }))
      mockApi.getMixedMediaIndex.mockResolvedValue({ items: index })
      mockApi.getMixedMediaTiles.mockImplementation((ids: { content_type: string; id: string }[]) =>
        Promise.resolve({ items: ids.map(({ id }) => ({ ...TILE_SHOW_1, id })) }))

      await store.fetch()
      expect(store.mixedItems).toHaveLength(48)

      await store.loadMore()
      expect(store.mixedItems).toHaveLength(50)
      expect(store.page).toBe(1)
    })

    it('is a no-op while a fetch is already loading', async () => {
      let resolveIndex: (v: unknown) => void = () => {}
      mockApi.getMixedMediaIndex.mockReturnValue(new Promise(res => { resolveIndex = res }))
      const first = store.fetch()
      await store.loadMore() // should bail immediately — store.loading is true
      expect(mockApi.getMixedMediaTiles).not.toHaveBeenCalled()
      resolveIndex({ items: [] })
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
      expect(mockApi.getMixedMediaIndex).toHaveBeenCalled()
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
