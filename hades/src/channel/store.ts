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
  EpisodeOrder, EpisodeSearchResult, EpgPreviewResponse, EpgProgram, FillerEntry, FillerEntryAdvancement,
  FillerSelectionMode, LibraryWithSource, Movie, NoHistoryBehavior, Playlist,
  PlaylistMode, PlayStyle, Show, StartScope, TimeslotSlot, TimeslotQueueEntry,
} from '../api/types'

let _debounce:  ReturnType<typeof setTimeout>
let _epgTimer:  ReturnType<typeof setTimeout> | null = null
let _ruleId = 0
let _searchCtrl: AbortController | null = null

// Slot offsets are purely derived — always the cumulative sum of preceding durations.
function recomputeSlotOffsets(slots: TimeslotSlot[]): TimeslotSlot[] {
  let offset = 0
  return slots.map(s => {
    const updated = { ...s, slot_offset_mins: offset }
    offset += s.slot_duration_mins
    return updated
  })
}

function raceAbort<T>(promise: Promise<T>, signal: AbortSignal): Promise<T> {
  return new Promise((resolve, reject) => {
    const onAbort = () => reject(new DOMException('Aborted', 'AbortError'))
    if (signal.aborted) { onAbort(); return }
    signal.addEventListener('abort', onAbort, { once: true })
    promise.then(
      v => { signal.removeEventListener('abort', onAbort); resolve(v) },
      e => { signal.removeEventListener('abort', onAbort); reject(e) },
    )
  })
}

export class ChannelDetailStore {
  blocks:      Block[]       = []
  savedBlocks: Block[]       = []   // snapshot of last-committed state
  blocksDirty: boolean       = false
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
  pickerSeasonFilter: string              = ''
  pickerDurationMax:  string              = ''
  pickerTab:         PickerTab             = 'shows'
  pickerShows:       Show[]                = []
  pickerMovies:      Movie[]               = []
  pickerEpisodes:    EpisodeSearchResult[] = []
  pickerPlaylists:   Playlist[]            = []
  contentPlaylists:  Playlist[]            = []
  pickerLoading:     boolean               = false
  pickerTotal:       number                = 0
  pickerEpsHasMore:  boolean               = false
  pickerLoadingMore: boolean               = false
  dragContent: { content_type: ContentType; content_id: string; title: string } | null = null

  allLibraries:      LibraryWithSource[]   = []

  filterRulesOpen:  boolean              = false
  filterMatch:      'all' | 'any'        = 'all'
  filterRules:      FilterRule[]         = []

  expandedShowId:         string | null = null
  expandedSeasons:        {number: number; name: string}[] = []
  expandedSeasonsLoading: boolean       = false

  isNewMode:     boolean        = false
  editingSlotId: string | null = null

  saving:          boolean       = false
  saveErr:         string | null = null
  creatingPremiers: boolean      = false

  bulkMode:        boolean       = false
  bulkSelectedIds: string[]      = []
  bulkSaving:      boolean       = false
  bulkErr:         string | null = null

  openSections: Record<string, boolean> = { schedule: true, timing: true, playback: false, content: true, filler: false, bumpers: false }
  modalOpen:          boolean = false
  showHints:          boolean = true
  fillerOverlayOpen:         boolean = false
  bumperOverlayOpen:         boolean = false
  channelFillerOverlayOpen:  boolean = false
  channelBumperOverlayOpen:  boolean = false

  fillerPickerOpen: boolean      = false
  fillerSaving:     boolean      = false

  channelFillerSaving: boolean        = false
  channelFillerErr:    string | null  = null

  draftContent:       BlockContent[]  = []
  draftFillerEntries: FillerEntry[]   = []
  draftSlots:         TimeslotSlot[]  = []
  contentDirty:       boolean         = false

  epgItems:   EpgProgram[]            = []
  epgLoading: boolean                 = false
  confirmedAnchors: Record<string, number> = {}
  previewAnchors:   Record<string, number> = {}

  channelDraftName:             string      = ''
  channelDraftNumber:           number      = 1
  channelDraftTimezone:         string      = 'UTC'
  channelDraftSeed:             number      = 12345
  channelDraftAdvanceMode:      AdvanceMode = 'scheduled'
  channelDraftOfflineVideoPath: string      = ''
  channelDraftOfflineImagePath: string      = ''
  channelDraftOfflineAudioId:    string      = ''
  channelDraftOfflineAudioType:  'episode' | 'movie' | '' = ''
  channelDraftOfflineAudioTitle: string      = ''
  channelDraftLogoPath:         string      = ''
  channelDirty:            boolean     = false
  channelSaving:        boolean = false
  channelSaveErr:       string | null = null
  epgClearing:          boolean = false

  constructor() { makeAutoObservable(this) }

  get isDirty(): boolean { return this.channelDirty || this.blocksDirty }

  get scheduleChanged(): boolean {
    const pa = this.previewAnchors
    const ca = this.confirmedAnchors
    const paKeys = Object.keys(pa)
    const caKeys = Object.keys(ca)
    if (paKeys.length !== caKeys.length) return true
    return paKeys.some(k => pa[k] !== ca[k])
  }

  initChannelDraft(channel: Channel) {
    this.channelDraftName             = channel.name
    this.channelDraftNumber           = channel.number
    this.channelDraftTimezone         = channel.timezone
    this.channelDraftSeed             = channel.seed !== undefined ? channel.seed : 12345
    this.channelDraftAdvanceMode      = channel.advance_mode ?? 'scheduled'
    this.channelDraftOfflineVideoPath = channel.offline_video_path ?? ''
    this.channelDraftOfflineImagePath = channel.offline_image_path ?? ''
    this.channelDraftOfflineAudioId    = channel.offline_audio_id    ?? ''
    this.channelDraftOfflineAudioType  = channel.offline_audio_type  ?? ''
    this.channelDraftOfflineAudioTitle = channel.offline_audio_title ?? ''
    this.channelDraftLogoPath         = channel.logo_path          ?? ''
    this.channelDirty                 = false
    this.confirmedAnchors             = channel.anchor_hashes ?? {}
    this.previewAnchors               = {}
    this.epgDay                       = todayEpgDay(channel.timezone)
  }

  setChannelDraft(patch: Partial<{ name: string; number: number; timezone: string; seed: number; advance_mode: AdvanceMode; offline_video_path: string; offline_image_path: string; offline_audio_id: string; offline_audio_type: 'episode' | 'movie' | ''; offline_audio_title: string; logo_path: string }>) {
    if (patch.name                 !== undefined) this.channelDraftName              = patch.name
    if (patch.number               !== undefined) this.channelDraftNumber            = patch.number
    if (patch.timezone             !== undefined) this.channelDraftTimezone          = patch.timezone
    if (patch.seed                 !== undefined) this.channelDraftSeed              = patch.seed
    if (patch.advance_mode         !== undefined) this.channelDraftAdvanceMode       = patch.advance_mode
    if (patch.offline_video_path   !== undefined) this.channelDraftOfflineVideoPath  = patch.offline_video_path
    if (patch.offline_image_path   !== undefined) this.channelDraftOfflineImagePath  = patch.offline_image_path
    if (patch.offline_audio_id     !== undefined) this.channelDraftOfflineAudioId    = patch.offline_audio_id
    if (patch.offline_audio_type   !== undefined) this.channelDraftOfflineAudioType  = patch.offline_audio_type
    if (patch.offline_audio_title  !== undefined) this.channelDraftOfflineAudioTitle = patch.offline_audio_title
    if (patch.logo_path            !== undefined) this.channelDraftLogoPath          = patch.logo_path
    this.channelDirty = true
  }

  // Commits all in-memory block changes + channel settings to the DB.
  // After success, sets savedBlocks = blocks and confirms anchor hashes.
  async saveChannel(channelId: string) {
    this.channelSaving = true; this.channelSaveErr = null
    try {
      await api.updateChannel(channelId, {
        name:                 this.channelDraftName,
        number:               this.channelDraftNumber,
        timezone:             this.channelDraftTimezone,
        seed:                 this.channelDraftSeed,
        advance_mode:         this.channelDraftAdvanceMode,
        offline_video_path:   this.channelDraftOfflineVideoPath,
        offline_image_path:   this.channelDraftOfflineImagePath,
        offline_audio_id:     this.channelDraftOfflineAudioId,
        offline_audio_type:   this.channelDraftOfflineAudioType,
        offline_audio_title:  this.channelDraftOfflineAudioTitle,
        logo_path:            this.channelDraftLogoPath,
      })

      const savedIds = new Set(this.savedBlocks.map(b => b.block_id))
      const draftIds = new Set(this.blocks.map(b => b.block_id))

      // Delete blocks removed in draft.
      for (const b of this.savedBlocks) {
        if (!draftIds.has(b.block_id)) {
          await api.deleteBlock(channelId, b.block_id)
        }
      }

      // Create new blocks and update existing ones.
      const idMap: Record<string, string> = {} // tempId → real block_id
      for (const b of this.blocks) {
        const isNew = b.block_id.startsWith('tmp_') || !savedIds.has(b.block_id)
        const payload = blockToDraft(b)

        if (isNew) {
          const res = await api.createBlock(channelId, payload as any)
          const realId = res.block_id
          idMap[b.block_id] = realId
          for (const c of b.content) {
            await api.addBlockContent(channelId, realId, {
              content_type: c.content_type, content_id: c.content_id,
              season_filter: c.season_filter, weight: c.weight, run_count: c.run_count,
              include_specials: c.include_specials, episode_order: c.episode_order,
            })
          }
          for (const fe of b.filler_entries) {
            await api.addBlockFiller(channelId, realId, {
              content_type: fe.content_type, content_id: fe.content_id,
              advancement: fe.advancement, weight: fe.weight,
            })
          }
          if (b.block_type === 'timeslot') {
            for (const s of b.slots ?? []) {
              const sr = await api.post<{ slot_id: string }>(`/blocks/${realId}/slots`, {
                slot_offset_mins: s.slot_offset_mins, slot_duration_mins: s.slot_duration_mins,
                overflow: s.overflow, late_start_mins: s.late_start_mins,
                early_start_secs: s.early_start_secs, align_to_mins: s.align_to_mins,
                start_scope: s.start_scope,
              })
              for (const q of s.queue) {
                await api.post(`/blocks/${realId}/slots/${sr.slot_id}/queue`, {
                  content_type: q.content_type, content_id: q.content_id,
                  premiere_date: q.premiere_date, pre_premiere_behavior: q.pre_premiere_behavior,
                })
              }
            }
          }
        } else {
          const realId = b.block_id
          await api.updateBlock(channelId, realId, payload as any)
          const savedBlock = this.savedBlocks.find(s => s.block_id === realId)
          if (savedBlock) {
            const toRemoveContent = savedBlock.content.filter(c => !b.content.some(dc => dc.id === c.id && dc.id > 0))
            const toAddContent    = b.content.filter(c => c.id < 0)
            const toUpdateContent = b.content.filter(c => {
              if (c.id < 0) return false
              const orig = savedBlock.content.find(o => o.id === c.id)
              return orig && (
                orig.weight !== c.weight || orig.run_count !== c.run_count ||
                orig.include_specials !== c.include_specials ||
                orig.episode_order !== c.episode_order ||
                orig.season_filter !== c.season_filter
              )
            })
            for (const c  of toRemoveContent) await api.removeBlockContent(channelId, realId, c.id)
            for (const c  of toAddContent)    await api.addBlockContent(channelId, realId, { content_type: c.content_type, content_id: c.content_id, season_filter: c.season_filter, weight: c.weight, run_count: c.run_count, include_specials: c.include_specials, episode_order: c.episode_order })
            for (const c  of toUpdateContent) await api.updateBlockContent(channelId, realId, c.id, { weight: c.weight, run_count: c.run_count, include_specials: c.include_specials, episode_order: c.episode_order, season_filter: c.season_filter })

            const toRemoveFiller = savedBlock.filler_entries.filter(fe => !b.filler_entries.some(dfe => dfe.id === fe.id && dfe.id > 0))
            const toAddFiller    = b.filler_entries.filter(fe => fe.id < 0)
            const toUpdateFiller = b.filler_entries.filter(fe => {
              if (fe.id < 0) return false
              const orig = savedBlock.filler_entries.find(o => o.id === fe.id)
              return orig && (orig.advancement !== fe.advancement || orig.weight !== fe.weight)
            })
            for (const fe of toRemoveFiller) await api.removeBlockFiller(channelId, realId, fe.id)
            for (const fe of toAddFiller)    await api.addBlockFiller(channelId, realId, { content_type: fe.content_type, content_id: fe.content_id, advancement: fe.advancement, weight: fe.weight, season_filter: fe.season_filter })
            for (const fe of toUpdateFiller) await api.updateBlockFiller(channelId, realId, fe.id, { advancement: fe.advancement, weight: fe.weight })

            if (b.block_type === 'timeslot') {
              const savedSlots = savedBlock.slots ?? []
              const draftSlots = b.slots ?? []
              for (const ss of savedSlots) {
                if (!draftSlots.some(ds => ds.slot_id === ss.slot_id))
                  await api.del(`/blocks/${realId}/slots/${ss.slot_id}`)
              }
              for (const ds of draftSlots) {
                if (ds.slot_id.startsWith('tmp_')) {
                  const sr = await api.post<{ slot_id: string }>(`/blocks/${realId}/slots`, {
                    slot_offset_mins: ds.slot_offset_mins, slot_duration_mins: ds.slot_duration_mins,
                    overflow: ds.overflow, late_start_mins: ds.late_start_mins,
                    early_start_secs: ds.early_start_secs, align_to_mins: ds.align_to_mins,
                    start_scope: ds.start_scope,
                  })
                  for (const q of ds.queue) {
                    await api.post(`/blocks/${realId}/slots/${sr.slot_id}/queue`, {
                      content_type: q.content_type, content_id: q.content_id,
                      premiere_date: q.premiere_date, pre_premiere_behavior: q.pre_premiere_behavior,
                    })
                  }
                } else {
                  const orig = savedSlots.find(ss => ss.slot_id === ds.slot_id)
                  if (orig) {
                    const sp: Record<string, unknown> = {}
                    if (orig.slot_offset_mins   !== ds.slot_offset_mins)   sp.slot_offset_mins   = ds.slot_offset_mins
                    if (orig.slot_duration_mins !== ds.slot_duration_mins) sp.slot_duration_mins = ds.slot_duration_mins
                    if (orig.overflow           !== ds.overflow)           sp.overflow           = ds.overflow
                    if (orig.late_start_mins    !== ds.late_start_mins)    sp.late_start_mins    = ds.late_start_mins
                    if (orig.early_start_secs   !== ds.early_start_secs)   sp.early_start_secs   = ds.early_start_secs
                    if (orig.align_to_mins      !== ds.align_to_mins)      sp.align_to_mins      = ds.align_to_mins
                    if (orig.start_scope        !== ds.start_scope)        sp.start_scope        = ds.start_scope
                    if (Object.keys(sp).length > 0)
                      await api.patch(`/blocks/${realId}/slots/${ds.slot_id}`, sp)
                    for (const oq of orig.queue) {
                      if (!ds.queue.some(dq => dq.entry_id === oq.entry_id))
                        await api.del(`/blocks/${realId}/slots/${ds.slot_id}/queue/${oq.entry_id}`)
                    }
                    for (const dq of ds.queue) {
                      if (dq.entry_id.startsWith('tmp_')) {
                        await api.post(`/blocks/${realId}/slots/${ds.slot_id}/queue`, {
                          content_type: dq.content_type, content_id: dq.content_id,
                          premiere_date: dq.premiere_date, pre_premiere_behavior: dq.pre_premiere_behavior,
                        })
                      } else {
                        const oq = orig.queue.find(q => q.entry_id === dq.entry_id)
                        if (oq) {
                          const qp: Record<string, string> = {}
                          if (oq.premiere_date         !== dq.premiere_date)         qp.premiere_date         = dq.premiere_date
                          if (oq.pre_premiere_behavior !== dq.pre_premiere_behavior) qp.pre_premiere_behavior = dq.pre_premiere_behavior
                          if (Object.keys(qp).length > 0)
                            await api.patch(`/blocks/${realId}/slots/${ds.slot_id}/queue/${dq.entry_id}`, qp)
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }

      // Persist anchor seeds from the latest preview.
      if (Object.keys(this.previewAnchors).length > 0) {
        await api.updateChannel(channelId, { anchor_hashes: this.previewAnchors })
      }

      await channelStore.fetchAll()
      const blocks = await api.getBlocks(channelId)
      const normalizedBlocks = blocks.map(normalizeBlock)

      runInAction(() => {
        this.blocks           = normalizedBlocks
        this.savedBlocks      = normalizedBlocks
        this.blocksDirty      = false
        this.channelDirty     = false
        this.confirmedAnchors = { ...this.previewAnchors }
        this.channelSaving    = false
        // Remap editor selection to real IDs (temp → real after creation).
        if (this.selectedId) {
          const realId = idMap[this.selectedId] ?? this.selectedId
          const block  = this.blocks.find(b => b.block_id === realId) ?? null
          this.selectedId         = realId
          this.editing            = block
          if (block) {
            this.draftContent       = [...block.content]
            this.draftFillerEntries = [...block.filler_entries]
            this.draftSlots         = (block.slots ?? []).map(s => ({ ...s, queue: [...s.queue] }))
          }
        }
      })
    } catch (e: any) {
      runInAction(() => { this.channelSaveErr = e.message; this.channelSaving = false })
    }
  }

  discardChanges(channelId: string) {
    this.blocks      = [...this.savedBlocks]
    this.blocksDirty = false
    this.channelDirty = false
    this.closeEditor()
    this.loadEpg(channelId)
  }

  async clearEpgCache(channelId: string) {
    this.epgClearing = true
    try {
      await api.clearChannelEpgCache(channelId)
      await this.loadEpg(channelId)
    } finally {
      runInAction(() => { this.epgClearing = false })
    }
  }

  async load(channelId: string) {
    this.loading = true; this.error = null
    try {
      const blocks = await api.getBlocks(channelId)
      runInAction(() => {
        const normalized = blocks.map(normalizeBlock)
        this.blocks      = normalized
        this.savedBlocks = normalized
        this.blocksDirty = false
        this.loading     = false
      })
      this.loadEpg(channelId)
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.loading = false })
    }
  }

  async loadEpg(channelId: string) {
    this.epgLoading = true
    try {
      // Only send draft blocks when they differ from DB — lets Kairos hit its live
      // EPG cache for normal previews and only runs the SAVEPOINT swap for real diffs.
      const draftBlocks = this.blocksDirty ? this.blocks : undefined
      const result = await api.previewChannelEpg(channelId, 336, this.channelDraftSeed, draftBlocks)
      runInAction(() => {
        this.epgItems       = result.programs
        this.previewAnchors = result.anchors
        this.epgLoading     = false
      })
    } catch {
      runInAction(() => { this.epgLoading = false })
    }
  }

  // Debounced EPG refresh — batches rapid block mutations into one preview call.
  scheduleEpgRefresh(channelId: string, delay = 700) {
    if (_epgTimer) clearTimeout(_epgTimer)
    _epgTimer = setTimeout(() => {
      _epgTimer = null
      this.loadEpg(channelId)
    }, delay)
  }

  select(blockId: string) {
    const block = this.blocks.find(b => b.block_id === blockId)
    if (!block) return
    this.selectedId         = blockId
    this.editing            = block
    this.isNewMode          = false
    this.editingSlotId      = null
    this.draft              = blockToDraft(block)
    this.draftContent       = [...block.content]
    this.draftFillerEntries = [...block.filler_entries]
    this.draftSlots         = (block.slots ?? []).map(s => ({ ...s, queue: [...s.queue] }))
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
    this.editingSlotId      = null
    this.draft              = { ...BLANK_DRAFT, priority: maxP + 1 }
    this.draftContent       = []
    this.draftFillerEntries = []
    this.draftSlots         = []
    this.contentDirty       = false
    this.saveErr            = null
    this.pickerOpen         = false
  }

  closeEditor() {
    this.selectedId         = null
    this.editing            = null
    this.isNewMode          = false
    this.editingSlotId      = null
    this.pickerOpen         = false
    this.fillerPickerOpen   = false
    this.draftContent       = []
    this.draftFillerEntries = []
    this.draftSlots         = []
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

  // Applies a bulk patch to all selected blocks in-memory (no immediate DB writes).
  applyBulk(channelId: string, patch: Partial<BlockDraft>) {
    if (!this.bulkSelectedIds.length) return
    this.bulkSaving = true; this.bulkErr = null
    try {
      this.blocks = this.blocks.map(b =>
        this.bulkSelectedIds.includes(b.block_id) ? { ...b, ...(patch as any) } : b
      )
      this.blocksDirty     = true
      this.bulkSaving      = false
      this.bulkSelectedIds = []
    } catch (e: any) {
      this.bulkErr = e.message; this.bulkSaving = false
    }
    this.scheduleEpgRefresh(channelId)
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

  // Saves the current block editor state to the in-memory draft (no DB writes).
  save(channelId: string) {
    this.saving = true; this.saveErr = null
    const payload = { ...this.draft, end_time: this.draft.end_time ?? '' }
    const { filler_selection, align_to_mins, inter_filler, early_start_secs, start_scope } = this.draft

    try {
      let blockId: string
      if (this.editing) {
        blockId = this.editing.block_id
        const idx = this.blocks.findIndex(b => b.block_id === blockId)
        if (idx >= 0) {
          const updated: Block = {
            ...this.editing,
            ...(payload as any),
            content:        [...this.draftContent],
            filler_entries: [...this.draftFillerEntries],
            slots:          [...this.draftSlots],
          }
          this.blocks = [...this.blocks.slice(0, idx), updated, ...this.blocks.slice(idx + 1)]
        }
      } else {
        blockId = `tmp_${Date.now()}`
        const newBlock: Block = {
          block_id:   blockId,
          channel_id: channelId,
          ...(payload as any),
          content:        [...this.draftContent],
          filler_entries: [...this.draftFillerEntries],
          slots:          [...this.draftSlots],
        }
        this.blocks = [...this.blocks, newBlock]
      }

      this.blocksDirty = true
      const block      = this.blocks.find(b => b.block_id === blockId) ?? null
      this.saving      = false
      this.isNewMode   = false
      this.selectedId  = blockId
      this.editing     = block
      this.contentDirty = false
      if (block) {
        this.draft              = { ...blockToDraft(block), filler_selection, align_to_mins, inter_filler, early_start_secs, start_scope }
        this.draftContent       = [...block.content]
        this.draftFillerEntries = [...block.filler_entries]
        this.draftSlots         = (block.slots ?? []).map(s => ({ ...s, queue: [...s.queue] }))
      }
    } catch (e: any) {
      this.saveErr = e.message; this.saving = false
    }
    this.scheduleEpgRefresh(channelId)
  }

  // Duplicates the current block into the in-memory draft.
  duplicate(channelId: string) {
    if (!this.editing) return
    const src = this.editing
    const { filler_selection, align_to_mins, inter_filler, early_start_secs, start_scope } = src
    const newId    = `tmp_${Date.now()}`
    const newBlock: Block = {
      ...src,
      block_id:       newId,
      name:           src.name + ' (copy)',
      content:        src.content.map(c => ({ ...c, id: -(Date.now() * 100 + Math.random() * 100 | 0), block_id: newId })),
      filler_entries: src.filler_entries.map(fe => ({ ...fe, id: -(Date.now() * 100 + Math.random() * 100 | 0), block_id: newId })),
      slots:          (src.slots ?? []).map((s, si) => ({
        ...s,
        slot_id: `tmp_${Date.now()}_s${si}`,
        queue:   s.queue.map((q, qi) => ({ ...q, entry_id: `tmp_${Date.now()}_q${si}_${qi}` })),
      })),
    }
    this.blocks             = [...this.blocks, newBlock]
    this.blocksDirty        = true
    this.selectedId         = newId
    this.editing            = newBlock
    this.isNewMode          = false
    this.draft              = { ...blockToDraft(newBlock), filler_selection, align_to_mins, inter_filler, early_start_secs, start_scope }
    this.draftContent       = [...newBlock.content]
    this.draftFillerEntries = [...newBlock.filler_entries]
    this.draftSlots         = (newBlock.slots ?? []).map(s => ({ ...s, queue: [...s.queue] }))
    this.contentDirty       = false
    this.scheduleEpgRefresh(channelId)
  }

  async createPremierBlocks(channelId: string) {
    if (!this.editing) return
    const isRerun = this.draft.play_style === 'rerun'
    if (!isRerun) return
    runInAction(() => { this.creatingPremiers = true })
    const premierBlocks = this.blocks.filter(b =>
      b.play_style === 'standard' && b.advancement === 'sequential' &&
      (b.cursor_scope === 'channel' || b.cursor_scope === 'global')
    )
    const showsToCreate  = this.draftContent.filter(c =>
      c.content_type === 'show' && c.id > 0 &&
      !premierBlocks.some(pb => pb.content.some(pc => pc.content_type === 'show' && pc.content_id === c.content_id))
    )
    const rerunPriority = this.draft.priority
    try {
      const newBlocks: Block[] = showsToCreate.map((item, i) => {
        const bid = `tmp_${Date.now()}_${i}`
        return {
          block_id:                   bid,
          channel_id:                 channelId,
          name:                       item.title,
          block_type:                 'episode'    as BlockType,
          day_mask:                   this.draft.day_mask,
          start_time:                 this.draft.start_time,
          end_time:                   '',
          play_style:                 'standard'   as PlayStyle,
          advancement:                'sequential' as Advancement,
          cursor_scope:               this.draft.cursor_scope as CursorScope,
          priority:                   rerunPriority + 1,
          program_count:              1,
          late_start_mins:            5,
          early_start_secs:           15,
          filler_selection:           'round_robin' as FillerSelectionMode,
          align_to_mins:              0,
          inter_filler:               false,
          smart_pct:                  30,
          start_scope:                'block' as StartScope,
          no_history_behavior:        'normal' as NoHistoryBehavior,
          max_consecutive_episodes:   0,
          snap_to_group_start:        true,
          interstitial_every_n:       1,
          intro_content_type:         '',
          intro_content_id:           '',
          outro_content_type:         '',
          outro_content_id:           '',
          interstitial_content_type:  '',
          interstitial_content_id:    '',
          filler_entries:             [],
          content: [{
            id:               -(Date.now() * 100 + i),
            block_id:         bid,
            content_type:     'show' as ContentType,
            content_id:       item.content_id,
            position:         0,
            title:            item.title,
            weight:           1,
            run_count:        1,
            include_specials: false,
            episode_order:    'season' as EpisodeOrder,
          }],
        }
      })
      runInAction(() => {
        this.blocks          = [...this.blocks, ...newBlocks]
        this.blocksDirty     = true
        this.creatingPremiers = false
      })
      this.scheduleEpgRefresh(channelId)
    } catch (e: any) {
      runInAction(() => { this.saveErr = e.message; this.creatingPremiers = false })
    }
  }

  // Removes a block from the in-memory draft (no immediate DB write).
  deleteBlock(channelId: string, blockId: string) {
    this.blocks      = this.blocks.filter(b => b.block_id !== blockId)
    this.blocksDirty = true
    this.closeEditor()
    this.scheduleEpgRefresh(channelId)
  }

  addContent(channelId: string, item: { content_type: ContentType; content_id: string; season_filter?: number | null; title?: string; include_specials?: boolean }) {
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
      include_specials: item.include_specials ?? false,
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
  }

  async setPlaylistMode(playlistId: string, mode: 'sequential' | 'show_collection') {
    await api.updatePlaylist(playlistId, { mode })
    runInAction(() => {
      const update = (list: Playlist[]) => list.map(p => p.playlist_id === playlistId ? { ...p, mode } : p)
      this.pickerPlaylists  = update(this.pickerPlaylists)
      this.contentPlaylists = update(this.contentPlaylists)
    })
  }

  async addBlockFiller(channelId: string, body: { content_type: string; content_id: string; title: string; advancement: FillerEntryAdvancement; weight: number; season_filter?: number }) {
    if (!this.editing && !this.isNewMode) return
    const entry: FillerEntry = {
      id:            -(Date.now() * 100 + this.draftFillerEntries.length),
      content_type:  body.content_type as FillerEntry['content_type'],
      content_id:    body.content_id,
      title:         body.title,
      advancement:   body.advancement,
      weight:        body.weight,
      position:      this.draftFillerEntries.length,
      season_filter: body.season_filter,
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

  // ── Slot draft mutations (timeslot blocks) ───────────────────────────────────

  setEditingSlot(id: string | null) { this.editingSlotId = id }

  convertContentToSlots() {
    const eligible = this.draftContent.filter(
      c => c.content_type === 'show' || c.content_type === 'movie',
    )
    if (!eligible.length) return
    this.draftSlots = recomputeSlotOffsets(eligible.map((c, i) => ({
      slot_id:            `tmp_${Date.now()}_s${i}`,
      slot_index:         i,
      slot_offset_mins:   0,   // overwritten by recomputeSlotOffsets
      slot_duration_mins: 30,
      overflow:           'cutoff'  as const,
      late_start_mins:    0,
      early_start_secs:   0,
      align_to_mins:      0,
      start_scope:        'block'   as const,
      queue_pos:          0,
      episode_pos:        0,
      queue: [{
        entry_id:              `tmp_${Date.now()}_q${i}`,
        queue_index:           0,
        content_type:          c.content_type as 'show' | 'movie',
        content_id:            c.content_id,
        title:                 c.title,
        premiere_date:         '',
        pre_premiere_behavior: 'replay_previous' as const,
      }],
    })))
    // Drop the converted entries; leave any non-convertible ones (episodes, playlists…)
    this.draftContent = this.draftContent.filter(
      c => c.content_type !== 'show' && c.content_type !== 'movie',
    )
    this.contentDirty = true
  }

  addDraftSlot() {
    const offset = this.draftSlots.reduce((acc, s) => acc + s.slot_duration_mins, 0)
    const idx    = this.draftSlots.length
    this.draftSlots = [...this.draftSlots, {
      slot_id:            `tmp_${Date.now()}_s${idx}`,
      slot_index:         idx,
      slot_offset_mins:   offset,
      slot_duration_mins: 30,
      overflow:           'cutoff',
      late_start_mins:    0,
      early_start_secs:   0,
      align_to_mins:      0,
      start_scope:        'block',
      queue_pos:          0,
      episode_pos:        0,
      queue:              [],
    }]
    this.contentDirty = true
  }

  removeDraftSlot(slotId: string) {
    const filtered = this.draftSlots
      .filter(s => s.slot_id !== slotId)
      .map((s, i) => ({ ...s, slot_index: i }))
    this.draftSlots   = recomputeSlotOffsets(filtered)
    this.contentDirty = true
  }

  patchDraftSlot(slotId: string, patch: Partial<TimeslotSlot>) {
    const patched     = this.draftSlots.map(s => s.slot_id === slotId ? { ...s, ...patch } : s)
    this.draftSlots   = recomputeSlotOffsets(patched)
    this.contentDirty = true
  }

  addDraftQueueEntry(slotId: string, entry: { content_type: 'show' | 'movie'; content_id: string; title: string }) {
    this.draftSlots = this.draftSlots.map(s => {
      if (s.slot_id !== slotId) return s
      const qi = s.queue.length
      const newEntry: TimeslotQueueEntry = {
        entry_id:              `tmp_${Date.now()}_q${qi}`,
        queue_index:           qi,
        content_type:          entry.content_type,
        content_id:            entry.content_id,
        title:                 entry.title,
        premiere_date:         '',
        pre_premiere_behavior: 'replay_previous',
      }
      return { ...s, queue: [...s.queue, newEntry] }
    })
    this.contentDirty = true
  }

  removeDraftQueueEntry(slotId: string, entryId: string) {
    this.draftSlots = this.draftSlots.map(s => {
      if (s.slot_id !== slotId) return s
      return {
        ...s,
        queue: s.queue
          .filter(q => q.entry_id !== entryId)
          .map((q, i) => ({ ...q, queue_index: i })),
      }
    })
    this.contentDirty = true
  }

  patchDraftQueueEntry(slotId: string, entryId: string, patch: Partial<Pick<TimeslotQueueEntry, 'premiere_date' | 'pre_premiere_behavior'>>) {
    this.draftSlots = this.draftSlots.map(s => {
      if (s.slot_id !== slotId) return s
      return { ...s, queue: s.queue.map(q => q.entry_id === entryId ? { ...q, ...patch } : q) }
    })
    this.contentDirty = true
  }

  async resetDraftSlotCursor(blockId: string, slotId: string) {
    if (!slotId.startsWith('tmp_') && !blockId.startsWith('tmp_')) {
      await api.post(`/blocks/${blockId}/slots/${slotId}/cursor/reset`, {})
    }
    runInAction(() => {
      this.draftSlots = this.draftSlots.map(s =>
        s.slot_id === slotId ? { ...s, queue_pos: 0, episode_pos: 0 } : s,
      )
    })
  }

  async saveChannelFiller(channelId: string, patch: { default_filler_selection?: FillerSelectionMode }) {
    const ch    = channelStore.channels.find(c => c.channel_id === channelId)
    const origSel = ch?.default_filler_selection
    runInAction(() => {
      if (ch && patch.default_filler_selection) ch.default_filler_selection = patch.default_filler_selection
      this.channelFillerSaving = true
    })
    try {
      await api.updateChannel(channelId, patch)
      runInAction(() => { this.channelFillerSaving = false })
      channelStore.fetchAll()
    } catch (e: any) {
      runInAction(() => {
        if (ch && origSel) ch.default_filler_selection = origSel
        this.channelFillerSaving = false
        this.channelFillerErr = e.message
      })
    }
  }

  async addChannelFiller(channelId: string, body: { content_type: string; content_id: string; title?: string; advancement: FillerEntryAdvancement; weight: number; season_filter?: number }) {
    const ch     = channelStore.channels.find(c => c.channel_id === channelId)
    const tempId = -(Date.now() * 100 + (ch?.default_filler_entries.length ?? 0))
    const optimistic: FillerEntry = {
      id:            tempId,
      content_type:  body.content_type as FillerEntry['content_type'],
      content_id:    body.content_id,
      title:         body.title ?? body.content_id,
      advancement:   body.advancement,
      weight:        body.weight,
      position:      ch?.default_filler_entries.length ?? 0,
      season_filter: body.season_filter,
    }
    runInAction(() => {
      if (ch) ch.default_filler_entries.push(optimistic)
      this.channelFillerSaving = true
    })
    try {
      await api.addChannelFiller(channelId, body)
      runInAction(() => { this.channelFillerSaving = false })
      channelStore.fetchAll()  // background reconcile — replaces temp ID with real one
    } catch (e: any) {
      runInAction(() => {
        if (ch) ch.default_filler_entries = ch.default_filler_entries.filter(e => e.id !== tempId)
        this.channelFillerSaving = false
        this.channelFillerErr = e.message
      })
    }
  }

  async updateChannelFiller(channelId: string, entryId: number, patch: { advancement?: FillerEntryAdvancement; weight?: number }) {
    const ch   = channelStore.channels.find(c => c.channel_id === channelId)
    const orig = ch?.default_filler_entries.find(e => e.id === entryId)
    runInAction(() => {
      if (ch) {
        const idx = ch.default_filler_entries.findIndex(e => e.id === entryId)
        if (idx !== -1) ch.default_filler_entries[idx] = { ...ch.default_filler_entries[idx], ...patch }
      }
    })
    try {
      await api.updateChannelFiller(channelId, entryId, patch)
      channelStore.fetchAll()
    } catch (e: any) {
      runInAction(() => {
        if (ch && orig) {
          const idx = ch.default_filler_entries.findIndex(e => e.id === entryId)
          if (idx !== -1) ch.default_filler_entries[idx] = orig
        }
        this.channelFillerErr = e.message
      })
    }
  }

  async removeChannelFiller(channelId: string, entryId: number) {
    const ch      = channelStore.channels.find(c => c.channel_id === channelId)
    const orig    = ch?.default_filler_entries.find(e => e.id === entryId)
    const origIdx = ch?.default_filler_entries.findIndex(e => e.id === entryId) ?? -1
    runInAction(() => {
      if (ch) ch.default_filler_entries = ch.default_filler_entries.filter(e => e.id !== entryId)
    })
    try {
      await api.removeChannelFiller(channelId, entryId)
      channelStore.fetchAll()
    } catch (e: any) {
      runInAction(() => {
        if (ch && orig && origIdx !== -1) {
          const arr = [...ch.default_filler_entries]
          arr.splice(origIdx, 0, orig)
          ch.default_filler_entries = arr
        }
        this.channelFillerErr = e.message
      })
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

  toggleHints() { this.showHints = !this.showHints }

  openPicker() {
    clearTimeout(_debounce)
    this.pickerOpen      = true; this.pickerQuery = ''; this.pickerSeasonFilter = ''
    this.filterRules     = []; this.filterRulesOpen = false; this.filterMatch = 'all'
    this.expandedShowId  = null; this.expandedSeasons = []
    this.pickerShows     = []; this.pickerMovies = []; this.pickerEpisodes = []
    this.pickerPlaylists = []
    this.pickerTab       = defaultPickerTab(this.draft.block_type)
    if (this.allLibraries.length === 0)
      api.getAllLibraries().then(libs => runInAction(() => { this.allLibraries = libs }))
    this.searchPicker()
  }

  closePicker() {
    clearTimeout(_debounce)
    this.pickerOpen  = false; this.pickerQuery = ''; this.pickerSeasonFilter = ''
    this.filterRules = []; this.filterRulesOpen = false; this.filterMatch = 'all'
    this.expandedShowId = null
    this.pickerShows = []; this.pickerMovies = []; this.pickerEpisodes = []
    this.pickerPlaylists = []
  }

  setPickerTab(t: PickerTab) {
    clearTimeout(_debounce)
    this.pickerTab   = t; this.expandedShowId = null; this.pickerSeasonFilter = ''; this.pickerDurationMax = ''
    this.filterRules = []; this.filterRulesOpen = false; this.filterMatch = 'all'
    this.searchPicker()
  }

  setPickerQuery(q: string) {
    this.pickerQuery = q; this.expandedShowId = null
    clearTimeout(_debounce)
    _debounce = setTimeout(() => this.searchPicker(), 250)
  }

  setPickerSeasonFilter(v: string) {
    this.pickerSeasonFilter = v
    clearTimeout(_debounce)
    _debounce = setTimeout(() => this.searchPicker(), 250)
  }

  setPickerDurationMax(v: string) {
    this.pickerDurationMax = v
  }

  async searchPicker() {
    _searchCtrl?.abort()
    _searchCtrl = new AbortController()
    const { signal } = _searchCtrl

    this.pickerLoading = true
    const q       = this.pickerQuery || undefined
    const isRules = this.filterRules.filter(r => r.op === 'is' && r.value.trim())
    const lib     = isRules.find(r => r.field === 'library')?.value        || undefined
    const genre   = isRules.find(r => r.field === 'genre')?.value          || undefined
    const yearStr = isRules.find(r => r.field === 'year')?.value
    const year    = yearStr ? parseInt(yearStr) : undefined
    const rating  = isRules.find(r => r.field === 'content_rating')?.value || undefined
    const label   = isRules.find(r => r.field === 'label')?.value          || undefined
    const network = isRules.find(r => r.field === 'network')?.value        || undefined
    const actor   = isRules.find(r => r.field === 'actor')?.value          || undefined
    const seasonParsed = this.pickerSeasonFilter.trim() !== '' ? parseInt(this.pickerSeasonFilter, 10) : undefined
    const season  = Number.isFinite(seasonParsed) ? seasonParsed : undefined
    try {
      switch (this.pickerTab) {
        case 'shows':        { const r = await raceAbort(api.getShows({ limit: 50, q, library_id: lib, genre, year, content_rating: rating, label, network, actor }), signal); runInAction(() => { this.pickerShows = r.items; this.pickerTotal = r.total; this.pickerLoading = false }); break }
        case 'movies':       { const r = await raceAbort(api.getMovies({ limit: 50, q, library_id: lib, genre, year, content_rating: rating, label, actor }), signal); runInAction(() => { this.pickerMovies = r.items; this.pickerTotal = r.total; this.pickerLoading = false }); break }
        case 'episodes':     { const r = await raceAbort(api.searchEpisodes({ q, season, limit: 50 }), signal); runInAction(() => { this.pickerEpisodes = r.items; this.pickerTotal = 0; this.pickerEpsHasMore = r.items.length >= 50; this.pickerLoading = false }); break }
        case 'playlists':    { const r = await raceAbort(api.getPlaylists(), signal); runInAction(() => { this.pickerPlaylists = r; this.pickerTotal = 0; this.pickerLoading = false }); break }
      }
    } catch (e) {
      if (signal.aborted) return
      runInAction(() => { this.pickerLoading = false })
    }
  }

  async loadMorePicker() {
    if (this.pickerLoadingMore) return
    const q       = this.pickerQuery || undefined
    const isRules = this.filterRules.filter(r => r.op === 'is' && r.value.trim())
    const lib     = isRules.find(r => r.field === 'library')?.value        || undefined
    const genre   = isRules.find(r => r.field === 'genre')?.value          || undefined
    const yearStr = isRules.find(r => r.field === 'year')?.value
    const year    = yearStr ? parseInt(yearStr) : undefined
    const rating  = isRules.find(r => r.field === 'content_rating')?.value || undefined
    const label   = isRules.find(r => r.field === 'label')?.value          || undefined
    const network = isRules.find(r => r.field === 'network')?.value        || undefined
    const actor   = isRules.find(r => r.field === 'actor')?.value          || undefined
    this.pickerLoadingMore = true
    try {
      if (this.pickerTab === 'shows') {
        const r = await api.getShows({ limit: 50, offset: this.pickerShows.length, q, library_id: lib, genre, year, content_rating: rating, label, network, actor })
        runInAction(() => { this.pickerShows = [...this.pickerShows, ...r.items]; this.pickerTotal = r.total; this.pickerLoadingMore = false })
      } else if (this.pickerTab === 'movies') {
        const r = await api.getMovies({ limit: 50, offset: this.pickerMovies.length, q, library_id: lib, genre, year, content_rating: rating, label, actor })
        runInAction(() => { this.pickerMovies = [...this.pickerMovies, ...r.items]; this.pickerTotal = r.total; this.pickerLoadingMore = false })
      } else if (this.pickerTab === 'episodes') {
        const seasonParsed = this.pickerSeasonFilter.trim() !== '' ? parseInt(this.pickerSeasonFilter, 10) : undefined
        const season = Number.isFinite(seasonParsed) ? seasonParsed : undefined
        const r = await api.searchEpisodes({ q, season, limit: 50, offset: this.pickerEpisodes.length })
        runInAction(() => { this.pickerEpisodes = [...this.pickerEpisodes, ...r.items]; this.pickerEpsHasMore = r.items.length >= 50; this.pickerLoadingMore = false })
      }
    } catch {
      runInAction(() => { this.pickerLoadingMore = false })
    }
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
