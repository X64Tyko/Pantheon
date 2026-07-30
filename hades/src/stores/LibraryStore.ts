import { makeAutoObservable, reaction, runInAction } from 'mobx'
import { api } from '../api/client'
import type {
    LibraryWithSource,
    Show,
    Movie,
    MixedIndexEntry,
    MixedMediaItem,
    ScraperSearchResult,
    EpisodeSearchResult,
    PlaylistBrowseEntry
} from '../api/types'
import type { LibraryDensity } from '../api/types'
import type { FilterField } from '../components/PickerFilters'
import { FilterTreeStore } from '../components/media/filterTree'
import { toFilterString } from '../components/media/filterQuery'

const DENSITY_KEY = 'hds-library-density'
const SIDEBAR_KEY = 'hds-library-sidebar'
const HIDE_EMPTY_KEY = 'hds-library-hide-empty'
const PLAYLISTS_SECTION_KEY = 'hds-library-playlists-open'
const PAGE_SIZE = 48

let _filterDebounce: ReturnType<typeof setTimeout>

export class LibraryStore {
  libraries:    LibraryWithSource[] = []
  shows:        Show[] = []
  movies:       Movie[] = []
    // contentType === 'all' browsing (see fetch()) — mixedIndex is the full,
    // already-sorted show+movie result (see kairos's MixedSort.h); mixedItems
    // is however much of it has been hydrated with render-ready tile data so
    // far. loadMore() only ever hydrates the next slice of the existing
    // index, never re-fetches/re-sorts it, so a "Random" sort doesn't
    // reshuffle out from under an in-progress infinite scroll.
    mixedIndex: MixedIndexEntry[] = []
    mixedItems: MixedMediaItem[] = []
  // Library's "Include Episodes" toggle (default off/hidden) — a 3rd,
  // independent bucket alongside shows/movies rather than a 4th exclusive
  // contentType, so a mixed movie+episode playlist can render both kinds at
  // once when viewing it (see MediaGrid.tsx). v1 scope deliberately doesn't
  // extend the full rule-builder field set to episodes — see
  // ContentRepository.h's EpisodeSearchParams comment.
  includeEpisodes: boolean = false
  episodes:     EpisodeSearchResult[] = []
  // Library's new "Playlists" section tiles (any-authenticated-user browse
  // summary, not the admin-only full list — see PlaylistRepository::listBrowse).
  playlists:    PlaylistBrowseEntry[] = []
  // Collapsible, not a hard show/hide — the section header stays put either
  // way so it's still discoverable once there are enough playlists that
  // always showing the tile row would crowd out the actual library grid.
  // Persisted like density/sidebarOpen/hideEmpty below.
  playlistsSectionOpen: boolean = localStorage.getItem(PLAYLISTS_SECTION_KEY) !== 'false'
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

    // "Save as Smart Playlist" — captures the current filter+sort as a new
    // smart playlist. savingSmartPlaylist just toggles the inline title
    // prompt; the actual create+save is smartPlaylistBusy/saveSmartPlaylist.
    savingSmartPlaylist: boolean = false
    smartPlaylistTitle: string = ''
    smartPlaylistBusy: boolean = false

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

  // Derived, not a separate field — a "playlist:<id>" rule in the filter
  // tree (added by enterPlaylist(), removable/addable like any other rule)
  // IS the active-playlist state, single source of truth. This is also what
  // makes "further filtering still works" free: the user can add more rules
  // alongside it, or remove the playlist rule itself to exit, through the
  // exact same rule-builder UI as any other field.
  get activePlaylistId(): string | null {
    const rule = this.filterTree.allRules.find(r => r.field === 'playlist' && r.value.trim())
    return rule ? rule.value : null
  }

  async loadPlaylists() {
    const playlists = await api.getPlaylistsBrowse()
    runInAction(() => { this.playlists = playlists })
  }

  // Library "Playlists" section tile click — turns it into a normal filter
  // (see activePlaylistId above) and defaults sort/Include Episodes to
  // whatever best shows the playlist's actual contents; both stay
  // user-changeable afterward (e.g. switch back to Title sort, or hide
  // episodes again) without losing the playlist scoping itself.
  enterPlaylist(playlistId: string) {
    this.filterTree.setSingleRule('playlist', playlistId)
    this.sort = 'playlist_order'
    this.includeEpisodes = true
    this.sidebarOpen = true
    localStorage.setItem(SIDEBAR_KEY, 'true')
    this.page = 0
    this.fetch()
  }

  // Dismissing the "Viewing: <title> ×" chip — just removes the playlist
  // rule (dropping back to whatever other rules were alongside it, if any),
  // no other state forced back (respects whatever sort/Include Episodes the
  // user has now, rather than assuming they want the pre-playlist values).
  exitPlaylist() {
    this.filterTree.items = this.filterTree.items.filter(it => !(it.kind === 'rule' && it.field === 'playlist'))
    if (this.sort === 'playlist_order') this.sort = 'recently_added'
    this.page = 0
    this.fetch()
  }

    setIncludeEpisodes(v: boolean) {
        this.includeEpisodes = v
        if (!v && this.sort === 'episode_number') this.sort = 'recently_added'
        this.page = 0
        this.fetch()
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
    // used to only read 'is' rules for 6 of 16 fields, and used to silently
    // drop 'library' rules entirely (the multi-select library pills below
    // still own "which libraries are visible at all" via library_ids, but a
    // 'library' rule in the panel is now a real, additional AND'd
    // constraint instead of a no-op).
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
      // Orthogonal to `filter`'s own `playlist:<id>` clause (which handles
      // inclusion) — this drives playlist_order's ORDER BY and, for
      // episodes, membership scoping too (see EpisodeSearchParams' comment
      // for why episodes don't get the full canon filter field set).
      playlist_id: this.activePlaylistId ?? undefined,
    }
  }

  private episodeSearchParams(page: number) {
      const sort: 'title' | 'episode_number' | 'playlist_order' =
          this.sort === 'playlist_order' || this.sort === 'episode_number' ? this.sort : 'title'
    return {
      q: this.query || undefined,
      limit: PAGE_SIZE,
      offset: page * PAGE_SIZE,
        sort,
      sort_dir: this.sortDir || undefined,
      playlist_id: this.activePlaylistId ?? undefined,
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

    // contentType 'all' truly interleaves shows+movies (see MixedSort.h) —
    // one full sorted index fetch, then hydrate whichever slice is being
    // rendered. Any other contentType has only one type to show, so plain
    // getShows/getMovies (paginated server-side as always) is simpler and
    // already sufficient — mixing only matters when both are in play.
    private get isMixedBrowse() {
        return this.contentType === 'all'
    }

    private async hydrateMixedSlice(index: MixedIndexEntry[], start: number, end: number): Promise<MixedMediaItem[]> {
        const slice = index.slice(start, end)
        if (slice.length === 0) return []
        return (await api.getMixedMediaTiles(slice.map(e => ({content_type: e.content_type, id: e.id})))).items
    }

  async fetch() {
    if (this.noLibrariesSelected) {
        runInAction(() => {
            this.shows = [];
            this.movies = [];
            this.mixedIndex = [];
            this.mixedItems = []
            this.episodes = [];
            this.total = 0;
            this.loading = false;
            this.error = null
        })
      return
    }
    runInAction(() => { this.loading = true })
    const base = this.searchParams(this.page)
    try {
        if (this.isMixedBrowse) {
            const [indexRes, episodeRes] = await Promise.all([
                api.getMixedMediaIndex({
                    library_ids: base.library_ids, q: base.q, filter: base.filter,
                    sort: this.sort || undefined, sort_dir: base.sort_dir, hideEmpty: base.hideEmpty,
                }),
                this.includeEpisodes ? api.searchEpisodes(this.episodeSearchParams(this.page)) : Promise.resolve({
                    items: [] as EpisodeSearchResult[],
                    total: 0
                }),
            ])
            const items = await this.hydrateMixedSlice(indexRes.items, 0, PAGE_SIZE)
            runInAction(() => {
                this.shows = [];
                this.movies = []
                this.mixedIndex = indexRes.items
                this.mixedItems = items
                this.episodes = episodeRes.items
                this.total = indexRes.items.length + episodeRes.total
                this.loading = false
                this.error = null
            })
        } else {
            const [showRes, movieRes, episodeRes] = await Promise.all([
                this.contentType !== 'movie' ? api.getShows({
                    ...base,
                    sort: this.showSort()
                }) : Promise.resolve({items: [] as Show[], total: 0}),
                this.contentType !== 'show' ? api.getMovies({
                    ...base,
                    sort: this.movieSort()
                }) : Promise.resolve({items: [] as Movie[], total: 0}),
                this.includeEpisodes ? api.searchEpisodes(this.episodeSearchParams(this.page)) : Promise.resolve({
                    items: [] as EpisodeSearchResult[],
                    total: 0
                }),
            ])
            runInAction(() => {
                this.shows = showRes.items
                this.movies = movieRes.items
                this.mixedIndex = [];
                this.mixedItems = []
                this.episodes = episodeRes.items
                this.total = showRes.total + movieRes.total + episodeRes.total
                this.loading = false
                this.error = null
            })
        }
    } catch (e: any) {
      // Cleared rather than left stale — a failed request used to just
      // leave whatever was on screen before, which reads exactly like "the
      // filter did nothing" instead of "this request failed."
      runInAction(() => {
          this.shows = [];
          this.movies = [];
          this.mixedIndex = [];
          this.mixedItems = []
          this.episodes = [];
          this.total = 0
        this.loading = false
        this.error = e?.message ?? 'Failed to load library'
      })
    }
  }

  async loadMore() {
    if (this.loading || this.loadingMore || this.noLibrariesSelected) return
      const loadedCount = this.isMixedBrowse
          ? this.mixedItems.length + this.episodes.length
          : this.shows.length + this.movies.length + this.episodes.length
      if (loadedCount >= this.total) return
    runInAction(() => { this.loadingMore = true })
    const nextPage = this.page + 1
    try {
        if (this.isMixedBrowse) {
            const [newTiles, episodeRes] = await Promise.all([
                this.hydrateMixedSlice(this.mixedIndex, this.mixedItems.length, this.mixedItems.length + PAGE_SIZE),
                this.includeEpisodes ? api.searchEpisodes(this.episodeSearchParams(nextPage)) : Promise.resolve({
                    items: [] as EpisodeSearchResult[],
                    total: 0
                }),
            ])
            runInAction(() => {
                this.mixedItems = [...this.mixedItems, ...newTiles]
                this.episodes = [...this.episodes, ...episodeRes.items]
                this.page = nextPage
                this.loadingMore = false
                this.error = null
            })
        } else {
            const base = this.searchParams(nextPage)
            const [showRes, movieRes, episodeRes] = await Promise.all([
                this.contentType !== 'movie' ? api.getShows({
                    ...base,
                    sort: this.showSort()
                }) : Promise.resolve({items: [] as Show[], total: 0}),
                this.contentType !== 'show' ? api.getMovies({
                    ...base,
                    sort: this.movieSort()
                }) : Promise.resolve({items: [] as Movie[], total: 0}),
                this.includeEpisodes ? api.searchEpisodes(this.episodeSearchParams(nextPage)) : Promise.resolve({
                    items: [] as EpisodeSearchResult[],
                    total: 0
                }),
            ])
            runInAction(() => {
                this.shows = [...this.shows, ...showRes.items]
                this.movies = [...this.movies, ...movieRes.items]
                this.episodes = [...this.episodes, ...episodeRes.items]
                this.page = nextPage
                this.loadingMore = false
                this.error = null
            })
        }
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

  // Same landing pattern as presetFilter, but for a Home shelf def's full
  // filter-set (contentType + sort + canon filter string — see HomePage.tsx's
  // HOME_SHELVES) rather than a single field/value pair, so "Continue in
  // Library" reproduces exactly what the shelf itself was built from instead
  // of just its contentType/sort. An empty filter string is a no-op on the
  // panel (setFromFilterString), same as a shelf with no extra criteria today.
  presetFilterFromString(contentType: 'show' | 'movie' | 'all', sort: string, filter: string) {
    this.contentType = contentType
    this.sort = sort
    if (filter) {
      this.filterTree.setFromFilterString(filter)
      this.sidebarOpen = true
      localStorage.setItem(SIDEBAR_KEY, 'true')
    }
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

  togglePlaylistsSection() {
    this.playlistsSectionOpen = !this.playlistsSectionOpen
    localStorage.setItem(PLAYLISTS_SECTION_KEY, String(this.playlistsSectionOpen))
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

    startSaveSmartPlaylist() {
        this.savingSmartPlaylist = true
        this.smartPlaylistTitle = this.query.trim() || 'New Smart Playlist'
    }

    cancelSaveSmartPlaylist() {
        this.savingSmartPlaylist = false
    }

    // Captures the current rule-builder filter + free-text query + sort as a
    // new smart playlist — contentType 'all' becomes smart_type 'mixed' (see
    // MixedSort.h), the same combined-filter convention withCombinedFilter
    // already uses for every getShows/getMovies request. Refreshes immediately
    // after saving so it's not left showing stale/empty membership until the
    // next background sync — same reasoning as PlaylistPage's saveSmartDef.
    // Returns the new playlist_id so the caller can navigate to it.
    async saveSmartPlaylist(): Promise<string> {
        const title = this.smartPlaylistTitle.trim() || 'New Smart Playlist'
        const filter_expr = [this.searchParams(0).filter, this.query.trim()].filter(Boolean).join(' ')
        const smart_type = this.contentType === 'all' ? 'mixed' : this.contentType

        runInAction(() => {
            this.smartPlaylistBusy = true
        })
        try {
            const {playlist_id} = await api.createPlaylist({title})
            await api.updatePlaylist(playlist_id, {
                membership: 'smart', smart_type, smart_sort: this.sort || 'title', filter_expr,
            })
            await api.refreshSmartPlaylist(playlist_id)
            runInAction(() => {
                this.smartPlaylistBusy = false;
                this.savingSmartPlaylist = false
            })
            return playlist_id
        } catch (e: any) {
            runInAction(() => {
                this.smartPlaylistBusy = false;
                this.error = e?.message ?? 'Failed to save smart playlist'
            })
            throw e
        }
    }
}

export const libraryStore = new LibraryStore()
