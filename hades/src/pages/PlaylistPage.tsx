import { observer } from 'mobx-react-lite'
import { makeAutoObservable, reaction, runInAction } from 'mobx'
import { useEffect, useRef, useState } from 'react'
import { api } from '../api/client'
import type {
  EpisodeSearchResult, LibraryWithSource, Movie, Playlist, PlexBrowseItem, PlexBrowseList,
  PlaylistDetail, PlaylistExport, PlaylistImportPreviewResult, PlaylistImportResult,
  PlaylistItem, PlaylistMembership, PlaylistMode, Show, SmartPlaylistType, Source,
} from '../api/types'
import { FilterSection } from '../components/PickerFilters'
import { FilterTreeStore } from '../components/media/filterTree'
import { toFilterString } from '../components/media/filterQuery'

function triggerJsonDownload(data: object, filename: string) {
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' })
  const url  = URL.createObjectURL(blob)
  const a    = document.createElement('a')
  a.href = url; a.download = filename; a.click()
  URL.revokeObjectURL(url)
}

// ─── Store ────────────────────────────────────────────────────────────────────

let searchDebounce: ReturnType<typeof setTimeout>

type PickerTab = 'episodes' | 'movies' | 'shows' | 'source_playlists' | 'source_collections'

class PlaylistPageStore {
  playlists:    Playlist[]      = []
  expanded:     string | null   = null
  detail:       PlaylistDetail | null = null
  detailLoading: boolean        = false
  loading:      boolean         = false
  error:        string | null   = null
  creating:     boolean         = false
  newTitle:     string          = ''

  pickerOpen:    boolean   = false
  pickerTab:     PickerTab = 'episodes'
  pickerQuery:   string    = ''
  pickerMovies:  Movie[]   = []
  pickerEpisodes: EpisodeSearchResult[] = []
  pickerLoading: boolean   = false

  // Filter rules (item picker's own search filter — see FilterRuleRow below
  // for the distinct smartFilterTree used by a smart playlist's definition)
  filterTree: FilterTreeStore = new FilterTreeStore()
  allLibraries: LibraryWithSource[] = []

  // Smart-playlist definition editor — separate FilterTreeStore from the
  // item picker's above (opening the picker while editing a smart def, or
  // vice versa, must not clobber the other's rules). Tied to whichever
  // playlist is currently expanded, same convention as pickerTab/pickerQuery.
  smartFilterTree: FilterTreeStore = new FilterTreeStore()
  smartType:    SmartPlaylistType = 'movie'
  smartSort:    string = 'title'
  smartLimit:   string = '' // '' = unlimited
  smartSaving:  boolean = false

  // Home-shelf fields — only meaningful (and only shown in the UI) once
  // showOnHome is on; homeActiveStart/End are 'MM-DD' or '' (both empty =
  // always shown).
  showOnHome:       boolean = false
  homeTileLimit:    string  = '16'
  homeActiveStart:  string  = ''
  homeActiveEnd:    string  = ''

  // Shows tab
  pickerShows:          Show[]       = []
  pickerShowsLoading:   boolean      = false
  expandedShowId:       string | null = null
  expandedSeasons:      {number: number; name: string}[] = []
  seasonsLoading:       boolean      = false
  importing:            boolean      = false
  importLabel:          string       = ''

  // Remote browse (Plex/Jellyfin/Emby — any source with a playlist/collection API)
  browseSources:        Source[]         = []
  selectedSource:       string           = ''
  browseLists:          PlexBrowseList[] = []
  browseLoading:        boolean          = false
  browseLibraryId:      string           = ''
  importingListId:      string           = ''

  constructor() {
    makeAutoObservable(this)
    // See LibraryStore's identical reaction — any change to the picker's
    // rule-builder tree re-searches, replacing the old per-mutator
    // debounce-and-searchPicker() calls.
    reaction(() => toFilterString(this.filterTree), () => {
      clearTimeout(searchDebounce)
      searchDebounce = setTimeout(() => this.searchPicker(), 250)
    })
  }

  async load() {
    this.loading = true
    try {
      const r = await api.getPlaylists()
      runInAction(() => { this.playlists = r; this.loading = false })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.loading = false })
    }
  }

  async create() {
    if (!this.newTitle.trim()) return
    try {
      const { playlist_id } = await api.createPlaylist({ title: this.newTitle.trim() })
      await this.load()
      runInAction(() => { this.newTitle = ''; this.creating = false })
      this.expand(playlist_id)
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
    this.expanded     = id
    this.detailLoading = true
    try {
      const d = await api.getPlaylist(id)
      runInAction(() => { this.detail = d; this.detailLoading = false; this.loadSmartEditor(d) })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.detailLoading = false })
    }
  }

  // Seeds the smart-definition editor from a playlist's stored fields —
  // called whenever a playlist expands, so the rule builder round-trips the
  // same filter_expr string it would serialize back to (setFromFilterString
  // is a no-op on an empty string, leaving a fresh/empty tree for a playlist
  // that hasn't been given a filter yet).
  loadSmartEditor(p: Pick<Playlist,
    'smart_type' | 'smart_sort' | 'smart_limit' | 'filter_expr'
    | 'show_on_home' | 'home_tile_limit' | 'home_active_start' | 'home_active_end'>
  ) {
    this.smartType  = p.smart_type
    this.smartSort  = p.smart_sort
    this.smartLimit = p.smart_limit > 0 ? String(p.smart_limit) : ''
    this.smartFilterTree.reset()
    this.smartFilterTree.setFromFilterString(p.filter_expr)
    this.showOnHome      = p.show_on_home
    this.homeTileLimit   = String(p.home_tile_limit)
    this.homeActiveStart = p.home_active_start
    this.homeActiveEnd   = p.home_active_end
  }

  async setMembership(id: string, membership: PlaylistMembership) {
    try {
      await api.updatePlaylist(id, { membership })
      runInAction(() => {
        this.playlists = this.playlists.map(p => p.playlist_id === id ? { ...p, membership } : p)
        if (this.detail?.playlist_id === id) this.detail = { ...this.detail, membership }
      })
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    }
  }

  // Instant, like setMembership — a visibility toggle shouldn't need "Save."
  // The backend rejects show_on_home=true for a non-smart playlist, so this
  // is only ever called while membership==='smart' (see the JSX gating below).
  async setShowOnHome(id: string, show_on_home: boolean) {
    try {
      await api.updatePlaylist(id, { show_on_home })
      runInAction(() => {
        this.showOnHome = show_on_home
        this.playlists = this.playlists.map(p => p.playlist_id === id ? { ...p, show_on_home } : p)
        if (this.detail?.playlist_id === id) this.detail = { ...this.detail, show_on_home }
      })
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    }
  }

  // Persists the smart-editor's current type/sort/limit/filter plus (if
  // showOnHome) the home-shelf display settings, then immediately
  // recomputes membership (PlaylistRepository::refreshSmart) — a "Save"
  // that doesn't also refresh would leave the playlist's actual items stale
  // until the next full sync cycle picks it up.
  async saveSmartDef(playlistId: string) {
    this.smartSaving = true
    try {
      const filter_expr = toFilterString(this.smartFilterTree)
      await api.updatePlaylist(playlistId, {
        smart_type:  this.smartType,
        smart_sort:  this.smartSort,
        smart_limit: this.smartLimit.trim() ? parseInt(this.smartLimit, 10) : 0,
        filter_expr,
        ...(this.showOnHome ? {
          home_tile_limit:   this.homeTileLimit.trim() ? parseInt(this.homeTileLimit, 10) : 16,
          home_active_start: this.homeActiveStart,
          home_active_end:   this.homeActiveEnd,
        } : {}),
      })
      await api.refreshSmartPlaylist(playlistId)
      const [d] = await Promise.all([api.getPlaylist(playlistId), this.load()])
      runInAction(() => { this.detail = d; this.smartSaving = false })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.smartSaving = false })
    }
  }

  async deletePlaylist(id: string) {
    await api.deletePlaylist(id)
    runInAction(() => {
      this.playlists = this.playlists.filter(p => p.playlist_id !== id)
      if (this.expanded === id) { this.expanded = null; this.detail = null }
    })
  }

  async removeItem(playlistId: string, iid: number) {
    await api.removePlaylistItem(playlistId, iid)
    runInAction(() => {
      if (this.detail)
        this.detail = { ...this.detail, items: this.detail.items.filter(i => i.id !== iid) }
    })
    this.load()
  }

  openPicker() {
    clearTimeout(searchDebounce)
    this.pickerOpen = true; this.pickerTab = 'episodes'; this.pickerQuery = ''
    this.filterTree.reset()
    this.pickerMovies = []; this.pickerEpisodes = []; this.pickerShows = []
    this.expandedShowId = null; this.browseLists = []
    if (this.allLibraries.length === 0)
      api.getAllLibraries().then(libs => runInAction(() => { this.allLibraries = libs }))
    this.searchPicker()
  }

  closePicker() {
    clearTimeout(searchDebounce)
    this.pickerOpen = false; this.pickerQuery = ''
    this.filterTree.reset()
    this.pickerMovies = []; this.pickerEpisodes = []; this.pickerShows = []
    this.expandedShowId = null; this.browseLists = []
  }

  setPickerTab(t: PickerTab) {
    this.pickerTab = t; this.pickerQuery = ''; this.expandedShowId = null
    this.filterTree.reset()
    this.browseLists = []; this.browseLibraryId = ''
    this.searchPicker()
  }

  setPickerQuery(q: string) {
    this.pickerQuery = q
    clearTimeout(searchDebounce)
    searchDebounce = setTimeout(() => this.searchPicker(), 250)
  }

  async searchPicker() {
    const q = this.pickerQuery || undefined

    const lib    = this.filterTree.allRules.find(r => r.field === 'library' && r.op === 'is' && r.value.trim())?.value || undefined
    const filter = toFilterString(this.filterTree) || undefined

    if (this.pickerTab === 'movies') {
      this.pickerLoading = true
      try {
        const r = await api.getMovies({ limit: 80, q, library_id: lib, filter })
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
        const r = await api.getShows({ limit: 100, q, library_id: lib, filter })
        runInAction(() => { this.pickerShows = r.items; this.pickerShowsLoading = false })
      } catch { runInAction(() => { this.pickerShowsLoading = false }) }
    } else if (this.pickerTab === 'source_playlists') {
      await this.loadBrowseSources()
      if (this.selectedSource) await this.loadBrowsePlaylists()
    } else if (this.pickerTab === 'source_collections') {
      await this.loadBrowseSources()
    }
  }

  async loadBrowseSources() {
    if (this.browseSources.length > 0) return
    try {
      const sources = await api.getSources()
      runInAction(() => {
        // Any source with a remote playlist/collection API — Plex, Jellyfin,
        // and Emby all implement it; Local has no remote server to browse.
        this.browseSources = sources.filter(s => s.source_type !== 'local' && s.enabled)
        if (this.browseSources.length === 1) this.selectedSource = this.browseSources[0].source_id
      })
    } catch {}
  }

  async loadBrowsePlaylists() {
    if (!this.selectedSource) return
    this.browseLoading = true
    try {
      const lists = await api.browsePlexPlaylists(this.selectedSource)
      runInAction(() => { this.browseLists = lists; this.browseLoading = false })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.browseLoading = false })
    }
  }

  async loadBrowseCollections() {
    if (!this.selectedSource || !this.browseLibraryId) return
    this.browseLoading = true
    try {
      const lists = await api.browsePlexCollections(this.selectedSource, this.browseLibraryId)
      runInAction(() => { this.browseLists = lists; this.browseLoading = false })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.browseLoading = false })
    }
  }

  setBrowseSource(id: string) {
    this.selectedSource = id; this.browseLists = []; this.browseLibraryId = ''
    if (this.pickerTab === 'source_playlists') this.loadBrowsePlaylists()
  }

  setBrowseLibrary(id: string) {
    this.browseLibraryId = id; this.browseLists = []
    this.loadBrowseCollections()
  }

  async expandShow(showId: string) {
    if (this.expandedShowId === showId) { this.expandedShowId = null; return }
    this.expandedShowId = showId; this.seasonsLoading = true; this.expandedSeasons = []
    try {
      const { seasons } = await api.getShowSeasons(showId)
      runInAction(() => { this.expandedSeasons = seasons; this.seasonsLoading = false })
    } catch { runInAction(() => { this.seasonsLoading = false }) }
  }

  async importShowEpisodes(playlistId: string, showId: string, season?: number) {
    this.importing = true
    this.importLabel = season != null ? `Importing S${String(season).padStart(2,'0')}…` : 'Importing all episodes…'
    try {
      const episodes = await api.getEpisodes(showId, season)
      const items = episodes.map(ep => ({ item_type: 'episode' as const, item_id: ep.episode_id }))
      await api.bulkAddPlaylistItems(playlistId, items)
      const d = await api.getPlaylist(playlistId)
      runInAction(() => { this.detail = d; this.importing = false; this.importLabel = '' })
      this.load()
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.importing = false; this.importLabel = '' })
    }
  }

  async importSourceItems(playlistId: string, browseItems: PlexBrowseItem[]) {
    this.importing = true; this.importLabel = 'Importing…'
    try {
      const items = browseItems.filter(i => i.available).map(i => ({ item_type: i.item_type, item_id: i.kairos_id }))
      await api.bulkAddPlaylistItems(playlistId, items)
      const d = await api.getPlaylist(playlistId)
      runInAction(() => { this.detail = d; this.importing = false; this.importLabel = '' })
      this.load()
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.importing = false; this.importLabel = '' })
    }
  }

  async importSourceList(playlistId: string, listId: string, kind: 'playlist' | 'collection') {
    if (!this.selectedSource) return
    this.importingListId = listId; this.importLabel = 'Syncing from source…'
    try {
      await api.sourceSyncPlaylist(playlistId, {
        source_id: this.selectedSource, external_id: listId, list_kind: kind,
      })
      const [d] = await Promise.all([api.getPlaylist(playlistId), this.load()])
      runInAction(() => { this.detail = d; this.importing = false; this.importLabel = '' })
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    } finally {
      runInAction(() => { this.importingListId = '' })
    }
  }

  async resyncPlaylist(playlist: Playlist) {
    if (!playlist.plex_link) return
    this.importing = true; this.importLabel = 'Syncing…'
    try {
      await api.sourceSyncPlaylist(playlist.playlist_id, {
        source_id: playlist.plex_link.source_id,
        external_id: playlist.plex_link.external_id,
        list_kind: playlist.plex_link.plex_type,
      })
      const [d] = await Promise.all([api.getPlaylist(playlist.playlist_id), this.load()])
      runInAction(() => { if (this.expanded === playlist.playlist_id) this.detail = d })
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    } finally {
      runInAction(() => { this.importing = false; this.importLabel = '' })
    }
  }

  async unlinkPlaylist(id: string) {
    try {
      await api.unlinkPlaylist(id)
      await this.load()
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    }
  }

  async setMode(id: string, mode: PlaylistMode) {
    try {
      await api.updatePlaylist(id, { mode })
      runInAction(() => {
        this.playlists = this.playlists.map(p => p.playlist_id === id ? { ...p, mode } : p)
      })
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    }
  }

  async syncAllLinkedPlaylists() {
    try {
      await api.syncAllLinkedPlaylists()
      setTimeout(() => this.load(), 2500)
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    }
  }

  async addItem(playlistId: string, item_type: 'episode' | 'movie', item_id: string) {
    await api.addPlaylistItem(playlistId, { item_type, item_id })
    const d = await api.getPlaylist(playlistId)
    runInAction(() => { this.detail = d })
    this.load()
  }

  // ── Export/import — portable JSON, same pattern as channel export/import ──

  importError:   string | null = null
  importResult:  PlaylistImportResult | null = null
  importPending: { data: PlaylistExport; title: string } | null = null
  importPreview: PlaylistImportPreviewResult | null = null
  importingFile: boolean = false

  async exportPlaylist(playlist: Playlist) {
    try {
      // Always deep — playlists are episode-heavy, and shallow export can't
      // round-trip episodes at all (BlockSerializer::resolveSlot's episode
      // branch only resolves with deep IDs; see project_kairos memory).
      const data = await api.exportPlaylist(playlist.playlist_id, true)
      triggerJsonDownload(data, `${playlist.title.replace(/[^a-z0-9]/gi, '_')}.json`)
    } catch (e: any) {
      runInAction(() => { this.error = `Export failed: ${e.message}` })
    }
  }

  async handleImportFile(file: File) {
    runInAction(() => {
      this.importError = null; this.importResult = null
      this.importPending = null; this.importPreview = null
    })
    try {
      const text = await file.text()
      const data = JSON.parse(text) as PlaylistExport
      if (!data.kairos_export || !data.playlist) throw new Error('Not a valid Kairos playlist export.')
      runInAction(() => { this.importPending = { data, title: data.playlist.title } })
      api.previewPlaylistImport(data).then(p => runInAction(() => { this.importPreview = p })).catch(() => {})
    } catch (err: any) {
      runInAction(() => { this.importError = err.message ?? 'Could not parse file' })
    }
  }

  async confirmImport() {
    if (!this.importPending) return
    this.importingFile = true; this.importError = null
    try {
      const payload: PlaylistExport = {
        ...this.importPending.data,
        playlist: { ...this.importPending.data.playlist, title: this.importPending.title },
      }
      const result = await api.importPlaylist(payload)
      runInAction(() => {
        this.importResult = result
        this.importPending = null; this.importPreview = null
        this.importingFile = false
      })
      this.load()
    } catch (err: any) {
      runInAction(() => { this.importError = err.message ?? 'Import failed'; this.importingFile = false })
    }
  }

  cancelImport() {
    this.importPending = null; this.importPreview = null; this.importError = null
  }
}

const store = new PlaylistPageStore()

// ─── Helpers ──────────────────────────────────────────────────────────────────

// A playlist's plex_link only stores source_id — resolving it to the source's
// actual type (Plex/Jellyfin/Emby) needs a lookup against allLibraries (any
// library row from that source carries source_type/source_name). Previously
// the badge just hardcoded "PLEX ..." regardless of what the link actually
// pointed at, so a Jellyfin-linked playlist displayed a wrong Plex label.
function sourceBadgeLabel(sourceId: string, libs: LibraryWithSource[]): string {
  const lib = libs.find(l => l.source_id === sourceId)
  return lib ? lib.source_type.toUpperCase() : 'SOURCE'
}

function fmtSyncAge(ts: number | null): string {
  if (!ts) return 'never synced'
  const s = Math.floor(Date.now() / 1000) - ts
  if (s < 60)    return 'synced just now'
  if (s < 3600)  return `synced ${Math.floor(s / 60)}m ago`
  if (s < 86400) return `synced ${Math.floor(s / 3600)}h ago`
  return `synced ${Math.floor(s / 86400)}d ago`
}

// ─── Page ─────────────────────────────────────────────────────────────────────

export default observer(function PlaylistPage() {
  useEffect(() => {
    store.load()
    // Loaded eagerly (not lazily on first picker-open) so PlaylistCard's
    // source badge can resolve a linked source's real type/name on first
    // render, not just once someone happens to open the item picker.
    if (store.allLibraries.length === 0)
      api.getAllLibraries().then(libs => runInAction(() => { store.allLibraries = libs }))
  }, [])

  const hasLinks = store.playlists.some(p => p.plex_link)
  const fileRef = useRef<HTMLInputElement>(null)

  return (
    <div className="space-y-5">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold text-zinc-100">Playlists</h1>
        <div className="flex gap-2">
          {hasLinks && (
            <button onClick={() => store.syncAllLinkedPlaylists()}
              className="btn-ghost text-xs text-violet-400 border-violet-800 hover:bg-violet-950/40">
              ↺ Sync all linked lists
            </button>
          )}
          <input ref={fileRef} type="file" accept=".json" className="hidden"
            onChange={e => { const f = e.target.files?.[0]; e.target.value = ''; if (f) store.handleImportFile(f) }} />
          <button onClick={() => fileRef.current?.click()} className="btn-secondary">
            Import Playlist
          </button>
          <button onClick={() => runInAction(() => { store.creating = !store.creating })}
            className="btn-primary">+ New Playlist</button>
        </div>
      </div>

      {store.creating && (
        <div className="card p-4 flex gap-3">
          <input className="input flex-1" placeholder="Playlist title…"
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

      {store.importError && (
        <div className="text-red-400 text-sm bg-red-950/30 border border-red-900/40 rounded-lg p-3 flex items-center justify-between">
          <span>{store.importError}</span>
          <button onClick={() => runInAction(() => { store.importError = null })} className="text-xs text-zinc-500 hover:text-zinc-300">Dismiss</button>
        </div>
      )}

      {store.importResult && (
        <div className="text-sm bg-emerald-950/30 border border-emerald-900/40 rounded-lg p-3 space-y-1">
          <div className="text-emerald-400 font-medium">Playlist imported successfully.</div>
          {store.importResult.unresolved.length > 0 && (
            <div className="text-zinc-400 text-xs space-y-0.5 mt-1">
              <div className="text-zinc-500 font-semibold mb-1">
                {store.importResult.unresolved.length} item{store.importResult.unresolved.length !== 1 ? 's' : ''} could not be resolved:
              </div>
              {store.importResult.unresolved.map((u, i) => (
                <div key={i} className="font-mono">{u.content_type}: {u.title}</div>
              ))}
            </div>
          )}
          <button onClick={() => runInAction(() => { store.importResult = null })} className="text-xs text-zinc-600 hover:text-zinc-400 mt-1">
            Dismiss
          </button>
        </div>
      )}

      {store.importPending && (
        <ImportPreviewPanel
          data={store.importPending.data}
          title={store.importPending.title}
          preview={store.importPreview}
          importing={store.importingFile}
          onTitleChange={t => runInAction(() => { if (store.importPending) store.importPending.title = t })}
          onConfirm={() => store.confirmImport()}
          onCancel={() => store.cancelImport()}
        />
      )}

      <div className="space-y-2">
        {store.playlists.length === 0 && !store.loading && (
          <p className="text-zinc-600 text-sm">No playlists yet.</p>
        )}
        {store.playlists.map(pl => (
          <PlaylistCard key={pl.playlist_id} playlist={pl} />
        ))}
      </div>
    </div>
  )
})

const ImportPreviewPanel = observer(function ImportPreviewPanel({
  data, title, preview, importing, onTitleChange, onConfirm, onCancel,
}: {
  data:      PlaylistExport
  title:     string
  preview:   PlaylistImportPreviewResult | null
  importing: boolean
  onTitleChange: (t: string) => void
  onConfirm: () => void
  onCancel:  () => void
}) {
  return (
    <div className="card p-5 space-y-4">
      <div className="flex items-center justify-between">
        <h2 className="section-label">Import Preview</h2>
        <div className="flex items-center gap-3">
          {preview && preview.unresolved_count > 0 && (
            <span className="text-[10px] font-mono text-amber-500">{preview.unresolved_count} unresolved</span>
          )}
          {preview && preview.unresolved_count === 0 && (
            <span className="text-[10px] font-mono text-emerald-500">all resolved</span>
          )}
          {!preview && <span className="text-[10px] font-mono text-zinc-600">resolving…</span>}
          <span className="text-[10px] font-mono text-zinc-600 uppercase">
            {data.depth} export · {data.items.length} item{data.items.length !== 1 ? 's' : ''}
          </span>
        </div>
      </div>

      <div className="space-y-1">
        <div className="text-[10px] text-zinc-500 uppercase tracking-widest">Playlist Title</div>
        <input value={title} onChange={e => onTitleChange(e.target.value)} className="input w-full" />
      </div>

      {preview && (
        <div className="max-h-64 overflow-y-auto scrollbar-dark space-y-1 rounded-lg border border-zinc-800/60 p-2">
          {preview.items.map((item, i) => (
            <div key={i} className={`flex items-center gap-2 px-2 py-1.5 rounded text-xs ${item.resolved ? 'bg-zinc-900/40' : 'bg-red-950/20'}`}>
              <span className={item.resolved ? 'text-emerald-500' : 'text-red-500'}>{item.resolved ? '✓' : '✕'}</span>
              <span className="text-zinc-600">{item.content_type}</span>
              <span className="text-zinc-300 truncate flex-1">{item.title}</span>
            </div>
          ))}
        </div>
      )}

      <div className="flex gap-2">
        <button onClick={onConfirm} disabled={importing} className="btn-primary disabled:opacity-40">
          {importing ? 'Importing…' : 'Confirm Import'}
        </button>
        <button onClick={onCancel} className="btn-ghost">Cancel</button>
      </div>
    </div>
  )
})

const PlaylistCard = observer(function PlaylistCard({ playlist }: { playlist: Playlist }) {
  const isOpen = store.expanded === playlist.playlist_id

  return (
    <div className="card overflow-hidden">
      <div className="flex items-center px-4 py-3 gap-3">
        <button onClick={() => store.expand(playlist.playlist_id)}
          className="flex-1 flex items-center gap-3 text-left min-w-0">
          <span className="text-zinc-500 text-xs shrink-0">{isOpen ? '▼' : '▶'}</span>
          <div className="min-w-0">
            <div className="flex items-center gap-2 flex-wrap">
              <span className="font-medium text-sm text-zinc-100">{playlist.title}</span>
              {playlist.plex_link && (
                <span className="text-[10px] font-semibold px-1.5 py-0.5 rounded
                                  bg-violet-900/50 text-violet-300 border border-violet-700/40 shrink-0">
                  {sourceBadgeLabel(playlist.plex_link.source_id, store.allLibraries)}{' '}
                  {playlist.plex_link.plex_type === 'collection' ? 'COLLECTION' : 'PLAYLIST'}
                </span>
              )}
            </div>
            <div className="text-[10px] text-zinc-600 mt-0.5">
              {playlist.item_count} items · {fmtDuration(playlist.total_ms)}
              {playlist.plex_link && (
                <span className="ml-2 text-zinc-700">
                  · {fmtSyncAge(playlist.plex_link.last_synced_at)}
                </span>
              )}
            </div>
          </div>
        </button>
        {playlist.plex_link && (
          <>
            <button
              onClick={() => store.resyncPlaylist(playlist)}
              disabled={store.importing}
              className="text-xs text-violet-400 hover:text-violet-200 transition-colors shrink-0 disabled:opacity-40">
              ↺ Sync
            </button>
            <button
              onClick={() => store.unlinkPlaylist(playlist.playlist_id)}
              className="text-xs text-zinc-600 hover:text-zinc-400 transition-colors shrink-0">
              Unlink
            </button>
          </>
        )}
        <button onClick={() => store.exportPlaylist(playlist)}
          className="btn-secondary text-xs shrink-0">Export</button>
        <button onClick={() => store.deletePlaylist(playlist.playlist_id)}
          className="btn-danger text-xs shrink-0">Delete</button>
      </div>

      {isOpen && (
        <div className="border-t border-zinc-800/60 px-4 py-3 space-y-3">
          {store.detailLoading ? (
            <p className="text-zinc-600 text-xs">Loading…</p>
          ) : (
            <>
              {/* Mode toggle */}
              <div className="flex flex-col gap-1.5 pb-1 border-b border-zinc-800/50">
                <div className="text-[10px] font-semibold tracking-widest text-zinc-500 uppercase">Scheduling Mode</div>
                <div className="flex gap-2">
                  <button
                    onClick={() => store.setMode(playlist.playlist_id, 'sequential')}
                    className={`px-3 py-1 rounded text-xs font-semibold transition-colors ${playlist.mode === 'sequential' ? 'bg-violet-700 text-white' : 'bg-zinc-800 text-zinc-400 hover:text-zinc-200'}`}
                  >In-Order</button>
                  <button
                    onClick={() => store.setMode(playlist.playlist_id, 'shuffle')}
                    className={`px-3 py-1 rounded text-xs font-semibold transition-colors ${playlist.mode === 'shuffle' ? 'bg-violet-700 text-white' : 'bg-zinc-800 text-zinc-400 hover:text-zinc-200'}`}
                  >Shuffle</button>
                  <button
                    onClick={() => store.setMode(playlist.playlist_id, 'show_collection')}
                    className={`px-3 py-1 rounded text-xs font-semibold transition-colors ${playlist.mode === 'show_collection' ? 'bg-violet-700 text-white' : 'bg-zinc-800 text-zinc-400 hover:text-zinc-200'}`}
                  >Show Collection</button>
                </div>
                <div className="text-[10px] text-zinc-500 leading-relaxed">
                  {playlist.mode === 'shuffle'
                    ? 'Shuffle: items play in a randomized order, reshuffled each time the playlist starts over.'
                    : playlist.mode === 'show_collection'
                    ? 'Show Collection: the block\'s advancement mode (rerun, shuffle, etc.) applies across the distinct shows inside this playlist. Each show\'s episode position is tracked independently.'
                    : 'In-Order: items play sequentially as a flat list, ignoring the block\'s advancement setting.'}
                </div>
              </div>

              {/* Membership toggle */}
              <div className="flex flex-col gap-1.5 pb-1 border-b border-zinc-800/50">
                <div className="text-[10px] font-semibold tracking-widest text-zinc-500 uppercase">Membership</div>
                <div className="flex gap-2">
                  <button
                    onClick={() => store.setMembership(playlist.playlist_id, 'static')}
                    className={`px-3 py-1 rounded text-xs font-semibold transition-colors ${playlist.membership !== 'smart' ? 'bg-violet-700 text-white' : 'bg-zinc-800 text-zinc-400 hover:text-zinc-200'}`}
                  >Manual</button>
                  <button
                    onClick={() => store.setMembership(playlist.playlist_id, 'smart')}
                    className={`px-3 py-1 rounded text-xs font-semibold transition-colors ${playlist.membership === 'smart' ? 'bg-violet-700 text-white' : 'bg-zinc-800 text-zinc-400 hover:text-zinc-200'}`}
                  >Smart (filter-based)</button>
                </div>
                <div className="text-[10px] text-zinc-500 leading-relaxed">
                  {playlist.membership === 'smart'
                    ? 'Smart: items are computed from the filter below, refreshed automatically every sync cycle (or on demand with Refresh Now).'
                    : 'Manual: items are added and removed one at a time below.'}
                </div>
              </div>

              {playlist.membership === 'smart' ? (
                <SmartPlaylistEditor playlist={playlist} />
              ) : (
                <>
                  {store.importing && store.expanded === playlist.playlist_id ? (
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

                  {store.pickerOpen && store.expanded === playlist.playlist_id && (
                    <ItemPicker playlistId={playlist.playlist_id} />
                  )}
                </>
              )}

              <div className="space-y-1">
                {(store.detail?.items ?? []).map((item, idx) => (
                  <PlaylistItemRow key={item.id} item={item} idx={idx} playlistId={playlist.playlist_id}
                    readOnly={playlist.membership === 'smart'} />
                ))}
                {(store.detail?.items.length ?? 0) === 0 && (
                  <p className="text-zinc-600 text-xs">
                    {playlist.membership === 'smart' ? 'No items match the filter yet.' : 'No items yet.'}
                  </p>
                )}
              </div>
            </>
          )}
        </div>
      )}
    </div>
  )
})

function PlaylistItemRow({ item, idx, playlistId, readOnly }: {
  item: PlaylistItem; idx: number; playlistId: string
  // Smart playlists' items are recomputed wholesale on every refresh — a
  // manual per-item remove would just be undone by the next one, so the
  // control is hidden entirely rather than left in to silently no-op later.
  readOnly?: boolean
}) {
  const icon = item.item_type === 'episode' ? '◈' : '▣'
  return (
    <div className="flex items-center gap-2 px-2.5 py-2 rounded-lg bg-zinc-800/50 border border-zinc-700/30">
      <span className="text-zinc-700 text-xs w-5 text-right shrink-0">{idx + 1}</span>
      <span className="text-zinc-600 text-xs shrink-0">{icon}</span>
      <span className="text-sm text-zinc-300 flex-1 truncate">{item.title}</span>
      <span className="text-zinc-600 text-xs shrink-0">{fmtDuration(item.duration_ms)}</span>
      {!readOnly && (
        <button onClick={() => store.removeItem(playlistId, item.id)}
          className="text-zinc-600 hover:text-red-400 transition-colors text-xs px-1 shrink-0">✕</button>
      )}
    </div>
  )
}

// ─── Smart playlist editor ──────────────────────────────────────────────────────

const SMART_SORTS: { value: string; label: string }[] = [
  { value: 'title',            label: 'Title' },
  { value: 'recently_added',   label: 'Recently Added' },
  { value: 'year',             label: 'Year' },
  { value: 'audience_rating',  label: 'Audience Rating' },
  { value: 'random',           label: 'Random' },
]

const SmartPlaylistEditor = observer(function SmartPlaylistEditor({ playlist }: { playlist: Playlist }) {
  const filteredLibs = store.allLibraries.filter(l =>
    l.library_type === store.smartType || l.library_type === 'mixed')

  return (
    <div className="rounded-lg border border-violet-900/30 bg-zinc-950/60 p-3 space-y-3">
      <div className="flex items-center gap-2 flex-wrap">
        <label className="text-[10px] text-zinc-500 uppercase tracking-widest">Type</label>
        <select
          value={store.smartType}
          onChange={e => runInAction(() => { store.smartType = e.target.value as 'show' | 'movie' })}
          className="input text-xs py-1"
        >
          <option value="movie">Movies</option>
          <option value="show">Shows (expands to episodes)</option>
        </select>

        <label className="text-[10px] text-zinc-500 uppercase tracking-widest ml-2">Sort</label>
        <select
          value={store.smartSort}
          onChange={e => runInAction(() => { store.smartSort = e.target.value })}
          className="input text-xs py-1"
        >
          {SMART_SORTS.map(s => <option key={s.value} value={s.value}>{s.label}</option>)}
        </select>

        <label className="text-[10px] text-zinc-500 uppercase tracking-widest ml-2">Limit</label>
        <input
          value={store.smartLimit}
          onChange={e => runInAction(() => { store.smartLimit = e.target.value.replace(/[^0-9]/g, '') })}
          placeholder="unlimited" className="input text-xs py-1 w-20"
        />
      </div>

      <FilterSection tree={store.smartFilterTree} filteredLibs={filteredLibs} />

      <div className="pt-2 border-t border-zinc-800/50 space-y-2">
        <label className="flex items-center gap-2 text-xs text-zinc-300">
          <input type="checkbox" checked={store.showOnHome}
            onChange={e => store.setShowOnHome(playlist.playlist_id, e.target.checked)} />
          Show on Home page
        </label>

        {/* Tile limit / active window only make sense once this playlist is
            actually shown as a shelf — hidden otherwise rather than left
            visible-but-inert. */}
        {store.showOnHome && (
          <div className="flex items-center gap-2 flex-wrap pl-5">
            <label className="text-[10px] text-zinc-500 uppercase tracking-widest">Tiles before "Continue in Library"</label>
            <input
              value={store.homeTileLimit}
              onChange={e => runInAction(() => { store.homeTileLimit = e.target.value.replace(/[^0-9]/g, '') })}
              placeholder="16" className="input text-xs py-1 w-16"
            />

            <label className="text-[10px] text-zinc-500 uppercase tracking-widest ml-2">Active window</label>
            <input
              value={store.homeActiveStart}
              onChange={e => runInAction(() => { store.homeActiveStart = e.target.value })}
              placeholder="MM-DD (e.g. 10-01)" className="input text-xs py-1 w-32"
            />
            <span className="text-zinc-600 text-xs">to</span>
            <input
              value={store.homeActiveEnd}
              onChange={e => runInAction(() => { store.homeActiveEnd = e.target.value })}
              placeholder="MM-DD (e.g. 10-31)" className="input text-xs py-1 w-32"
            />
            <span className="text-[10px] text-zinc-600 w-full">Leave both blank to always show — e.g. a Halloween shelf set to 10-01 → 10-31 only appears in October.</span>
          </div>
        )}
      </div>

      <div className="flex items-center gap-3">
        <button
          onClick={() => store.saveSmartDef(playlist.playlist_id)}
          disabled={store.smartSaving}
          className="btn-primary text-xs disabled:opacity-40"
        >
          {store.smartSaving ? 'Saving…' : 'Save & Refresh'}
        </button>
        <span className="text-[10px] text-zinc-600">{fmtSyncAge(playlist.last_smart_refresh_at)}</span>
      </div>
    </div>
  )
})

// ─── Item picker ──────────────────────────────────────────────────────────────

const TABS: { id: PickerTab; label: string }[] = [
  { id: 'episodes',            label: 'Episodes' },
  { id: 'movies',              label: 'Movies' },
  { id: 'shows',                label: 'Shows' },
  { id: 'source_playlists',    label: 'Playlists' },
  { id: 'source_collections',  label: 'Collections' },
]

const ItemPicker = observer(function ItemPicker({ playlistId }: { playlistId: string }) {
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
        <FilterSection tree={store.filterTree} filteredLibs={filteredLibs} />
      )}

      {/* Source header row */}
      {(store.pickerTab === 'source_playlists' || store.pickerTab === 'source_collections') && (
        <div className="flex items-center gap-2 p-2 border-t border-zinc-800/60 flex-wrap">
          {store.browseSources.length > 1 && (
            <select className="input text-xs py-1 flex-1"
              value={store.selectedSource} onChange={e => store.setBrowseSource(e.target.value)}>
              <option value="">Select source…</option>
              {store.browseSources.map(s => <option key={s.source_id} value={s.source_id}>{s.display_name}</option>)}
            </select>
          )}
          {store.pickerTab === 'source_collections' && store.selectedSource && (
            <select className="input text-xs py-1 flex-1"
              value={store.browseLibraryId} onChange={e => store.setBrowseLibrary(e.target.value)}>
              <option value="">Select library…</option>
              {store.allLibraries.filter(l => l.source_id === store.selectedSource).map(l =>
                <option key={l.library_id} value={l.library_id}>{l.display_name}</option>
              )}
            </select>
          )}
          <button onClick={() => store.closePicker()}
            className="text-zinc-600 hover:text-zinc-300 text-xs px-1 shrink-0 ml-auto">✕</button>
        </div>
      )}

      <div className="max-h-56 overflow-y-auto scrollbar-dark">
        {store.pickerTab === 'episodes' && <EpisodeList playlistId={playlistId} />}
        {store.pickerTab === 'movies' && <MovieList playlistId={playlistId} />}
        {store.pickerTab === 'shows' && <ShowList playlistId={playlistId} />}
        {(store.pickerTab === 'source_playlists' || store.pickerTab === 'source_collections') && (
          <SourceBrowseList playlistId={playlistId} kind={store.pickerTab === 'source_playlists' ? 'playlist' : 'collection'} />
        )}
      </div>
    </div>
  )
})

const EpisodeList = observer(function EpisodeList({ playlistId }: { playlistId: string }) {
  const q = store.pickerQuery.toLowerCase()
  const results = store.pickerEpisodes.filter(e => !q || e.title.toLowerCase().includes(q) || e.show_title.toLowerCase().includes(q))
  if (store.pickerLoading) return <Spinner />
  if (results.length === 0) return <Empty msg="No results. Type to search episodes." />
  return (
    <>
      {results.map(ep => (
        <button key={ep.episode_id} onClick={() => store.addItem(playlistId, 'episode', ep.episode_id)}
          className="w-full flex items-center gap-2 px-3 py-2 text-left hover:bg-violet-950/30 transition-colors">
          <span className="text-zinc-600 text-xs shrink-0">◈</span>
          <div className="min-w-0">
            <div className="text-xs text-zinc-500 truncate">{ep.show_title}</div>
            <div className="text-sm text-zinc-300 truncate">
              S{String(ep.season).padStart(2,'0')}E{String(ep.episode).padStart(2,'0')} — {ep.title}
            </div>
          </div>
          <span className="text-zinc-600 text-xs shrink-0 ml-auto">{fmtDuration(ep.duration_ms)}</span>
        </button>
      ))}
    </>
  )
})

const MovieList = observer(function MovieList({ playlistId }: { playlistId: string }) {
  const q = store.pickerQuery.toLowerCase()
  const results = store.pickerMovies.filter(m => !q || m.title.toLowerCase().includes(q))
  if (store.pickerLoading) return <Spinner />
  if (results.length === 0) return <Empty />
  return (
    <>
      {results.map(m => (
        <button key={m.movie_id} onClick={() => store.addItem(playlistId, 'movie', m.movie_id)}
          className="w-full flex items-center gap-2 px-3 py-2 text-left text-sm text-zinc-300 hover:bg-violet-950/30 transition-colors">
          <span className="text-zinc-600 text-xs shrink-0">▣</span>
          <span className="truncate flex-1">{m.title}</span>
          {m.year && <span className="text-zinc-600 text-xs shrink-0">{m.year}</span>}
          <span className="text-zinc-600 text-xs shrink-0">{fmtDuration(m.duration_ms)}</span>
        </button>
      ))}
    </>
  )
})

const ShowList = observer(function ShowList({ playlistId }: { playlistId: string }) {
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
                    <SeasonBtn label="All episodes" onClick={() => store.importShowEpisodes(playlistId, show.show_id)} />
                    {store.expandedSeasons.map(s => (
                      <SeasonBtn key={s.number} label={s.name || `S${String(s.number).padStart(2,'0')}`}
                        onClick={() => store.importShowEpisodes(playlistId, show.show_id, s.number)} />
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

const SourceBrowseList = observer(function SourceBrowseList({ playlistId, kind }: { playlistId: string; kind: 'playlist' | 'collection' }) {
  if (!store.selectedSource || (kind === 'collection' && !store.browseLibraryId)) {
    return <Empty msg={store.browseSources.length === 0 ? 'No Plex/Jellyfin/Emby sources configured.' : kind === 'collection' ? 'Select a library above.' : 'Select a source above.'} />
  }
  if (store.browseLoading) return <Spinner />
  if (store.browseLists.length === 0) return <Empty msg={`No ${kind}s found.`} />
  return (
    <>
      {store.browseLists.map(list => (
        <div key={list.id} className="flex items-center gap-2 px-3 py-2 border-b border-zinc-800/40">
          <div className="flex-1 min-w-0">
            <div className="text-sm text-zinc-300 truncate">{list.title}</div>
            <div className="text-[10px] text-zinc-600">{list.item_count} items</div>
          </div>
          {store.importingListId === list.id ? (
            <span className="text-violet-400 text-xs flex items-center gap-1">
              <span className="w-1.5 h-1.5 rounded-full bg-violet-500 animate-pulse" />importing
            </span>
          ) : (
            <button onClick={() => store.importSourceList(playlistId, list.id, kind)}
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
