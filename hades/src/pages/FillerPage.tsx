import { observer } from 'mobx-react-lite'
import { makeAutoObservable, runInAction } from 'mobx'
import { useEffect } from 'react'
import { api } from '../api/client'
import type {
  EpisodeSearchResult, FillerAdvancement, FillerList, FillerListDetail, FillerListItem,
  LibraryWithSource, Movie, PlexBrowseItem, PlexBrowseList, Show, Source,
} from '../api/types'
import { FilterSection, FIELD_DEFS } from '../components/PickerFilters'
import type { FilterField, FilterOp, FilterRule } from '../components/PickerFilters'

// ─── Store ────────────────────────────────────────────────────────────────────

let searchDebounce: ReturnType<typeof setTimeout>
let _ruleId = 0

type PickerTab = 'episodes' | 'movies' | 'shows' | 'plex_playlists' | 'plex_collections'

class FillerPageStore {
  lists:        FillerList[]       = []
  expanded:     string | null      = null
  detail:       FillerListDetail | null = null
  detailLoading: boolean           = false
  loading:      boolean            = false
  error:        string | null      = null
  creating:     boolean            = false
  newTitle:     string             = ''

  pickerOpen:    boolean   = false
  pickerTab:     PickerTab = 'episodes'
  pickerQuery:   string    = ''
  pickerMovies:  Movie[]   = []
  pickerEpisodes: EpisodeSearchResult[] = []
  pickerLoading: boolean   = false

  // Filter rules
  filterRulesOpen: boolean = false
  filterMatch: 'all' | 'any' = 'all'
  filterRules: FilterRule[] = []
  allLibraries: LibraryWithSource[] = []

  // Shows tab
  pickerShows:        Show[]        = []
  pickerShowsLoading: boolean       = false
  expandedShowId:     string | null = null
  expandedSeasons:    {number: number; name: string}[] = []
  seasonsLoading:     boolean       = false
  importing:          boolean       = false
  importLabel:        string        = ''

  // Plex browse
  plexSources:         Source[]         = []
  plexSelectedSource:  string           = ''
  plexLists:           PlexBrowseList[] = []
  plexBrowseLoading:   boolean          = false
  plexBrowseLibraryId: string           = ''
  plexImportingId:     string           = ''

  constructor() { makeAutoObservable(this) }

  async load() {
    this.loading = true
    try {
      const r = await api.getFillerLists()
      runInAction(() => { this.lists = r; this.loading = false })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.loading = false })
    }
  }

  async create() {
    if (!this.newTitle.trim()) return
    try {
      const { filler_list_id } = await api.createFillerList({ title: this.newTitle.trim(), advancement: 'shuffle' })
      await this.load()
      runInAction(() => { this.newTitle = ''; this.creating = false })
      this.expand(filler_list_id)
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    }
  }

  async expand(id: string) {
    if (this.expanded === id) {
      this.expanded = null; this.detail = null
      this.pickerOpen = false; this.pickerQuery = ''
      return
    }
    this.expanded      = id
    this.detailLoading = true
    try {
      const d = await api.getFillerList(id)
      runInAction(() => { this.detail = d; this.detailLoading = false })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.detailLoading = false })
    }
  }

  async deleteList(id: string) {
    await api.deleteFillerList(id)
    runInAction(() => {
      this.lists = this.lists.filter(f => f.filler_list_id !== id)
      if (this.expanded === id) { this.expanded = null; this.detail = null }
    })
  }

  async updateAdvancement(id: string, advancement: FillerAdvancement) {
    await api.updateFillerList(id, { advancement })
    runInAction(() => {
      this.lists = this.lists.map(f => f.filler_list_id === id ? { ...f, advancement } : f)
      if (this.detail?.filler_list_id === id) this.detail = { ...this.detail, advancement }
    })
  }

  async removeItem(listId: string, iid: number) {
    await api.removeFillerListItem(listId, iid)
    runInAction(() => {
      if (this.detail)
        this.detail = { ...this.detail, items: this.detail.items.filter(i => i.id !== iid) }
    })
    this.load()
  }

  openPicker() {
    clearTimeout(searchDebounce)
    this.pickerOpen = true; this.pickerTab = 'episodes'; this.pickerQuery = ''
    this.filterRules = []; this.filterRulesOpen = false; this.filterMatch = 'all'
    this.pickerMovies = []; this.pickerEpisodes = []; this.pickerShows = []
    this.expandedShowId = null; this.plexLists = []
    if (this.allLibraries.length === 0)
      api.getAllLibraries().then(libs => runInAction(() => { this.allLibraries = libs }))
    this.searchPicker()
  }

  closePicker() {
    clearTimeout(searchDebounce)
    this.pickerOpen = false; this.pickerQuery = ''
    this.filterRules = []; this.filterRulesOpen = false; this.filterMatch = 'all'
    this.pickerMovies = []; this.pickerEpisodes = []; this.pickerShows = []
    this.expandedShowId = null; this.plexLists = []
  }

  setPickerTab(t: PickerTab) {
    this.pickerTab = t; this.pickerQuery = ''; this.expandedShowId = null
    this.filterRules = []; this.filterRulesOpen = false; this.filterMatch = 'all'
    this.plexLists = []; this.plexBrowseLibraryId = ''
    this.searchPicker()
  }

  setPickerQuery(q: string) {
    this.pickerQuery = q
    clearTimeout(searchDebounce)
    searchDebounce = setTimeout(() => this.searchPicker(), 250)
  }

  addFilterRule() {
    this.filterRules.push({ id: String(++_ruleId), field: 'genre', op: 'is', value: '' })
    clearTimeout(searchDebounce)
    searchDebounce = setTimeout(() => this.searchPicker(), 250)
  }

  removeFilterRule(id: string) {
    this.filterRules = this.filterRules.filter(r => r.id !== id)
    clearTimeout(searchDebounce)
    searchDebounce = setTimeout(() => this.searchPicker(), 250)
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
    clearTimeout(searchDebounce)
    searchDebounce = setTimeout(() => this.searchPicker(), 250)
  }

  setFilterMatch(m: 'all' | 'any') {
    this.filterMatch = m
    clearTimeout(searchDebounce)
    searchDebounce = setTimeout(() => this.searchPicker(), 250)
  }

  async searchPicker() {
    const q = this.pickerQuery || undefined

    const isRules = this.filterRules.filter(r => r.op === 'is' && r.value.trim())
    const lib    = isRules.find(r => r.field === 'library')?.value        || undefined
    const genre  = isRules.find(r => r.field === 'genre')?.value          || undefined
    const yearStr = isRules.find(r => r.field === 'year')?.value
    const year   = yearStr ? parseInt(yearStr) : undefined
    const rating = isRules.find(r => r.field === 'content_rating')?.value || undefined

    if (this.pickerTab === 'movies') {
      this.pickerLoading = true
      try {
        const r = await api.getMovies({ limit: 80, q, library_id: lib, genre, year, content_rating: rating })
        runInAction(() => { this.pickerMovies = r.items; this.pickerLoading = false })
      } catch { runInAction(() => { this.pickerLoading = false }) }
    } else if (this.pickerTab === 'episodes') {
      this.pickerLoading = true
      try {
        const r = await api.searchEpisodes({ q, limit: 80 })
        runInAction(() => { this.pickerEpisodes = r.items; this.pickerLoading = false })
      } catch { runInAction(() => { this.pickerLoading = false }) }
    } else if (this.pickerTab === 'shows') {
      this.pickerShowsLoading = true
      try {
        const r = await api.getShows({ limit: 100, q, library_id: lib, genre, year, content_rating: rating })
        runInAction(() => { this.pickerShows = r.items; this.pickerShowsLoading = false })
      } catch { runInAction(() => { this.pickerShowsLoading = false }) }
    } else if (this.pickerTab === 'plex_playlists') {
      await this.loadPlexSources()
      if (this.plexSelectedSource) await this.loadPlexPlaylists()
    } else if (this.pickerTab === 'plex_collections') {
      await this.loadPlexSources()
    }
  }

  async loadPlexSources() {
    if (this.plexSources.length > 0) return
    try {
      const sources = await api.getSources()
      runInAction(() => {
        this.plexSources = sources.filter(s => s.source_type === 'plex' && s.enabled)
        if (this.plexSources.length === 1) this.plexSelectedSource = this.plexSources[0].source_id
      })
    } catch {}
  }

  async loadPlexPlaylists() {
    if (!this.plexSelectedSource) return
    this.plexBrowseLoading = true
    try {
      const lists = await api.browsePlexPlaylists(this.plexSelectedSource)
      runInAction(() => { this.plexLists = lists; this.plexBrowseLoading = false })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.plexBrowseLoading = false })
    }
  }

  async loadPlexCollections() {
    if (!this.plexSelectedSource || !this.plexBrowseLibraryId) return
    this.plexBrowseLoading = true
    try {
      const lists = await api.browsePlexCollections(this.plexSelectedSource, this.plexBrowseLibraryId)
      runInAction(() => { this.plexLists = lists; this.plexBrowseLoading = false })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.plexBrowseLoading = false })
    }
  }

  setPlexSource(id: string) {
    this.plexSelectedSource = id; this.plexLists = []; this.plexBrowseLibraryId = ''
    if (this.pickerTab === 'plex_playlists') this.loadPlexPlaylists()
  }

  setPlexLibrary(id: string) {
    this.plexBrowseLibraryId = id; this.plexLists = []
    this.loadPlexCollections()
  }

  async expandShow(showId: string) {
    if (this.expandedShowId === showId) { this.expandedShowId = null; return }
    this.expandedShowId = showId; this.seasonsLoading = true; this.expandedSeasons = []
    try {
      const { seasons } = await api.getShowSeasons(showId)
      runInAction(() => { this.expandedSeasons = seasons; this.seasonsLoading = false })
    } catch { runInAction(() => { this.seasonsLoading = false }) }
  }

  async importShowEpisodes(listId: string, showId: string, season?: number) {
    this.importing = true
    this.importLabel = season != null ? `Importing S${String(season).padStart(2,'0')}…` : 'Importing all episodes…'
    try {
      const episodes = await api.getEpisodes(showId, season)
      const items = episodes.map(ep => ({ item_type: 'episode' as const, item_id: ep.episode_id }))
      await api.bulkAddFillerListItems(listId, items)
      const d = await api.getFillerList(listId)
      runInAction(() => { this.detail = d; this.importing = false; this.importLabel = '' })
      this.load()
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.importing = false; this.importLabel = '' })
    }
  }

  async importPlexItems(listId: string, plexItems: PlexBrowseItem[]) {
    this.importing = true; this.importLabel = 'Importing…'
    try {
      const items = plexItems.filter(i => i.available).map(i => ({ item_type: i.item_type, item_id: i.kairos_id }))
      await api.bulkAddFillerListItems(listId, items)
      const d = await api.getFillerList(listId)
      runInAction(() => { this.detail = d; this.importing = false; this.importLabel = '' })
      this.load()
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.importing = false; this.importLabel = '' })
    }
  }

  async importPlexList(listId: string, plexListId: string, kind: 'playlist' | 'collection') {
    if (!this.plexSelectedSource) return
    this.plexImportingId = plexListId; this.importLabel = 'Syncing from Plex…'
    try {
      await api.plexSyncFillerList(listId, {
        source_id: this.plexSelectedSource, external_id: plexListId, plex_type: kind,
      })
      const [d] = await Promise.all([api.getFillerList(listId), this.load()])
      runInAction(() => { this.detail = d; this.importing = false; this.importLabel = '' })
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    } finally {
      runInAction(() => { this.plexImportingId = '' })
    }
  }

  async resyncList(list: FillerList) {
    if (!list.plex_link) return
    this.importing = true; this.importLabel = 'Syncing…'
    try {
      await api.plexSyncFillerList(list.filler_list_id, {
        source_id: list.plex_link.source_id,
        external_id: list.plex_link.external_id,
        plex_type: list.plex_link.plex_type,
      })
      const [d] = await Promise.all([api.getFillerList(list.filler_list_id), this.load()])
      runInAction(() => { if (this.expanded === list.filler_list_id) this.detail = d })
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    } finally {
      runInAction(() => { this.importing = false; this.importLabel = '' })
    }
  }

  async unlinkList(id: string) {
    try {
      await api.unlinkFillerList(id)
      await this.load()
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    }
  }

  async syncAllPlexLinks() {
    try {
      await api.plexSyncAllFillerLists()
      setTimeout(() => this.load(), 2500)
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    }
  }

  async addItem(listId: string, item_type: 'episode' | 'movie', item_id: string) {
    await api.addFillerListItem(listId, { item_type, item_id })
    const d = await api.getFillerList(listId)
    runInAction(() => { this.detail = d })
    this.load()
  }
}

const store = new FillerPageStore()

// ─── Page ─────────────────────────────────────────────────────────────────────

function fmtSyncAge(ts: number | null): string {
  if (!ts) return 'never synced'
  const s = Math.floor(Date.now() / 1000) - ts
  if (s < 60)    return 'synced just now'
  if (s < 3600)  return `synced ${Math.floor(s / 60)}m ago`
  if (s < 86400) return `synced ${Math.floor(s / 3600)}h ago`
  return `synced ${Math.floor(s / 86400)}d ago`
}

export default observer(function FillerPage() {
  useEffect(() => { store.load() }, [])

  const hasPlexLinks = store.lists.some(l => l.plex_link)

  return (
    <div className="space-y-5">
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-xl font-semibold text-zinc-100">Filler Lists</h1>
          <p className="text-xs text-zinc-600 mt-0.5">Pools of short content used by filler blocks to pad gaps</p>
        </div>
        <div className="flex gap-2">
          {hasPlexLinks && (
            <button onClick={() => store.syncAllPlexLinks()}
              className="btn-ghost text-xs text-violet-400 border-violet-800 hover:bg-violet-950/40">
              ↺ Sync all Plex links
            </button>
          )}
          <button onClick={() => runInAction(() => { store.creating = !store.creating })}
            className="btn-primary">+ New Filler List</button>
        </div>
      </div>

      {store.creating && (
        <div className="card p-4 flex gap-3">
          <input className="input flex-1" placeholder="Filler list title…"
            value={store.newTitle}
            onChange={e => runInAction(() => { store.newTitle = e.target.value })}
            onKeyDown={e => e.key === 'Enter' && store.create()} autoFocus />
          <button onClick={() => store.create()} className="btn-primary">Create</button>
          <button onClick={() => runInAction(() => { store.creating = false })} className="btn-ghost">Cancel</button>
        </div>
      )}

      {store.error && (
        <div className="text-red-400 text-sm bg-red-950/30 border border-red-900/40 rounded-lg p-3">{store.error}</div>
      )}

      <div className="space-y-2">
        {store.lists.length === 0 && !store.loading && (
          <p className="text-zinc-600 text-sm">No filler lists yet.</p>
        )}
        {store.lists.map(fl => (
          <FillerListCard key={fl.filler_list_id} list={fl} />
        ))}
      </div>
    </div>
  )
})

const FillerListCard = observer(function FillerListCard({ list }: { list: FillerList }) {
  const isOpen = store.expanded === list.filler_list_id

  return (
    <div className="card overflow-hidden">
      <div className="flex items-center px-4 py-3 gap-3">
        <button onClick={() => store.expand(list.filler_list_id)}
          className="flex-1 flex items-center gap-3 text-left min-w-0">
          <span className="text-zinc-500 text-xs shrink-0">{isOpen ? '▼' : '▶'}</span>
          <div className="min-w-0">
            <div className="flex items-center gap-2 flex-wrap">
              <span className="font-medium text-sm text-zinc-100">{list.title}</span>
              {list.plex_link && (
                <span className="text-[10px] font-semibold px-1.5 py-0.5 rounded
                                  bg-violet-900/50 text-violet-300 border border-violet-700/40 shrink-0">
                  {list.plex_link.plex_type === 'collection' ? 'PLEX COLLECTION' : 'PLEX PLAYLIST'}
                </span>
              )}
            </div>
            <div className="text-[10px] text-zinc-600 mt-0.5">
              {list.item_count} items · {fmtDuration(list.total_ms)} total
              {list.plex_link && (
                <span className="ml-2 text-zinc-700">· {fmtSyncAge(list.plex_link.last_synced_at)}</span>
              )}
            </div>
          </div>
        </button>
        {list.plex_link && (
          <>
            <button
              onClick={() => store.resyncList(list)}
              disabled={store.importing}
              className="text-xs text-violet-400 hover:text-violet-200 transition-colors shrink-0 disabled:opacity-40">
              ↺ Sync
            </button>
            <button
              onClick={() => store.unlinkList(list.filler_list_id)}
              className="text-xs text-zinc-600 hover:text-zinc-400 transition-colors shrink-0">
              Unlink
            </button>
          </>
        )}
        <select
          value={list.advancement}
          onChange={e => store.updateAdvancement(list.filler_list_id, e.target.value as FillerAdvancement)}
          className="text-xs bg-zinc-800 border border-zinc-700 rounded px-2 py-1 text-zinc-400 focus:outline-none focus:border-violet-600 shrink-0"
        >
          <option value="shuffle">Shuffle</option>
          <option value="sequential">Sequential</option>
        </select>
        <button onClick={() => store.deleteList(list.filler_list_id)}
          className="btn-danger text-xs shrink-0">Delete</button>
      </div>

      {isOpen && (
        <div className="border-t border-zinc-800/60 px-4 py-3 space-y-3">
          {store.detailLoading ? (
            <p className="text-zinc-600 text-xs">Loading…</p>
          ) : (
            <>
              {store.importing && store.expanded === list.filler_list_id ? (
                <div className="text-xs text-violet-400 flex items-center gap-2">
                  <span className="w-1.5 h-1.5 rounded-full bg-violet-500 animate-pulse" />
                  {store.importLabel || 'Importing…'}
                </div>
              ) : (
                <button onClick={() => store.openPicker()}
                  className="text-xs text-violet-400 hover:text-violet-200 transition-colors">
                  + Add item
                </button>
              )}

              {store.pickerOpen && store.expanded === list.filler_list_id && (
                <FillerItemPicker listId={list.filler_list_id} />
              )}

              <div className="space-y-1">
                {(store.detail?.items ?? []).map(item => (
                  <FillerItemRow key={item.id} item={item} listId={list.filler_list_id} />
                ))}
                {(store.detail?.items.length ?? 0) === 0 && (
                  <p className="text-zinc-600 text-xs">No items yet. Add short clips, bumpers, or interstitials.</p>
                )}
              </div>
            </>
          )}
        </div>
      )}
    </div>
  )
})

function FillerItemRow({ item, listId }: { item: FillerListItem; listId: string }) {
  const icon = item.item_type === 'episode' ? '◈' : '▣'
  return (
    <div className="flex items-center gap-2 px-2.5 py-2 rounded-lg bg-zinc-800/50 border border-zinc-700/30">
      <span className="text-zinc-600 text-xs shrink-0">{icon}</span>
      <span className="text-sm text-zinc-300 flex-1 truncate">{item.title}</span>
      <span className="text-zinc-600 text-xs shrink-0 tabular-nums">{fmtDuration(item.duration_ms)}</span>
      <button onClick={() => store.removeItem(listId, item.id)}
        className="text-zinc-600 hover:text-red-400 transition-colors text-xs px-1 shrink-0">✕</button>
    </div>
  )
}

// ─── Filler item picker ───────────────────────────────────────────────────────

const TABS: { id: PickerTab; label: string }[] = [
  { id: 'episodes',         label: 'Episodes' },
  { id: 'movies',           label: 'Movies' },
  { id: 'shows',            label: 'Shows' },
  { id: 'plex_playlists',   label: 'Plex Playlists' },
  { id: 'plex_collections', label: 'Plex Collections' },
]

const FillerItemPicker = observer(function FillerItemPicker({ listId }: { listId: string }) {
  const showFilters  = store.pickerTab === 'movies' || store.pickerTab === 'shows'
  const libType      = store.pickerTab === 'movies' ? 'movie' : 'show'
  const filteredLibs = store.allLibraries.filter(l => l.library_type === libType || l.library_type === 'mixed')

  return (
    <div className="rounded-lg border border-violet-900/30 bg-zinc-950/60 overflow-hidden">
      {/* Tab row */}
      <div className="flex items-center gap-1 px-2.5 pt-2.5 pb-0 flex-wrap">
        {TABS.map(t => (
          <button key={t.id} onClick={() => store.setPickerTab(t.id)}
            className={`px-2.5 py-1 rounded-t text-xs ${
              store.pickerTab === t.id
                ? 'bg-violet-950/80 text-violet-300 border border-b-0 border-violet-800/40'
                : 'text-zinc-500 hover:text-zinc-300'
            }`}>{t.label}</button>
        ))}
      </div>

      {/* Search row */}
      {(store.pickerTab === 'episodes' || store.pickerTab === 'movies' || store.pickerTab === 'shows') && (
        <div className="flex items-center gap-2 px-2 py-1.5 border-t border-zinc-800/60">
          <input className="input flex-1 text-xs py-1 min-w-0" placeholder="Search…"
            value={store.pickerQuery} onChange={e => store.setPickerQuery(e.target.value)} autoFocus />
          <button onClick={() => store.closePicker()}
            className="text-zinc-600 hover:text-zinc-300 text-xs px-1 shrink-0">✕</button>
        </div>
      )}

      {/* Expandable filter section */}
      {showFilters && (
        <FilterSection
          rulesOpen={store.filterRulesOpen}
          filterMatch={store.filterMatch}
          filterRules={store.filterRules}
          filteredLibs={filteredLibs}
          onToggleOpen={() => runInAction(() => { store.filterRulesOpen = !store.filterRulesOpen })}
          onSetMatch={m => store.setFilterMatch(m)}
          onAddRule={() => store.addFilterRule()}
          onUpdateRule={(id, patch) => store.updateFilterRule(id, patch)}
          onRemoveRule={id => store.removeFilterRule(id)}
        />
      )}

      {/* Plex header row */}
      {(store.pickerTab === 'plex_playlists' || store.pickerTab === 'plex_collections') && (
        <div className="flex items-center gap-2 p-2 border-t border-zinc-800/60 flex-wrap">
          {store.plexSources.length > 1 && (
            <select className="input text-xs py-1 flex-1"
              value={store.plexSelectedSource} onChange={e => store.setPlexSource(e.target.value)}>
              <option value="">Select Plex source…</option>
              {store.plexSources.map(s => <option key={s.source_id} value={s.source_id}>{s.display_name}</option>)}
            </select>
          )}
          {store.pickerTab === 'plex_collections' && store.plexSelectedSource && (
            <select className="input text-xs py-1 flex-1"
              value={store.plexBrowseLibraryId} onChange={e => store.setPlexLibrary(e.target.value)}>
              <option value="">Select library…</option>
              {store.allLibraries.filter(l => l.source_id === store.plexSelectedSource).map(l =>
                <option key={l.library_id} value={l.library_id}>{l.display_name}</option>
              )}
            </select>
          )}
          <button onClick={() => store.closePicker()}
            className="text-zinc-600 hover:text-zinc-300 text-xs px-1 shrink-0 ml-auto">✕</button>
        </div>
      )}

      <div className="max-h-56 overflow-y-auto scrollbar-dark">
        {store.pickerTab === 'episodes' && <EpisodeList listId={listId} />}
        {store.pickerTab === 'movies' && <MovieList listId={listId} />}
        {store.pickerTab === 'shows' && <ShowList listId={listId} />}
        {(store.pickerTab === 'plex_playlists' || store.pickerTab === 'plex_collections') && (
          <PlexBrowsePane listId={listId} kind={store.pickerTab === 'plex_playlists' ? 'playlist' : 'collection'} />
        )}
      </div>
    </div>
  )
})

const EpisodeList = observer(function EpisodeList({ listId }: { listId: string }) {
  const q = store.pickerQuery.toLowerCase()
  const results = store.pickerEpisodes.filter(e => !q || e.title.toLowerCase().includes(q) || e.show_title.toLowerCase().includes(q))
  if (store.pickerLoading) return <Spinner />
  if (results.length === 0) return <Empty msg="No results. Type to search episodes." />
  return (
    <>
      {results.map(ep => (
        <button key={ep.episode_id} onClick={() => store.addItem(listId, 'episode', ep.episode_id)}
          className="w-full flex items-center justify-between gap-2 px-3 py-2 text-left hover:bg-violet-950/30 transition-colors">
          <div className="flex items-center gap-2 min-w-0">
            <span className="text-zinc-600 text-xs shrink-0">◈</span>
            <div className="min-w-0">
              <div className="text-xs text-zinc-500 truncate">{ep.show_title}</div>
              <div className="text-sm text-zinc-300 truncate">
                S{String(ep.season).padStart(2,'0')}E{String(ep.episode).padStart(2,'0')} — {ep.title}
              </div>
            </div>
          </div>
          <span className="text-zinc-600 text-xs shrink-0">{fmtDuration(ep.duration_ms)}</span>
        </button>
      ))}
    </>
  )
})

const MovieList = observer(function MovieList({ listId }: { listId: string }) {
  const q = store.pickerQuery.toLowerCase()
  const results = store.pickerMovies.filter(m => !q || m.title.toLowerCase().includes(q))
  if (store.pickerLoading) return <Spinner />
  if (results.length === 0) return <Empty />
  return (
    <>
      {results.map(m => (
        <button key={m.movie_id} onClick={() => store.addItem(listId, 'movie', m.movie_id)}
          className="w-full flex items-center justify-between gap-2 px-3 py-2 text-left text-sm text-zinc-300 hover:bg-violet-950/30 transition-colors">
          <div className="flex items-center gap-2 min-w-0">
            <span className="text-zinc-600 text-xs shrink-0">▣</span>
            <span className="truncate">{m.title}</span>
            {m.year && <span className="text-zinc-600 text-xs shrink-0">{m.year}</span>}
          </div>
          <span className="text-zinc-600 text-xs shrink-0">{fmtDuration(m.duration_ms)}</span>
        </button>
      ))}
    </>
  )
})

const ShowList = observer(function ShowList({ listId }: { listId: string }) {
  const q = store.pickerQuery.toLowerCase()
  const results = store.pickerShows.filter(s => !q || s.title.toLowerCase().includes(q))
  if (store.pickerShowsLoading) return <Spinner />
  if (results.length === 0) return <Empty />
  return (
    <>
      {results.map(show => {
        const expanded = store.expandedShowId === show.show_id
        return (
          <div key={show.show_id}>
            <div className="flex items-center gap-2 px-3 py-2 cursor-pointer hover:bg-violet-950/20 transition-colors border-b border-zinc-800/40"
              onClick={() => store.expandShow(show.show_id)}>
              <span className="text-zinc-600 text-xs shrink-0">{expanded ? '▼' : '▶'}</span>
              <span className="text-sm text-zinc-300 flex-1 truncate">{show.title}</span>
              <span className="text-zinc-600 text-[10px] shrink-0">{show.episode_count} ep</span>
            </div>
            {expanded && (
              <div className="flex flex-wrap gap-1.5 px-4 py-2 bg-zinc-900/50 border-b border-zinc-800/40">
                {store.seasonsLoading ? (
                  <span className="text-zinc-600 text-xs">Loading seasons…</span>
                ) : (
                  <>
                    <SeasonBtn label="All episodes" onClick={() => store.importShowEpisodes(listId, show.show_id)} />
                    {store.expandedSeasons.map(s => (
                      <SeasonBtn key={s.number} label={s.name || `S${String(s.number).padStart(2,'0')}`}
                        onClick={() => store.importShowEpisodes(listId, show.show_id, s.number)} />
                    ))}
                  </>
                )}
              </div>
            )}
          </div>
        )
      })}
    </>
  )
})

const PlexBrowsePane = observer(function PlexBrowsePane({ listId, kind }: { listId: string; kind: 'playlist' | 'collection' }) {
  if (!store.plexSelectedSource || (kind === 'collection' && !store.plexBrowseLibraryId)) {
    return <Empty msg={store.plexSources.length === 0 ? 'No Plex sources configured.' : kind === 'collection' ? 'Select a library above.' : 'Select a source above.'} />
  }
  if (store.plexBrowseLoading) return <Spinner />
  if (store.plexLists.length === 0) return <Empty msg={`No ${kind}s found.`} />
  return (
    <>
      {store.plexLists.map(list => (
        <div key={list.id} className="flex items-center gap-2 px-3 py-2 border-b border-zinc-800/40">
          <div className="flex-1 min-w-0">
            <div className="text-sm text-zinc-300 truncate">{list.title}</div>
            <div className="text-[10px] text-zinc-600">{list.item_count} items</div>
          </div>
          {store.plexImportingId === list.id ? (
            <span className="text-violet-400 text-xs flex items-center gap-1">
              <span className="w-1.5 h-1.5 rounded-full bg-violet-500 animate-pulse" />importing
            </span>
          ) : (
            <button onClick={() => store.importPlexList(listId, list.id, kind)}
              className="text-xs text-violet-400 hover:text-violet-200 shrink-0 px-1">Import</button>
          )}
        </div>
      ))}
    </>
  )
})

// ─── Micro components ─────────────────────────────────────────────────────────

function SeasonBtn({ label, onClick }: { label: string; onClick: () => void }) {
  return (
    <button onClick={onClick}
      className="px-2 py-0.5 rounded border border-zinc-700 text-zinc-400 text-xs hover:border-violet-600 hover:text-violet-300 transition-colors">
      {label}
    </button>
  )
}

function Spinner() {
  return (
    <div className="flex items-center gap-2 text-zinc-600 text-xs p-3">
      <span className="w-1.5 h-1.5 rounded-full bg-violet-500 animate-pulse" />Loading…
    </div>
  )
}

function Empty({ msg = 'No results.' }: { msg?: string }) {
  return <p className="text-zinc-600 text-xs p-3">{msg}</p>
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

function fmtDuration(ms: number): string {
  if (!ms) return '—'
  const h = Math.floor(ms / 3600000)
  const m = Math.floor((ms % 3600000) / 60000)
  return h > 0 ? `${h}h ${m}m` : `${m}m`
}
