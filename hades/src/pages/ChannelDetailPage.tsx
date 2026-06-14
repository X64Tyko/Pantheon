import { observer } from 'mobx-react-lite'
import { makeAutoObservable, runInAction } from 'mobx'
import { Component, CSSProperties, ReactNode, useEffect, useRef, useState } from 'react'
import { useParams, Link } from 'react-router-dom'
import { api } from '../api/client'
import { channelStore } from '../stores'
import { FilterSection, FIELD_DEFS } from '../components/PickerFilters'
import type { FilterRule } from '../components/PickerFilters'
import type {
  Advancement, Block, BlockContent, BlockType, Channel, ContentType, CursorScope,
  EpisodeSearchResult, EpgProgram, FillerEntry, FillerEntryAdvancement, FillerList, FillerSelectionMode,
  LibraryWithSource, Movie, NoHistoryBehavior, Playlist, Show,
} from '../api/types'

// ─── Constants ────────────────────────────────────────────────────────────────

const PPH_DEFAULT = 46
const PPH_MIN = 22
const PPH_MAX = 96
const GUTTER_W = 58
const DAY_MIN_W = 94
const DAYS = [['Mo', 'MON'], ['Tu', 'TUE'], ['We', 'WED'], ['Th', 'THU'], ['Fr', 'FRI'], ['Sa', 'SAT'], ['Su', 'SUN']] as const
// day_mask bits: Sun=1 Mon=2 Tue=4 Wed=8 Thu=16 Fri=32 Sat=64  →  index 0=Mon…6=Sun
const DAY_BITS = [2, 4, 8, 16, 32, 64, 1]

type LimitMode = 'programs' | 'end' | 'fill'
type PickerTab = 'shows' | 'movies' | 'episodes' | 'playlists' | 'filler_lists'
type BlockDraft = Omit<Block, 'block_id' | 'channel_id' | 'content' | 'filler_entries'>

const BLOCK_META: Record<BlockType, { name: string; bg: string; solid: string; edge: string; border: string }> = {
  episode: { name: 'Episode', bg: 'linear-gradient(160deg, oklch(0.37 0.095 287), oklch(0.31 0.08 287))', solid: 'oklch(0.36 0.09 287)', edge: 'oklch(0.74 0.13 287)', border: 'oklch(0.48 0.1 287)' },
  movie:   { name: 'Movie',   bg: 'linear-gradient(160deg, oklch(0.4 0.09 58), oklch(0.33 0.075 56))',     solid: 'oklch(0.39 0.085 58)',  edge: 'oklch(0.78 0.12 68)',  border: 'oklch(0.5 0.1 60)' },
  premier: { name: 'Premier', bg: 'linear-gradient(160deg, oklch(0.42 0.13 19), oklch(0.34 0.11 18))',     solid: 'oklch(0.41 0.12 18)',   edge: 'oklch(0.76 0.17 24)',  border: 'oklch(0.52 0.14 20)' },
  filler:  { name: 'Filler',  bg: 'linear-gradient(160deg, oklch(0.32 0.018 262), oklch(0.27 0.015 262))', solid: 'oklch(0.31 0.018 262)', edge: 'oklch(0.62 0.03 262)', border: 'oklch(0.42 0.02 262)' },
}

const RATINGS = ['', 'TV-Y', 'TV-Y7', 'TV-G', 'TV-PG', 'TV-14', 'TV-MA']
const DELAY_OPTS: [number, string][]     = [[0,'None'],[5,'5 min'],[10,'10 min'],[15,'15 min'],[30,'30 min'],[60,'1 hr']]
const EARLY_OPTS: [number, string][]     = [[0,'None'],[15,'15 sec'],[30,'30 sec'],[60,'1 min'],[120,'2 min']]
const ALIGN_OPTS: [number, string][]     = [[0,'None'],[15,':00/:15/:30/:45'],[30,':00/:30'],[60,'Top of hour']]
const FILLER_ADV_OPTS: [FillerEntryAdvancement, string][] = [['sequential','Sequential'],['shuffle','Shuffle'],['sized','Sized']]
const FILLER_SEL_OPTS: [FillerSelectionMode, string][]    = [['round_robin','Round-robin'],['random','Random'],['weighted','Weighted']]

const NO_HISTORY_OPTS: [NoHistoryBehavior, string, string][] = [
  ['normal',       'Normal',       'Shows without premiers play as a regular episode show'],
  ['fallback_all', 'Fallback All', 'Use the full episode catalog as the rerun pool'],
  ['exclude',      'Exclude',      'Skip shows with no play history during selection'],
  ['filler',       'Filler',       'Fill the slot with filler content'],
  ['skip',         'Skip',         'Leave the slot empty'],
]

const BLANK_DRAFT: BlockDraft = {
  block_type: 'episode', day_mask: 62,
  start_time: '20:00', end_time: '21:00',
  program_count: 0, late_start_mins: 0, early_start_secs: 0,
  advancement: 'sequential', cursor_scope: 'block',
  priority: 1, max_content_rating: '',
  filler_selection: 'round_robin',
  align_to_mins: 0, inter_filler: false,
  smart_pct: 30, start_scope: 'block',
  no_history_behavior: 'normal',
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

function t2m(t: string): number {
  if (!t) return 0
  const [h, m] = t.split(':').map(Number)
  return h * 60 + m
}

function m2t(m: number): string {
  const mm = ((m % 1440) + 1440) % 1440
  return `${String(Math.floor(mm / 60)).padStart(2, '0')}:${String(mm % 60).padStart(2, '0')}`
}

function endOf(block: Block): number {
  if (block.end_time) return t2m(block.end_time)
  if (block.program_count > 0) return Math.min(t2m(block.start_time) + 60, 1440)
  return 1440
}

function getLimitMode(d: BlockDraft): LimitMode {
  if (d.end_time) return 'end'
  if (d.program_count > 0) return 'programs'
  return 'fill'
}

function defaultPickerTab(t: BlockType): PickerTab {
  if (t === 'filler') return 'filler_lists'
  if (t === 'movie') return 'movies'
  return 'shows'
}

function availablePickerTabs(t: BlockType): PickerTab[] {
  if (t === 'filler') return ['filler_lists']
  if (t === 'movie') return ['movies', 'playlists']
  return ['shows', 'episodes', 'movies', 'playlists']
}

function normalizeBlock(b: Block): Block {
  return {
    ...b,
    filler_entries:   b.filler_entries   ?? [],
    filler_selection: b.filler_selection ?? 'round_robin',
    align_to_mins:    b.align_to_mins    ?? 0,
    inter_filler:     b.inter_filler     ?? false,
    early_start_secs: b.early_start_secs ?? 0,
  }
}

function blockToDraft(block: Block): BlockDraft {
  return {
    block_type: block.block_type, day_mask: block.day_mask,
    start_time: block.start_time, end_time: block.end_time ?? '',
    program_count: block.program_count,
    late_start_mins: block.late_start_mins, early_start_secs: block.early_start_secs ?? 0,
    advancement: block.advancement, cursor_scope: block.cursor_scope,
    priority: block.priority, max_content_rating: block.max_content_rating,
    filler_selection: block.filler_selection ?? 'round_robin',
    align_to_mins: block.align_to_mins ?? 0, inter_filler: block.inter_filler ?? false,
    smart_pct: block.smart_pct ?? 30, start_scope: block.start_scope ?? 'block',
    no_history_behavior: block.no_history_behavior ?? 'normal',
  }
}

// ─── Store ────────────────────────────────────────────────────────────────────

let _debounce: ReturnType<typeof setTimeout>
let _ruleId = 0

class ChannelDetailStore {
  blocks:      Block[]       = []
  loading:     boolean       = false
  error:       string | null = null

  selectedId:  string | null = null
  editing:     Block | null  = null
  draft:       BlockDraft    = { ...BLANK_DRAFT }

  pxPerHour:   number = PPH_DEFAULT
  epgDay:      number = 4

  painting:    boolean = false
  paintVal:    boolean = true

  pickerOpen:       boolean              = false
  pickerQuery:      string               = ''
  pickerTab:        PickerTab            = 'shows'
  pickerShows:      Show[]               = []
  pickerMovies:     Movie[]              = []
  pickerEpisodes:   EpisodeSearchResult[] = []
  pickerPlaylists:  Playlist[]           = []
  pickerFillerLists: FillerList[]        = []
  pickerLoading:    boolean              = false
  dragItem:         string | null        = null

  allLibraries:     LibraryWithSource[]  = []

  filterRulesOpen:  boolean              = false
  filterMatch:      'all' | 'any'        = 'all'
  filterRules:      FilterRule[]         = []

  expandedShowId:          string | null = null
  expandedSeasons:         number[]      = []
  expandedSeasonsLoading:  boolean       = false

  isNewMode: boolean = false

  saving:  boolean       = false
  saveErr: string | null = null

  openSections: Record<string, boolean> = { schedule: true, timing: true, playback: false, content: true, filler: false }
  modalOpen: boolean = false

  allFillerLists:    FillerList[]   = []
  fillerPickerOpen:  boolean        = false
  fillerSaving:      boolean        = false

  channelFillerSaving: boolean        = false
  channelFillerErr:    string | null  = null

  draftContent:        BlockContent[] = []
  draftFillerEntries:  FillerEntry[]  = []
  contentDirty:        boolean        = false

  epgItems:    EpgProgram[] = []
  epgLoading:  boolean      = false

  // Channel-level draft (name, timezone, seed) — edited in ChannelDefaultsPanel
  channelDraftName:     string  = ''
  channelDraftNumber:   number  = 1
  channelDraftTimezone: string  = 'UTC'
  channelDraftSeed:     number  = 12345
  channelDirty:         boolean = false
  channelSaving:        boolean = false
  channelSaveErr:       string | null = null

  constructor() { makeAutoObservable(this) }

  initChannelDraft(channel: Channel) {
    this.channelDraftName     = channel.name
    this.channelDraftNumber   = channel.number
    this.channelDraftTimezone = channel.timezone
    this.channelDraftSeed     = channel.seed !== undefined ? channel.seed : 12345
    this.channelDirty         = false
  }

  setChannelDraft(patch: Partial<{ name: string; number: number; timezone: string; seed: number }>) {
    if (patch.name     !== undefined) this.channelDraftName     = patch.name
    if (patch.number   !== undefined) this.channelDraftNumber   = patch.number
    if (patch.timezone !== undefined) this.channelDraftTimezone = patch.timezone
    if (patch.seed     !== undefined) { this.channelDraftSeed = patch.seed; this.channelDirty = true }
    this.channelDirty = true
  }

  async saveChannel(channelId: string) {
    this.channelSaving = true; this.channelSaveErr = null
    try {
      await api.updateChannel(channelId, {
        name:     this.channelDraftName,
        number:   this.channelDraftNumber,
        timezone: this.channelDraftTimezone,
        seed:     this.channelDraftSeed,
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
  }

  openNew() {
    const maxP               = Math.max(0, ...this.blocks.map(b => b.priority))
    this.selectedId          = null
    this.editing             = null
    this.isNewMode           = true
    this.draft               = { ...BLANK_DRAFT, priority: maxP + 1 }
    this.draftContent        = []
    this.draftFillerEntries  = []
    this.contentDirty        = false
    this.saveErr             = null
    this.pickerOpen          = false
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
    this.pxPerHour = Math.max(PPH_MIN, Math.min(PPH_MAX, Math.round(next)))
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

      // Sync content: remove deleted items, then add new draft items
      const toRemoveContent = origContent.filter(c  => !draftContent.some(dc => dc.id === c.id))
      const toAddContent    = draftContent.filter(dc => dc.id < 0)
      for (const c  of toRemoveContent) await api.removeBlockContent(channelId, blockId, c.id)
      for (const c  of toAddContent)    await api.addBlockContent(channelId, blockId, { content_type: c.content_type, content_id: c.content_id, season_filter: c.season_filter, weight: c.weight, run_count: c.run_count })

      // Sync filler entries
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
    }]
    this.contentDirty = true
  }

  removeContent(channelId: string, cid: number) {
    this.draftContent = this.draftContent.filter(c => c.id !== cid)
    this.contentDirty = true
  }

  updateContentField(channelId: string, cid: number, field: 'weight' | 'run_count', value: number) {
    this.draftContent = this.draftContent.map(c => c.id === cid ? { ...c, [field]: value } : c)
    this.contentDirty = true
    // Persist immediately for saved items (id > 0)
    if (cid > 0 && this.editing) {
      api.updateBlockContent(channelId, this.editing.block_id, cid, { [field]: value }).catch(() => {})
    }
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
        case 'shows':       { const r = await api.getShows({ limit: 100, q, library_id: lib, genre, year, content_rating: rating }); runInAction(() => { this.pickerShows = r.items; this.pickerLoading = false }); break }
        case 'movies':      { const r = await api.getMovies({ limit: 100, q, library_id: lib, genre, year, content_rating: rating }); runInAction(() => { this.pickerMovies = r.items; this.pickerLoading = false }); break }
        case 'episodes':    { const r = await api.searchEpisodes({ q, limit: 80 }); runInAction(() => { this.pickerEpisodes = r.items; this.pickerLoading = false }); break }
        case 'playlists':   { const r = await api.getPlaylists(); runInAction(() => { this.pickerPlaylists = r; this.pickerLoading = false }); break }
        case 'filler_lists':{ const r = await api.getFillerLists(); runInAction(() => { this.pickerFillerLists = r; this.pickerLoading = false }); break }
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

const store = new ChannelDetailStore()

// ─── Page ─────────────────────────────────────────────────────────────────────

export default observer(function ChannelDetailPage() {
  const { id } = useParams<{ id: string }>()
  const channel = channelStore.channels.find(c => c.channel_id === id)
  const scrollRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!id) return
    if (channelStore.channels.length === 0) channelStore.fetchAll()
    store.closeEditor()
    store.load(id).then(() => {
      setTimeout(() => {
        if (scrollRef.current) scrollRef.current.scrollTop = Math.round(15.5 * store.pxPerHour)
      }, 80)
    })
  }, [id])

  useEffect(() => {
    if (channel) store.initChannelDraft(channel)
  }, [channel?.channel_id, channel?.seed, channel?.name, channel?.number, channel?.timezone])

  useEffect(() => {
    const up  = () => store.stopPainting()
    const esc = (e: KeyboardEvent) => { if (e.key === 'Escape') store.closeEditor() }
    window.addEventListener('mouseup', up)
    document.addEventListener('keydown', esc)
    return () => { window.removeEventListener('mouseup', up); document.removeEventListener('keydown', esc) }
  }, [])

  if (!id) return null

  const pph    = store.pxPerHour
  const gridH  = 24 * pph
  const zoomPct = Math.round(pph / PPH_DEFAULT * 100) + '%'

  // Merge the active draft into blocks so the EPG preview reacts to unsaved changes.
  const epgBlocks: Block[] = (() => {
    if (store.editing) {
      return store.blocks.map(b =>
        b.block_id === store.editing!.block_id ? { ...b, ...store.draft } : b
      )
    }
    if (store.isNewMode) {
      const virtual: Block = {
        block_id: '_draft_', channel_id: id,
        content: store.draftContent, filler_entries: store.draftFillerEntries,
        ...store.draft,
      }
      return [...store.blocks, virtual]
    }
    return store.blocks
  })()

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100vh', overflow: 'hidden', background: 'var(--hds-bg)', fontFamily: "'JetBrains Mono', monospace", fontSize: 13, letterSpacing: '0.01em', color: 'var(--hds-txt)' }}>

      {/* ── Top bar ───────────────────────────────────────────────────────── */}
      <header style={{ display: 'flex', alignItems: 'center', gap: 18, padding: '14px 24px', borderBottom: '1px solid var(--hds-line-s)', background: 'oklch(0.17 0.018 286 / 0.6)', flexShrink: 0 }}>
        <Link to="/channels" style={{ display: 'flex', alignItems: 'center', gap: 7, color: 'var(--hds-txt-2)', textDecoration: 'none', padding: '6px 9px', borderRadius: 7 }}>
          <span style={{ fontSize: 14 }}>←</span><span>Channels</span>
        </Link>
        <div style={{ width: 1, height: 22, background: 'var(--hds-line-s)' }} />
        <div style={{ display: 'flex', alignItems: 'center', gap: 11 }}>
          <span style={{ display: 'inline-flex', alignItems: 'center', justifyContent: 'center', minWidth: 28, height: 28, padding: '0 8px', borderRadius: 7, background: 'var(--hds-bg-3)', color: 'var(--hds-gold)', fontWeight: 700, fontSize: 13, boxShadow: 'inset 0 0 0 1px var(--hds-line)' }}>
            {channel?.number ?? '?'}
          </span>
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 18, letterSpacing: '0.01em' }}>
            {channel?.name ?? 'Channel'}
          </span>
          <span style={{ fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt-3)', padding: '3px 7px', border: '1px solid var(--hds-line-s)', borderRadius: 5 }}>
            {channel?.timezone ?? 'UTC'}
          </span>
        </div>

        <div style={{ flex: 1 }} />

        {/* Zoom */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 2, background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)', borderRadius: 9, padding: 3 }}>
          <button onClick={() => store.zoom(-1)} style={zoomBtnStyle}>−</button>
          <span style={{ minWidth: 52, textAlign: 'center', fontSize: 11, color: 'var(--hds-txt-2)', letterSpacing: '0.06em' }}>{zoomPct}</span>
          <button onClick={() => store.zoom(1)} style={zoomBtnStyle}>+</button>
        </div>

        {/* Add Block */}
        <button
          onClick={() => store.openNew()}
          style={{ display: 'flex', alignItems: 'center', gap: 7, padding: '9px 16px', border: 'none', borderRadius: 9, background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))', color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 13, cursor: 'pointer', boxShadow: '0 4px 16px -4px oklch(0.83 0.13 84 / 0.5)' }}
        >
          <span style={{ fontSize: 15, lineHeight: 1 }}>+</span> Add Block
        </button>
      </header>

      {store.error && (
        <div style={{ padding: '10px 24px', background: 'oklch(0.2 0.05 22 / 0.3)', borderBottom: '1px solid oklch(0.4 0.1 22 / 0.4)', color: 'oklch(0.72 0.16 22)', fontSize: 12, flexShrink: 0 }}>
          {store.error}
        </div>
      )}

      {/* ── Body ──────────────────────────────────────────────────────────── */}
      <div style={{ flex: 1, minHeight: 0, display: 'flex' }}>

        {/* Grid + EPG column */}
        <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>

          {/* Scrollable grid */}
          <div ref={scrollRef} style={{ flex: 1, minHeight: 0, overflow: 'auto' }} className="scrollbar-dark">
            <div style={{ minWidth: GUTTER_W + DAY_MIN_W * 7 }}>

              {/* Sticky day header */}
              <div style={{ display: 'flex', position: 'sticky', top: 0, zIndex: 25, borderBottom: '1px solid var(--hds-line-s)', background: 'var(--hds-bg)' }}>
                <div style={{ width: GUTTER_W, flexShrink: 0 }} />
                {DAYS.map(([, long]) => (
                  <div key={long} style={{ flex: `1 0 ${DAY_MIN_W}px`, textAlign: 'center', padding: '11px 0', fontSize: 10, letterSpacing: '0.24em', color: 'var(--hds-txt-2)', borderLeft: '1px solid var(--hds-line-s)' }}>
                    {long}
                  </div>
                ))}
              </div>

              {/* Body */}
              <div style={{ display: 'flex' }}>
                {/* Time gutter */}
                <div style={{ width: GUTTER_W, flexShrink: 0, position: 'relative', height: gridH }}>
                  {Array.from({ length: 25 }, (_, h) => (
                    <div key={h} style={{ position: 'absolute', top: h * pph, right: 9, transform: 'translateY(-50%)', fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.04em' }}>
                      {String(h).padStart(2, '0')}:00
                    </div>
                  ))}
                </div>
                {/* Day columns */}
                {DAYS.map(([, long], di) => (
                  <DayColumn key={long} dayIdx={di} blocks={store.blocks} pph={pph} selectedId={store.selectedId} store={store} channelId={id} />
                ))}
              </div>
            </div>
          </div>

          {/* EPG Preview */}
          <EpgErrorBoundary>
            <EpgPreview blocks={epgBlocks} epgItems={store.epgItems} epgLoading={store.epgLoading} epgDay={store.epgDay} timezone={channel?.timezone ?? 'UTC'} onDay={d => { store.epgDay = d }} onRefresh={() => store.loadEpg(id)} onSelectBlock={blockId => store.select(blockId)} />
          </EpgErrorBoundary>
        </div>

        {/* Side panel (always visible) */}
        <aside style={{ flexShrink: 0, width: 392, borderLeft: '1px solid var(--hds-line-s)', background: 'var(--hds-bg-2)', display: 'flex', flexDirection: 'column', minHeight: 0 }}>
          {store.selectedId !== null ? (
            <BlockEditor channelId={id} store={store} />
          ) : store.isNewMode ? (
            <NewBlockEditor channelId={id} store={store} />
          ) : (
            <ChannelDefaultsPanel channel={channel} channelId={id} store={store} />
          )}
        </aside>
      </div>
      {store.modalOpen && (id) && (
        <BlockEditorModal channelId={id} store={store} />
      )}
    </div>
  )
})

// ─── Shared button styles ─────────────────────────────────────────────────────

const zoomBtnStyle: CSSProperties = {
  width: 28, height: 26, border: 'none', background: 'transparent',
  color: 'var(--hds-txt-2)', borderRadius: 6, cursor: 'pointer', fontSize: 16,
  fontFamily: "'JetBrains Mono', monospace",
}

// ─── Day column ───────────────────────────────────────────────────────────────

const DayColumn = observer(function DayColumn({ dayIdx, blocks, pph, selectedId, store, channelId }: {
  dayIdx:     number
  blocks:     Block[]
  pph:        number
  selectedId: string | null
  store:      ChannelDetailStore
  channelId:  string
}) {
  const bit       = DAY_BITS[dayIdx]
  const colBlocks = blocks.filter(b => (b.day_mask & bit) !== 0)
  const colMax    = Math.max(0, ...colBlocks.map(b => b.priority))
  const gridH     = 24 * pph
  const lineBg    = `repeating-linear-gradient(to bottom, transparent 0px, transparent ${pph - 1}px, var(--hds-line-s) ${pph - 1}px, var(--hds-line-s) ${pph}px)`

  return (
    <div style={{ flex: `1 0 ${DAY_MIN_W}px`, position: 'relative', height: gridH, borderLeft: '1px solid var(--hds-line-s)', background: lineBg }}>
      {colBlocks.slice().sort((a, b) => a.priority - b.priority).map(block => {
        const m      = BLOCK_META[block.block_type]
        const start  = t2m(block.start_time)
        const end    = endOf(block)
        const limitM = block.end_time ? 'end' : block.program_count > 0 ? 'programs' : 'fill'
        const flexEnd = limitM === 'programs'
        const top    = Math.round((start / 60) * pph)
        const height = Math.max(Math.round(((end - start) / 60) * pph), 30)
        const left   = Math.min((block.priority - 1) * 11, 33)
        const depth  = colMax > 1 ? colMax - block.priority : 0
        const isTop  = block.priority === colMax
        const sel    = selectedId === block.block_id
        const filter = isTop ? 'none' : `brightness(${(0.82 - depth * 0.1).toFixed(2)}) saturate(0.85)`
        const shadow = block.priority > 1 ? '0 8px 22px -8px rgba(0,0,0,0.65)' : 'none'
        const outline = sel ? '2px solid var(--hds-gold)' : 'none'
        const firstName = block.content[0]?.title ?? m.name
        const showMeta  = height > 62
        const metaLabel = m.name.toUpperCase() + ' · P' + block.priority + (limitM === 'programs' ? ` · ${block.program_count}×` : '') + (limitM === 'programs' ? ' · flex' : '')

        const boxStyle: CSSProperties = {
          position: 'absolute', left, right: 5,
          top, height,
          zIndex: block.priority * 2 + (sel ? 40 : 0),
          background: m.bg,
          borderRadius: limitM === 'programs' ? '7px 7px 0 0' : 7,
          border: `1px solid ${m.border}`,
          borderLeft: `3px solid ${m.edge}`,
          borderBottom: limitM === 'programs' ? `2px dashed ${m.edge}` : `1px solid ${m.border}`,
          boxShadow: shadow,
          padding: '7px 9px',
          overflow: 'hidden',
          cursor: 'pointer',
          outline,
          outlineOffset: 1,
          filter,
          transition: 'filter .12s',
        }

        return (
          <div
            key={block.block_id}
            style={boxStyle}
            onClick={() => store.select(block.block_id)}
            onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.filter = 'brightness(1.15) saturate(1)'}
            onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.filter = filter}
          >
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 6 }}>
              <span style={{ display: 'flex', alignItems: 'center', gap: 5, minWidth: 0 }}>
                <span style={{ fontWeight: 700, fontSize: 11, color: 'var(--hds-txt)', letterSpacing: '0.03em', whiteSpace: 'nowrap' }}>
                  {block.start_time}
                </span>
                {block.late_start_mins > 0 && (
                  <span style={{ fontSize: 8.5, padding: '1px 4px', borderRadius: 4, background: 'oklch(0.98 0 0 / 0.14)', color: 'var(--hds-txt)', whiteSpace: 'nowrap' }}>
                    ↧{block.late_start_mins}m
                  </span>
                )}
              </span>
              <button
                onClick={e => { e.stopPropagation(); store.select(block.block_id); store.duplicate(channelId) }}
                title="Duplicate"
                style={{ width: 18, height: 18, border: 'none', borderRadius: 5, background: 'oklch(0.98 0 0 / 0.1)', color: 'var(--hds-txt)', cursor: 'pointer', fontSize: 10, lineHeight: 1, padding: 0, opacity: 0.65, flexShrink: 0 }}
              >⧉</button>
            </div>
            <div style={{ fontSize: 12, fontWeight: 600, color: 'var(--hds-txt)', marginTop: 3, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
              {firstName}
            </div>
            {showMeta && (
              <div style={{ display: 'inline-flex', alignItems: 'center', gap: 5, marginTop: 6, fontSize: 9.5, letterSpacing: '0.08em', color: m.edge }}>
                <span style={{ width: 6, height: 6, borderRadius: 2, background: m.edge, flexShrink: 0 }} />
                {metaLabel}
              </div>
            )}
          </div>
        )
      })}
    </div>
  )
})

// ─── EPG Preview ──────────────────────────────────────────────────────────────

interface EpgSeg {
  leftPct:    number
  widthPct:   number
  bg:         string
  time:       string
  title:      string        // show title or movie title
  subtitle?:  string        // episode title (for episodes inside a show)
  epLabel?:   string        // "S01E03"
  blockId?:   string        // present when clickable
  faded?:     boolean       // true for idle gaps
}

function computeEpg(blocks: Block[], dayIdx: number): EpgSeg[] {
  const bit       = DAY_BITS[dayIdx]
  const dayBlocks = blocks.filter(b => (b.day_mask & bit) !== 0)

  function winnerAt(mn: number): Block | null {
    let w: Block | null = null
    for (const b of dayBlocks) {
      const s = t2m(b.start_time), e = endOf(b)
      if (mn >= s && mn < e && (!w || b.priority > w.priority)) w = b
    }
    return w
  }

  const segs: EpgSeg[] = []
  let curId = '∅', segStart = 0
  for (let mn = 0; mn <= 1440; mn++) {
    const w  = mn < 1440 ? winnerAt(mn) : null
    const id = w ? w.block_id : '∅'
    if (mn === 0) { curId = id; segStart = 0; continue }
    if (id !== curId || mn === 1440) {
      const b = dayBlocks.find(x => x.block_id === curId)
      const m = b ? BLOCK_META[b.block_type] : null
      segs.push({
        leftPct:  segStart / 1440 * 100,
        widthPct: (mn - segStart) / 1440 * 100,
        bg:       b ? m!.bg : 'oklch(0.18 0.012 286)',
        time:     m2t(segStart),
        title:    b ? (b.content[0]?.title ?? m!.name) : '— idle —',
        blockId:  b?.block_id,
        faded:    !b,
      })
      curId = id; segStart = mn
    }
  }
  return segs
}

const EPG_ZOOM_LEVELS = [1, 2, 3, 6] as const

class EpgErrorBoundary extends Component<{ children: ReactNode }, { error: string | null }> {
  state = { error: null }
  static getDerivedStateFromError(e: unknown) { return { error: String(e) } }
  render() {
    if (this.state.error) return (
      <div style={{ padding: '12px 24px', fontSize: 11, color: 'oklch(0.65 0.12 22)', borderTop: '1px solid var(--hds-line-s)' }}>
        EPG preview error: {this.state.error}
      </div>
    )
    return this.props.children
  }
}

// Convert an epoch-ms timestamp to minutes-since-midnight in the given IANA timezone.
// Falls back to UTC if the timezone is invalid or the date is out of range.
function msToTzMins(ms: number, tz: string): number {
  try {
    const d = new Date(ms)
    if (isNaN(d.getTime())) return 0
    const safeTz = tz || 'UTC'
    const parts = new Intl.DateTimeFormat('en-US', {
      timeZone: safeTz, hour: 'numeric', minute: 'numeric', hour12: false,
    }).formatToParts(d)
    const h = parseInt(parts.find(p => p.type === 'hour')?.value   ?? '0', 10) % 24
    const m = parseInt(parts.find(p => p.type === 'minute')?.value ?? '0', 10)
    return h * 60 + m
  } catch {
    const d = new Date(ms)
    return d.getUTCHours() * 60 + d.getUTCMinutes()
  }
}

// Returns JS getDay() (0=Sun … 6=Sat) for the date in the given timezone.
// Falls back to UTC if the timezone is invalid or the date is out of range.
function getDayInTZ(ms: number, tz: string): number {
  try {
    const d = new Date(ms)
    if (isNaN(d.getTime())) return d.getUTCDay()
    const safeTz = tz || 'UTC'
    const p = new Intl.DateTimeFormat('en-US', {
      timeZone: safeTz, year: 'numeric', month: '2-digit', day: '2-digit',
    }).formatToParts(d)
    const yr = parseInt(p.find(x => x.type === 'year')?.value  ?? '2000', 10)
    const mo = parseInt(p.find(x => x.type === 'month')?.value ?? '1',    10) - 1
    const dy = parseInt(p.find(x => x.type === 'day')?.value   ?? '1',    10)
    return new Date(yr, mo, dy).getDay()
  } catch {
    return new Date(ms).getUTCDay()
  }
}

function EpgPreview({ blocks, epgItems, epgLoading, epgDay, timezone, onDay, onRefresh, onSelectBlock }: {
  blocks:        Block[]
  epgItems:      EpgProgram[]
  epgLoading:    boolean
  epgDay:        number
  timezone:      string
  onDay:         (d: number) => void
  onRefresh:     () => void
  onSelectBlock: (blockId: string) => void
}) {
  const tz = timezone || 'UTC'

  const [zoom, setZoom] = useState<number>(1)
  const zoomIdx    = EPG_ZOOM_LEVELS.indexOf(zoom as typeof EPG_ZOOM_LEVELS[number])
  const canZoomIn  = zoomIdx < EPG_ZOOM_LEVELS.length - 1
  const canZoomOut = zoomIdx > 0

  const targetDOW = [1, 2, 3, 4, 5, 6, 0][epgDay]
  const dayItems  = epgItems.filter(item => getDayInTZ(item.wall_clock_start_ms, tz) === targetDOW)
  const hasEpg    = dayItems.length > 0
  const hasCached = hasEpg && dayItems.some(i => i.status === 'aired' || i.status === 'scheduled')

  const segs: EpgSeg[] = hasEpg
    ? dayItems.flatMap(item => {
        let startMins = msToTzMins(item.wall_clock_start_ms, tz)
        let endMins   = msToTzMins(item.wall_clock_end_ms,   tz)
        if (endMins <= startMins && endMins < 60) endMins = 1440
        endMins = Math.min(endMins, 1440)
        if (endMins <= startMins) return []
        const isFiller = item.item_type === 'filler'
        const block    = blocks.find(b => b.block_id === item.block_id)
        const meta     = isFiller ? BLOCK_META.filler : (block ? BLOCK_META[block.block_type] : BLOCK_META.episode)
        const isEp     = item.item_type === 'episode' && item.show_title
        const epLabel  = isEp && item.season != null && item.episode_num != null
          ? `S${String(item.season).padStart(2, '0')}E${String(item.episode_num).padStart(2, '0')}`
          : undefined
        return [{
          leftPct:  startMins / 1440 * 100,
          widthPct: Math.max((endMins - startMins) / 1440 * 100, 0.25),
          bg:       meta.bg,
          time:     m2t(startMins),
          title:    isEp ? item.show_title! : isFiller ? 'Filler' : item.title,
          subtitle: isEp ? item.title : undefined,
          epLabel,
          blockId:  item.block_id || undefined,
          faded:    isFiller,
        }]
      })
    : computeEpg(blocks, epgDay)

  const statusText = epgLoading
    ? 'loading…'
    : hasEpg
      ? `${hasCached ? 'live' : 'simulated'} · ${dayItems.length} programs`
      : 'block coverage · no scheduled programs'

  const btnBase: React.CSSProperties = { padding: '3px 7px', border: '1px solid var(--hds-line)', borderRadius: 5, background: 'transparent', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer', color: 'var(--hds-txt-2)' }

  return (
    <div style={{ flexShrink: 0, borderTop: '1px solid var(--hds-line-s)', background: 'oklch(0.17 0.018 286 / 0.7)', padding: '12px 24px 16px' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, marginBottom: 10 }}>
        <span style={{ fontSize: 10, letterSpacing: '0.22em', color: 'var(--hds-txt-2)', whiteSpace: 'nowrap' }}>EPG PREVIEW</span>
        <span style={{ flex: 1, fontSize: 10, color: hasCached ? 'oklch(0.62 0.1 145)' : hasEpg ? 'oklch(0.62 0.1 220)' : 'var(--hds-txt-3)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
          {statusText}
        </span>

        {/* Zoom controls */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 2, flexShrink: 0 }}>
          <button onClick={() => canZoomOut && setZoom(EPG_ZOOM_LEVELS[zoomIdx - 1])} disabled={!canZoomOut} title="Zoom out" style={{ ...btnBase, opacity: canZoomOut ? 1 : 0.35, cursor: canZoomOut ? 'pointer' : 'default' }}>−</button>
          <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', width: 24, textAlign: 'center', letterSpacing: '0.05em' }}>{zoom}×</span>
          <button onClick={() => canZoomIn  && setZoom(EPG_ZOOM_LEVELS[zoomIdx + 1])} disabled={!canZoomIn}  title="Zoom in"  style={{ ...btnBase, opacity: canZoomIn  ? 1 : 0.35, cursor: canZoomIn  ? 'pointer' : 'default' }}>+</button>
        </div>

        <button onClick={onRefresh} title="Refresh EPG" style={{ ...btnBase, color: epgLoading ? 'var(--hds-txt-3)' : 'var(--hds-txt-2)', cursor: epgLoading ? 'default' : 'pointer', opacity: epgLoading ? 0.5 : 1 }}>↺</button>
        <div style={{ display: 'flex', gap: 3, background: 'var(--hds-bg-3)', borderRadius: 8, padding: 3, flexShrink: 0 }}>
          {DAYS.map(([short], i) => {
            const active = epgDay === i
            return (
              <button key={short} onClick={() => onDay(i)} style={{ padding: '5px 9px', border: 'none', borderRadius: 6, background: active ? 'var(--hds-gold)' : 'transparent', color: active ? 'oklch(0.2 0.04 70)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, letterSpacing: '0.1em', cursor: 'pointer' }}>
                {short}
              </button>
            )
          })}
        </div>
      </div>

      {/* Scrollable timeline wrapper */}
      <div style={{ borderRadius: 8, border: '1px solid var(--hds-line-s)', overflowX: zoom > 1 ? 'auto' : 'hidden', overflowY: 'hidden' }}>
        <div style={{ position: 'relative', height: 68, width: `${zoom * 100}%`, minWidth: '100%', background: 'oklch(0.13 0.014 286)' }}>
          {segs.map((s, i) => {
            const clickable = !!s.blockId && !s.faded
            const tooltip   = [s.time, s.title, s.subtitle, s.epLabel].filter(Boolean).join('  ·  ')
            return (
              <div
                key={i}
                title={tooltip}
                onClick={() => s.blockId && onSelectBlock(s.blockId)}
                style={{
                  position: 'absolute', top: 0, bottom: 0,
                  left: `${s.leftPct}%`, width: `${s.widthPct}%`,
                  background: s.bg,
                  borderLeft: '1px solid oklch(0.13 0.014 286)',
                  padding: '7px 8px',
                  overflow: 'hidden',
                  cursor: clickable ? 'pointer' : 'default',
                  opacity: s.faded ? 0.5 : 1,
                  transition: 'filter .1s',
                }}
                onMouseEnter={e => { if (clickable) (e.currentTarget as HTMLDivElement).style.filter = 'brightness(1.2)' }}
                onMouseLeave={e => { if (clickable) (e.currentTarget as HTMLDivElement).style.filter = '' }}
              >
                <div style={{ fontSize: 9, color: s.faded ? 'var(--hds-txt-3)' : 'oklch(0.78 0.04 286)', letterSpacing: '0.06em', lineHeight: 1 }}>
                  {s.time}
                </div>
                <div style={{ fontSize: 11, fontWeight: 700, color: s.faded ? 'var(--hds-txt-3)' : 'var(--hds-txt)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', marginTop: 3, lineHeight: 1.2 }}>
                  {s.title}
                </div>
                {(s.subtitle || s.epLabel) && (
                  <div style={{ display: 'flex', alignItems: 'center', gap: 5, marginTop: 3, minWidth: 0 }}>
                    {s.epLabel && (
                      <span style={{ flexShrink: 0, fontSize: 8.5, letterSpacing: '0.06em', padding: '1px 4px', borderRadius: 3, background: 'oklch(0.98 0 0 / 0.13)', color: 'oklch(0.82 0.05 286)' }}>
                        {s.epLabel}
                      </span>
                    )}
                    {s.subtitle && (
                      <span style={{ fontSize: 10, color: 'var(--hds-txt-2)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
                        {s.subtitle}
                      </span>
                    )}
                  </div>
                )}
              </div>
            )
          })}
          {segs.length === 0 && !epgLoading && (
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100%', color: 'var(--hds-txt-3)', fontSize: 11 }}>No programs scheduled for this day</div>
          )}
          {epgLoading && (
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100%', color: 'var(--hds-txt-3)', fontSize: 11 }}>Loading…</div>
          )}
        </div>
      </div>
    </div>
  )
}

// ─── Channel defaults panel ───────────────────────────────────────────────────

const ChannelDefaultsPanel = observer(function ChannelDefaultsPanel({ channel, channelId, store }: {
  channel:   Channel | undefined
  channelId: string
  store:     ChannelDetailStore
}) {
  const [addOpen, setAddOpen]   = useState(false)
  const [addListId, setAddListId] = useState('')
  const [addAdv, setAddAdv]     = useState<FillerEntryAdvancement>('sequential')
  const [addWeight, setAddWeight] = useState(1)

  const selectionMode = channel?.default_filler_selection ?? 'round_robin'
  const entries       = channel?.default_filler_entries   ?? []
  const showWeight    = selectionMode === 'weighted'

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      <div style={{ padding: '18px 20px 14px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 15, letterSpacing: '0.04em' }}>Channel Settings</span>
      </div>

      <div style={{ flex: 1, overflow: 'auto', padding: 20 }} className="scrollbar-dark">
        <div style={{ fontSize: 12.5, color: 'var(--hds-txt-2)', lineHeight: 1.6, marginBottom: 22 }}>
          Select a block to edit it, or press <span style={{ color: 'var(--hds-gold)' }}>Add Block</span> to create one.
        </div>

        {/* ── Channel identity ─────────────────────────────────────────────── */}
        <SectionLabel>CHANNEL</SectionLabel>
        <div style={{ display: 'flex', gap: 7, marginBottom: 8 }}>
          <div style={{ flex: 1 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>NAME</div>
            <input value={store.channelDraftName} onChange={e => store.setChannelDraft({ name: e.target.value })} style={inputStyle} />
          </div>
          <div style={{ width: 64 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>CH #</div>
            <input type="number" min={1} value={store.channelDraftNumber} onChange={e => store.setChannelDraft({ number: Math.max(1, +e.target.value || 1) })} style={inputStyle} />
          </div>
        </div>
        <div style={{ marginBottom: 8 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>TIMEZONE</div>
          <input value={store.channelDraftTimezone} onChange={e => store.setChannelDraft({ timezone: e.target.value })} style={inputStyle} placeholder="UTC" />
        </div>
        <div style={{ marginBottom: 14 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>EPG SEED</div>
          <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
            <input type="number" min={0} value={store.channelDraftSeed} onChange={e => store.setChannelDraft({ seed: Math.max(0, +e.target.value || 0) })} style={{ ...inputStyle, flex: 1 }} />
            <button
              onClick={() => store.setChannelDraft({ seed: Math.floor(Math.random() * 99999) + 1 })}
              title="Randomize seed"
              style={{ padding: '5px 9px', border: '1px solid var(--hds-line)', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer' }}
            >⚄</button>
          </div>
          <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
            Controls the starting position for EPG simulation when no live schedule exists. Change to get a different ordering.
          </div>
        </div>

        {store.channelSaveErr && (
          <div style={{ padding: '7px 10px', marginBottom: 10, borderRadius: 6, background: 'oklch(0.2 0.05 22 / 0.3)', border: '1px solid oklch(0.4 0.1 22 / 0.4)', color: 'oklch(0.72 0.16 22)', fontSize: 11 }}>
            {store.channelSaveErr}
          </div>
        )}

        <button
          onClick={() => store.saveChannel(channelId)}
          disabled={store.channelSaving}
          style={{ width: '100%', padding: '9px 0', border: 'none', borderRadius: 8, background: store.channelDirty ? 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))' : 'var(--hds-bg-3)', color: store.channelDirty ? 'oklch(0.2 0.04 70)' : 'var(--hds-txt-3)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 13, cursor: store.channelSaving ? 'default' : 'pointer', marginBottom: 22, opacity: store.channelSaving ? 0.6 : 1, transition: 'background 0.15s, color 0.15s' }}
        >
          {store.channelSaving ? 'Saving…' : 'Save Channel'}
        </button>

        <SectionLabel>DEFAULT FILLER</SectionLabel>
        <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginBottom: 12, lineHeight: 1.55 }}>
          Used when a block has no filler lists of its own.
        </div>

        {entries.length > 1 && (
          <div style={{ marginBottom: 12 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>SELECT BY</div>
            <select value={selectionMode} onChange={e => store.saveChannelFiller(channelId, { default_filler_selection: e.target.value as FillerSelectionMode })} style={inputStyle}>
              {FILLER_SEL_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
          </div>
        )}

        <div style={{ display: 'flex', flexDirection: 'column', gap: 7, marginBottom: 10 }}>
          {entries.map(entry => (
            <FillerEntryRow
              key={entry.id}
              entry={entry}
              showWeight={showWeight}
              onAdvancement={adv => store.updateChannelFiller(channelId, entry.id, { advancement: adv })}
              onWeight={w   => store.updateChannelFiller(channelId, entry.id, { weight: w })}
              onRemove={()  => store.removeChannelFiller(channelId, entry.id)}
            />
          ))}
        </div>

        {entries.length === 0 && !addOpen && (
          <div style={{ textAlign: 'center', padding: '10px 6px', color: 'var(--hds-txt-3)', fontSize: 11 }}>
            No default filler lists configured
          </div>
        )}

        <button
          onClick={() => setAddOpen(o => !o)}
          style={{ padding: '6px 12px', border: '1px solid var(--hds-line)', borderRadius: 7, background: 'transparent', color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer', marginBottom: addOpen ? 8 : 0 }}
        >
          {addOpen ? '✕ Cancel' : '+ Add filler list'}
        </button>

        {addOpen && (
          <div style={{ padding: '11px 12px', background: 'oklch(0.16 0.016 286)', border: '1px solid var(--hds-line)', borderRadius: 9 }}>
            <div style={{ display: 'flex', gap: 7, alignItems: 'center', flexWrap: 'wrap' }}>
              <select value={addListId} onChange={e => setAddListId(e.target.value)} style={{ ...filterInputStyle, flex: '1 1 140px' }}>
                <option value="">Select filler list…</option>
                {store.allFillerLists.map(f => <option key={f.filler_list_id} value={f.filler_list_id}>{f.title}</option>)}
              </select>
              <select value={addAdv} onChange={e => setAddAdv(e.target.value as FillerEntryAdvancement)} style={{ ...filterInputStyle, width: 96 }}>
                {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
              </select>
              {showWeight && (
                <input type="number" min={1} value={addWeight} onChange={e => setAddWeight(Math.max(1, +e.target.value || 1))} style={{ ...filterInputStyle, width: 48 }} placeholder="Wt" />
              )}
              <button
                onClick={() => { if (addListId) { store.addChannelFiller(channelId, { filler_list_id: addListId, advancement: addAdv, weight: addWeight }); setAddListId(''); setAddOpen(false) } }}
                disabled={!addListId}
                style={{ padding: '5px 12px', border: 'none', borderRadius: 6, background: 'var(--hds-violet)', color: 'oklch(0.15 0.02 286)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700, cursor: addListId ? 'pointer' : 'default', opacity: addListId ? 1 : 0.4 }}
              >
                Add
              </button>
            </div>
          </div>
        )}

        {store.channelFillerErr && (
          <div style={{ marginTop: 8, fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{store.channelFillerErr}</div>
        )}
      </div>
    </div>
  )
})

// ─── Block editor (existing block) ───────────────────────────────────────────

const BlockEditor = observer(function BlockEditor({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const block = store.editing
  const d     = store.draft
  if (!block) return null

  const m        = BLOCK_META[d.block_type]
  const limitMode = getLimitMode(d)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      {/* Header */}
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '18px 20px 14px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 15, letterSpacing: '0.04em' }}>
          {m.name} Block
        </span>
        <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
          <button onClick={() => { store.modalOpen = true }} title="Open in modal" style={{ width: 28, height: 28, border: 'none', borderRadius: 7, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 14 }}>⊞</button>
          <button onClick={() => store.closeEditor()} style={{ width: 28, height: 28, border: 'none', borderRadius: 7, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 15 }}>×</button>
        </div>
      </div>

      <EditorForm channelId={channelId} store={store} limitMode={limitMode} />

      {/* Footer */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '16px 20px', borderTop: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle}>
          {store.saving ? 'Saving…' : 'Save Changes'}
        </button>
        <button onClick={() => store.duplicate(channelId)} style={ghostBtnStyle}>⧉ Duplicate</button>
        <div style={{ flex: 1 }} />
        <button onClick={() => store.deleteBlock(channelId, block.block_id)} style={dangerBtnStyle}>Delete</button>
      </div>

      {store.saveErr && (
        <div style={{ padding: '8px 20px', fontSize: 11, color: 'oklch(0.72 0.16 22)', background: 'oklch(0.2 0.05 22 / 0.3)', borderTop: '1px solid oklch(0.4 0.1 22 / 0.4)', flexShrink: 0 }}>
          {store.saveErr}
        </div>
      )}
    </div>
  )
})

// ─── New block editor ────────────────────────────────────────────────────────

const NewBlockEditor = observer(function NewBlockEditor({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const d       = store.draft
  const m       = BLOCK_META[d.block_type]
  const limitMode = getLimitMode(d)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '18px 20px 14px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 15, letterSpacing: '0.04em' }}>
          {m.name} Block
        </span>
        <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
          <button onClick={() => { store.modalOpen = true }} title="Open in modal" style={{ width: 28, height: 28, border: 'none', borderRadius: 7, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 14 }}>⊞</button>
          <button onClick={() => store.closeEditor()} style={{ width: 28, height: 28, border: 'none', borderRadius: 7, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 15 }}>×</button>
        </div>
      </div>

      <EditorForm channelId={channelId} store={store} limitMode={limitMode} />

      <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '16px 20px', borderTop: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle}>
          {store.saving ? 'Saving…' : 'Create Block'}
        </button>
        <button onClick={() => store.closeEditor()} style={ghostBtnStyle}>Cancel</button>
      </div>

      {store.saveErr && (
        <div style={{ padding: '8px 20px', fontSize: 11, color: 'oklch(0.72 0.16 22)', background: 'oklch(0.2 0.05 22 / 0.3)', borderTop: '1px solid oklch(0.4 0.1 22 / 0.4)', flexShrink: 0 }}>
          {store.saveErr}
        </div>
      )}
    </div>
  )
})

// ─── Accordion section ────────────────────────────────────────────────────────

function AccordionSection({ title, badge, open, onToggle, children }: {
  title:    string
  badge?:   ReactNode
  open:     boolean
  onToggle: () => void
  children: ReactNode
}) {
  return (
    <div style={{ borderRadius: 9, border: '1px solid var(--hds-line-s)', marginBottom: 8, overflow: 'hidden' }}>
      <button
        onClick={onToggle}
        style={{ display: 'flex', alignItems: 'center', gap: 10, width: '100%', padding: '9px 13px', background: 'oklch(0.2 0.018 286 / 0.6)', border: 'none', cursor: 'pointer' }}
      >
        <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', display: 'inline-block', transition: 'transform .15s', transform: open ? 'rotate(90deg)' : 'none' }}>▶</span>
        <span style={{ flex: 1, fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", textAlign: 'left' }}>{title}</span>
        {badge}
      </button>
      {open && (
        <div style={{ padding: '14px 14px 16px', borderTop: '1px solid var(--hds-line-s)' }}>
          {children}
        </div>
      )}
    </div>
  )
}

// ─── Editor form (shared between new + existing) ──────────────────────────────

const EditorForm = observer(function EditorForm({ channelId, store, limitMode }: {
  channelId: string
  store:     ChannelDetailStore
  limitMode: LimitMode
}) {
  const d   = store.draft
  const m   = BLOCK_META[d.block_type]
  const sec = store.openSections
  const tog = (s: string) => store.toggleSection(s)

  const limitHelp = limitMode === 'programs'
    ? 'Plays this many programs then yields. End time flexes with real runtime.'
    : limitMode === 'end'
    ? 'Hard cutoff at end time. Whatever is playing is cut off when the clock hits it.'
    : 'Fills until midnight, or until a higher-priority block takes over.'

  const contentCount  = store.draftContent.length
  const fillerCount   = store.draftFillerEntries.length

  return (
    <div style={{ flex: 1, minHeight: 0, overflow: 'auto', padding: '12px 12px 20px' }} className="scrollbar-dark">


      {/* ── SCHEDULE ── */}
      <AccordionSection title="SCHEDULE" open={sec.schedule} onToggle={() => tog('schedule')}>
        {/* Block type */}
        <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 7 }}>BLOCK TYPE</div>
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(4,1fr)', gap: 6, marginBottom: 16 }}>
          {(['episode', 'movie', 'premier', 'filler'] as BlockType[]).map(t => {
            const tm = BLOCK_META[t]
            const on = d.block_type === t
            return (
              <button key={t} onClick={() => store.setDraft('block_type', t)} style={{ padding: '8px 4px', border: `1px solid ${on ? tm.edge : 'var(--hds-line)'}`, borderRadius: 8, background: on ? tm.solid : 'var(--hds-bg-3)', color: on ? 'var(--hds-txt)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontWeight: 600, fontSize: 11.5, cursor: 'pointer', transition: '.12s' }}>
                {tm.name}
              </button>
            )
          })}
        </div>

        {/* Days */}
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 7 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)' }}>DAYS</div>
          <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)' }}>drag to paint</span>
        </div>
        <div style={{ display: 'flex', gap: 5, marginBottom: 7, userSelect: 'none' }}>
          {DAYS.map(([short], i) => {
            const bit = DAY_BITS[i]
            const on  = (d.day_mask & bit) !== 0
            return (
              <div key={short} onMouseDown={() => store.dayDown(i)} onMouseEnter={() => store.dayEnter(i)}
                style={{ flex: 1, textAlign: 'center', padding: '9px 0', border: `1px solid ${on ? 'oklch(0.7 0.12 84 / 0.5)' : 'var(--hds-line)'}`, borderRadius: 7, background: on ? 'oklch(0.55 0.1 84 / 0.22)' : 'var(--hds-bg-3)', color: on ? 'var(--hds-gold)' : 'var(--hds-txt-2)', fontWeight: 700, fontSize: 11, cursor: 'pointer', transition: '.1s' }}>
                {short}
              </div>
            )
          })}
        </div>
        <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
          {[['Weekdays', 62], ['Weekend', 65], ['Every day', 127], ['Clear', 0]].map(([label, mask]) => (
            <button key={label} onClick={() => store.setDraft('day_mask', mask as number)} style={{ padding: '4px 9px', border: '1px solid var(--hds-line)', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer' }}>
              {label}
            </button>
          ))}
        </div>
      </AccordionSection>

      {/* ── TIMING ── */}
      <AccordionSection title="TIMING" open={sec.timing} onToggle={() => tog('timing')}>
        {/* Start + Late + Early */}
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: 9, marginBottom: 16 }}>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>START TIME</div>
            <input type="time" value={d.start_time} onChange={e => store.setDraft('start_time', e.target.value)} style={inputStyle} />
          </div>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>LATE START</div>
            <select value={String(d.late_start_mins)} onChange={e => store.setDraft('late_start_mins', +e.target.value)} style={inputStyle}>
              {DELAY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
          </div>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>EARLY START</div>
            <select value={String(d.early_start_secs)} onChange={e => store.setDraft('early_start_secs', +e.target.value)} style={inputStyle}>
              {EARLY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
          </div>
        </div>

        {/* Stop condition */}
        <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 7 }}>STOP CONDITION</div>
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3,1fr)', gap: 6, marginBottom: 9 }}>
          {([['programs', '# Programs'], ['end', 'End time'], ['fill', 'Fill day']] as [LimitMode, string][]).map(([k, label]) => {
            const on = limitMode === k
            return (
              <button key={k} onClick={() => store.setLimitMode(k)} style={{ padding: '8px 4px', border: `1px solid ${on ? 'oklch(0.7 0.12 84 / 0.5)' : 'var(--hds-line)'}`, borderRadius: 8, background: on ? 'oklch(0.55 0.1 84 / 0.2)' : 'var(--hds-bg-3)', color: on ? 'var(--hds-gold)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontWeight: 600, fontSize: 11, cursor: 'pointer', transition: '.12s' }}>
                {label}
              </button>
            )
          })}
        </div>
        {limitMode === 'programs' && (
          <input type="number" min={1} value={d.program_count} onChange={e => store.setDraft('program_count', Math.max(1, +e.target.value || 1))} placeholder="number of programs" style={{ ...inputStyle, width: '100%', marginBottom: 7 }} />
        )}
        {limitMode === 'end' && (
          <input type="time" value={d.end_time ?? ''} onChange={e => store.setDraft('end_time', e.target.value)} style={{ ...inputStyle, width: '100%', marginBottom: 7 }} />
        )}
        <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginBottom: 14, lineHeight: 1.55 }}>{limitHelp}</div>

        {/* Align start */}
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 5 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)' }}>ALIGN START</div>
          {d.align_to_mins > 0 && (
            <div style={{ display: 'flex', border: '1px solid var(--hds-line)', borderRadius: 6, overflow: 'hidden' }}>
              {(['block', 'episode'] as const).map(scope => {
                const on = (d.start_scope ?? 'block') === scope
                return (
                  <button key={scope} onClick={() => store.setDraft('start_scope', scope)}
                    style={{ padding: '3px 8px', border: 'none', background: on ? 'var(--hds-violet)' : 'var(--hds-bg-3)', color: on ? '#fff' : 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 9, cursor: 'pointer', textTransform: 'uppercase', letterSpacing: '0.08em' }}>
                    {scope}
                  </button>
                )
              })}
            </div>
          )}
        </div>
        <select value={String(d.align_to_mins)} onChange={e => store.setDraft('align_to_mins', +e.target.value)} style={{ ...inputStyle, width: '100%', marginBottom: 6 }}>
          {ALIGN_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
        </select>
        <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', lineHeight: 1.55 }}>
          {(d.start_scope ?? 'block') === 'episode'
            ? 'Snaps each episode to the next time boundary. Early/late start define the tolerance window.'
            : 'Snaps the first program of the block to the next time boundary.'}
        </div>
      </AccordionSection>

      {/* ── PLAYBACK ── */}
      <AccordionSection title="PLAYBACK" open={sec.playback} onToggle={() => tog('playback')}>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 9, marginBottom: 14 }}>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>PRIORITY</div>
            <input type="number" min={1} value={d.priority} onChange={e => store.setDraft('priority', Math.max(1, +e.target.value || 1))} style={inputStyle} />
            <div style={{ fontSize: 9, color: 'var(--hds-txt-3)', marginTop: 4 }}>higher wins conflicts</div>
          </div>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>MAX RATING</div>
            <select value={d.max_content_rating} onChange={e => store.setDraft('max_content_rating', e.target.value)} style={inputStyle}>
              {RATINGS.map(r => <option key={r} value={r}>{r || 'No limit'}</option>)}
            </select>
          </div>
        </div>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 9 }}>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>ORDER</div>
            <select value={d.advancement} onChange={e => store.setDraft('advancement', e.target.value as Advancement)} style={inputStyle}>
              <optgroup label="Standard">
                <option value="sequential">Sequential</option>
                <option value="shuffle">Shuffle</option>
                <option value="smart_shuffle">Smart Shuffle</option>
              </optgroup>
              <optgroup label="Reruns">
                <option value="rerun_shuffle">Rerun Shuffle</option>
                <option value="rerun_smart">Rerun Smart</option>
              </optgroup>
            </select>
          </div>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>CURSOR</div>
            <select value={d.cursor_scope} onChange={e => store.setDraft('cursor_scope', e.target.value as CursorScope)} style={inputStyle}>
              <option value="block">Per block</option>
              <option value="channel">Per channel</option>
              <option value="global">Global</option>
            </select>
          </div>
        </div>
        {(d.advancement === 'smart_shuffle' || d.advancement === 'rerun_smart') && (
          <div style={{ marginTop: 9 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>COOLDOWN THRESHOLD</div>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
              <input type="range" min={5} max={80} step={5}
                value={d.smart_pct ?? 30}
                onChange={e => store.setDraft('smart_pct', Number(e.target.value))}
                style={{ flex: 1 }} />
              <span style={{ fontSize: 11, color: 'var(--hds-txt-2)', minWidth: 36 }}>{d.smart_pct ?? 30}%</span>
            </div>
            <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 3 }}>
              Episodes won't repeat until {d.smart_pct ?? 30}% of the pool has played since last air
            </div>
          </div>
        )}
        {(d.advancement === 'rerun_shuffle' || d.advancement === 'rerun_smart') && (
          <div style={{ marginTop: 9 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>NO HISTORY BEHAVIOR</div>
            <select
              value={d.no_history_behavior ?? 'normal'}
              onChange={e => store.setDraft('no_history_behavior', e.target.value as NoHistoryBehavior)}
              style={inputStyle}
            >
              {NO_HISTORY_OPTS.map(([v, label]) => <option key={v} value={v}>{label}</option>)}
            </select>
            <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 3 }}>
              {NO_HISTORY_OPTS.find(([v]) => v === (d.no_history_behavior ?? 'normal'))?.[2]}
            </div>
          </div>
        )}
      </AccordionSection>

      {/* ── CONTENT ── */}
      <AccordionSection
        title="CONTENT"
        open={sec.content}
        onToggle={() => tog('content')}
        badge={contentCount > 0 ? <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.04em' }}>{contentCount}</span> : undefined}
      >
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 9 }}>
          <span />
          {(store.editing || store.isNewMode) && (
            <button onClick={() => store.openPicker()} style={{ color: 'var(--hds-violet)', background: 'transparent', border: 'none', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer', padding: '2px 4px' }}>+ Add</button>
          )}
        </div>

        <div
          onDragOver={e => e.preventDefault()}
          onDrop={e => { e.preventDefault(); if (store.dragItem) { const sh = store.pickerShows.find(s => s.show_id === store.dragItem); store.addContent(channelId, { content_type: 'show', content_id: store.dragItem!, title: sh?.title }); store.dragItem = null } }}
          style={{ display: 'flex', flexDirection: 'column', gap: 6, minHeight: 40, padding: store.dragItem ? 8 : 0, border: store.dragItem ? '1px dashed var(--hds-violet)' : '1px solid transparent', borderRadius: 9, transition: '.12s' }}
        >
          {store.draftContent.map(item => {
            const dot       = BLOCK_META[item.content_type === 'movie' ? 'movie' : 'episode'].edge
            const canReset  = item.id > 0 && item.content_type === 'show' && !!store.editing
            const isRerun   = store.draft.advancement === 'rerun_shuffle' || store.draft.advancement === 'rerun_smart'
            const showRerunControls = isRerun && item.content_type === 'show'
            const miniInp: React.CSSProperties = { width: 36, padding: '2px 4px', background: 'var(--hds-bg)', border: '1px solid var(--hds-line)', borderRadius: 4, color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, textAlign: 'center' }
            return (
              <div key={item.id} style={{ background: 'var(--hds-bg-3)', border: `1px solid ${item.id < 0 ? 'oklch(0.55 0.12 290 / 0.6)' : 'var(--hds-line-s)'}`, borderRadius: 7, overflow: 'hidden' }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 9, padding: '8px 10px' }}>
                  <span style={{ color: 'var(--hds-txt-3)', fontSize: 13 }}>⋮⋮</span>
                  <span style={{ width: 7, height: 7, borderRadius: 2, background: dot, flexShrink: 0 }} />
                  <span style={{ flex: 1, fontSize: 12, fontWeight: 500, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{item.title || item.content_id}</span>
                  {canReset && (
                    <button
                      onClick={() =>
                        api.resetBlockContentCursor(channelId, store.editing!.block_id, item.id)
                          .then(() => store.loadEpg(channelId, true))
                          .catch(e => alert(`Cursor reset failed: ${e?.message ?? e}`))
                      }
                      title="Reset cursor to beginning"
                      style={{ width: 22, height: 22, border: 'none', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 12 }}
                    >⏮</button>
                  )}
                  <button onClick={() => store.removeContent(channelId, item.id)} style={{ width: 22, height: 22, border: 'none', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13 }}>×</button>
                </div>
                {showRerunControls && (
                  <div style={{ display: 'flex', gap: 16, padding: '4px 10px 8px', borderTop: '1px solid var(--hds-line-s)' }}>
                    <label style={{ display: 'flex', alignItems: 'center', gap: 5, fontSize: 9.5, letterSpacing: '0.1em', color: 'var(--hds-txt-3)' }}>
                      WEIGHT
                      <input type="number" min={1} max={99} value={item.weight ?? 1} style={miniInp}
                        onChange={e => store.updateContentField(channelId, item.id, 'weight', Number(e.target.value))} />
                    </label>
                    <label style={{ display: 'flex', alignItems: 'center', gap: 5, fontSize: 9.5, letterSpacing: '0.1em', color: 'var(--hds-txt-3)' }}>
                      RUN
                      <input type="number" min={1} max={99} value={item.run_count ?? 1} style={miniInp}
                        onChange={e => store.updateContentField(channelId, item.id, 'run_count', Number(e.target.value))} />
                    </label>
                  </div>
                )}
              </div>
            )
          })}
          {(store.editing || store.isNewMode) && store.draftContent.length === 0 && !store.pickerOpen && (
            <div style={{ textAlign: 'center', padding: 6, color: 'var(--hds-txt-3)', fontSize: 11 }}>Drag shows or movies here, or use + Add</div>
          )}
        </div>

        {store.pickerOpen && (store.editing || store.isNewMode) && (
          <ContentPicker channelId={channelId} store={store} />
        )}
      </AccordionSection>

      {/* ── FILLER ── */}
      <AccordionSection
        title="FILLER"
        open={sec.filler}
        onToggle={() => tog('filler')}
        badge={fillerCount > 0 ? <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.04em' }}>{fillerCount}</span> : undefined}
      >
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 9 }}>
          <span />
          {(store.editing || store.isNewMode) && (
            <button
              onClick={() => { store.fillerPickerOpen = !store.fillerPickerOpen }}
              style={{ color: 'var(--hds-violet)', background: 'transparent', border: 'none', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer', padding: '2px 4px' }}
            >
              {store.fillerPickerOpen ? '✕ Close' : '+ Add list'}
            </button>
          )}
        </div>

        {store.draftFillerEntries.length > 0 && (
          <>
            {store.draftFillerEntries.length > 1 && (
              <div style={{ marginBottom: 10 }}>
                <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>SELECT BY</div>
                <select value={d.filler_selection} onChange={e => store.setDraft('filler_selection', e.target.value as FillerSelectionMode)} style={inputStyle}>
                  {FILLER_SEL_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                </select>
              </div>
            )}
            <div style={{ display: 'flex', flexDirection: 'column', gap: 6, marginBottom: 10 }}>
              {store.draftFillerEntries.map(entry => (
                <FillerEntryRow
                  key={entry.id}
                  entry={entry}
                  showWeight={d.filler_selection === 'weighted'}
                  onAdvancement={adv => store.updateBlockFiller(channelId, store.editing?.block_id ?? '', entry.id, { advancement: adv })}
                  onWeight={w   => store.updateBlockFiller(channelId, store.editing?.block_id ?? '', entry.id, { weight: w })}
                  onRemove={()  => store.removeBlockFiller(channelId, store.editing?.block_id ?? '', entry.id)}
                />
              ))}
            </div>
          </>
        )}

        {(store.editing || store.isNewMode) && store.draftFillerEntries.length === 0 && !store.fillerPickerOpen && (
          <div style={{ textAlign: 'center', padding: '6px 0', color: 'var(--hds-txt-3)', fontSize: 11 }}>
            No filler lists — channel default will be used
          </div>
        )}

        {store.fillerPickerOpen && (store.editing || store.isNewMode) && (
          <FillerAddPanel channelId={channelId} store={store} />
        )}

        <div style={{ marginTop: 12 }}>
          <button
            onClick={() => store.setDraft('inter_filler', !d.inter_filler)}
            style={{
              display: 'flex', alignItems: 'center', justifyContent: 'space-between',
              width: '100%', padding: '8px 10px',
              border: `1px solid ${d.inter_filler ? 'oklch(0.7 0.12 84 / 0.5)' : 'var(--hds-line)'}`,
              borderRadius: 7, background: d.inter_filler ? 'oklch(0.55 0.1 84 / 0.12)' : 'var(--hds-bg-3)',
              cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace",
            }}
          >
            <span style={{ fontSize: 11, color: 'var(--hds-txt-2)' }}>Filler between programs</span>
            <span style={{ fontSize: 10, fontWeight: 700, letterSpacing: '0.08em', color: d.inter_filler ? 'var(--hds-gold)' : 'var(--hds-txt-3)' }}>{d.inter_filler ? 'ON' : 'OFF'}</span>
          </button>
          <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 6, lineHeight: 1.55 }}>
            When off, filler only fills leftover time at end of block. When on, also fills between programs.
          </div>
        </div>
      </AccordionSection>
    </div>
  )
})

// ─── Filler entry row ─────────────────────────────────────────────────────────

function FillerEntryRow({ entry, showWeight, onAdvancement, onWeight, onRemove }: {
  entry:         FillerEntry
  showWeight:    boolean
  onAdvancement: (a: FillerEntryAdvancement) => void
  onWeight:      (w: number) => void
  onRemove:      () => void
}) {
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '8px 10px', background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)', borderRadius: 8 }}>
      <span style={{ width: 7, height: 7, borderRadius: 2, background: BLOCK_META.filler.edge, flexShrink: 0 }} />
      <span style={{ flex: 1, fontSize: 12, fontWeight: 500, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{entry.title || entry.filler_list_id}</span>
      <select
        value={entry.advancement}
        onChange={e => onAdvancement(e.target.value as FillerEntryAdvancement)}
        style={{ ...filterInputStyle, width: 92 }}
      >
        {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
      </select>
      {showWeight && (
        <input
          type="number" min={1} value={entry.weight}
          onChange={e => onWeight(Math.max(1, +e.target.value || 1))}
          style={{ ...filterInputStyle, width: 44, textAlign: 'center' }}
          title="Weight"
        />
      )}
      <button onClick={onRemove} style={{ width: 22, height: 22, border: 'none', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13, flexShrink: 0 }}>×</button>
    </div>
  )
}

// ─── Filler add panel ─────────────────────────────────────────────────────────

const FillerAddPanel = observer(function FillerAddPanel({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const [listId, setListId] = useState('')
  const [advancement, setAdvancement] = useState<FillerEntryAdvancement>('sequential')
  const [weight, setWeight] = useState(1)
  const showWeight = store.draft.filler_selection === 'weighted'

  return (
    <div style={{ marginTop: 10, padding: '11px 12px', background: 'oklch(0.16 0.016 286)', border: '1px solid var(--hds-line)', borderRadius: 9 }}>
      <div style={{ display: 'flex', gap: 7, alignItems: 'center', flexWrap: 'wrap' }}>
        <select value={listId} onChange={e => setListId(e.target.value)} style={{ ...filterInputStyle, flex: '1 1 140px' }}>
          <option value="">Select filler list…</option>
          {store.allFillerLists.map(f => <option key={f.filler_list_id} value={f.filler_list_id}>{f.title}</option>)}
        </select>
        <select value={advancement} onChange={e => setAdvancement(e.target.value as FillerEntryAdvancement)} style={{ ...filterInputStyle, width: 96 }}>
          {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
        </select>
        {showWeight && (
          <input type="number" min={1} value={weight} onChange={e => setWeight(Math.max(1, +e.target.value || 1))} style={{ ...filterInputStyle, width: 48 }} title="Weight" placeholder="Wt" />
        )}
        <button
          onClick={() => { if (listId) { store.addBlockFiller(channelId, { filler_list_id: listId, advancement, weight }).then(() => setListId('')) } }}
          disabled={!listId || store.fillerSaving}
          style={{ padding: '5px 12px', border: 'none', borderRadius: 6, background: 'var(--hds-violet)', color: 'oklch(0.15 0.02 286)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700, cursor: (listId && !store.fillerSaving) ? 'pointer' : 'default', opacity: (listId && !store.fillerSaving) ? 1 : 0.4 }}
        >
          {store.fillerSaving ? '…' : 'Add'}
        </button>
      </div>
    </div>
  )
})

// ─── Content picker ───────────────────────────────────────────────────────────

const TAB_LABELS: Record<PickerTab, string> = { shows: 'Shows', movies: 'Movies', episodes: 'Episodes', playlists: 'Playlists', filler_lists: 'Filler Lists' }

const ContentPicker = observer(function ContentPicker({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const tabs        = availablePickerTabs(store.draft.block_type)
  const showFilters = store.pickerTab === 'shows' || store.pickerTab === 'movies'
  const libType     = store.pickerTab === 'movies' ? 'movie' : 'show'
  const filteredLibs = store.allLibraries.filter(l => l.library_type === libType || l.library_type === 'mixed')

  return (
    <div className="hds-in" style={{ marginTop: 10, border: '1px solid var(--hds-line)', borderRadius: 10, background: 'oklch(0.16 0.016 286)', overflow: 'hidden' }}>
      {/* Tab row */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '10px 10px 7px', borderBottom: showFilters ? 'none' : '1px solid var(--hds-line-s)' }}>
        <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 7, padding: 3 }}>
          {tabs.map(t => (
            <button key={t} onClick={() => store.setPickerTab(t)} style={{ padding: '4px 10px', border: 'none', borderRadius: 5, background: store.pickerTab === t ? 'var(--hds-violet)' : 'transparent', color: store.pickerTab === t ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer' }}>
              {TAB_LABELS[t]}
            </button>
          ))}
        </div>
        {store.pickerTab !== 'filler_lists' && store.pickerTab !== 'playlists' && (
          <input value={store.pickerQuery} onChange={e => store.setPickerQuery(e.target.value)} placeholder="Search…" style={{ flex: 1, ...inputStyle, fontSize: 11.5, padding: '6px 9px' }} autoFocus />
        )}
        <button onClick={() => store.closePicker()} style={{ color: 'var(--hds-txt-3)', background: 'transparent', border: 'none', cursor: 'pointer', fontSize: 12, padding: '0 3px', marginLeft: 'auto' }}>✕</button>
      </div>
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
      <div style={{ maxHeight: 200, overflow: 'auto' }} className="scrollbar-dark">
        {store.pickerLoading ? (
          <div style={{ padding: 12, color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
        ) : store.pickerTab === 'shows' ? (
          <ShowPickerList store={store} channelId={channelId} query={store.pickerQuery.toLowerCase()} />
        ) : store.pickerTab === 'movies' ? (
          <SimplePickerList
            items={store.pickerMovies.filter(m => !store.pickerQuery || m.title.toLowerCase().includes(store.pickerQuery.toLowerCase()))}
            getId={m => m.movie_id} dot={BLOCK_META.movie.edge}
            onAdd={(id, title) => store.addContent(channelId, { content_type: 'movie', content_id: id, title })}
          />
        ) : store.pickerTab === 'episodes' ? (
          <EpisodePickerList store={store} channelId={channelId} query={store.pickerQuery.toLowerCase()} />
        ) : store.pickerTab === 'playlists' ? (
          <SimplePickerList
            items={store.pickerPlaylists} getId={p => p.playlist_id} dot={BLOCK_META.episode.edge}
            onAdd={(id, title) => store.addContent(channelId, { content_type: 'playlist', content_id: id, title })}
          />
        ) : (
          <SimplePickerList
            items={store.pickerFillerLists} getId={f => f.filler_list_id} dot={BLOCK_META.filler.edge}
            onAdd={(id, title) => store.addContent(channelId, { content_type: 'filler_list', content_id: id, title })}
          />
        )}
      </div>
    </div>
  )
})

function SimplePickerList<T extends { title: string }>({ items, getId, dot, onAdd }: { items: T[]; getId: (i: T) => string; dot: string; onAdd: (id: string, title: string) => void }) {
  if (items.length === 0) return <p style={{ color: 'var(--hds-txt-3)', fontSize: 12, padding: 12 }}>No results.</p>
  return (
    <>
      {items.map(item => {
        const id = getId(item)
        return (
          <div key={id} draggable onClick={() => onAdd(id, item.title)} style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '9px 13px', cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)' }}
            onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)'}
            onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.background = ''}
          >
            <span style={{ color: 'var(--hds-txt-3)', fontSize: 12 }}>⋮⋮</span>
            <span style={{ width: 7, height: 7, borderRadius: 2, background: dot, flexShrink: 0 }} />
            <span style={{ flex: 1, fontSize: 12.5 }}>{item.title}</span>
            <span style={{ color: 'var(--hds-violet)', fontSize: 11 }}>+</span>
          </div>
        )
      })}
    </>
  )
}

const ShowPickerList = observer(function ShowPickerList({ store, channelId, query }: { store: ChannelDetailStore; channelId: string; query: string }) {
  const shows = store.pickerShows.filter(s => !query || s.title.toLowerCase().includes(query))
  if (shows.length === 0) return <p style={{ color: 'var(--hds-txt-3)', fontSize: 12, padding: 12 }}>No results.</p>
  return (
    <>
      {shows.map(show => {
        const expanded = store.expandedShowId === show.show_id
        return (
          <div key={show.show_id}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '9px 13px', cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)' }}
              onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)'}
              onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.background = ''}
              onClick={() => store.expandShow(show.show_id)}
            >
              <span style={{ color: 'var(--hds-txt-3)', fontSize: 12 }}>⋮⋮</span>
              <span style={{ width: 7, height: 7, borderRadius: 2, background: BLOCK_META.episode.edge, flexShrink: 0 }} />
              <span style={{ flex: 1, fontSize: 12.5 }}>{show.title}</span>
              <span style={{ color: 'var(--hds-txt-3)', fontSize: 11 }}>{expanded ? '▲' : '▼'}</span>
            </div>
            {expanded && (
              <div style={{ display: 'flex', flexWrap: 'wrap', gap: 4, padding: '6px 14px 10px' }}>
                {store.expandedSeasonsLoading ? (
                  <span style={{ fontSize: 11, color: 'var(--hds-txt-3)' }}>Loading…</span>
                ) : (
                  <>
                    <button onClick={() => store.addContent(channelId, { content_type: 'show', content_id: show.show_id, season_filter: null, title: show.title })} style={seasonBtnStyle}>All seasons</button>
                    {store.expandedSeasons.map(s => (
                      <button key={s} onClick={() => store.addContent(channelId, { content_type: 'show', content_id: show.show_id, season_filter: s, title: `${show.title} S${String(s).padStart(2, '0')}` })} style={seasonBtnStyle}>S{String(s).padStart(2, '0')}</button>
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

function EpisodePickerList({ store, channelId, query }: { store: ChannelDetailStore; channelId: string; query: string }) {
  const eps = store.pickerEpisodes.filter(e => !query || e.title.toLowerCase().includes(query) || e.show_title.toLowerCase().includes(query))
  if (eps.length === 0) return <p style={{ color: 'var(--hds-txt-3)', fontSize: 12, padding: 12 }}>No results. Type to search episodes.</p>
  return (
    <>
      {eps.map(ep => (
        <div key={ep.episode_id} style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '9px 13px', cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)' }}
          onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)'}
          onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.background = ''}
          onClick={() => store.addContent(channelId, { content_type: 'episode', content_id: ep.episode_id, title: `${ep.show_title} S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}` })}
        >
          <span style={{ width: 7, height: 7, borderRadius: 2, background: BLOCK_META.episode.edge, flexShrink: 0 }} />
          <div style={{ minWidth: 0 }}>
            <div style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>{ep.show_title}</div>
            <div style={{ fontSize: 12.5 }}>S{String(ep.season).padStart(2,'0')}E{String(ep.episode).padStart(2,'0')} — {ep.title}</div>
          </div>
        </div>
      ))}
    </>
  )
}

// ─── Block editor modal ───────────────────────────────────────────────────────

const BlockEditorModal = observer(function BlockEditorModal({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const d         = store.draft
  const m         = BLOCK_META[d.block_type]
  const limitMode = getLimitMode(d)

  useEffect(() => {
    const esc = (e: KeyboardEvent) => { if (e.key === 'Escape') store.modalOpen = false }
    document.addEventListener('keydown', esc)
    return () => document.removeEventListener('keydown', esc)
  }, [])

  return (
    <div style={{ position: 'fixed', inset: 0, zIndex: 200, display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'oklch(0.08 0.015 286 / 0.85)' }}
      onClick={e => { if (e.target === e.currentTarget) store.modalOpen = false }}>
      <div style={{ display: 'flex', flexDirection: 'column', width: 'min(95vw, 860px)', height: '92vh', background: 'var(--hds-bg-2)', borderRadius: 14, border: '1px solid var(--hds-line)', boxShadow: '0 32px 80px -16px rgba(0,0,0,0.8)', overflow: 'hidden', fontFamily: "'JetBrains Mono', monospace", fontSize: 13, color: 'var(--hds-txt)' }}>
        {/* Modal header */}
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '16px 22px 13px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 16, letterSpacing: '0.04em' }}>
            {store.isNewMode ? 'New' : m.name} Block
          </span>
          <button onClick={() => { store.modalOpen = false }} style={{ width: 30, height: 30, border: 'none', borderRadius: 8, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 16 }}>×</button>
        </div>
        {/* Form */}
        <EditorForm channelId={channelId} store={store} limitMode={limitMode} />
        {/* Footer */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '14px 22px', borderTop: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
          {store.editing ? (
            <>
              <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle}>
                {store.saving ? 'Saving…' : 'Save Changes'}
              </button>
              <button onClick={() => store.duplicate(channelId)} style={ghostBtnStyle}>⧉ Duplicate</button>
              <div style={{ flex: 1 }} />
              <button onClick={() => store.deleteBlock(channelId, store.editing!.block_id)} style={dangerBtnStyle}>Delete</button>
            </>
          ) : (
            <>
              <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle}>
                {store.saving ? 'Saving…' : 'Create Block'}
              </button>
              <button onClick={() => { store.closeEditor(); store.modalOpen = false }} style={ghostBtnStyle}>Cancel</button>
            </>
          )}
        </div>
        {store.saveErr && (
          <div style={{ padding: '7px 22px', fontSize: 11, color: 'oklch(0.72 0.16 22)', background: 'oklch(0.2 0.05 22 / 0.3)', borderTop: '1px solid oklch(0.4 0.1 22 / 0.4)', flexShrink: 0 }}>
            {store.saveErr}
          </div>
        )}
      </div>
    </div>
  )
})

// ─── Micro styles ─────────────────────────────────────────────────────────────

function SectionLabel({ children }: { children: ReactNode }) {
  return <div style={{ fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt-3)', marginBottom: 9 }}>{children}</div>
}

const inputStyle: CSSProperties = {
  width: '100%', padding: '9px 10px',
  border: '1px solid var(--hds-line)', borderRadius: 8,
  background: 'var(--hds-bg-3)', color: 'var(--hds-txt)',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 13,
  boxSizing: 'border-box',
}

const filterInputStyle: CSSProperties = {
  padding: '5px 7px',
  border: '1px solid var(--hds-line)', borderRadius: 6,
  background: 'var(--hds-bg-3)', color: 'var(--hds-txt)',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
  boxSizing: 'border-box',
}

const goldBtnStyle: CSSProperties = {
  padding: '10px 18px', border: 'none', borderRadius: 9,
  background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))',
  color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif",
  fontWeight: 700, fontSize: 13, cursor: 'pointer',
  boxShadow: '0 4px 14px -4px oklch(0.83 0.13 84 / 0.4)',
}

const ghostBtnStyle: CSSProperties = {
  display: 'flex', alignItems: 'center', gap: 6,
  padding: '10px 14px', border: '1px solid var(--hds-line)', borderRadius: 9,
  background: 'transparent', color: 'var(--hds-txt-2)',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 12, cursor: 'pointer',
}

const dangerBtnStyle: CSSProperties = {
  padding: '10px 16px', border: '1px solid oklch(0.5 0.14 22 / 0.5)', borderRadius: 9,
  background: 'transparent', color: 'oklch(0.72 0.16 22)',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 12, cursor: 'pointer',
}

const seasonBtnStyle: CSSProperties = {
  padding: '2px 6px', borderRadius: 4, border: '1px solid var(--hds-line)',
  background: 'transparent', color: 'var(--hds-txt-2)',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer',
}
