import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'
import type { LibraryWithSource, Show, Movie, ScraperSearchResult } from '../api/types'
import type { LibraryDensity } from '../api/types'
import { FIELD_DEFS, type FilterField, type FilterRule } from '../components/PickerFilters'

const DENSITY_KEY = 'hds-library-density'
const SIDEBAR_KEY = 'hds-library-sidebar'
const HIDE_EMPTY_KEY = 'hds-library-hide-empty'
const PAGE_SIZE = 48

let _ruleId = 0
let _filterDebounce: ReturnType<typeof setTimeout>

export class LibraryStore {
  libraries:    LibraryWithSource[] = []
  shows:        Show[] = []
  movies:       Movie[] = []
  total:        number = 0
  loading:      boolean = false
  loadingMore:  boolean = false
  page:         number = 0
  query:        string = ''
  contentType:  'show' | 'movie' | 'all' = 'all'
  // No user-facing sort picker in LibraryFilters yet — reachable only via
  // Home's shelf "Continue in Library" tiles for now (see HomePage.tsx).
  sort:         string = 'recently_added'
  activeLibId:  string | null = null
  // Simple single-value genre selection — still used as-is by TvLibrary's
  // GenreChip row (10-foot surface, no room for the rule builder below).
  // searchParams() prefers a "genre" advanced-filter rule when one is set,
  // falling back to this so the two surfaces share one store without
  // stepping on each other.
  filterGenre:  string = ''
  // Advanced filter panel (web only) — same rule-builder (field/operator/
  // value, Match All/Any) already used by Playlists/Filler Lists/channel
  // content pickers (see components/PickerFilters.tsx), reused here for
  // consistency. Only rules with op 'is' actually affect the query (see
  // searchParams below) — matches every other page that uses this same
  // component; the rest of FIELD_DEFS's fields/operators are UI-only there
  // too, not something introduced here.
  filterRulesOpen: boolean = false
  filterMatch:     'all' | 'any' = 'all'
  filterRules:     FilterRule[] = []
  density:      LibraryDensity = (localStorage.getItem(DENSITY_KEY) as LibraryDensity | null) ?? 'standard'
  sidebarOpen:  boolean = localStorage.getItem(SIDEBAR_KEY) !== 'false'
  // Hides shows/movies with no actual media behind them (unsynced or
  // orphan-pruned-down-to-nothing metadata stubs). Defaults on; flip it to
  // see everything for admin/debugging purposes.
  hideEmpty:    boolean = localStorage.getItem(HIDE_EMPTY_KEY) !== 'false'
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
    // Only 'is' rules affect the query — same convention as every other
    // page using this rule builder (see FilterSection's callers). 'library'
    // is deliberately never translated here even though it's a selectable
    // field in the UI: SourceSwitcher already owns library_id on this page,
    // and letting both drive the same param would just fight each other.
    const isRules = this.filterRules.filter(r => r.op === 'is' && r.value.trim())
    const ruleGenre = isRules.find(r => r.field === 'genre')?.value
    const yearStr   = isRules.find(r => r.field === 'year')?.value
    const year      = yearStr ? parseInt(yearStr, 10) : undefined
    const rating    = isRules.find(r => r.field === 'content_rating')?.value || undefined
    const label     = isRules.find(r => r.field === 'label')?.value          || undefined
    const network   = isRules.find(r => r.field === 'network')?.value        || undefined
    const actor     = isRules.find(r => r.field === 'actor')?.value          || undefined

    return {
      limit: PAGE_SIZE,
      offset: page * PAGE_SIZE,
      q: this.query || undefined,
      library_id: this.activeLibId ?? undefined,
      genre: ruleGenre || this.filterGenre || undefined,
      year,
      content_rating: rating,
      label,
      network,
      actor,
      sort: this.sort || undefined,
      hideEmpty: this.hideEmpty || undefined,
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
  setSort(s: string)                { this.sort        = s; this.page = 0; this.fetch() }
  setPage(p: number)               { this.page        = p; this.fetch() }
  setFilterGenre(g: string)        { this.filterGenre = g; this.page = 0; this.fetch() }

  private debouncedFetch() {
    clearTimeout(_filterDebounce)
    _filterDebounce = setTimeout(() => this.fetch(), 250)
  }

  addFilterRule() {
    this.filterRules.push({ id: String(++_ruleId), field: 'genre', op: 'is', value: '' })
    this.page = 0
    this.debouncedFetch()
  }

  removeFilterRule(id: string) {
    this.filterRules = this.filterRules.filter(r => r.id !== id)
    this.page = 0
    this.debouncedFetch()
  }

  updateFilterRule(id: string, patch: Partial<Omit<FilterRule, 'id'>>) {
    const rule = this.filterRules.find(r => r.id === id)
    if (!rule) return
    if (patch.field !== undefined) {
      rule.field = patch.field
      rule.op    = FIELD_DEFS[patch.field].ops[0].id
      rule.value = ''
    }
    if (patch.op    !== undefined) rule.op    = patch.op
    if (patch.value !== undefined) rule.value = patch.value
    this.page = 0
    this.debouncedFetch()
  }

  setFilterMatch(m: 'all' | 'any') {
    this.filterMatch = m
    this.page = 0
    this.debouncedFetch()
  }

  toggleFilterRulesOpen() { this.filterRulesOpen = !this.filterRulesOpen }

  // For Home shelf tiles that land on Library with a specific tag/value
  // already filtering the view (e.g. a future genre-based holiday shelf's
  // own "Continue in Library" tile) — unlike setContentType/setSort, this
  // makes the preset a real, visible, editable rule in the advanced panel
  // instead of silent store state, and opens the panel so it's immediately
  // seen. Mutate-then-navigate, same convention as HomePage's
  // continueInLibrary: LibraryPage's mount effect calls fetch() fresh on
  // arrival, so this deliberately doesn't fetch itself.
  presetFilter(field: FilterField, value: string) {
    this.filterRules = [{ id: String(++_ruleId), field, op: 'is', value }]
    this.filterRulesOpen = true
    this.sidebarOpen = true
    localStorage.setItem(SIDEBAR_KEY, 'true')
    this.page = 0
  }

  setHideEmpty(v: boolean) {
    this.hideEmpty = v
    localStorage.setItem(HIDE_EMPTY_KEY, String(v))
    this.page = 0
    this.fetch()
  }

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
