import { makeAutoObservable, reaction, runInAction } from 'mobx'
import { api } from '../api/client'
import type { LibraryWithSource, Show, Movie, ScraperSearchResult } from '../api/types'
import type { LibraryDensity } from '../api/types'
import type { FilterField } from '../components/PickerFilters'
import { FilterTreeStore } from '../components/media/filterTree'
import { toFilterString } from '../components/media/filterQuery'

const DENSITY_KEY = 'hds-library-density'
const SIDEBAR_KEY = 'hds-library-sidebar'
const HIDE_EMPTY_KEY = 'hds-library-hide-empty'
const PAGE_SIZE = 48

let _filterDebounce: ReturnType<typeof setTimeout>

export class LibraryStore {
  libraries:    LibraryWithSource[] = []
  shows:        Show[] = []
  movies:       Movie[] = []
  total:        number = 0
  loading:      boolean = false
  loadingMore:  boolean = false
  // Surfaced rather than silently left as stale/unfiltered results on
  // screen — a failed request (e.g. a filter combination the backend
  // rejects) used to just leave whatever was showing before, which reads
  // exactly like "the filter did nothing."
  error:        string | null = null
  page:         number = 0
  query:        string = ''
  contentType:  'show' | 'movie' | 'all' = 'all'
  // No user-facing sort picker in LibraryFilters yet — reachable only via
  // Home's shelf "Continue in Library" tiles for now (see HomePage.tsx).
  sort:         string = 'recently_added'
  sortDir:      'asc' | 'desc' | '' = ''
  // Only meaningful while sort === 'random'. A fixed seed keeps the shuffled
  // order stable across a fetch + loadMore pagination sequence (SQLite's
  // bare RANDOM() would otherwise reshuffle on every single query, so
  // scrolling through a "Random" sorted browse would show duplicates/skips
  // rather than one consistent shuffled order) — the die button in
  // LibraryFilters.tsx picks a new one to reshuffle on demand.
  randomSeed:   number = Math.floor(Math.random() * 2147483647)
  // Multi-select library scoping (SourceSwitcher.tsx pills) — defaults to
  // every library selected the first time loadLibraries() resolves. Sent to
  // the API as library_ids only when it's a strict subset (all-selected
  // stays the unscoped fast path; none-selected short-circuits to an empty
  // result client-side rather than falling back to "all" — see fetch()).
  selectedLibIds: Set<string> = new Set()
  // Simple single-value genre selection — still used as-is by TvLibrary's
  // GenreChip row (10-foot surface, no room for the rule builder below).
  // searchParams() prefers a "genre" advanced-filter rule when one is set,
  // falling back to this so the two surfaces share one store without
  // stepping on each other.
  filterGenre:  string = ''
  // Advanced filter panel (web only) — same rule-builder (nested groups,
  // every field/operator) already used by Playlists/Filler Lists/channel
  // content pickers (see components/PickerFilters.tsx / media/filterTree.ts).
  filterTree:   FilterTreeStore = new FilterTreeStore()
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

  constructor() {
    makeAutoObservable(this)
    // Any change to the rule-builder tree (add/remove/edit a rule or group,
    // flip Match All/Any) re-fetches — debounced the same as the old
    // per-mutator debouncedFetch() calls, but centralized here now that
    // FilterTreeStore's mutators don't know about fetching themselves.
    // toFilterString reads every observable field the tree exposes, so this
    // fires exactly when the resulting filter string would actually change
    // (opening/closing the panel doesn't touch any of those fields).
    reaction(() => toFilterString(this.filterTree), () => {
      this.page = 0
      this.debouncedFetch()
    })
  }

  async loadLibraries() {
    const libs = await api.getAllLibraries()
    runInAction(() => {
      const firstLoad = this.libraries.length === 0
      this.libraries = libs
      // Only auto-select-all the first time libraries resolve — a later
      // refetch (e.g. after adding a source) shouldn't silently undo a
      // manual deselection.
      if (firstLoad) this.selectedLibIds = new Set(libs.map(l => l.library_id))
    })
  }

  private searchParams(page: number) {
    // Every rule/operator the rule-builder can produce now round-trips
    // through toFilterString (see components/media/filterQuery.ts) — this
    // used to only read 'is' rules for 6 of 16 fields. 'library' is still
    // deliberately excluded from the filter string itself: the multi-select
    // library pills (SourceSwitcher.tsx) own library scoping on this page,
    // and letting both drive the same thing would just fight each other.
    const hasGenreRule = this.filterTree.allRules.some(r => r.field === 'genre' && r.value.trim())
    const filter = (hasGenreRule || !this.filterGenre
      ? toFilterString(this.filterTree)
      : toFilterString({
          match: this.filterTree.match,
          items: [...this.filterTree.items, { kind: 'rule' as const, id: '_tv_genre_chip', field: 'genre' as const, op: 'is' as const, value: this.filterGenre }],
        })) || undefined

    const allSelected = this.libraries.length > 0 && this.selectedLibIds.size >= this.libraries.length
    const library_ids = allSelected ? undefined : Array.from(this.selectedLibIds).join(',')

    return {
      limit: PAGE_SIZE,
      offset: page * PAGE_SIZE,
      q: this.query || undefined,
      library_ids,
      filter,
      sort: this.sort || undefined,
      sort_dir: this.sortDir || undefined,
      seed: this.sort === 'random' ? this.randomSeed : undefined,
      hideEmpty: this.hideEmpty || undefined,
    }
  }

  // True once libraries have loaded and the admin has explicitly deselected
  // every one of them — distinct from "not loaded yet" (empty Set before
  // the first loadLibraries() resolves), which should show the normal
  // loading state instead of an empty-selection message.
  get noLibrariesSelected(): boolean {
    return this.libraries.length > 0 && this.selectedLibIds.size === 0
  }

  // "Recently Aired/Released" is one combined option in the sort picker
  // (shows and movies don't share a single backend sort mode for it — see
  // ContentRepository::searchShows/searchMovies's "recently_aired" vs
  // "recently_released"), so the single-request `base` params need a
  // per-content-type override for just this one value.
  private showSort()  { return this.sort === 'recently_released_or_aired' ? 'recently_aired'    : this.sort }
  private movieSort() { return this.sort === 'recently_released_or_aired' ? 'recently_released' : this.sort }

  async fetch() {
    if (this.noLibrariesSelected) {
      runInAction(() => { this.shows = []; this.movies = []; this.total = 0; this.loading = false; this.error = null })
      return
    }
    runInAction(() => { this.loading = true })
    const base = this.searchParams(this.page)
    try {
      const [showRes, movieRes] = await Promise.all([
        this.contentType !== 'movie' ? api.getShows({ ...base, sort: this.showSort() }) : Promise.resolve({ items: [] as Show[], total: 0 }),
        this.contentType !== 'show'  ? api.getMovies({ ...base, sort: this.movieSort() }) : Promise.resolve({ items: [] as Movie[], total: 0 }),
      ])
      runInAction(() => {
        this.shows  = showRes.items
        this.movies = movieRes.items
        this.total  = showRes.total + movieRes.total
        this.loading = false
        this.error = null
      })
    } catch (e: any) {
      // Cleared rather than left stale — a failed request used to just
      // leave whatever was on screen before, which reads exactly like "the
      // filter did nothing" instead of "this request failed."
      runInAction(() => {
        this.shows = []; this.movies = []; this.total = 0
        this.loading = false
        this.error = e?.message ?? 'Failed to load library'
      })
    }
  }

  async loadMore() {
    if (this.loading || this.loadingMore || this.noLibrariesSelected) return
    if (this.shows.length + this.movies.length >= this.total) return
    runInAction(() => { this.loadingMore = true })
    const nextPage = this.page + 1
    const base = this.searchParams(nextPage)
    try {
      const [showRes, movieRes] = await Promise.all([
        this.contentType !== 'movie' ? api.getShows({ ...base, sort: this.showSort() }) : Promise.resolve({ items: [] as Show[], total: 0 }),
        this.contentType !== 'show'  ? api.getMovies({ ...base, sort: this.movieSort() }) : Promise.resolve({ items: [] as Movie[], total: 0 }),
      ])
      runInAction(() => {
        this.shows      = [...this.shows, ...showRes.items]
        this.movies     = [...this.movies, ...movieRes.items]
        this.page       = nextPage
        this.loadingMore = false
        this.error = null
      })
    } catch (e: any) {
      runInAction(() => { this.loadingMore = false; this.error = e?.message ?? 'Failed to load more' })
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
  toggleLibrary(id: string) {
    const next = new Set(this.selectedLibIds)
    if (next.has(id)) next.delete(id); else next.add(id)
    this.selectedLibIds = next
    this.page = 0
    this.fetch()
  }
  selectAllLibraries() {
    this.selectedLibIds = new Set(this.libraries.map(l => l.library_id))
    this.page = 0
    this.fetch()
  }
  selectNoLibraries() {
    this.selectedLibIds = new Set()
    this.page = 0
    this.fetch()
  }
  setContentType(t: 'show' | 'movie' | 'all') { this.contentType = t; this.page = 0; this.fetch() }
  setSort(s: string)                { this.sort        = s; this.page = 0; this.fetch() }
  setSortDir(d: 'asc' | 'desc' | '') { this.sortDir     = d; this.page = 0; this.fetch() }
  // Picks a new random order (see randomSeed's doc comment) — a no-op
  // unless sort is actually 'random', since the seed is only ever read then.
  rerollRandom() { this.randomSeed = Math.floor(Math.random() * 2147483647); this.page = 0; this.fetch() }
  setPage(p: number)               { this.page        = p; this.fetch() }
  setFilterGenre(g: string)        { this.filterGenre = g; this.page = 0; this.fetch() }

  private debouncedFetch() {
    clearTimeout(_filterDebounce)
    _filterDebounce = setTimeout(() => this.fetch(), 250)
  }

  // For Home shelf tiles that land on Library with a specific tag/value
  // already filtering the view (e.g. a future genre-based holiday shelf's
  // own "Continue in Library" tile) — unlike setContentType/setSort, this
  // makes the preset a real, visible, editable rule in the advanced panel
  // instead of silent store state, and opens the panel so it's immediately
  // seen. Mutate-then-navigate, same convention as HomePage's
  // continueInLibrary: LibraryPage's mount effect calls fetch() fresh on
  // arrival, so this deliberately doesn't fetch itself (the reaction would
  // anyway, but the page is about to fetch fresh on mount regardless).
  presetFilter(field: FilterField, value: string) {
    this.filterTree.setSingleRule(field, value)
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
