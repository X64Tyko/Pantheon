import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'
import { channelStore } from '../stores'
import { BLANK_DRAFT, DAY_BITS } from './constants'
import { defaultPickerTab, normalizeBlock, blockToDraft, m2t, t2m, todayEpgDay } from './utils'
import { FIELD_DEFS } from '../components/PickerFilters'
import type { FilterRule } from '../components/PickerFilters'
import type { BlockDraft, LimitMode, PickerTab } from './types'
import type {
  AdvanceMode, Advancement, Block, BlockContent, BlockType, Channel, ContentType, CursorScope,
  EpisodeSearchResult, EpgProgram, FillerEntry, FillerEntryAdvancement, FillerList, FillerSelectionMode,
  LibraryWithSource, Movie, NoHistoryBehavior, Playlist, PlaylistMode, Show,
} from '../api/types'

let _debounce: ReturnType<typeof setTimeout>
let _ruleId = 0

export class ChannelDetailStore {
  blocks:      Block[]       = []
  loading:     boolean       = false
  error:       string | null = null

  selectedId:  string | null = null
  editing:     Block | null  = null
  draft:       BlockDraft    = { ...BLANK_DRAFT }

  pxPerHour:   number = 46
  epgDay:      number = todayEpgDay('UTC')

  painting:    boolean = false
  paintVal:    boolean = true

  pickerOpen:        boolean               = false
  pickerQuery:       string                = ''
  pickerTab:         PickerTab             = 'shows'
  pickerShows:       Show[]                = []
  pickerMovies:      Movie[]               = []
  pickerEpisodes:    EpisodeSearchResult[] = []
  pickerPlaylists:   Playlist[]            = []
  contentPlaylists:  Playlist[]            = []
  pickerFillerLists: FillerList[]          = []
  pickerLoading:     boolean               = false
  dragItem:          string | null         = null

  allLibraries:      LibraryWithSource[]   = []

  filterRulesOpen:  boolean              = false
  filterMatch:      'all' | 'any'        = 'all'
  filterRules:      FilterRule[]         = []

  expandedShowId:         string | null = null
  expandedSeasons:        number[]      = []
  expandedSeasonsLoading: boolean       = false

  isNewMode: boolean = false

  saving:          boolean       = false
  saveErr:         string | null = null
  creatingPremiers: boolean      = false

  bulkMode:        boolean       = false
  bulkSelectedIds: string[]      = []
  bulkSaving:      boolean       = false
  bulkErr:         string | null = null

  openSections: Record<string, boolean> = { schedule: true, timing: true, playback: false, content: true, filler: false }
  modalOpen: boolean = false

  allFillerLists:   FillerList[] = []
  fillerPickerOpen: boolean      = false
  fillerSaving:     boolean      = false

  channelFillerSaving: boolean        = false
  channelFillerErr:    string | null  = null

  draftContent:       BlockContent[] = []
  draftFillerEntries: FillerEntry[]  = []
  contentDirty:       boolean        = false

  epgItems:   EpgProgram[] = []
  epgLoading: boolean      = false

  channelDraftName:        string      = ''
  channelDraftNumber:      number      = 1
  channelDraftTimezone:    string      = 'UTC'
  channelDraftSeed:        number      = 12345
  channelDraftAdvanceMode: AdvanceMode = 'scheduled'
  channelDirty:            boolean     = false
  channelSaving:        boolean = false
  channelSaveErr:       string | null = null

  constructor() { makeAutoObservable(this) }

  initChannelDraft(channel: Channel) {
    this.channelDraftName        = channel.name
    this.channelDraftNumber      = channel.number
    this.channelDraftTimezone    = channel.timezone
    this.channelDraftSeed        = channel.seed !== undefined ? channel.seed : 12345
    this.channelDraftAdvanceMode = channel.advance_mode ?? 'scheduled'
    this.channelDirty            = false
    this.epgDay                  = todayEpgDay(channel.timezone)
  }

  setChannelDraft(patch: Partial<{ name: string; number: number; timezone: string; seed: number; advance_mode: AdvanceMode }>) {
    if (patch.name         !== undefined) this.channelDraftName        = patch.name
    if (patch.number       !== undefined) this.channelDraftNumber      = patch.number
    if (patch.timezone     !== undefined) this.channelDraftTimezone    = patch.timezone
    if (patch.seed         !== undefined) this.channelDraftSeed        = patch.seed
    if (patch.advance_mode !== undefined) this.channelDraftAdvanceMode = patch.advance_mode
    this.channelDirty = true
  }

  async saveChannel(channelId: string) {
    this.channelSaving = true; this.channelSaveErr = null
    try {
      await api.updateChannel(channelId, {
        name:         this.channelDraftName,
        number:       this.channelDraftNumber,
        timezone:     this.channelDraftTimezone,
        seed:         this.channelDraftSeed,
        advance_mode: this.channelDraftAdvanceMode,
      })
      await channelStore.fetchAll()
      runInAction(() => { this.channelSaving = false; this.channelDirty = false })
      this.loadEpg(channelId)
    } catch (e: any) {
      runInAction(() => { this.channelSaveErr = e.message; this.channelSaving = false })
    }
  }

  async load(channelId: string) {
    this.loading = true; this.error = null
    try {
      const [blocks, fillerLists] = await Promise.all([api.getBlocks(channelId), api.getFillerLists()])
      runInAction(() => {
        this.blocks         = blocks.map(normalizeBlock)
        this.allFillerLists = fillerLists
        this.loading        = false
      })
      this.loadEpg(channelId)
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.loading = false })
    }
  }

  async loadEpg(channelId: string, force = false) {
    this.epgLoading = true
    try {
      const items = await api.previewChannelEpg(channelId, 168, this.channelDraftSeed, force)
      runInAction(() => { this.epgItems = items; this.epgLoading = false })
    } catch {
      runInAction(() => { this.epgLoading = false })
    }
  }

  select(blockId: string) {
    const block = this.blocks.find(b => b.block_id === blockId)
    if (!block) return
    this.selectedId         = blockId
    this.editing            = block
    this.isNewMode          = false
    this.draft              = blockToDraft(block)
    this.draftContent       = [...block.content]
    this.draftFillerEntries = [...block.filler_entries]
    this.contentDirty       = false
    this.saveErr            = null
    this.pickerOpen         = false
    this.fillerPickerOpen   = false
    if (block.content.some(c => c.content_type === 'playlist')) {
      api.getPlaylists().then(r => runInAction(() => { this.pickerPlaylists = r; this.contentPlaylists = r })).catch(() => {})
    } else {
      this.contentPlaylists = []
    }
  }

  openNew() {
    const maxP              = Math.max(0, ...this.blocks.map(b => b.priority))
    this.selectedId         = null
    this.editing            = null
    this.isNewMode          = true
    this.draft              = { ...BLANK_DRAFT, priority: maxP + 1 }
    this.draftContent       = []
    this.draftFillerEntries = []
    this.contentDirty       = false
    this.saveErr            = null
    this.pickerOpen         = false
  }

  closeEditor() {
    this.selectedId         = null
    this.editing            = null
    this.isNewMode          = false
    this.pickerOpen         = false
    this.fillerPickerOpen   = false
    this.draftContent       = []
    this.draftFillerEntries = []
    this.contentDirty       = false
  }

  toggleBulkMode() {
    this.bulkMode = !this.bulkMode
    this.bulkSelectedIds = []
    this.bulkErr = null
    if (this.bulkMode) this.closeEditor()
  }

  toggleBulkBlock(id: string) {
    if (this.bulkSelectedIds.includes(id))
      this.bulkSelectedIds = this.bulkSelectedIds.filter(x => x !== id)
    else
      this.bulkSelectedIds = [...this.bulkSelectedIds, id]
  }

  selectAllBulk()           { this.bulkSelectedIds = this.blocks.map(b => b.block_id) }
  clearBulkSelection()      { this.bulkSelectedIds = [] }
  selectBulkByType(t: BlockType) {
    const ids = this.blocks.filter(b => b.block_type === t).map(b => b.block_id)
    this.bulkSelectedIds = [...new Set([...this.bulkSelectedIds, ...ids])]
  }

  async applyBulk(channelId: string, patch: Partial<BlockDraft>) {
    if (!this.bulkSelectedIds.length) return
    this.bulkSaving = true; this.bulkErr = null
    try {
      for (const id of this.bulkSelectedIds)
        await api.updateBlock(channelId, id, patch as any)
      await this.load(channelId)
      runInAction(() => { this.bulkSaving = false; this.bulkSelectedIds = [] })
    } catch (e: any) {
      runInAction(() => { this.bulkErr = e.message; this.bulkSaving = false })
    }
  }

  setDraft<K extends keyof BlockDraft>(k: K, v: BlockDraft[K]) {
    this.draft = { ...this.draft, [k]: v }
  }

  setLimitMode(mode: LimitMode) {
    if (mode === 'programs') {
      this.draft = { ...this.draft, end_time: '', program_count: Math.max(1, this.draft.program_count || 1) }
    } else if (mode === 'end') {
      this.draft = { ...this.draft, end_time: m2t(Math.min(1439, t2m(this.draft.start_time) + 60)), program_count: 0 }
    } else {
      this.draft = { ...this.draft, end_time: '', program_count: 0 }
    }
  }

  dayDown(dayIdx: number) {
    const bit = DAY_BITS[dayIdx]
    const has = (this.draft.day_mask & bit) !== 0
    this.painting = true
    this.paintVal = !has
    this.draft = { ...this.draft, day_mask: !has ? this.draft.day_mask | bit : this.draft.day_mask & ~bit }
  }

  dayEnter(dayIdx: number) {
    if (!this.painting) return
    const bit = DAY_BITS[dayIdx]
    const has = (this.draft.day_mask & bit) !== 0
    if (has !== this.paintVal) {
      this.draft = { ...this.draft, day_mask: this.paintVal ? this.draft.day_mask | bit : this.draft.day_mask & ~bit }
    }
  }

  stopPainting() { this.painting = false }

  zoom(dir: 1 | -1) {
    const next = dir > 0 ? this.pxPerHour * 1.25 : this.pxPerHour / 1.25
    this.pxPerHour = Math.max(22, Math.min(96, Math.round(next)))
  }

  async save(channelId: string) {
    this.saving = true; this.saveErr = null
    // Always include end_time — empty string tells the backend's PATCH handler to NULL the column.
    const payload = { ...this.draft, end_time: this.draft.end_time ?? '' }

    const origContent  = [...(this.editing?.content        ?? [])]
    const origFiller   = [...(this.editing?.filler_entries ?? [])]
    const draftContent = [...this.draftContent]
    const draftFiller  = [...this.draftFillerEntries]

    try {
      let blockId: string
      if (this.editing) {
        await api.updateBlock(channelId, this.editing.block_id, payload)
        blockId = this.editing.block_id
      } else {
        const res = await api.createBlock(channelId, payload as any)
        blockId = res.block_id
      }

      const toRemoveContent = origContent.filter(c  => !draftContent.some(dc => dc.id === c.id))
      const toAddContent    = draftContent.filter(dc => dc.id < 0)
      for (const c  of toRemoveContent) await api.removeBlockContent(channelId, blockId, c.id)
      for (const c  of toAddContent)    await api.addBlockContent(channelId, blockId, { content_type: c.content_type, content_id: c.content_id, season_filter: c.season_filter, weight: c.weight, run_count: c.run_count })

      const toRemoveFiller = origFiller.filter(fe  => !draftFiller.some(dfe => dfe.id === fe.id))
      const toAddFiller    = draftFiller.filter(dfe => dfe.id < 0)
      const toUpdateFiller = draftFiller.filter(dfe => {
        if (dfe.id < 0) return false
        const orig = origFiller.find(fe => fe.id === dfe.id)
        return orig && (orig.advancement !== dfe.advancement || orig.weight !== dfe.weight)
      })
      for (const fe of toRemoveFiller) await api.removeBlockFiller(channelId, blockId, fe.id)
      for (const fe of toAddFiller)    await api.addBlockFiller(channelId, blockId, { filler_list_id: fe.filler_list_id, advancement: fe.advancement, weight: fe.weight })
      for (const fe of toUpdateFiller) await api.updateBlockFiller(channelId, blockId, fe.id, { advancement: fe.advancement, weight: fe.weight })

      await this.load(channelId)
      runInAction(() => {
        const { filler_selection, align_to_mins, inter_filler, early_start_secs, start_scope } = this.draft
        const block = this.blocks.find(b => b.block_id === blockId) ?? null
        this.saving       = false
        this.isNewMode    = false
        this.selectedId   = blockId
        this.editing      = block
        this.contentDirty = false
        if (block) {
          this.draft              = { ...blockToDraft(block), filler_selection, align_to_mins, inter_filler, early_start_secs, start_scope }
          this.draftContent       = [...block.content]
          this.draftFillerEntries = [...block.filler_entries]
        }
      })
    } catch (e: any) {
      runInAction(() => { this.saveErr = e.message; this.saving = false })
    }
  }

  async duplicate(channelId: string) {
    if (!this.editing) return
    const src = this.editing
    const payload = blockToDraft(src)
    const { filler_selection, align_to_mins, inter_filler, early_start_secs, start_scope } = src
    try {
      const { block_id } = await api.createBlock(channelId, payload)
      for (const c of src.content) {
        await api.addBlockContent(channelId, block_id, { content_type: c.content_type, content_id: c.content_id, season_filter: c.season_filter })
      }
      for (const f of src.filler_entries) {
        await api.addBlockFiller(channelId, block_id, { filler_list_id: f.filler_list_id, advancement: f.advancement, weight: f.weight })
      }
      await this.load(channelId)
      runInAction(() => {
        const block = this.blocks.find(b => b.block_id === block_id)
        if (block) {
          this.selectedId         = block_id
          this.editing            = block
          this.isNewMode          = false
          this.draft              = { ...blockToDraft(block), filler_selection, align_to_mins, inter_filler, early_start_secs, start_scope }
          this.draftContent       = [...block.content]
          this.draftFillerEntries = [...block.filler_entries]
          this.contentDirty       = false
        }
      })
    } catch (e: any) {
      runInAction(() => { this.saveErr = (e as Error).message })
    }
  }

  async createPremierBlocks(channelId: string) {
    if (!this.editing) return
    const isRerun = this.draft.advancement === 'rerun_shuffle' || this.draft.advancement === 'rerun_smart'
    if (!isRerun) return
    runInAction(() => { this.creatingPremiers = true })
    const premierBlocks = this.blocks.filter(b => b.block_type === 'premier')
    const showsToCreate = this.draftContent.filter(c =>
      c.content_type === 'show' && c.id > 0 &&
      !premierBlocks.some(pb => pb.content.some(pc => pc.content_type === 'show' && pc.content_id === c.content_id))
    )
    const rerunPriority = this.draft.priority
    try {
      for (const item of showsToCreate) {
        const payload = {
          ...BLANK_DRAFT,
          block_type:       'premier'    as BlockType,
          day_mask:         this.draft.day_mask,
          start_time:       this.draft.start_time,
          advancement:      'sequential' as Advancement,
          cursor_scope:     this.draft.cursor_scope,
          priority:         rerunPriority + 1,
          program_count:    1,
          late_start_mins:  5,
          early_start_secs: 15,
          end_time:         '',
        }
        const { block_id } = await api.createBlock(channelId, payload as any)
        await api.addBlockContent(channelId, block_id, { content_type: 'show', content_id: item.content_id })
      }
      await this.load(channelId)
    } catch (e: any) {
      runInAction(() => { this.saveErr = e.message })
    } finally {
      runInAction(() => { this.creatingPremiers = false })
    }
  }

  async deleteBlock(channelId: string, blockId: string) {
    await api.deleteBlock(channelId, blockId)
    runInAction(() => {
      this.blocks = this.blocks.filter(b => b.block_id !== blockId)
      this.closeEditor()
    })
  }

  addContent(channelId: string, item: { content_type: ContentType; content_id: string; season_filter?: number | null; title?: string }) {
    if (!this.editing && !this.isNewMode) return
    const pos = this.draftContent.length > 0 ? Math.max(...this.draftContent.map(c => c.position)) + 1 : 0
    this.draftContent = [...this.draftContent, {
      id: -(Date.now() * 100 + this.draftContent.length),
      block_id: this.editing?.block_id ?? '',
      content_type: item.content_type,
      content_id: item.content_id,
      position: pos,
      season_filter: item.season_filter ?? undefined,
      title: item.title ?? item.content_id,
      weight: 1,
      run_count: 1,
      include_specials: false,
      episode_order: 'season',
    }]
    this.contentDirty = true
  }

  removeContent(channelId: string, cid: number) {
    this.draftContent = this.draftContent.filter(c => c.id !== cid)
    this.contentDirty = true
  }

  updateContentField(channelId: string, cid: number, field: 'weight' | 'run_count' | 'episode_order' | 'include_specials', value: number | string | boolean) {
    this.draftContent = this.draftContent.map(c => c.id === cid ? { ...c, [field]: value } : c)
    this.contentDirty = true
    if (cid > 0 && this.editing) {
      api.updateBlockContent(channelId, this.editing.block_id, cid, { [field]: value } as any).catch(() => {})
    }
  }

  async setPlaylistMode(playlistId: string, mode: 'sequential' | 'show_collection') {
    await api.updatePlaylist(playlistId, { mode })
    runInAction(() => {
      const update = (list: Playlist[]) => list.map(p => p.playlist_id === playlistId ? { ...p, mode } : p)
      this.pickerPlaylists  = update(this.pickerPlaylists)
      this.contentPlaylists = update(this.contentPlaylists)
    })
  }

  async addBlockFiller(channelId: string, body: { filler_list_id: string; advancement: FillerEntryAdvancement; weight: number }) {
    if (!this.editing && !this.isNewMode) return
    const title = this.allFillerLists.find(f => f.filler_list_id === body.filler_list_id)?.title ?? body.filler_list_id
    const entry: FillerEntry = {
      id: -(Date.now() * 100 + this.draftFillerEntries.length),
      filler_list_id: body.filler_list_id,
      title,
      advancement: body.advancement,
      weight: body.weight,
      position: this.draftFillerEntries.length,
    }
    runInAction(() => {
      this.draftFillerEntries = [...this.draftFillerEntries, entry]
      this.fillerPickerOpen   = false
      this.contentDirty       = true
    })
  }

  updateBlockFiller(channelId: string, blockId: string, entryId: number, patch: { advancement?: FillerEntryAdvancement; weight?: number }) {
    this.draftFillerEntries = this.draftFillerEntries.map(e => e.id === entryId ? { ...e, ...patch } : e)
    this.contentDirty = true
  }

  removeBlockFiller(channelId: string, blockId: string, entryId: number) {
    this.draftFillerEntries = this.draftFillerEntries.filter(e => e.id !== entryId)
    this.contentDirty = true
  }

  async saveChannelFiller(channelId: string, patch: { default_filler_selection?: FillerSelectionMode }) {
    this.channelFillerSaving = true
    try {
      await api.updateChannel(channelId, patch)
      await channelStore.fetchAll()
      runInAction(() => { this.channelFillerSaving = false })
    } catch (e: any) {
      runInAction(() => { this.channelFillerSaving = false; this.channelFillerErr = e.message })
    }
  }

  async addChannelFiller(channelId: string, body: { filler_list_id: string; advancement: FillerEntryAdvancement; weight: number }) {
    this.channelFillerSaving = true
    try {
      await api.addChannelFiller(channelId, body)
      await channelStore.fetchAll()
      runInAction(() => { this.channelFillerSaving = false })
    } catch (e: any) {
      runInAction(() => { this.channelFillerSaving = false; this.channelFillerErr = e.message })
    }
  }

  async updateChannelFiller(channelId: string, entryId: number, patch: { advancement?: FillerEntryAdvancement; weight?: number }) {
    try {
      await api.updateChannelFiller(channelId, entryId, patch)
      await channelStore.fetchAll()
    } catch (e: any) {
      runInAction(() => { this.channelFillerErr = e.message })
    }
  }

  async removeChannelFiller(channelId: string, entryId: number) {
    try {
      await api.removeChannelFiller(channelId, entryId)
      await channelStore.fetchAll()
    } catch (e: any) {
      runInAction(() => { this.channelFillerErr = e.message })
    }
  }

  addFilterRule() {
    this.filterRules.push({ id: String(++_ruleId), field: 'genre', op: 'is', value: '' })
    clearTimeout(_debounce)
    _debounce = setTimeout(() => this.searchPicker(), 250)
  }

  removeFilterRule(id: string) {
    this.filterRules = this.filterRules.filter(r => r.id !== id)
    clearTimeout(_debounce)
    _debounce = setTimeout(() => this.searchPicker(), 250)
  }

  updateFilterRule(id: string, patch: Partial<Omit<FilterRule, 'id'>>) {
    const rule = this.filterRules.find(r => r.id === id)
    if (!rule) return
    if (patch.field !== undefined) { rule.field = patch.field; rule.op = FIELD_DEFS[patch.field].ops[0].id; rule.value = '' }
    if (patch.op    !== undefined) rule.op    = patch.op
    if (patch.value !== undefined) rule.value = patch.value
    clearTimeout(_debounce)
    _debounce = setTimeout(() => this.searchPicker(), 250)
  }

  setFilterMatch(m: 'all' | 'any') {
    this.filterMatch = m
    clearTimeout(_debounce)
    _debounce = setTimeout(() => this.searchPicker(), 250)
  }

  toggleSection(s: string) {
    this.openSections = { ...this.openSections, [s]: !this.openSections[s] }
  }

  openPicker() {
    clearTimeout(_debounce)
    this.pickerOpen      = true; this.pickerQuery = ''
    this.filterRules     = []; this.filterRulesOpen = false; this.filterMatch = 'all'
    this.expandedShowId  = null; this.expandedSeasons = []
    this.pickerShows     = []; this.pickerMovies = []; this.pickerEpisodes = []
    this.pickerPlaylists = []; this.pickerFillerLists = []
    this.pickerTab       = defaultPickerTab(this.draft.block_type)
    if (this.allLibraries.length === 0)
      api.getAllLibraries().then(libs => runInAction(() => { this.allLibraries = libs }))
    this.searchPicker()
  }

  closePicker() {
    clearTimeout(_debounce)
    this.pickerOpen  = false; this.pickerQuery = ''
    this.filterRules = []; this.filterRulesOpen = false; this.filterMatch = 'all'
    this.expandedShowId = null
    this.pickerShows = []; this.pickerMovies = []; this.pickerEpisodes = []
    this.pickerPlaylists = []; this.pickerFillerLists = []
  }

  setPickerTab(t: PickerTab) {
    this.pickerTab   = t; this.expandedShowId = null
    this.filterRules = []; this.filterRulesOpen = false; this.filterMatch = 'all'
    this.searchPicker()
  }

  setPickerQuery(q: string) {
    this.pickerQuery = q; this.expandedShowId = null
    clearTimeout(_debounce)
    _debounce = setTimeout(() => this.searchPicker(), 250)
  }

  async searchPicker() {
    this.pickerLoading = true
    const q       = this.pickerQuery || undefined
    const isRules = this.filterRules.filter(r => r.op === 'is' && r.value.trim())
    const lib     = isRules.find(r => r.field === 'library')?.value        || undefined
    const genre   = isRules.find(r => r.field === 'genre')?.value          || undefined
    const yearStr = isRules.find(r => r.field === 'year')?.value
    const year    = yearStr ? parseInt(yearStr) : undefined
    const rating  = isRules.find(r => r.field === 'content_rating')?.value || undefined
    try {
      switch (this.pickerTab) {
        case 'shows':        { const r = await api.getShows({ limit: 100, q, library_id: lib, genre, year, content_rating: rating }); runInAction(() => { this.pickerShows = r.items; this.pickerLoading = false }); break }
        case 'movies':       { const r = await api.getMovies({ limit: 100, q, library_id: lib, genre, year, content_rating: rating }); runInAction(() => { this.pickerMovies = r.items; this.pickerLoading = false }); break }
        case 'episodes':     { const r = await api.searchEpisodes({ q, limit: 80 }); runInAction(() => { this.pickerEpisodes = r.items; this.pickerLoading = false }); break }
        case 'playlists':    { const r = await api.getPlaylists(); runInAction(() => { this.pickerPlaylists = r; this.pickerLoading = false }); break }
        case 'filler_lists': { const r = await api.getFillerLists(); runInAction(() => { this.pickerFillerLists = r; this.pickerLoading = false }); break }
      }
    } catch { runInAction(() => { this.pickerLoading = false }) }
  }

  async expandShow(showId: string) {
    if (this.expandedShowId === showId) { this.expandedShowId = null; return }
    this.expandedShowId = showId; this.expandedSeasonsLoading = true
    try {
      const { seasons } = await api.getShowSeasons(showId)
      runInAction(() => { this.expandedSeasons = seasons; this.expandedSeasonsLoading = false })
    } catch { runInAction(() => { this.expandedSeasonsLoading = false }) }
  }
}

export const store = new ChannelDetailStore()
