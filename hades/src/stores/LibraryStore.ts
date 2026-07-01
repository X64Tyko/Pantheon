import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'
import type { LibraryWithSource, Show, Movie, ScraperSearchResult } from '../api/types'
import type { LibraryDensity } from '../api/types'

const DENSITY_KEY = 'hds-library-density'
const SIDEBAR_KEY = 'hds-library-sidebar'
const PAGE_SIZE = 48

class LibraryStore {
  libraries:    LibraryWithSource[] = []
  shows:        Show[] = []
  movies:       Movie[] = []
  total:        number = 0
  loading:      boolean = false
  loadingMore:  boolean = false
  page:         number = 0
  query:        string = ''
  contentType:  'show' | 'movie' | 'all' = 'all'
  activeLibId:  string | null = null
  filterGenre:  string = ''
  density:      LibraryDensity = (localStorage.getItem(DENSITY_KEY) as LibraryDensity | null) ?? 'standard'
  sidebarOpen:  boolean = localStorage.getItem(SIDEBAR_KEY) !== 'false'
  selectedId:   string | null = null
  selectedType: 'show' | 'movie' | null = null

  // Discover mode — searches scrapers rather than local library
  discoverMode:    boolean = false
  discoverResults: ScraperSearchResult[] = []
  discoverLoading: boolean = false

  constructor() { makeAutoObservable(this) }

  async loadLibraries() {
    const libs = await api.getAllLibraries()
    runInAction(() => { this.libraries = libs })
  }

  private searchParams(page: number) {
    return {
      limit: PAGE_SIZE,
      offset: page * PAGE_SIZE,
      q: this.query || undefined,
      library_id: this.activeLibId ?? undefined,
      genre: this.filterGenre || undefined,
    }
  }

  async fetch() {
    runInAction(() => { this.loading = true })
    const base = this.searchParams(this.page)
    try {
      const [showRes, movieRes] = await Promise.all([
        this.contentType !== 'movie' ? api.getShows(base) : Promise.resolve({ items: [] as Show[], total: 0 }),
        this.contentType !== 'show'  ? api.getMovies(base) : Promise.resolve({ items: [] as Movie[], total: 0 }),
      ])
      runInAction(() => {
        this.shows  = showRes.items
        this.movies = movieRes.items
        this.total  = showRes.total + movieRes.total
        this.loading = false
      })
    } catch {
      runInAction(() => { this.loading = false })
    }
  }

  async loadMore() {
    if (this.loading || this.loadingMore) return
    if (this.shows.length + this.movies.length >= this.total) return
    runInAction(() => { this.loadingMore = true })
    const nextPage = this.page + 1
    const base = this.searchParams(nextPage)
    try {
      const [showRes, movieRes] = await Promise.all([
        this.contentType !== 'movie' ? api.getShows(base) : Promise.resolve({ items: [] as Show[], total: 0 }),
        this.contentType !== 'show'  ? api.getMovies(base) : Promise.resolve({ items: [] as Movie[], total: 0 }),
      ])
      runInAction(() => {
        this.shows      = [...this.shows, ...showRes.items]
        this.movies     = [...this.movies, ...movieRes.items]
        this.page       = nextPage
        this.loadingMore = false
      })
    } catch {
      runInAction(() => { this.loadingMore = false })
    }
  }

  setQuery(q: string) {
    this.query = q
    this.page = 0
    if (this.discoverMode) {
      this.discoverSearch()
    } else {
      this.fetch()
    }
  }
  setLibrary(id: string | null)    { this.activeLibId = id; this.page = 0; this.fetch() }
  setContentType(t: 'show' | 'movie' | 'all') { this.contentType = t; this.page = 0; this.fetch() }
  setPage(p: number)               { this.page        = p; this.fetch() }
  setFilterGenre(g: string)        { this.filterGenre = g; this.page = 0; this.fetch() }

  setDensity(d: LibraryDensity) {
    this.density = d
    localStorage.setItem(DENSITY_KEY, d)
  }

  toggleSidebar() {
    this.sidebarOpen = !this.sidebarOpen
    localStorage.setItem(SIDEBAR_KEY, String(this.sidebarOpen))
  }

  selectItem(id: string, type: 'show' | 'movie') {
    if (this.selectedId === id) { this.selectedId = null; this.selectedType = null }
    else { this.selectedId = id; this.selectedType = type }
  }

  clearSelection() { this.selectedId = null; this.selectedType = null }

  toggleDiscoverMode() {
    this.discoverMode = !this.discoverMode
    this.discoverResults = []
    if (this.discoverMode && this.query) {
      this.discoverSearch()
    } else if (!this.discoverMode) {
      this.fetch()
    }
  }

  async discoverSearch() {
    if (!this.query.trim()) { runInAction(() => { this.discoverResults = [] }); return }
    runInAction(() => { this.discoverLoading = true })
    const type = this.contentType === 'all' ? undefined : this.contentType
    try {
      const res = await api.scraperSearch(this.query, type)
      runInAction(() => { this.discoverResults = res.items; this.discoverLoading = false })
    } catch {
      runInAction(() => { this.discoverLoading = false })
    }
  }
}

export const libraryStore = new LibraryStore()
