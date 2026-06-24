import { observer } from 'mobx-react-lite'
import { makeAutoObservable, runInAction } from 'mobx'
import { useEffect, useRef, useState } from 'react'
import { api } from '../api/client'
import type { Episode, EpisodeGroup, GroupingCandidate, LibraryWithSource, Movie, MovieDetail, Show, ShowDetail } from '../api/types'

// ─── SmartInput ───────────────────────────────────────────────────────────────
// Single-value combobox with server-side suggestions (filter-values API).

function SmartInput({ field, type, value, onChange, placeholder, className }: {
  field:       string
  type:        'show' | 'movie'
  value:       string
  onChange:    (v: string) => void
  placeholder: string
  className:   string
}) {
  const [options, setOptions] = useState<string[]>([])
  const [open,    setOpen]    = useState(false)
  const [hiIdx,   setHiIdx]   = useState(0)
  const wrapRef  = useRef<HTMLDivElement>(null)
  const inputRef = useRef<HTMLInputElement>(null)

  useEffect(() => {
    api.getFilterValues(field, { type }).then(setOptions).catch(() => {})
  }, [field, type])

  useEffect(() => {
    const h = (e: MouseEvent) => {
      if (wrapRef.current && !wrapRef.current.contains(e.target as Node)) setOpen(false)
    }
    document.addEventListener('mousedown', h)
    return () => document.removeEventListener('mousedown', h)
  }, [])

  const filtered = value === ''
    ? options
    : options.filter(o => o.toLowerCase().includes(value.toLowerCase()))

  const pick = (v: string) => { onChange(v); setOpen(false); setHiIdx(0) }

  const handleKey = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (!open || filtered.length === 0) return
    if      (e.key === 'ArrowDown')  { e.preventDefault(); setHiIdx(i => Math.min(i + 1, filtered.length - 1)) }
    else if (e.key === 'ArrowUp')    { e.preventDefault(); setHiIdx(i => Math.max(i - 1, 0)) }
    else if (e.key === 'Enter')      { e.preventDefault(); pick(filtered[hiIdx] ?? filtered[0]) }
    else if (e.key === 'Escape')     { setOpen(false) }
  }

  return (
    <div ref={wrapRef} className="relative">
      <input
        ref={inputRef}
        value={value}
        onChange={e => { onChange(e.target.value); setOpen(true); setHiIdx(0) }}
        onFocus={() => setOpen(true)}
        onKeyDown={handleKey}
        placeholder={placeholder}
        className={className}
        autoComplete="off"
        spellCheck={false}
      />
      {open && filtered.length > 0 && (
        <ul className="absolute z-50 top-full left-0 right-0 mt-0.5 max-h-52 overflow-y-auto
                        bg-zinc-900 border border-zinc-700 rounded-lg shadow-xl text-xs">
          {filtered.map((opt, i) => (
            <li
              key={opt}
              onMouseDown={e => { e.preventDefault(); pick(opt) }}
              onMouseEnter={() => setHiIdx(i)}
              className={`px-3 py-1.5 cursor-pointer truncate ${
                i === hiIdx ? 'bg-violet-700 text-white' : 'text-zinc-300 hover:bg-zinc-800'
              }`}
            >{opt}</li>
          ))}
        </ul>
      )}
    </div>
  )
}

// ─── Store ───────────────────────────────────────────────────────────────────

const PAGE_SIZE = 50

type DetailItem = ShowDetail | MovieDetail

class ContentStore {
  libraries:   LibraryWithSource[] = []
  selectedLib: string | null = null
  tab:         'shows' | 'movies' = 'shows'

  shows:   Show[]  = []
  movies:  Movie[] = []
  total:   number  = 0
  page:    number  = 1

  // Filters
  query:          string = ''
  filterGenre:    string = ''
  filterYear:     string = ''
  filterRating:   string = ''

  loading: boolean       = false
  error:   string | null = null

  // Detail overlay
  detailItem:     DetailItem | null = null
  detailLoading:  boolean = false
  detailTab:      string  = 'overview'
  episodes:           Episode[] = []
  groupingCandidates: GroupingCandidate[] = []
  groupingLoading:    boolean = false
  confirmedGroups:    EpisodeGroup[] = []
  groupsLoading:      boolean = false
  groupsSaving:       boolean = false

  // Edit
  editDraft:  Partial<DetailItem> = {}
  editSaving: boolean = false
  editError:  string | null = null

  constructor() { makeAutoObservable(this) }

  get pageCount()  { return Math.max(1, Math.ceil(this.total / PAGE_SIZE)) }
  get offset()     { return (this.page - 1) * PAGE_SIZE }

  get selectedLibObj(): LibraryWithSource | null {
    return this.libraries.find(l => l.library_id === this.selectedLib) ?? null
  }

  get visibleTabs(): Array<'shows' | 'movies'> {
    const t = this.selectedLibObj?.library_type
    if (t === 'show')  return ['shows']
    if (t === 'movie') return ['movies']
    return ['shows', 'movies']
  }

  get isShowDetail(): boolean {
    return this.detailItem != null && 'show_id' in this.detailItem
  }

  async loadLibraries() {
    try {
      const libs = await api.getAllLibraries()
      runInAction(() => { this.libraries = libs })
    } catch {}
  }

  selectLib(id: string | null) {
    this.selectedLib = id
    this.page = 1
    const lib = this.libraries.find(l => l.library_id === id)
    if (lib?.library_type === 'show')  this.tab = 'shows'
    if (lib?.library_type === 'movie') this.tab = 'movies'
    this.fetch()
  }

  selectTab(t: 'shows' | 'movies') {
    this.tab  = t
    this.page = 1
    this.fetch()
  }

  goToPage(p: number) {
    this.page = Math.max(1, Math.min(p, this.pageCount))
    this.fetch()
  }

  setQuery(q: string) {
    this.query = q
    this.page  = 1
    this.fetch()
  }

  setFilterGenre(g: string) {
    this.filterGenre = g
    this.page = 1
    this.fetch()
  }

  setFilterYear(y: string) {
    this.filterYear = y
    this.page = 1
    this.fetch()
  }

  setFilterRating(r: string) {
    this.filterRating = r
    this.page = 1
    this.fetch()
  }

  clearFilters() {
    this.query       = ''
    this.filterGenre = ''
    this.filterYear  = ''
    this.filterRating = ''
    this.page = 1
    this.fetch()
  }

  async openDetail(item: Show | Movie) {
    this.detailItem    = null
    this.detailLoading = true
    this.detailTab     = 'overview'
    this.episodes      = []
    this.editDraft     = {}
    this.editError     = null

    try {
      if ('show_id' in item) {
        const [detail, eps] = await Promise.all([
          api.getShow(item.show_id),
          api.getEpisodes(item.show_id),
        ])
        runInAction(() => {
          this.detailItem    = detail
          this.episodes      = eps
          this.editDraft     = { ...detail }
          this.detailLoading = false
        })
      } else {
        const detail = await api.getMovie(item.movie_id)
        runInAction(() => {
          this.detailItem    = detail
          this.editDraft     = { ...detail }
          this.detailLoading = false
        })
      }
    } catch {
      runInAction(() => { this.detailLoading = false })
    }
  }

  closeDetail() {
    this.detailItem = null
    this.episodes   = []
  }

  setDetailTab(t: string) {
    this.detailTab = t
    if (t === 'grouping' && this.detailItem && 'show_id' in this.detailItem) {
      this.loadGroupingTab((this.detailItem as ShowDetail).show_id)
    }
  }

  loadGroupingTab(showId: string) {
    this.groupingLoading = true
    this.groupsLoading   = true
    this.groupingCandidates = []
    this.confirmedGroups    = []
    Promise.all([
      api.getGroupingCandidates(showId),
      api.getEpisodeGroups(showId),
    ]).then(([cands, groups]) => runInAction(() => {
      this.groupingCandidates = cands.candidates
      this.confirmedGroups    = groups
      this.groupingLoading    = false
      this.groupsLoading      = false
    })).catch(() => runInAction(() => {
      this.groupingLoading = false
      this.groupsLoading   = false
    }))
  }

  async confirmGroup(showId: string, candidate: GroupingCandidate) {
    this.groupsSaving = true
    try {
      const { group_id } = await api.createEpisodeGroup(showId, { name: candidate.base_title, group_type: 'multipart' })
      await Promise.all(candidate.parts.map(p => api.addGroupMember(showId, group_id, { episode_id: p.episode_id, part_num: p.part_num })))
    } catch {}
    runInAction(() => { this.groupsSaving = false })
    this.loadGroupingTab(showId)
  }

  async deleteGroup(showId: string, groupId: string) {
    this.groupsSaving = true
    try {
      await api.deleteEpisodeGroup(showId, groupId)
    } catch {}
    runInAction(() => { this.groupsSaving = false })
    this.loadGroupingTab(showId)
  }

  patchEdit(field: string, value: unknown) {
    this.editDraft = { ...this.editDraft, [field]: value }
  }

  async saveEdit() {
    if (!this.detailItem) return
    this.editSaving = true
    this.editError  = null
    try {
      if (this.isShowDetail) {
        await api.updateShow((this.detailItem as ShowDetail).show_id, this.editDraft as Partial<ShowDetail>)
        const updated = await api.getShow((this.detailItem as ShowDetail).show_id)
        runInAction(() => {
          this.detailItem = updated
          this.editDraft  = { ...updated }
          this.editSaving = false
        })
      } else {
        await api.updateMovie((this.detailItem as MovieDetail).movie_id, this.editDraft as Partial<MovieDetail>)
        const updated = await api.getMovie((this.detailItem as MovieDetail).movie_id)
        runInAction(() => {
          this.detailItem = updated
          this.editDraft  = { ...updated }
          this.editSaving = false
        })
      }
    } catch (e: any) {
      runInAction(() => { this.editError = e.message; this.editSaving = false })
    }
  }

  async fetch() {
    this.loading = true
    this.error   = null
    const params = {
      limit:          PAGE_SIZE,
      offset:         this.offset,
      library_id:     this.selectedLib ?? undefined,
      q:              this.query       || undefined,
      genre:          this.filterGenre || undefined,
      year:           this.filterYear  ? Number(this.filterYear) : undefined,
      content_rating: this.filterRating || undefined,
    }
    try {
      if (this.tab === 'shows') {
        const r = await api.getShows(params)
        runInAction(() => { this.shows = r.items; this.total = r.total; this.loading = false })
      } else {
        const r = await api.getMovies(params)
        runInAction(() => { this.movies = r.items; this.total = r.total; this.loading = false })
      }
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.loading = false })
    }
  }
}

const store = new ContentStore()

// ─── Helpers ─────────────────────────────────────────────────────────────────

function groupBySource(libs: LibraryWithSource[]) {
  const map = new Map<string, { name: string; type: string; items: LibraryWithSource[] }>()
  for (const lib of libs) {
    if (!map.has(lib.source_id))
      map.set(lib.source_id, { name: lib.source_name, type: lib.source_type, items: [] })
    map.get(lib.source_id)!.items.push(lib)
  }
  return [...map.values()]
}

function libTypeIcon(t: string) {
  return t === 'movie' ? '▣' : t === 'show' ? '▤' : '◫'
}

function fmtMins(ms: number) { return `${Math.round(ms / 60000)}m` }

// ─── Component ───────────────────────────────────────────────────────────────

export default observer(function ContentPage() {
  const listRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    store.loadLibraries()
    store.fetch()
  }, [])

  useEffect(() => {
    listRef.current?.scrollTo({ top: 0 })
  }, [store.page, store.tab, store.selectedLib])

  const groups = groupBySource(store.libraries)

  return (
    <>
    <div className="flex flex-col h-[calc(100vh-3.5rem)] gap-0">
      <div className="flex items-center justify-between pb-4 shrink-0">
        <h1 className="text-xl font-semibold text-zinc-100">Content Library</h1>
        <span className="text-xs text-zinc-600">
          {store.total > 0 && `${store.total.toLocaleString()} ${store.tab}`}
        </span>
      </div>

      <div className="flex gap-4 flex-1 min-h-0">
        {/* Library sidebar */}
        <aside className="w-44 shrink-0 flex flex-col gap-0.5 overflow-y-auto scrollbar-dark pr-1">
          <button
            onClick={() => store.selectLib(null)}
            className={`flex items-center gap-2 px-2.5 py-1.5 rounded text-sm transition-all text-left ${
              store.selectedLib === null
                ? 'bg-amber-500/10 text-amber-400 ring-1 ring-amber-500/20'
                : 'text-zinc-500 hover:text-zinc-200 hover:bg-violet-950/30'
            }`}
          >
            <span className={store.selectedLib === null ? 'text-amber-400' : 'text-zinc-600'}>◆</span>
            All Content
          </button>

          {groups.length === 0 && (
            <p className="text-[11px] text-zinc-700 px-2 pt-2">No libraries configured.</p>
          )}

          {groups.map(group => (
            <div key={group.name} className="mt-3 first:mt-1">
              <div className="px-2.5 pb-1">
                <span className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60">
                  {group.name}
                </span>
              </div>
              {group.items.map(lib => (
                <button
                  key={lib.library_id}
                  onClick={() => store.selectLib(lib.library_id)}
                  className={`w-full flex items-center gap-2 px-2.5 py-1.5 rounded text-sm transition-all text-left ${
                    store.selectedLib === lib.library_id
                      ? 'bg-amber-500/10 text-amber-400 ring-1 ring-amber-500/20'
                      : 'text-zinc-500 hover:text-zinc-200 hover:bg-violet-950/30'
                  }`}
                >
                  <span className={`text-base leading-none ${
                    store.selectedLib === lib.library_id ? 'text-amber-400' : 'text-zinc-700'
                  }`}>
                    {libTypeIcon(lib.library_type)}
                  </span>
                  <span className="truncate">{lib.display_name}</span>
                </button>
              ))}
            </div>
          ))}
        </aside>

        {/* Content area */}
        <div className="flex-1 flex flex-col min-w-0 min-h-0 gap-3">
          {/* Search + filter bar */}
          <div className="flex items-center gap-2 shrink-0">
            <input
              type="search"
              value={store.query}
              onChange={e => store.setQuery(e.target.value)}
              placeholder="Search by title…"
              className="flex-1 bg-zinc-800/60 border border-zinc-700/50 rounded-lg px-3 py-1.5 text-sm text-zinc-200 placeholder:text-zinc-600 focus:outline-none focus:border-violet-700/60"
            />
            <SmartInput
              field="genre"
              type={store.tab === 'movies' ? 'movie' : 'show'}
              value={store.filterGenre}
              onChange={v => store.setFilterGenre(v)}
              placeholder="Genre"
              className="w-28 bg-zinc-800/60 border border-zinc-700/50 rounded-lg px-3 py-1.5 text-sm text-zinc-200 placeholder:text-zinc-600 focus:outline-none focus:border-violet-700/60"
            />
            <input
              type="number"
              value={store.filterYear}
              onChange={e => store.setFilterYear(e.target.value)}
              placeholder="Year"
              className="w-20 bg-zinc-800/60 border border-zinc-700/50 rounded-lg px-3 py-1.5 text-sm text-zinc-200 placeholder:text-zinc-600 focus:outline-none focus:border-violet-700/60"
            />
            <SmartInput
              field="content_rating"
              type={store.tab === 'movies' ? 'movie' : 'show'}
              value={store.filterRating}
              onChange={v => store.setFilterRating(v)}
              placeholder="Rating"
              className="w-20 bg-zinc-800/60 border border-zinc-700/50 rounded-lg px-3 py-1.5 text-sm text-zinc-200 placeholder:text-zinc-600 focus:outline-none focus:border-violet-700/60"
            />
            {(store.query || store.filterGenre || store.filterYear || store.filterRating) && (
              <button
                onClick={() => store.clearFilters()}
                className="text-xs text-zinc-500 hover:text-zinc-300 transition-colors whitespace-nowrap"
              >✕ Clear</button>
            )}
          </div>

          <div className="flex gap-1 border-b border-zinc-800/80 shrink-0">
            {store.visibleTabs.map(t => (
              <button
                key={t}
                onClick={() => store.selectTab(t)}
                className={`px-4 py-2 text-sm capitalize transition-all border-b-2 -mb-px ${
                  store.tab === t
                    ? 'border-amber-500 text-amber-400'
                    : 'border-transparent text-zinc-500 hover:text-zinc-300'
                }`}
              >
                {t}
              </button>
            ))}
          </div>

          {store.error && (
            <div className="text-red-400 text-sm bg-red-950/30 border border-red-900/40 rounded-lg p-3 shrink-0">
              {store.error}
            </div>
          )}

          <div ref={listRef} className="flex-1 overflow-y-auto scrollbar-dark min-h-0 space-y-1 pr-1">
            {store.loading ? (
              <div className="flex items-center gap-2 text-zinc-600 text-sm py-6 px-1">
                <span className="w-1.5 h-1.5 rounded-full bg-violet-500 animate-pulse" />
                Loading…
              </div>
            ) : store.tab === 'shows' ? (
              store.shows.length === 0 ? (
                <p className="text-zinc-600 text-sm py-6 px-1">No shows in this library.</p>
              ) : store.shows.map(show => (
                <ShowRow key={show.show_id} show={show} onClick={() => store.openDetail(show)} />
              ))
            ) : (
              store.movies.length === 0 ? (
                <p className="text-zinc-600 text-sm py-6 px-1">No movies in this library.</p>
              ) : store.movies.map(movie => (
                <MovieRow key={movie.movie_id} movie={movie} onClick={() => store.openDetail(movie)} />
              ))
            )}
          </div>

          {store.total > PAGE_SIZE && (
            <Pagination
              page={store.page}
              pageCount={store.pageCount}
              onPage={p => store.goToPage(p)}
            />
          )}
        </div>
      </div>
    </div>

    {(store.detailItem || store.detailLoading) && (
      <DetailOverlay store={store} />
    )}
    </>
  )
})

// ─── List rows ────────────────────────────────────────────────────────────────

function ShowRow({ show, onClick }: { show: Show; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      className="w-full flex items-center justify-between rounded-lg border text-left
                  border-violet-900/20 bg-zinc-900/60 px-4 py-2.5
                  hover:border-violet-800/40 hover:bg-zinc-900 transition-colors cursor-pointer"
    >
      <span className="text-sm text-zinc-200 truncate mr-4">{show.title}</span>
      <div className="flex items-center gap-3 shrink-0">
        <span className="text-xs text-zinc-600">{show.episode_count} ep</span>
        {show.content_rating && <Rating value={show.content_rating} />}
      </div>
    </button>
  )
}

function MovieRow({ movie, onClick }: { movie: Movie; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      className="w-full flex items-center justify-between rounded-lg border text-left
                  border-violet-900/20 bg-zinc-900/60 px-4 py-2.5
                  hover:border-violet-800/40 hover:bg-zinc-900 transition-colors cursor-pointer"
    >
      <div className="flex items-center gap-2 min-w-0 mr-4">
        <span className="text-sm text-zinc-200 truncate">{movie.title}</span>
        {movie.year && <span className="text-zinc-600 text-xs shrink-0">({movie.year})</span>}
      </div>
      <div className="flex items-center gap-3 shrink-0">
        <span className="text-xs text-zinc-600">{fmtMins(movie.duration_ms)}</span>
        {movie.content_rating && <Rating value={movie.content_rating} />}
      </div>
    </button>
  )
}

function Rating({ value }: { value: string }) {
  return (
    <span className="px-1.5 py-0.5 rounded bg-zinc-800 text-zinc-500 text-[10px] font-mono">
      {value}
    </span>
  )
}

// ─── Pagination ───────────────────────────────────────────────────────────────

function Pagination({ page, pageCount, onPage }: {
  page:      number
  pageCount: number
  onPage:    (p: number) => void
}) {
  const pages: Array<number | '…'> = []
  const add = (n: number) => { if (!pages.includes(n)) pages.push(n) }
  add(1)
  if (page > 3) pages.push('…')
  for (let i = Math.max(2, page - 1); i <= Math.min(pageCount - 1, page + 1); i++) add(i)
  if (page < pageCount - 2) pages.push('…')
  if (pageCount > 1) add(pageCount)

  return (
    <div className="flex items-center justify-between shrink-0 pt-1 border-t border-zinc-800/60">
      <button onClick={() => onPage(page - 1)} disabled={page === 1}
        className="px-2.5 py-1 text-xs text-zinc-400 hover:text-zinc-100 disabled:opacity-30 disabled:cursor-not-allowed transition-colors">
        ← Prev
      </button>
      <div className="flex items-center gap-1">
        {pages.map((p, i) =>
          p === '…' ? (
            <span key={`e${i}`} className="px-1 text-xs text-zinc-600">…</span>
          ) : (
            <button key={p} onClick={() => onPage(p as number)}
              className={`w-7 h-7 text-xs rounded transition-all ${
                p === page ? 'bg-amber-500/20 text-amber-400 ring-1 ring-amber-500/30'
                           : 'text-zinc-500 hover:text-zinc-200 hover:bg-violet-950/40'
              }`}>
              {p}
            </button>
          )
        )}
      </div>
      <button onClick={() => onPage(page + 1)} disabled={page === pageCount}
        className="px-2.5 py-1 text-xs text-zinc-400 hover:text-zinc-100 disabled:opacity-30 disabled:cursor-not-allowed transition-colors">
        Next →
      </button>
    </div>
  )
}

// ─── Detail overlay ───────────────────────────────────────────────────────────

const DetailOverlay = observer(function DetailOverlay({ store }: { store: ContentStore }) {
  useEffect(() => {
    const h = (e: KeyboardEvent) => { if (e.key === 'Escape') store.closeDetail() }
    document.addEventListener('keydown', h)
    return () => document.removeEventListener('keydown', h)
  }, [])

  const item     = store.detailItem
  const isShow   = item != null && 'show_id' in item
  const show     = isShow  ? item as ShowDetail  : null
  const movie    = !isShow ? item as MovieDetail : null
  const id       = show?.show_id ?? movie?.movie_id ?? ''
  const itemType = isShow ? 'shows' : 'movies'

  const tabs = isShow
    ? ['overview', 'episodes', 'grouping', 'details', 'edit'] as const
    : ['overview', 'details', 'edit'] as const

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-zinc-950/85 backdrop-blur-sm"
      onClick={() => store.closeDetail()}
    >
      <div
        className="relative w-full max-w-3xl max-h-[90vh] flex flex-col bg-zinc-900
                    border border-violet-900/30 rounded-xl shadow-2xl overflow-hidden mx-4"
        onClick={e => e.stopPropagation()}
      >
        {/* Art backdrop header */}
        <div className="relative shrink-0 h-44 overflow-hidden bg-zinc-950">
          {item && (
            <img
              src={`/api/${itemType}/${id}/art`}
              className="absolute inset-0 w-full h-full object-cover opacity-30 blur-sm scale-105"
              onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
              alt=""
            />
          )}
          <div className="absolute inset-0 bg-gradient-to-t from-zinc-900 via-zinc-900/60 to-transparent" />

          {/* Close button */}
          <button
            onClick={() => store.closeDetail()}
            className="absolute top-3 right-3 z-10 text-zinc-400 hover:text-zinc-100
                        transition-colors w-7 h-7 flex items-center justify-center
                        rounded-full bg-zinc-900/60 hover:bg-zinc-800"
          >
            ✕
          </button>

          {/* Poster + title row */}
          <div className="absolute bottom-0 left-0 right-0 flex items-end gap-4 px-5 pb-4">
            {/* Poster */}
            <div className="shrink-0 w-24 h-36 rounded-lg overflow-hidden bg-zinc-800
                              border border-violet-900/30 shadow-xl -mb-8 relative z-10">
              {item ? (
                <img
                  src={`/api/${itemType}/${id}/thumb`}
                  className="w-full h-full object-cover"
                  onError={e => {
                    const el = e.target as HTMLImageElement
                    el.style.display = 'none'
                    el.parentElement!.classList.add('flex', 'items-center', 'justify-center')
                    el.parentElement!.innerHTML = `<span class="text-zinc-600 text-3xl">▤</span>`
                  }}
                  alt=""
                />
              ) : (
                <div className="w-full h-full flex items-center justify-center">
                  <span className="text-zinc-600 text-3xl">▤</span>
                </div>
              )}
            </div>

            {/* Title / meta */}
            <div className="flex-1 min-w-0 pb-1">
              {store.detailLoading ? (
                <div className="space-y-2">
                  <div className="h-5 w-48 bg-zinc-800 rounded animate-pulse" />
                  <div className="h-3 w-32 bg-zinc-800 rounded animate-pulse" />
                </div>
              ) : item ? (
                <>
                  <h2 className="text-lg font-semibold text-zinc-100 leading-tight truncate">
                    {item.title}
                  </h2>
                  <div className="flex items-center gap-2 mt-1 flex-wrap">
                    {item.content_rating && <Rating value={item.content_rating} />}
                    {item.year && <span className="text-xs text-zinc-400">{item.year}</span>}
                    {show?.status && <span className="text-xs text-zinc-500">{show.status}</span>}
                    {show?.studio && <span className="text-xs text-zinc-600">· {show.studio}</span>}
                    {movie?.director && <span className="text-xs text-zinc-600">dir. {movie.director}</span>}
                    {movie && <span className="text-xs text-zinc-500">{fmtMins(movie.duration_ms)}</span>}
                    {item.audience_rating != null && (
                      <span className="text-xs text-amber-400">★ {item.audience_rating.toFixed(1)}</span>
                    )}
                    {item.locked && (
                      <span className="text-[10px] text-violet-400 bg-violet-950/50 px-1.5 py-0.5 rounded font-mono">
                        locked
                      </span>
                    )}
                  </div>
                </>
              ) : null}
            </div>
          </div>
        </div>

        {/* Spacer for poster overflow */}
        <div className="shrink-0 h-8 bg-zinc-900" />

        {/* Tab bar */}
        <div className="flex gap-0 border-b border-zinc-800/80 shrink-0 px-5">
          {tabs.map(t => (
            <button
              key={t}
              onClick={() => store.setDetailTab(t)}
              disabled={store.detailLoading}
              className={`px-4 py-2.5 text-sm capitalize transition-all border-b-2 -mb-px ${
                store.detailTab === t
                  ? 'border-amber-500 text-amber-400'
                  : 'border-transparent text-zinc-500 hover:text-zinc-300 disabled:opacity-40'
              }`}
            >
              {t}
            </button>
          ))}
        </div>

        {/* Tab content */}
        <div className="flex-1 overflow-y-auto scrollbar-dark min-h-0">
          {store.detailLoading ? (
            <LoadingSkeleton />
          ) : item ? (
            <>
              {store.detailTab === 'overview' && (
                <OverviewTab item={item} isShow={isShow} show={show} movie={movie} />
              )}
              {store.detailTab === 'episodes' && isShow && (
                <EpisodesTab episodes={store.episodes} showId={show!.show_id} />
              )}
              {store.detailTab === 'grouping' && isShow && (
                <GroupingTab
                  candidates={store.groupingCandidates}
                  confirmedGroups={store.confirmedGroups}
                  loading={store.groupingLoading || store.groupsLoading}
                  saving={store.groupsSaving}
                  onConfirm={c => store.confirmGroup(show!.show_id, c)}
                  onDelete={gid => store.deleteGroup(show!.show_id, gid)}
                />
              )}
              {store.detailTab === 'details' && (
                <DetailsTab item={item} isShow={isShow} show={show} movie={movie} />
              )}
              {store.detailTab === 'edit' && (
                <EditTab
                  draft={store.editDraft}
                  isShow={isShow}
                  saving={store.editSaving}
                  error={store.editError}
                  onChange={(f, v) => store.patchEdit(f, v)}
                  onSave={() => store.saveEdit()}
                />
              )}
            </>
          ) : null}
        </div>
      </div>
    </div>
  )
})

// ─── Tab: Overview ────────────────────────────────────────────────────────────

function OverviewTab({ item, isShow, show, movie }: {
  item: DetailItem; isShow: boolean
  show: ShowDetail | null; movie: MovieDetail | null
}) {
  return (
    <div className="p-5 space-y-4">
      {/* Genres */}
      {item.genres.length > 0 && (
        <div className="flex flex-wrap gap-1.5">
          {item.genres.map(g => (
            <span key={g} className="px-2 py-0.5 rounded-full text-[11px] bg-violet-950/60
                                      text-violet-300 border border-violet-800/30">
              {g}
            </span>
          ))}
        </div>
      )}

      {/* Tagline */}
      {movie?.tagline && (
        <p className="text-sm text-zinc-400 italic">{movie.tagline}</p>
      )}

      {/* Overview */}
      {item.overview ? (
        <p className="text-sm text-zinc-300 leading-relaxed">{item.overview}</p>
      ) : (
        <p className="text-sm text-zinc-600 italic">No overview available.</p>
      )}

      {/* Stats row */}
      <div className="grid grid-cols-2 gap-3 pt-2">
        {isShow && show && (
          <>
            <Stat label="Episodes" value={String(show.episode_count)} />
            {show.originally_available_at && (
              <Stat label="First Aired" value={show.originally_available_at} />
            )}
            {show.studio && <Stat label="Network" value={show.studio} />}
            {show.status && <Stat label="Status" value={show.status} />}
          </>
        )}
        {!isShow && movie && (
          <>
            <Stat label="Duration" value={fmtMins(movie.duration_ms)} />
            {movie.year && <Stat label="Year" value={String(movie.year)} />}
            {movie.studio && <Stat label="Studio" value={movie.studio} />}
            {movie.director && <Stat label="Director" value={movie.director} />}
          </>
        )}
      </div>
    </div>
  )
}

// ─── Tab: Episodes ────────────────────────────────────────────────────────────

function EpisodesTab({ episodes, showId }: { episodes: Episode[]; showId: string }) {
  const bySeasonMap = new Map<number, Episode[]>()
  for (const ep of episodes) {
    const arr = bySeasonMap.get(ep.season) ?? []
    arr.push(ep)
    bySeasonMap.set(ep.season, arr)
  }
  const seasons = [...bySeasonMap.entries()].sort(([a], [b]) => a - b)

  if (episodes.length === 0)
    return <p className="p-5 text-zinc-600 text-sm">No episodes found.</p>

  return (
    <div className="p-5 space-y-6">
      {seasons.map(([season, eps]) => (
        <div key={season}>
          <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60 mb-2">
            {season === 0 ? 'Specials' : `Season ${season}`}
          </div>
          <div className="space-y-1">
            {eps.map(ep => (
              <EpisodeRow key={ep.episode_id} ep={ep} showId={showId} season={season} />
            ))}
          </div>
        </div>
      ))}
    </div>
  )
}

function EpisodeRow({ ep, showId, season }: { ep: Episode; showId: string; season: number }) {
  const [open, setOpen] = useState(false)
  const code = season > 0
    ? `S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')}`
    : `Sp${ep.episode}`

  return (
    <div
      className="rounded-lg border border-violet-900/10 bg-zinc-900/40 overflow-hidden
                  hover:border-violet-900/30 transition-colors cursor-pointer"
      onClick={() => setOpen(o => !o)}
    >
      <div className="flex items-center gap-3 px-3 py-2">
        {/* Thumbnail */}
        <div className="shrink-0 w-16 h-9 rounded bg-zinc-800 overflow-hidden">
          <img
            src={`/api/episodes/${ep.episode_id}/thumb`}
            className="w-full h-full object-cover"
            onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
            alt=""
          />
        </div>
        <span className="text-[10px] font-mono text-violet-400/70 shrink-0 w-12">{code}</span>
        <span className="text-sm text-zinc-300 flex-1 truncate">{ep.title}</span>
        {ep.air_date && <span className="text-xs text-zinc-600 shrink-0">{ep.air_date}</span>}
        <span className="text-xs text-zinc-600 shrink-0 ml-1">{fmtMins(ep.duration_ms)}</span>
        <span className={`text-zinc-600 text-xs ml-1 transition-transform ${open ? 'rotate-180' : ''}`}>▾</span>
      </div>
      {open && ep.overview && (
        <div className="px-3 pb-3 pt-0">
          <p className="text-xs text-zinc-500 leading-relaxed pl-[4.5rem]">{ep.overview}</p>
        </div>
      )}
    </div>
  )
}

// ─── Tab: Groups (confirmed + candidates) ────────────────────────────────────

function GroupingTab({
  candidates, confirmedGroups, loading, saving,
  onConfirm, onDelete,
}: {
  candidates:     GroupingCandidate[]
  confirmedGroups: EpisodeGroup[]
  loading:  boolean
  saving:   boolean
  onConfirm: (c: GroupingCandidate) => void
  onDelete:  (groupId: string)      => void
}) {
  if (loading) return <div className="p-6 text-xs text-zinc-500">Loading groups…</div>

  const unconfirmedCandidates = candidates.filter(c => !c.already_grouped)

  return (
    <div className="p-4 space-y-5">

      {/* ── Confirmed groups ── */}
      <section>
        <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60 mb-2">
          Confirmed Groups
        </div>
        {confirmedGroups.length === 0 ? (
          <p className="text-xs text-zinc-600">No confirmed groups yet. Confirm a candidate below to create one.</p>
        ) : (
          <div className="space-y-2">
            {confirmedGroups.map(g => (
              <div key={g.group_id}
                className="rounded-lg border border-emerald-900/40 bg-emerald-950/10 p-3 space-y-1.5"
              >
                <div className="flex items-center gap-2">
                  <span className="text-xs font-medium text-zinc-200 flex-1 truncate">{g.name}</span>
                  <span className="text-[10px] text-emerald-400">{g.members.length} parts</span>
                  <button
                    disabled={saving}
                    onClick={() => onDelete(g.group_id)}
                    className="text-[10px] text-zinc-600 hover:text-red-400 transition-colors disabled:opacity-40"
                  >Delete</button>
                </div>
                <div className="space-y-0.5">
                  {g.members.map(m => (
                    <div key={m.id} className="flex items-center gap-2 text-[11px] text-zinc-400">
                      <span className="w-12 shrink-0 text-zinc-600">Part {m.part_num}</span>
                      <span className="text-zinc-500 w-14 shrink-0">S{String(m.season).padStart(2,'0')}E{String(m.episode).padStart(2,'0')}</span>
                      <span className="truncate">{m.title}</span>
                    </div>
                  ))}
                </div>
              </div>
            ))}
          </div>
        )}
      </section>

      {/* ── Candidates ── */}
      <section>
        <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60 mb-2">
          Detected Candidates
        </div>
        {unconfirmedCandidates.length === 0 ? (
          <p className="text-xs text-zinc-600">
            {candidates.length === 0
              ? 'No multi-part patterns detected. Patterns checked: "(N)", ", Part N", ": Part N", " Part N", and word numbers.'
              : 'All detected candidates are already confirmed.'}
          </p>
        ) : (
          <div className="space-y-2">
            {unconfirmedCandidates.sort((a, b) => b.confidence - a.confidence).map((c, i) => (
              <div key={i}
                className="rounded-lg border p-3 space-y-1.5"
                style={{ borderColor: c.confidence >= 80 ? 'oklch(0.6 0.18 260 / 0.4)' : 'oklch(0.4 0.05 260 / 0.25)' }}
              >
                <div className="flex items-center gap-2">
                  <span className="text-xs font-medium text-zinc-200 flex-1 truncate">{c.base_title}</span>
                  <span
                    className="text-[10px] font-semibold px-1.5 py-0.5 rounded"
                    style={{
                      background: c.confidence >= 80 ? 'oklch(0.55 0.18 260 / 0.25)' : 'oklch(0.4 0.08 260 / 0.15)',
                      color:      c.confidence >= 80 ? 'oklch(0.75 0.18 260)'         : 'oklch(0.6 0.08 260)',
                    }}
                  >{c.confidence}%</span>
                  {!c.adjacent && <span className="text-[10px] text-amber-400">non-adjacent</span>}
                  <button
                    disabled={saving}
                    onClick={() => onConfirm(c)}
                    className="text-[10px] px-2 py-0.5 rounded border border-violet-700/50 text-violet-400
                               hover:border-violet-500 hover:text-violet-300 transition-colors disabled:opacity-40"
                  >Confirm</button>
                </div>
                <div className="space-y-0.5">
                  {c.parts.map(p => (
                    <div key={p.episode_id} className="flex items-center gap-2 text-[11px] text-zinc-400">
                      <span className="w-12 shrink-0 text-zinc-600">Part {p.part_num}</span>
                      <span className="text-zinc-500 w-14 shrink-0">S{String(p.season).padStart(2,'0')}E{String(p.episode).padStart(2,'0')}</span>
                      <span className="truncate">{p.title}</span>
                    </div>
                  ))}
                </div>
              </div>
            ))}
          </div>
        )}
      </section>
    </div>
  )
}

// ─── Tab: Details ─────────────────────────────────────────────────────────────

function DetailsTab({ item, isShow, show, movie }: {
  item: DetailItem; isShow: boolean
  show: ShowDetail | null; movie: MovieDetail | null
}) {
  const plexUrl = item.source_base_url && item.external_id
    ? `${item.source_base_url}/web/index.html#!/server/details?key=/library/metadata/${item.external_id}`
    : null

  return (
    <div className="p-5 space-y-5">
      {/* External links */}
      <div>
        <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60 mb-2">
          External Links
        </div>
        <div className="flex flex-wrap gap-2">
          {plexUrl && <ExternalLink href={plexUrl} label="Plex" color="amber" />}
          {item.imdb_id && (
            <ExternalLink href={`https://www.imdb.com/title/${item.imdb_id}`} label="IMDb" color="yellow" />
          )}
          {show?.tvdb_id && (
            <ExternalLink href={`https://thetvdb.com/?id=${show.tvdb_id}&tab=series`} label="TVDb" color="blue" />
          )}
          {item.tmdb_id && (
            <ExternalLink
              href={`https://www.themoviedb.org/${isShow ? 'tv' : 'movie'}/${item.tmdb_id}`}
              label="TMDb" color="green"
            />
          )}
          {!plexUrl && !item.imdb_id && !show?.tvdb_id && !item.tmdb_id && (
            <p className="text-xs text-zinc-600">No external IDs stored. Run a sync or add them in Edit.</p>
          )}
        </div>
      </div>

      {/* IDs */}
      <div>
        <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60 mb-2">
          Identifiers
        </div>
        <div className="space-y-1.5">
          <IdRow label="Kairos ID"   value={isShow ? show!.show_id : movie!.movie_id} />
          {item.external_id && <IdRow label="Source ID" value={item.external_id} />}
          {item.imdb_id     && <IdRow label="IMDb"      value={item.imdb_id} />}
          {show?.tvdb_id    && <IdRow label="TVDb"      value={show.tvdb_id} />}
          {item.tmdb_id     && <IdRow label="TMDb"      value={item.tmdb_id} />}
        </div>
      </div>

      {/* Technical */}
      <div>
        <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60 mb-2">
          Technical
        </div>
        <div className="space-y-1.5">
          {item.source_base_url && <IdRow label="Source URL" value={item.source_base_url} />}
          {item.locked && (
            <div className="text-xs text-violet-400 bg-violet-950/30 border border-violet-900/30
                              rounded px-2.5 py-1.5">
              This record is locked — manual edits will not be overwritten by future syncs.
            </div>
          )}
        </div>
      </div>
    </div>
  )
}

// ─── Tab: Edit ────────────────────────────────────────────────────────────────

function EditTab({ draft, isShow, saving, error, onChange, onSave }: {
  draft:    Partial<DetailItem>
  isShow:   boolean
  saving:   boolean
  error:    string | null
  onChange: (field: string, value: unknown) => void
  onSave:   () => void
}) {
  const d = draft as any

  return (
    <div className="p-5 space-y-4">
      <div className="grid grid-cols-2 gap-3">
        <Field label="Title" span={2}>
          <input className="input w-full" value={d.title ?? ''} onChange={e => onChange('title', e.target.value)} />
        </Field>
        <Field label="Year">
          <input className="input w-full" type="number" value={d.year ?? ''} onChange={e => onChange('year', Number(e.target.value))} />
        </Field>
        <Field label="Content Rating">
          <input className="input w-full" value={d.content_rating ?? ''} onChange={e => onChange('content_rating', e.target.value)} />
        </Field>
        {isShow && (
          <>
            <Field label="Network / Studio">
              <input className="input w-full" value={d.studio ?? ''} onChange={e => onChange('studio', e.target.value)} />
            </Field>
            <Field label="Status">
              <input className="input w-full" value={d.status ?? ''} onChange={e => onChange('status', e.target.value)} />
            </Field>
            <Field label="First Aired" span={2}>
              <input className="input w-full" value={d.originally_available_at ?? ''} onChange={e => onChange('originally_available_at', e.target.value)} />
            </Field>
          </>
        )}
        {!isShow && (
          <>
            <Field label="Director">
              <input className="input w-full" value={d.director ?? ''} onChange={e => onChange('director', e.target.value)} />
            </Field>
            <Field label="Studio">
              <input className="input w-full" value={d.studio ?? ''} onChange={e => onChange('studio', e.target.value)} />
            </Field>
            <Field label="Tagline" span={2}>
              <input className="input w-full" value={d.tagline ?? ''} onChange={e => onChange('tagline', e.target.value)} />
            </Field>
          </>
        )}
        <Field label="Genres (comma-separated)" span={2}>
          <input
            className="input w-full"
            value={Array.isArray(d.genres) ? d.genres.join(', ') : (d.genres ?? '')}
            onChange={e => onChange('genres', e.target.value.split(',').map((s: string) => s.trim()).filter(Boolean))}
          />
        </Field>
        <Field label="IMDb ID">
          <input className="input w-full font-mono" placeholder="tt0000000" value={d.imdb_id ?? ''} onChange={e => onChange('imdb_id', e.target.value)} />
        </Field>
        {isShow ? (
          <Field label="TVDb ID">
            <input className="input w-full font-mono" value={d.tvdb_id ?? ''} onChange={e => onChange('tvdb_id', e.target.value)} />
          </Field>
        ) : (
          <Field label="TMDb ID">
            <input className="input w-full font-mono" value={d.tmdb_id ?? ''} onChange={e => onChange('tmdb_id', e.target.value)} />
          </Field>
        )}
        {isShow && (
          <Field label="TMDb ID">
            <input className="input w-full font-mono" value={d.tmdb_id ?? ''} onChange={e => onChange('tmdb_id', e.target.value)} />
          </Field>
        )}
        <Field label="Overview" span={2}>
          <textarea
            className="input w-full resize-none"
            rows={4}
            value={d.overview ?? ''}
            onChange={e => onChange('overview', e.target.value)}
          />
        </Field>
        <Field label="Custom Poster URL" span={2}>
          <div className="flex gap-2 items-start">
            <input className="input flex-1 font-mono text-xs" value={d.thumb ?? ''} onChange={e => onChange('thumb', e.target.value)} />
            {d.thumb && (
              <img src={d.thumb.startsWith('http') ? d.thumb : undefined}
                   className="w-10 h-14 rounded object-cover bg-zinc-800 shrink-0 border border-violet-900/30"
                   onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} alt="" />
            )}
          </div>
        </Field>
        <Field label="Custom Backdrop URL" span={2}>
          <input className="input w-full font-mono text-xs" value={d.art ?? ''} onChange={e => onChange('art', e.target.value)} />
        </Field>
      </div>

      {error && (
        <div className="text-red-400 text-xs bg-red-950/30 border border-red-900/40 rounded p-2">
          {error}
        </div>
      )}

      <div className="flex items-center gap-3 pt-1">
        <button onClick={onSave} disabled={saving} className="btn-primary">
          {saving ? 'Saving…' : 'Save Changes'}
        </button>
        <p className="text-xs text-zinc-600">Saving locks this record against future syncs.</p>
      </div>
    </div>
  )
}

// ─── Small shared components ──────────────────────────────────────────────────

function Stat({ label, value }: { label: string; value: string }) {
  return (
    <div className="bg-zinc-800/40 rounded-lg px-3 py-2">
      <div className="text-[10px] text-zinc-600 uppercase tracking-wider">{label}</div>
      <div className="text-sm text-zinc-300 mt-0.5">{value}</div>
    </div>
  )
}

function IdRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex items-center justify-between text-xs">
      <span className="text-zinc-600 shrink-0 w-24">{label}</span>
      <span className="text-zinc-400 font-mono truncate text-right">{value}</span>
    </div>
  )
}

function ExternalLink({ href, label, color }: { href: string; label: string; color: string }) {
  const colors: Record<string, string> = {
    amber: 'text-amber-400 border-amber-800/40 hover:bg-amber-950/30',
    yellow:'text-yellow-400 border-yellow-800/40 hover:bg-yellow-950/30',
    blue:  'text-blue-400  border-blue-800/40  hover:bg-blue-950/30',
    green: 'text-green-400 border-green-800/40 hover:bg-green-950/30',
  }
  return (
    <a href={href} target="_blank" rel="noreferrer"
       className={`inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg border text-xs
                   transition-colors ${colors[color] ?? colors.blue}`}>
      {label} ↗
    </a>
  )
}

function Field({ label, children, span = 1 }: {
  label: string; children: React.ReactNode; span?: 1 | 2
}) {
  return (
    <div className={span === 2 ? 'col-span-2' : ''}>
      <label className="block text-[10px] text-zinc-600 uppercase tracking-wider mb-1">{label}</label>
      {children}
    </div>
  )
}

function LoadingSkeleton() {
  return (
    <div className="p-5 space-y-3 animate-pulse">
      <div className="h-3 w-3/4 bg-zinc-800 rounded" />
      <div className="h-3 w-full bg-zinc-800 rounded" />
      <div className="h-3 w-5/6 bg-zinc-800 rounded" />
      <div className="h-3 w-2/3 bg-zinc-800 rounded" />
    </div>
  )
}
