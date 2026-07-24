import { observer } from 'mobx-react-lite'
import { makeAutoObservable, runInAction } from 'mobx'
import { useEffect, useState, useCallback, useRef } from 'react'
import { useAuth } from '../auth/AuthContext'
import { api, mediaUrl, ApiError } from '../api/client'
import { ExternalIdEditor } from '../components/media/LibraryAdminPanel'
import type {
  ReviewQueueItem, ItemMatchCandidate,
  EpisodeGroup, GroupingCandidate, ShowGroupingResult,
  ArrLookupResult, ArrServiceOptions, ContentRequest, RequestStatus,
  ScraperSearchResult, ChapterReviewItem, Chapter, ChapterType,
  ItemMetadata, ExternalId, MergedInto, DuplicateCandidate, BrokenSubtitleItem,
} from '../api/types'
import { MatchBadge } from '../components/media/MatchBadge'
import type { MatchStatus } from '../components/media/MatchBadge'
import { folderBaseName } from '../components/media/useMediaDetail'
import sharedStyles from '../channel/sharedStyles.module.css'
import { useDebounce } from '../hooks/useDebounce'
import { usePlaybackSession } from '../player/usePlaybackSession'
import type { PlaybackTarget } from '../player/usePlaybackSession'
import { VideoPlayer } from '../player/VideoPlayer'
import { LoadingThrobber } from '../player/LoadingThrobber'
import styles from './ReviewPage.module.css'

// ── Episode Groups store ───────────────────────────────────────────────────────

class GroupsStore {
  shows:           ShowGroupingResult[] = []
  loading:         boolean = false
  selectedShowId:  string | null = null
  confirmedGroups: EpisodeGroup[] = []
  groupsLoading:   boolean = false
  saving:          boolean = false

  constructor() { makeAutoObservable(this) }

  get selectedShow(): ShowGroupingResult | null {
    return this.shows.find(s => s.show_id === this.selectedShowId) ?? null
  }

  get pendingCount(): number {
    return this.shows.reduce((n, s) => n + s.candidates.filter(c => !c.already_grouped).length, 0)
  }

  async load() {
    this.loading = true
    try {
      const results = await api.getAllGroupingCandidates()
      runInAction(() => {
        this.shows   = results
        this.loading = false
        if (this.selectedShowId && !results.find(s => s.show_id === this.selectedShowId))
          this.selectedShowId = null
        if (!this.selectedShowId && results.length > 0)
          this.selectShow(results[0].show_id)
      })
    } catch {
      runInAction(() => { this.loading = false })
    }
  }

  selectShow(id: string) {
    this.selectedShowId  = id
    this.confirmedGroups = []
    this.groupsLoading   = true
    api.getEpisodeGroups(id)
      .then(gs => runInAction(() => { this.confirmedGroups = gs; this.groupsLoading = false }))
      .catch(()  => runInAction(() => { this.groupsLoading = false }))
  }

  async confirmGroup(candidate: GroupingCandidate) {
    const showId = this.selectedShowId
    if (!showId) return
    this.saving = true
    try {
      const { group_id } = await api.createEpisodeGroup(showId, { name: candidate.base_title, group_type: 'multipart' })
      await Promise.all(candidate.parts.map(p => api.addGroupMember(showId, group_id, { episode_id: p.episode_id, part_num: p.part_num })))
    } catch {}
    runInAction(() => { this.saving = false })
    this.load()
    api.getEpisodeGroups(showId).then(gs => runInAction(() => { this.confirmedGroups = gs })).catch(() => {})
  }

  async deleteGroup(groupId: string) {
    const showId = this.selectedShowId
    if (!showId) return
    this.saving = true
    try { await api.deleteEpisodeGroup(showId, groupId) } catch {}
    runInAction(() => { this.saving = false })
    this.load()
    api.getEpisodeGroups(showId).then(gs => runInAction(() => { this.confirmedGroups = gs })).catch(() => {})
  }

  async confirmAllHigh() {
    const showId = this.selectedShowId
    const show   = this.selectedShow
    if (!showId || !show) return
    const high = show.candidates.filter(c => !c.already_grouped && c.confidence >= 80)
    this.saving = true
    try {
      for (const c of high) {
        const { group_id } = await api.createEpisodeGroup(showId, { name: c.base_title, group_type: 'multipart' })
        await Promise.all(c.parts.map(p => api.addGroupMember(showId, group_id, { episode_id: p.episode_id, part_num: p.part_num })))
      }
    } catch {}
    runInAction(() => { this.saving = false })
    this.load()
    api.getEpisodeGroups(showId).then(gs => runInAction(() => { this.confirmedGroups = gs })).catch(() => {})
  }
}

const groupsStore = new GroupsStore()

// ── Tab type ──────────────────────────────────────────────────────────────────

type Tab = 'queue' | 'groups' | 'requests' | 'duplicates' | 'chapters' | 'subtitles'

// ── Page ─────────────────────────────────────────────────────────────────────

export default observer(function ReviewPage() {
  const { user } = useAuth()
  const [tab, setTab] = useState<Tab>('queue')

  // ── Queue state ──────────────────────────────────────────────────────────────
  const [queueItems,    setQueueItems]    = useState<ReviewQueueItem[]>([])
  const [queueTotal,    setQueueTotal]    = useState(0)
  const [queueLoading,  setQueueLoading]  = useState(true)
  const [queueFilter,   setQueueFilter]   = useState<'all'|'uncertain'|'unmatched'|'auto_unconfirmed'>('all')
  const [selectedQueue, setSelectedQueue] = useState<ReviewQueueItem | null>(null)
  const [matching,      setMatching]      = useState(false)
  // Set when accepting/manually-matching a candidate turns out to duplicate an
  // already-matched item — that row gets merged away rather than becoming its
  // own match, so this is the only feedback the reviewer gets about it.
  const [mergeNotice,   setMergeNotice]   = useState<{ fromTitle: string; into: MergedInto } | null>(null)

  const fetchQueue = useCallback(() => {
    setQueueLoading(true)
    api.getReviewQueue({ status: queueFilter, limit: 48 })
      .then(r => { setQueueItems(r.items); setQueueTotal(r.total) })
      .catch(() => {})
      .finally(() => setQueueLoading(false))
  }, [queueFilter])

  useEffect(() => { fetchQueue() }, [fetchQueue])

  // Resolved items drop out of the queue, so re-finding the old kairos_id
  // after a refetch never works — instead select whatever now sits at the
  // resolved item's old position, which is the next item in the list.
  const advanceQueueSelection = async (prevKairosId: string | undefined) => {
    const prevIndex = prevKairosId ? queueItems.findIndex(i => i.kairos_id === prevKairosId) : -1
    const updated = await api.getReviewQueue({ status: queueFilter, limit: 48 })
    setQueueItems(updated.items)
    setQueueTotal(updated.total)
    const next = prevIndex >= 0 ? updated.items[Math.min(prevIndex, updated.items.length - 1)] : null
    setSelectedQueue(next ?? null)
    return updated
  }

  const handleAccept = async (candidate_id: string) => {
    const fromTitle = selectedQueue?.title
    console.log('Accepting candidate', candidate_id, 'from', fromTitle)
    const result = await api.acceptCandidate(candidate_id)
    await advanceQueueSelection(selectedQueue?.kairos_id)
    setMergeNotice(result.merged_into && fromTitle ? { fromTitle, into: result.merged_into } : null)
  }

  const handleReject = async (candidate_id: string) => {
    await api.rejectCandidate(candidate_id)
    const updated = await api.getReviewQueue({ status: queueFilter, limit: 48 })
    setQueueItems(updated.items)
    setQueueTotal(updated.total)
    if (selectedQueue) {
      const refreshed = updated.items.find(i => i.kairos_id === selectedQueue.kairos_id)
      setSelectedQueue(refreshed ?? null)
    }
  }

  const triggerMatch = async () => {
    setMatching(true)
    await api.triggerMatch()
    setTimeout(() => { setMatching(false); fetchQueue() }, 3000)
  }

  // ── Groups state ─────────────────────────────────────────────────────────────
  useEffect(() => {
    if (tab === 'groups') groupsStore.load()
  }, [tab])

  // ── Requests state ───────────────────────────────────────────────────────────
  const [requests,     setRequests]     = useState<ContentRequest[]>([])
  const [reqLoading,   setReqLoading]   = useState(false)
  const [reqFilter,    setReqFilter]    = useState<'all'|RequestStatus>('pending')
  const [selectedReq,  setSelectedReq]  = useState<ContentRequest | null>(null)

  const fetchRequests = useCallback(() => {
    setReqLoading(true)
    api.getRequests().then(setRequests).catch(() => {}).finally(() => setReqLoading(false))
  }, [])

  useEffect(() => {
    if (tab === 'requests') fetchRequests()
  }, [tab, fetchRequests])

  const visibleRequests = requests.filter(r => reqFilter === 'all' || r.status === reqFilter)
  const pendingReqCount = requests.filter(r => r.status === 'pending').length

  // ── Duplicates state ─────────────────────────────────────────────────────────
  const [duplicateItems,     setDuplicateItems]     = useState<DuplicateCandidate[]>([])
  const [duplicateTotal,     setDuplicateTotal]     = useState(0)
  const [duplicateLoading,   setDuplicateLoading]   = useState(false)
  const [duplicateFilter,    setDuplicateFilter]    = useState<'all'|'show'|'movie'>('all')
  const [selectedDuplicate,  setSelectedDuplicate]  = useState<DuplicateCandidate | null>(null)

  const fetchDuplicates = useCallback(() => {
    setDuplicateLoading(true)
    api.getDuplicatesQueue({ item_type: duplicateFilter === 'all' ? undefined : duplicateFilter, limit: 48 })
      .then(r => { setDuplicateItems(r.items); setDuplicateTotal(r.total) })
      .catch(() => {})
      .finally(() => setDuplicateLoading(false))
  }, [duplicateFilter])

  useEffect(() => {
    if (tab === 'duplicates') fetchDuplicates()
  }, [tab, fetchDuplicates])

  // ── Chapters state ───────────────────────────────────────────────────────────
  const [chapterItems,     setChapterItems]     = useState<ChapterReviewItem[]>([])
  const [chapterTotal,     setChapterTotal]     = useState(0)
  const [chapterLoading,   setChapterLoading]   = useState(false)
  const [chapterMediaType, setChapterMediaType] = useState<'all'|'episode'|'movie'>('all')
  const [chapterType,      setChapterType]      = useState<'all'|ChapterType>('all')
  const [chapterQueryRaw,  setChapterQueryRaw]  = useState('')
  const chapterQuery = useDebounce(chapterQueryRaw, 300)
  const [selectedChapterItem, setSelectedChapterItem] = useState<ChapterReviewItem | null>(null)

  const fetchChapterItems = useCallback(() => {
    setChapterLoading(true)
    api.getChapterReviewItems({ media_type: chapterMediaType, chapter_type: chapterType, q: chapterQuery, limit: 48 })
      .then(r => { setChapterItems(r.items); setChapterTotal(r.total) })
      .catch(() => {})
      .finally(() => setChapterLoading(false))
  }, [chapterMediaType, chapterType, chapterQuery])

  useEffect(() => { if (tab === 'chapters') fetchChapterItems() }, [tab, fetchChapterItems])

  // ── Broken subtitles state ──────────────────────────────────────────────────
  const [subtitleItems,     setSubtitleItems]     = useState<BrokenSubtitleItem[]>([])
  const [subtitleLoading,   setSubtitleLoading]   = useState(false)
  const [selectedSubtitle,  setSelectedSubtitle]  = useState<BrokenSubtitleItem | null>(null)

  const fetchSubtitleItems = useCallback(() => {
    setSubtitleLoading(true)
    api.getBrokenSubtitles()
      .then(setSubtitleItems)
      .catch(() => {})
      .finally(() => setSubtitleLoading(false))
  }, [])

  useEffect(() => { if (tab === 'subtitles') fetchSubtitleItems() }, [tab, fetchSubtitleItems])

  // ── Tab switch ───────────────────────────────────────────────────────────────
  const switchTab = (t: Tab) => {
    setTab(t)
    setSelectedQueue(null)
    setSelectedReq(null)
    setSelectedDuplicate(null)
    setSelectedChapterItem(null)
    setSelectedSubtitle(null)
  }

  // ── Render ───────────────────────────────────────────────────────────────────
  return (
    <div className={styles.root}>
      {tab === 'queue' && mergeNotice && (
        <div className={styles.mergeNotice}>
          <div className={styles.mergeNoticeText}>
            <strong>{mergeNotice.fromTitle}</strong> was a duplicate of <strong>{mergeNotice.into.title}</strong> — merged into it.
          </div>
          <button
            onClick={() => setMergeNotice(null)}
            className={styles.iconCloseBtnSm}
          >✕</button>
        </div>
      )}
      {/* ── Left panel ─────────────────────────────────────────────────────── */}
      <div className={styles.leftPanel}>
        {/* Tab bar */}
        <div className={styles.tabBar}>
          {([
            { key: 'queue',    label: 'Queue',    badge: null,                                                            admin: false },
            { key: 'groups',   label: 'Groups',   badge: groupsStore.pendingCount > 0 ? groupsStore.pendingCount : null, admin: false },
            { key: 'requests', label: 'Requests', badge: pendingReqCount > 0 ? pendingReqCount : null,                   admin: true  },
            { key: 'duplicates', label: 'Duplicates', badge: duplicateTotal > 0 ? duplicateTotal : null,                 admin: true  },
            { key: 'chapters', label: 'Chapters', badge: null,                                                            admin: false },
            { key: 'subtitles', label: 'Subtitles', badge: subtitleItems.length > 0 ? subtitleItems.length : null,       admin: true  },
          ] as const).filter(t => !t.admin || user?.role === 'admin').map(({ key, label, badge }) => (
            <button
              key={key}
              onClick={() => switchTab(key as Tab)}
              className={`${styles.mainTab} ${tab === key ? styles.mainTabActive : ''}`}
            >
              {label.toUpperCase()}
              {badge !== null && (
                <span className={styles.tabBadge}>{badge > 99 ? '99+' : badge}</span>
              )}
            </button>
          ))}
        </div>

        {/* List area */}
        {tab === 'queue'    && <QueueListPanel
          items={queueItems} total={queueTotal} loading={queueLoading}
          filter={queueFilter} selected={selectedQueue}
          onFilterChange={setQueueFilter}
          onSelect={setSelectedQueue}
          onTriggerMatch={triggerMatch}
          matching={matching}
        />}
        {tab === 'groups'   && <GroupsListPanel store={groupsStore} />}
        {tab === 'requests' && <RequestsListPanel
          items={visibleRequests} loading={reqLoading}
          filter={reqFilter} selected={selectedReq}
          allRequests={requests}
          onFilterChange={setReqFilter}
          onSelect={r => setSelectedReq(prev => prev?.request_id === r.request_id ? null : r)}
        />}
        {tab === 'duplicates' && <DuplicatesListPanel
          items={duplicateItems} total={duplicateTotal} loading={duplicateLoading}
          filter={duplicateFilter} selected={selectedDuplicate}
          onFilterChange={setDuplicateFilter}
          onSelect={d => setSelectedDuplicate(prev => prev?.candidate_id === d.candidate_id ? null : d)}
        />}
        {tab === 'chapters' && <ChaptersListPanel
          items={chapterItems} total={chapterTotal} loading={chapterLoading}
          mediaType={chapterMediaType} chapterType={chapterType} query={chapterQueryRaw}
          selected={selectedChapterItem}
          onMediaTypeChange={setChapterMediaType}
          onChapterTypeChange={setChapterType}
          onQueryChange={setChapterQueryRaw}
          onSelect={setSelectedChapterItem}
        />}
        {tab === 'subtitles' && <SubtitlesListPanel
          items={subtitleItems} loading={subtitleLoading} selected={selectedSubtitle}
          onSelect={setSelectedSubtitle}
        />}
      </div>

      {/* ── Right panel ────────────────────────────────────────────────────── */}
      {tab === 'queue' && (
        selectedQueue
          ? <CandidatePanel
              key={selectedQueue.kairos_id}
              item={selectedQueue}
              onAccept={handleAccept}
              onReject={handleReject}
              onClose={() => setSelectedQueue(null)}
              onMatched={async (merged) => {
                const fromTitle = selectedQueue?.title
                await advanceQueueSelection(selectedQueue?.kairos_id)
                setMergeNotice(merged && fromTitle ? { fromTitle, into: merged } : null)
              }}
            />
          : <EmptyHint>Select an item to review candidates</EmptyHint>
      )}
      {tab === 'groups' && <GroupsDetailPanel store={groupsStore} />}
      {tab === 'requests' && (
        selectedReq
          ? <RequestDetailPanel
              request={selectedReq}
              onClose={() => setSelectedReq(null)}
              onStatusChange={(id, status) => {
                setRequests(prev => prev.map(r => r.request_id === id ? { ...r, status } : r))
                setSelectedReq(prev => prev?.request_id === id ? { ...prev, status } : prev)
              }}
            />
          : <EmptyHint>Select a request to review</EmptyHint>
      )}
      {tab === 'duplicates' && (
        selectedDuplicate
          ? <DuplicateDetailPanel
              key={selectedDuplicate.candidate_id}
              item={selectedDuplicate}
              onClose={() => setSelectedDuplicate(null)}
              onResolved={() => { setSelectedDuplicate(null); fetchDuplicates() }}
            />
          : <EmptyHint>Select a pair to compare</EmptyHint>
      )}
      {tab === 'chapters' && (
        selectedChapterItem
          ? <ChapterInspectorPanel
              key={selectedChapterItem.media_type + selectedChapterItem.media_id}
              item={selectedChapterItem}
              onClose={() => setSelectedChapterItem(null)}
            />
          : <EmptyHint>Select an item to inspect its chapters</EmptyHint>
      )}
      {tab === 'subtitles' && (
        selectedSubtitle
          ? <SubtitleInspectorPanel
              key={selectedSubtitle.subtitle_id}
              item={selectedSubtitle}
              onClose={() => setSelectedSubtitle(null)}
              onChanged={updated => {
                setSelectedSubtitle(updated)
                if (updated.valid) {
                  // Fixed on disk since it was flagged (a recheck, not an
                  // edit — editing language/forced/sdh never changes
                  // validity) — no longer belongs in a "broken" list at all.
                  setSubtitleItems(prev => prev.filter(i => i.subtitle_id !== updated.subtitle_id))
                  setSelectedSubtitle(null)
                } else {
                  setSubtitleItems(prev => prev.map(i => i.subtitle_id === updated.subtitle_id ? updated : i))
                }
              }}
              onDeleted={id => {
                setSubtitleItems(prev => prev.filter(i => i.subtitle_id !== id))
                setSelectedSubtitle(null)
              }}
            />
          : <EmptyHint>Select a file to inspect</EmptyHint>
      )}
    </div>
  )
})

// ── Shared ────────────────────────────────────────────────────────────────────

function EmptyHint({ children }: { children: React.ReactNode }) {
  return (
    <div className={styles.emptyHint}>
      {children}
    </div>
  )
}

// ── Queue tab ─────────────────────────────────────────────────────────────────

const QUEUE_FILTER_LABELS: Record<'all'|'uncertain'|'unmatched'|'auto_unconfirmed', string> = {
  all:              'ALL',
  uncertain:        'UNCERTAIN',
  unmatched:        'UNMATCHED',
  // Auto-matched by the scraper, never reviewed by a human — distinct from a
  // genuinely user-confirmed match. See match_confirmed / acceptCandidate().
  auto_unconfirmed: 'AUTO-MATCHED',
}

function QueueListPanel({
  items, total, loading, filter, selected, matching,
  onFilterChange, onSelect, onTriggerMatch,
}: {
  items: ReviewQueueItem[]; total: number; loading: boolean
  filter: 'all'|'uncertain'|'unmatched'|'auto_unconfirmed'; selected: ReviewQueueItem | null; matching: boolean
  onFilterChange: (f: 'all'|'uncertain'|'unmatched'|'auto_unconfirmed') => void
  onSelect: (item: ReviewQueueItem) => void
  onTriggerMatch: () => void
}) {
  return (
    <>
      {/* Header */}
      <div className={styles.panelHeader}>
        <div className={styles.panelHeaderRow}>
          <span className={styles.panelTitle}>
            Review Queue
          </span>
          <span className={styles.panelCount}>
            {total} item{total !== 1 ? 's' : ''}
          </span>
        </div>
        <div className={styles.filterRow}>
          {(['all', 'uncertain', 'unmatched', 'auto_unconfirmed'] as const).map(f => (
            <button key={f} onClick={() => onFilterChange(f)} className={`${styles.filterPill} ${filter === f ? styles.filterPillActive : ''}`}>
              {QUEUE_FILTER_LABELS[f]}
            </button>
          ))}
        </div>
      </div>

      {/* List */}
      <div className={`${styles.listScroll} scrollbar-dark`}>
        {loading ? (
          <div className={styles.skeletonListGap1}>
            {Array.from({ length: 8 }, (_, i) => (
              <div key={i} className={`hds-skeleton ${styles.skeletonRow60}`} />
            ))}
          </div>
        ) : items.length === 0 ? (
          <div className={styles.emptyState}>
            Nothing in the queue.<br />
            <span className={styles.emptyStateSub}>Run a match pass to find uncertain items.</span>
          </div>
        ) : (
          items.map(item => (
            <button
              key={item.kairos_id}
              onClick={() => onSelect(item)}
              className={`${styles.queueListRow}
                ${selected?.kairos_id === item.kairos_id ? styles.queueListRowSelected : ''}
                ${item.match_status === 'uncertain' ? styles.queueListRowUncertain
                  : item.match_status === 'matched'  ? styles.queueListRowMatched
                  : styles.queueListRowUnmatched}`}
            >
              {item.thumb && (
                <img
                  src={mediaUrl(`/api/${item.item_type}s/${item.kairos_id}/thumb`)}
                  alt=""
                  className={styles.queueThumb}
                  onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
                />
              )}
              <div className={styles.queueInfo}>
                <div className={styles.queueTitle}>{item.title}</div>
                <div className={styles.queueMeta}>
                  {item.item_type}{item.year ? ` · ${item.year}` : ''}
                </div>
                {item.folder_path && (
                  <div
                    title={item.folder_path}
                    className={styles.queueFolder}
                  >{folderBaseName(item.folder_path)}</div>
                )}
                <div className={styles.queueMatchRow}>
                  <MatchBadge status={item.match_status as MatchStatus} score={item.match_score} size="sm" />
                  {item.match_score > 0 && (
                    <span className={styles.queueMatchPct}>
                      {Math.round(item.match_score * 100)}%
                    </span>
                  )}
                </div>
              </div>
              <span className={styles.queueCandidateCount}>
                {item.candidates.length}
              </span>
            </button>
          ))
        )}
      </div>

      {/* Run match button */}
      <div className={styles.queueRunMatchWrap}>
        <button
          onClick={onTriggerMatch}
          disabled={matching}
          className={`${sharedStyles.goldBtn} ${styles.queueRunMatchBtn} ${matching ? styles.queueRunMatchBtnDisabled : ''}`}
        >
          {matching ? 'Running…' : 'Run Match Pass'}
        </button>
      </div>
    </>
  )
}

// ── Queue candidate panel ─────────────────────────────────────────────────────

// Normalized row from api.getShows()/api.getMovies() for the Link panel.
interface LinkTarget {
  kairos_id: string
  title:     string
  year?:     number
  thumb?:    string
}

function CandidatePanel({
  item, onAccept, onReject, onClose, onMatched,
}: {
  item:      ReviewQueueItem
  onAccept:  (id: string) => void
  onReject:  (id: string) => void
  onClose:   () => void
  onMatched: (merged?: MergedInto) => void
}) {
  const { user } = useAuth()
  const isAdmin  = user?.role === 'admin'
  const [mode,          setMode]          = useState<'candidates'|'search'|'link'>(
    item.candidates.length === 0 ? 'search' : 'candidates'
  )
  const [searchQuery,   setSearchQuery]   = useState(item.title)
  const [searchResults, setSearchResults] = useState<ScraperSearchResult[]>([])
  const [searchLoading, setSearchLoading] = useState(false)
  const [matchingId,    setMatchingId]    = useState<string | null>(null)

  const [linkQuery,   setLinkQuery]   = useState(item.title)
  const [linkResults, setLinkResults] = useState<LinkTarget[]>([])
  const [linkLoading, setLinkLoading] = useState(false)
  const [linkingId,   setLinkingId]   = useState<string | null>(null)
  const [folderWarning, setFolderWarning] = useState<{ kairos_id: string; targetFolder: string; duplicateFolder: string } | null>(null)
  const [linkError,   setLinkError]   = useState<string | null>(null)
  const [metadata,    setMetadata]    = useState<ItemMetadata | null>(null)
  const [loadingMeta, setLoadingMeta] = useState(false)

  const loadMeta = async () => {
    setLoadingMeta(true)
    try { setMetadata(await api.getItemMetadata(item.item_type, item.kairos_id)) } catch {}
    setLoadingMeta(false)
  }

  useEffect(() => { if (mode === 'candidates') loadMeta() }, [item.kairos_id, mode])

  // Only for the case above where no candidates default us straight into
  // Search — switchMode() below covers every other way of getting there.
  useEffect(() => {
    if (item.candidates.length === 0) runSearch(item.title)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  // Guards against an earlier, slower search response (AniDB in particular
  // is rate-limited to ~1 req/2s server-side, so an in-flight search can
  // easily still be running when a newer one is fired) landing after a
  // more recent search and clobbering its results.
  const searchSeq = useRef(0)
  const runSearch = async (q: string) => {
    if (!q.trim()) return
    const seq = ++searchSeq.current
    setSearchLoading(true)
    setSearchResults([])
    try {
      const r = await api.scraperSearch(q.trim(), item.item_type)
      if (seq === searchSeq.current) setSearchResults(r.items)
    } catch {}
    if (seq === searchSeq.current) setSearchLoading(false)
  }

  const handleManualMatch = async (result: ScraperSearchResult) => {
    const key = result.source + ':' + result.external_id
    setMatchingId(key)
    try {
      const r = await api.manualMatch(item.kairos_id, {
        item_type:   item.item_type,
        source:      result.source,
        external_id: result.external_id,
        title:       result.title,
        year:        result.year,
        poster_url:  result.poster_url,
        overview:    result.overview,
      })
      onMatched(r.merged_into)
    } catch {
      setMatchingId(null)
    }
  }

  const runLinkSearch = async (q: string) => {
    if (!q.trim()) return
    setLinkLoading(true)
    setLinkResults([])
    try {
      if (item.item_type === 'show') {
        const r = await api.getShows({ q: q.trim(), limit: 12 })
        setLinkResults(r.items.filter(s => s.show_id !== item.kairos_id).map(s => ({ kairos_id: s.show_id, title: s.title, year: s.year, thumb: s.thumb })))
      } else {
        const r = await api.getMovies({ q: q.trim(), limit: 12 })
        setLinkResults(r.items.filter(m => m.movie_id !== item.kairos_id).map(m => ({ kairos_id: m.movie_id, title: m.title, year: m.year, thumb: m.thumb })))
      }
    } catch {}
    setLinkLoading(false)
  }

  const handleLink = async (target: LinkTarget, confirmed = false) => {
    setLinkingId(target.kairos_id)
    setFolderWarning(null)
    setLinkError(null)
    try {
      if (item.item_type === 'show') await api.mergeShow(target.kairos_id, item.kairos_id, confirmed)
      else                            await api.mergeMovie(target.kairos_id, item.kairos_id, confirmed)
      onMatched()
    } catch (e) {
      if (e instanceof ApiError && e.status === 409 && e.body?.error === 'folder_mismatch') {
        setFolderWarning({ kairos_id: target.kairos_id, targetFolder: e.body.target_folder, duplicateFolder: e.body.duplicate_folder })
      } else if (e instanceof ApiError && e.status === 423) {
        setLinkError('Library sync is in progress — try linking again in a moment.')
      } else if (e instanceof ApiError) {
        setLinkError(e.body?.error || 'Unable to link — please try again.')
      } else {
        setLinkError('Unable to link — please try again.')
      }
      setLinkingId(null)
    }
  }

  const switchMode = (m: 'candidates'|'search'|'link') => {
    setMode(m)
    if (m === 'search') { setSearchResults([]); setSearchQuery(item.title); runSearch(item.title) }
    if (m === 'link')   { setLinkResults([]);   setLinkQuery(item.title); setFolderWarning(null); setLinkError(null) }
  }

  return (
    <div className={styles.panelBody}>
      {/* Header */}
      <div className={styles.candidateHeader}>
        <div className={styles.detailHeaderTop}>
          <h2 className={styles.detailHeaderTitle}>{item.title}</h2>
          <div className={styles.detailHeaderChips}>
            {item.year && <span className={styles.chip}>{item.year}</span>}
            <span className={styles.chip}>{item.item_type}</span>
            <MatchBadge status={item.match_status as MatchStatus} score={item.match_score} />
          </div>
          {item.folder_path && (
            <div
              title={item.folder_path}
              className={styles.detailHeaderFolder}
            >
              <span className={styles.mutedLabel}>Folder: </span>{folderBaseName(item.folder_path)}
            </div>
          )}
        </div>
        <div className={styles.modeSwitchRow}>
          {([
            { key: 'candidates', label: 'Candidates' },
            { key: 'search',     label: 'Search' },
            { key: 'link',       label: 'Link Existing' },
          ] as const).map(t => (
            <button
              key={t.key}
              onClick={() => switchMode(t.key)}
              className={`${styles.modeSwitchBtn} ${mode === t.key ? styles.modeSwitchBtnActive : ''}`}
            >
              {t.label}
            </button>
          ))}
        </div>
        <button onClick={onClose} className={styles.iconCloseBtn}>✕</button>
      </div>

      {mode === 'link' ? (
        /* ── Link Existing panel ── */
        <div className={styles.panelBody}>
          <div className={styles.searchBarWrap}>
            <div className={styles.linkNoteText}>
              Link this to an existing library {item.item_type} — for the same title synced separately
              from another source (Plex/Jellyfin/local) that didn't auto-merge. The item you pick below
              survives; this queue item is absorbed into it.
            </div>
            <div className={styles.searchBarRow}>
              <input
                autoFocus
                value={linkQuery}
                onChange={e => setLinkQuery(e.target.value)}
                onKeyDown={e => e.key === 'Enter' && runLinkSearch(linkQuery)}
                placeholder={`Search existing ${item.item_type}s…`}
                className={styles.searchInput}
              />
              <button
                onClick={() => runLinkSearch(linkQuery)}
                disabled={linkLoading || !linkQuery.trim()}
                className={`${styles.searchBtn} ${(linkLoading || !linkQuery.trim()) ? styles.searchBtnDisabled : ''}`}
              >
                {linkLoading ? '…' : 'Search'}
              </button>
            </div>
          </div>

          {linkError && (
            <div className={styles.linkErrorBanner}>
              {linkError}
            </div>
          )}

          <div className={`${styles.panelBodyScroll} scrollbar-dark`}>
            {linkLoading ? (
              <div className={styles.skeletonListGap8}>
                {Array.from({ length: 4 }, (_, i) => (
                  <div key={i} className={`hds-skeleton ${styles.skeletonRow60}`} />
                ))}
              </div>
            ) : linkResults.length === 0 ? (
              <div className={`${styles.emptyState} ${styles.emptyStatePad0}`}>
                {linkQuery.trim() ? 'No matches in the library.' : 'Enter a title to search.'}
              </div>
            ) : (
              <div className={styles.skeletonListGap8}>
                {linkResults.map(r => {
                  const isLinking = linkingId === r.kairos_id
                  const warning   = folderWarning?.kairos_id === r.kairos_id ? folderWarning : null
                  return (
                    <div key={r.kairos_id} className={`${styles.resultCard} ${warning ? styles.resultCardWarning : ''} ${isLinking ? styles.resultCardLinking : ''}`}>
                      <div className={styles.resultCardTopRow}>
                        {r.thumb && (
                          <img src={mediaUrl(`/api/${item.item_type === 'show' ? 'shows' : 'movies'}/${r.kairos_id}/thumb`)} alt=""
                            className={styles.resultThumb}
                            onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
                          />
                        )}
                        <div className={styles.resultInfo}>
                          <div className={styles.resultTitle}>{r.title}</div>
                          {r.year && <div className={styles.resultYear}>{r.year}</div>}
                        </div>
                        {!warning && (
                          <button
                            onClick={() => handleLink(r)}
                            disabled={isLinking}
                            className={`${styles.linkBtn} ${isLinking ? styles.linkBtnDisabled : ''}`}
                          >
                            {isLinking ? '…' : 'Link'}
                          </button>
                        )}
                      </div>
                      {warning && (
                        <div className={styles.warningBlock}>
                          <div className={styles.warningText}>
                            These are in different folders — make sure they're really the same title before merging.
                            <div className={styles.warningFolderPath}>{warning.targetFolder}</div>
                            <div className={styles.warningFolderPath}>{warning.duplicateFolder}</div>
                          </div>
                          <div className={styles.warningActions}>
                            <button
                              onClick={() => handleLink(r, true)}
                              disabled={isLinking}
                              className={styles.warningMergeBtn}
                            >
                              {isLinking ? '…' : 'Merge Anyway'}
                            </button>
                            <button
                              onClick={() => setFolderWarning(null)}
                              className={styles.warningCancelBtn}
                            >
                              Cancel
                            </button>
                          </div>
                        </div>
                      )}
                    </div>
                  )
                })}
              </div>
            )}
          </div>
        </div>
      ) : mode === 'search' ? (
        /* ── Search panel ── */
        <div className={styles.panelBody}>
          <div className={styles.searchBarWrap}>
            <div className={styles.searchBarRow}>
              <input
                autoFocus
                value={searchQuery}
                onChange={e => setSearchQuery(e.target.value)}
                onKeyDown={e => e.key === 'Enter' && runSearch(searchQuery)}
                placeholder="Title or tmdb:12345 / tvdb:12345"
                className={styles.searchInput}
              />
              <button
                onClick={() => runSearch(searchQuery)}
                disabled={searchLoading || !searchQuery.trim()}
                className={`${styles.searchBtn} ${(searchLoading || !searchQuery.trim()) ? styles.searchBtnDisabled : ''}`}
              >
                {searchLoading ? '…' : 'Search'}
              </button>
            </div>
          </div>

          <div className={`${styles.panelBodyScroll} scrollbar-dark`}>
            {searchLoading ? (
              <div className={styles.skeletonListGap8}>
                {Array.from({ length: 4 }, (_, i) => (
                  <div key={i} className={`hds-skeleton ${styles.skeletonRow90}`} />
                ))}
              </div>
            ) : searchResults.length === 0 ? (
              <div className={`${styles.emptyState} ${styles.emptyStatePad0}`}>
                {searchQuery.trim() ? 'No results. Try a different title or use tmdb:ID / tvdb:ID.' : 'Enter a title or ID to search.'}
              </div>
            ) : (
              <div className={styles.candidateStack}>
                {searchResults.map(r => {
                  const key       = r.source + ':' + r.external_id
                  const isMatching = matchingId === key
                  return (
                    <div key={key} className={`${styles.searchResultCard} ${isMatching ? styles.searchResultCardMatching : ''}`}>
                      {r.poster_url && (
                        <img src={mediaUrl(r.poster_url)} alt=""
                          className={styles.searchResultPoster}
                          onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
                        />
                      )}
                      <div className={styles.searchResultBody}>
                        <div className={styles.searchResultTopRow}>
                          <div className={styles.searchResultTitle}>{r.title}</div>
                          <button
                            onClick={() => handleManualMatch(r)}
                            disabled={isMatching}
                            className={`${styles.matchBtn} ${isMatching ? styles.matchBtnDisabled : ''}`}
                          >
                            {isMatching ? '…' : 'Match'}
                          </button>
                        </div>
                        <div className={styles.searchResultChipRow}>
                          {r.year && <span className={styles.chip}>{r.year}</span>}
                          <span className={`${styles.chip} ${styles.chipCurrentColor} ${r.source === 'tmdb' ? styles.chipTmdb : styles.chipTvdb}`}>{r.source.toUpperCase()}</span>
                          <span className={`${styles.chip} ${styles.chipSmall}`}>ID: {r.external_id}</span>
                          {r.in_library && <span className={`${styles.chip} ${styles.chipCurrentColor} ${styles.chipApproved}`}>in library</span>}
                        </div>
                        {r.overview && (
                          <p className={styles.searchResultOverview}>{r.overview}</p>
                        )}
                      </div>
                    </div>
                  )
                })}
              </div>
            )}
          </div>
        </div>
      ) : (
        /* ── Candidates panel ── */
        <div className={`${styles.panelBodyScroll} scrollbar-dark`}>
          {isAdmin && item.match_status === 'matched' && (
            <div className={styles.priorityBox}>
              <span className={`${styles.fieldLabel} ${styles.priorityLabel}`}>PRIORITY & IDENTIFIERS</span>
              {loadingMeta ? (
                <div className={styles.loadingMetaText}>Loading…</div>
              ) : (
                <ExternalIdEditor
                  type={item.item_type}
                  id={item.kairos_id}
                  metadata={metadata}
                  isAdmin={isAdmin}
                  onChanged={setMetadata}
                />
              )}
            </div>
          )}
          {item.candidates.length === 0 ? (
            <div className={styles.noCandidatesText}>
              No candidates found — use Search to find a match manually.
            </div>
          ) : (
            <div className={styles.candidateStack}>
              <div className={styles.candidateCountLine}>
                {item.candidates.length} CANDIDATE{item.candidates.length !== 1 ? 'S' : ''} — sorted by confidence
              </div>
              {item.candidates.map(c => (
                <CandidateCard
                  key={c.candidate_id}
                  candidate={c}
                  onAccept={() => onAccept(c.candidate_id)}
                  onReject={() => onReject(c.candidate_id)}
                />
              ))}
            </div>
          )}
        </div>
      )}
    </div>
  )
}

function CandidateCard({
  candidate, onAccept, onReject,
}: {
  candidate: ItemMatchCandidate
  onAccept:  () => void
  onReject:  () => void
}) {
  const pct        = Math.round(candidate.score * 100)
  const scoreClass = pct >= 90 ? styles.scoreHigh : pct >= 70 ? styles.scoreMid : styles.scoreLow
  const accepted   = candidate.accepted

  return (
    <div className={`${styles.candidateCard} ${accepted === true ? styles.candidateCardAccepted : accepted === false ? styles.candidateCardRejected : ''}`}>
      <div className={styles.candidateCardTop}>
        {candidate.poster_url && (
          <img src={mediaUrl(candidate.poster_url)} alt=""
            className={styles.candidatePoster}
            onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
          />
        )}
        <div className={styles.candidateBody}>
          <div className={styles.candidateTitleRow}>
            <div className={styles.candidateTitle}>{candidate.title}</div>
            <span className={`${styles.candidateScore} ${scoreClass}`}>{pct}%</span>
          </div>
          <div className={styles.candidateChipRow}>
            {candidate.year && <span className={styles.chip}>{candidate.year}</span>}
            <span className={`${styles.chip} ${styles.chipCurrentColor} ${candidate.source === 'tmdb' ? styles.chipTmdb : styles.chipTvdb}`}>{candidate.source.toUpperCase()}</span>
            <span className={`${styles.chip} ${styles.chipSmall}`}>ID: {candidate.external_id}</span>
          </div>
          {candidate.overview && (
            <p className={styles.candidateOverview}>{candidate.overview}</p>
          )}
          {accepted === null && (
            <div className={styles.candidateActionsRow}>
              <button onClick={onAccept} className={styles.acceptBtn}>Accept</button>
              <button onClick={onReject} className={styles.rejectBtn}>Reject</button>
            </div>
          )}
          {accepted === true && (
            // A candidate only shows accepted=true within a queue row that's
            // still listed at all when match_status='matched' — which only
            // happens under the auto_unconfirmed filter (match_confirmed=0).
            // So this is always a never-reviewed auto-match: re-running
            // onAccept (acceptCandidate) on the same candidate_id is
            // idempotent — it just sets match_confirmed=1 and re-pulls fresh
            // metadata, same as picking it fresh would.
            <div className={styles.autoMatchedRow}>
              <span className={styles.autoMatchedLabel}>✓ Auto-matched</span>
              <button onClick={onAccept} className={styles.confirmMatchBtn}>Confirm Match</button>
            </div>
          )}
          {accepted === false && <div className={styles.rejectedLabel}>Rejected</div>}
        </div>
      </div>
    </div>
  )
}

// ── Groups tab ────────────────────────────────────────────────────────────────

const GroupsListPanel = observer(function GroupsListPanel({ store }: { store: GroupsStore }) {
  return (
    <>
      <div className={styles.panelHeader}>
        <div className={styles.panelHeaderRow}>
          <span className={styles.panelTitle}>
            Episode Groups
          </span>
          {store.pendingCount > 0 && (
            <span className={styles.panelCount}>
              {store.pendingCount} pending
            </span>
          )}
        </div>
      </div>
      <div className={`${styles.listScroll} scrollbar-dark`}>
        {store.loading && store.shows.length === 0 ? (
          <div className={styles.groupsLoadingRow}>
            <span className={styles.groupsPulseDot} />
            Scanning shows…
          </div>
        ) : store.shows.length === 0 ? (
          <div className={styles.emptyState}>
            No pending grouping candidates.
          </div>
        ) : (
          store.shows.map(s => {
            const pending  = s.candidates.filter(c => !c.already_grouped).length
            const isActive = s.show_id === store.selectedShowId
            return (
              <button
                key={s.show_id}
                onClick={() => store.selectShow(s.show_id)}
                className={`${styles.groupsShowRow} ${isActive ? styles.groupsShowRowActive : ''}`}
              >
                <span className={styles.groupsShowTitle}>
                  {s.show_title}
                </span>
                <span className={`${styles.groupsShowCount} ${isActive ? styles.groupsShowCountActive : ''}`}>{pending}</span>
              </button>
            )
          })
        )}
      </div>
    </>
  )
})

const GroupsDetailPanel = observer(function GroupsDetailPanel({ store }: { store: GroupsStore }) {
  if (!store.selectedShow) {
    return <EmptyHint>Select a show to review grouping candidates</EmptyHint>
  }

  const show       = store.selectedShow
  const unconfirmed = show.candidates.filter(c => !c.already_grouped)
  const highConf   = unconfirmed.filter(c => c.confidence >= 80)

  return (
    <div className={styles.panelBody}>
      <div className={styles.groupsDetailHeader}>
        <h2 className={styles.groupsDetailTitle}>
          {show.show_title}
        </h2>
        {highConf.length > 1 && (
          <button
            disabled={store.saving}
            onClick={() => store.confirmAllHigh()}
            className={`${styles.confirmAllBtn} ${store.saving ? styles.confirmAllBtnDisabled : ''}`}
          >
            Confirm all high-confidence ({highConf.length})
          </button>
        )}
      </div>

      <div className={`${styles.panelBodyScroll} scrollbar-dark`}>
        {/* Confirmed groups */}
        <div className={styles.groupsSectionLabel}>CONFIRMED GROUPS</div>
        {store.groupsLoading ? (
          <div className={styles.groupsEmptyText}>Loading…</div>
        ) : store.confirmedGroups.length === 0 ? (
          <div className={styles.groupsEmptyText}>No confirmed groups yet.</div>
        ) : (
          <div className={styles.groupsConfirmedStack}>
            {store.confirmedGroups.map(g => (
              <div key={g.group_id} className={styles.groupsConfirmedCard}>
                <div className={styles.groupsCardTopRow}>
                  <span className={styles.groupsCardName}>{g.name}</span>
                  <span className={styles.groupsCardPartsCount}>{g.members.length} parts</span>
                  <button
                    disabled={store.saving}
                    onClick={() => store.deleteGroup(g.group_id)}
                    className={`${styles.groupsDeleteBtn} ${store.saving ? styles.groupsDeleteBtnDisabled : ''}`}
                  >Delete</button>
                </div>
                <div className={styles.groupsMembersStack}>
                  {g.members.map(m => (
                    <div key={m.id} className={styles.groupsMemberRow}>
                      <span className={styles.groupsMemberPart}>Part {m.part_num}</span>
                      <span className={styles.groupsMemberEp}>S{String(m.season).padStart(2,'0')}E{String(m.episode).padStart(2,'0')}</span>
                      <span className={styles.groupsMemberTitle}>{m.title}</span>
                    </div>
                  ))}
                </div>
              </div>
            ))}
          </div>
        )}

        {/* Candidates */}
        <div className={styles.groupsSectionLabel}>DETECTED CANDIDATES</div>
        {unconfirmed.length === 0 ? (
          <div className={styles.groupsEmptyText}>All candidates already confirmed.</div>
        ) : (
          <div className={styles.candidateStack}>
            {unconfirmed.sort((a, b) => b.confidence - a.confidence).map((c, i) => {
              const isHigh = c.confidence >= 80
              return (
                <div key={i} className={`${styles.groupsCandidateCard} ${isHigh ? styles.groupsCandidateCardHigh : ''}`}>
                  <div className={styles.groupsCandidateTopRow}>
                    <span className={styles.groupsCandidateTitle}>{c.base_title}</span>
                    <span className={`${styles.groupsConfidenceBadge} ${isHigh ? styles.groupsConfidenceBadgeHigh : ''}`}>{c.confidence}%</span>
                    {!c.adjacent && <span className={styles.groupsNonAdjacent}>non-adjacent</span>}
                    <button
                      disabled={store.saving}
                      onClick={() => store.confirmGroup(c)}
                      className={`${styles.groupsConfirmBtn} ${store.saving ? styles.groupsConfirmBtnDisabled : ''}`}
                    >Confirm</button>
                  </div>
                  <div className={styles.groupsMembersStack}>
                    {c.parts.map(p => (
                      <div key={p.episode_id} className={styles.groupsMemberRow}>
                        <span className={styles.groupsMemberPart}>Part {p.part_num}</span>
                        <span className={styles.groupsMemberEp}>S{String(p.season).padStart(2,'0')}E{String(p.episode).padStart(2,'0')}</span>
                        <span className={styles.groupsMemberTitle}>{p.title}</span>
                      </div>
                    ))}
                  </div>
                </div>
              )
            })}
          </div>
        )}
      </div>
    </div>
  )
})

// ── Requests tab ──────────────────────────────────────────────────────────────

const STATUS_CLASS: Record<RequestStatus, string> = {
  pending:  styles.statusPending,
  approved: styles.statusApproved,
  rejected: styles.statusRejected,
}
const STATUS_PILL_CLASS: Record<RequestStatus, string> = {
  pending:  styles.statusPillPending,
  approved: styles.statusPillApproved,
  rejected: styles.statusPillRejected,
}

function RequestsListPanel({
  items, loading, filter, selected, allRequests, onFilterChange, onSelect,
}: {
  items:           ContentRequest[]
  loading:         boolean
  filter:          'all'|RequestStatus
  selected:        ContentRequest | null
  allRequests:     ContentRequest[]
  onFilterChange:  (f: 'all'|RequestStatus) => void
  onSelect:        (r: ContentRequest) => void
}) {
  return (
    <>
      <div className={styles.panelHeader}>
        <div className={styles.panelHeaderRow}>
          <span className={styles.panelTitle}>
            Requests
          </span>
        </div>
        <div className={styles.filterRow}>
          {(['pending', 'all', 'approved', 'rejected'] as const).map(f => (
            <button key={f} onClick={() => onFilterChange(f)} className={`${styles.filterPill} ${filter === f ? styles.filterPillActive : ''}`}>
              {f === 'all' ? 'ALL' : f.toUpperCase()}
              {f !== 'all' && (
                <span className={styles.filterPillCount}>
                  {allRequests.filter(r => r.status === f).length}
                </span>
              )}
            </button>
          ))}
        </div>
      </div>

      <div className={`${styles.listScroll} scrollbar-dark`}>
        {loading ? (
          <div className={styles.skeletonListGap8}>
            {[...Array(5)].map((_, i) => <div key={i} className={`hds-skeleton ${styles.skeletonRow72}`} />)}
          </div>
        ) : items.length === 0 ? (
          <div className={styles.emptyState}>
            {filter === 'pending' ? 'No pending requests' : `No ${filter} requests`}
          </div>
        ) : (
          <div className={styles.reqListWrap}>
            {items.map(r => {
              const isSelected = selected?.request_id === r.request_id
              return (
                <div
                  key={r.request_id}
                  onClick={() => onSelect(r)}
                  className={`${styles.reqRow} ${isSelected ? styles.reqRowSelected : ''}`}
                >
                  <div className={styles.reqThumbWrap}>
                    {r.poster_url && <img src={mediaUrl(r.poster_url)} alt="" className={styles.reqThumbImg} onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />}
                  </div>
                  <div className={styles.reqInfo}>
                    <div className={styles.reqTitle}>{r.title}</div>
                    <div className={styles.reqMetaRow}>
                      {r.year && <span className={styles.reqYear}>{r.year}</span>}
                      <span className={`${styles.reqSourceBadge} ${r.source === 'tmdb' ? styles.chipTmdb : styles.chipTvdb}`}>{r.source.toUpperCase()}</span>
                    </div>
                  </div>
                  <span className={`${styles.reqStatusLabel} ${STATUS_CLASS[r.status]}`}>
                    {r.status.toUpperCase()}
                  </span>
                </div>
              )
            })}
          </div>
        )}
      </div>
    </>
  )
}

function RequestDetailPanel({ request: r, onClose, onStatusChange }: {
  request:        ContentRequest
  onClose:        () => void
  onStatusChange: (id: string, status: 'approved'|'rejected') => void
}) {
  const [arrStep,          setArrStep]          = useState<'idle'|'loading'|'form'|'adding'|'done'|'error'>('idle')
  const [arrResult,        setArrResult]        = useState<ArrLookupResult | null>(null)
  const [options,          setOptions]          = useState<ArrServiceOptions | null>(null)
  const [qualityProfileId, setQualityProfileId] = useState<number | null>(null)
  const [rootFolder,       setRootFolder]       = useState('')
  const [searchOnAdd,      setSearchOnAdd]      = useState(true)
  const [arrError,         setArrError]         = useState('')
  const [rejecting,        setRejecting]        = useState(false)

  const serviceLabel = r.content_type === 'show' ? 'Sonarr' : 'Radarr'

  const handleApproveClick = async () => {
    setArrStep('loading')
    setArrError('')
    try {
      const params: Parameters<typeof api.arrLookup>[0] = { type: r.content_type }
      if (r.source === 'tvdb')             params.tvdb_id = r.external_id
      else if (r.content_type === 'movie') params.tmdb_id = r.external_id
      else                                 params.title   = r.title
      const [results, opts] = await Promise.all([api.arrLookup(params), api.arrOptions(r.content_type)])
      if (results.length === 0) { setArrError(`Not found in ${serviceLabel}.`); setArrStep('error'); return }
      setArrResult(results[0])
      setOptions(opts)
      if (opts.quality_profiles.length > 0) setQualityProfileId(opts.quality_profiles[0].id)
      if (opts.root_folders.length > 0)     setRootFolder(opts.root_folders[0])
      setArrStep('form')
    } catch (e: any) {
      setArrError(e.message ?? `Could not reach ${serviceLabel}.`)
      setArrStep('error')
    }
  }

  const handleConfirmApprove = async () => {
    if (!arrResult || qualityProfileId === null || !rootFolder) return
    setArrStep('adding')
    try {
      await api.arrAdd({ type: r.content_type, add_data: arrResult.add_data, quality_profile_id: qualityProfileId, root_folder: rootFolder, search_on_add: searchOnAdd })
      await api.updateRequest(r.request_id, 'approved')
      onStatusChange(r.request_id, 'approved')
      setArrStep('done')
    } catch (e: any) {
      setArrError(e.message ?? `Failed to add to ${serviceLabel}.`)
      setArrStep('error')
    }
  }

  const handleReject = async () => {
    setRejecting(true)
    try {
      await api.updateRequest(r.request_id, 'rejected')
      onStatusChange(r.request_id, 'rejected')
    } finally {
      setRejecting(false)
    }
  }

  return (
    <div className={styles.panelBody}>
      {/* Backdrop */}
      <div className={styles.reqBackdrop}>
        {r.poster_url ? (
          <div className={styles.reqBackdropImg} style={{ backgroundImage: `url(${mediaUrl(r.poster_url)})` }} />
        ) : (
          <div className={styles.reqBackdropFallback} />
        )}
        <div className={styles.reqBackdropScrim} />
        <button onClick={onClose} className={styles.iconCloseBtnCircle}>×</button>
        {r.poster_url && (
          <img src={mediaUrl(r.poster_url)} alt="" className={styles.reqBackdropPoster} />
        )}
      </div>

      <div className={`${styles.reqDetailBody} ${r.poster_url ? styles.reqDetailBodyWithPoster : ''}`}>
        <div className={styles.reqDetailTitleRow}>
          <h2 className={styles.reqDetailTitle}>
            {r.title}
          </h2>
          <span className={`${styles.reqStatusPill} ${STATUS_PILL_CLASS[r.status]}`}>{r.status.toUpperCase()}</span>
        </div>
        <div className={styles.reqDetailChipRow}>
          {r.year && <span className={styles.chip}>{r.year}</span>}
          <span className={styles.chip}>{r.content_type}</span>
          <span className={`${styles.chip} ${styles.reqSourceChip} ${r.source === 'tmdb' ? styles.chipTmdb : styles.chipTvdb}`}>{r.source.toUpperCase()}</span>
        </div>
        <div className={styles.reqDetailMeta}>
          Requested {new Date(r.created_at * 1000).toLocaleString()} · user {r.user_id.slice(0, 8)}
        </div>

        {r.status === 'pending' && (
          <div className={styles.arrSection}>
            <div className={styles.arrSectionLabel}>
              {serviceLabel.toUpperCase()}
            </div>
            {arrStep === 'idle' && (
              <div className={styles.arrIdleRow}>
                <button onClick={handleApproveClick} className={styles.arrApproveBtn}>Approve → {serviceLabel}</button>
                <button onClick={handleReject} disabled={rejecting} className={`${styles.arrRejectBtn} ${rejecting ? styles.arrRejectBtnDisabled : ''}`}>Reject</button>
              </div>
            )}
            {(arrStep === 'loading' || arrStep === 'adding') && (
              <div className={styles.arrBusyText}>
                {arrStep === 'loading' ? `Looking up in ${serviceLabel}…` : `Adding to ${serviceLabel}…`}
              </div>
            )}
            {arrStep === 'form' && options && (
              <div className={styles.arrForm}>
                <label className={styles.formLabel}>
                  Quality Profile
                  <select value={qualityProfileId ?? ''} onChange={e => setQualityProfileId(Number(e.target.value))} className={styles.selectInput}>
                    {options.quality_profiles.map(p => <option key={p.id} value={p.id}>{p.name}</option>)}
                  </select>
                </label>
                <label className={styles.formLabel}>
                  Root Folder
                  <select value={rootFolder} onChange={e => setRootFolder(e.target.value)} className={styles.selectInput}>
                    {options.root_folders.map(f => <option key={f} value={f}>{f}</option>)}
                  </select>
                </label>
                <label className={styles.arrCheckboxLabel}>
                  <input type="checkbox" checked={searchOnAdd} onChange={e => setSearchOnAdd(e.target.checked)} className={styles.arrCheckbox} />
                  Search immediately
                </label>
                <div className={styles.arrFormActions}>
                  <button onClick={() => setArrStep('idle')} className={styles.arrCancelBtn}>Cancel</button>
                  <button onClick={handleConfirmApprove} className={styles.arrConfirmBtn}>
                    Confirm → {serviceLabel}
                  </button>
                </div>
              </div>
            )}
            {arrStep === 'error' && (
              <div className={styles.arrErrorBox}>
                <div className={styles.arrErrorBanner}>{arrError}</div>
                <button onClick={() => setArrStep('idle')} className={styles.arrTryAgainBtn}>Try Again</button>
              </div>
            )}
          </div>
        )}
        {arrStep === 'done' && (
          <div className={`${styles.arrStatusBanner} ${styles.arrStatusBannerApproved}`}>
            Approved — added to {serviceLabel}{searchOnAdd ? ', search queued' : ''}
          </div>
        )}
        {r.status === 'approved' && arrStep !== 'done' && (
          <div className={`${styles.arrStatusBanner} ${styles.arrStatusBannerApproved}`}>Approved</div>
        )}
        {r.status === 'rejected' && (
          <div className={`${styles.arrStatusBanner} ${styles.arrStatusBannerRejected}`}>Rejected</div>
        )}
      </div>
    </div>
  )
}

// ── Duplicates tab ────────────────────────────────────────────────────────────
// Auto-detected "possible duplicate" pairs from SyncManager's sync-time dedup
// (fuzzy title match, or a folder match that only succeeded via a
// case-insensitive fallback — see DuplicateCandidate). A human confirms
// (Merge) or dismisses (Not a duplicate, remembered permanently) each pair;
// this is separate from CandidatePanel's "Link Existing" mode, which stays
// as the manual last-resort fallback for duplicates neither signal here nor
// the match-time external-id check can catch.

function DuplicatesListPanel({
  items, total, loading, filter, selected, onFilterChange, onSelect,
}: {
  items:          DuplicateCandidate[]
  total:          number
  loading:        boolean
  filter:         'all'|'show'|'movie'
  selected:       DuplicateCandidate | null
  onFilterChange: (f: 'all'|'show'|'movie') => void
  onSelect:       (d: DuplicateCandidate) => void
}) {
  return (
    <>
      <div className={styles.panelHeader}>
        <div className={styles.panelHeaderRow}>
          <span className={styles.panelTitle}>
            Duplicates
          </span>
          <span className={styles.panelCount}>{total}</span>
        </div>
        <div className={styles.filterRow}>
          {(['all', 'show', 'movie'] as const).map(f => (
            <button key={f} onClick={() => onFilterChange(f)} className={`${styles.filterPill} ${filter === f ? styles.filterPillActive : ''}`}>{f.toUpperCase()}</button>
          ))}
        </div>
      </div>

      <div className={`${styles.listScroll} scrollbar-dark`}>
        {loading ? (
          <div className={styles.skeletonListGap8}>
            {[...Array(5)].map((_, i) => <div key={i} className={`hds-skeleton ${styles.skeletonRow64}`} />)}
          </div>
        ) : items.length === 0 ? (
          <div className={styles.emptyState}>
            No pending duplicates
          </div>
        ) : (
          <div className={styles.dupListWrap}>
            {items.map(d => {
              const isSelected = selected?.candidate_id === d.candidate_id
              const isSimilar = d.title_similarity >= 0.8
              return (
                <div
                  key={d.candidate_id}
                  onClick={() => onSelect(d)}
                  className={`${styles.dupRow} ${isSelected ? styles.dupRowSelected : ''} ${isSimilar ? styles.dupRowSimilar : ''}`}
                >
                  <div className={styles.dupTitleLine}>
                    {d.title_a} <span className={styles.dupVs}>vs</span> {d.title_b}
                  </div>
                  <div className={styles.dupReason}>{d.reason}</div>
                </div>
              )
            })}
          </div>
        )}
      </div>
    </>
  )
}

function DuplicateDetailPanel({ item, onClose, onResolved }: {
  item:       DuplicateCandidate
  onClose:    () => void
  onResolved: () => void
}) {
  const [merging,     setMerging]     = useState(false)
  const [dismissing,  setDismissing]  = useState(false)
  const [error,       setError]       = useState<string | null>(null)
  const busy = merging || dismissing

  // Arbitrary but consistent: id_a survives as the merge target, id_b is
  // absorbed and deleted — neither side is more "correct" than the other,
  // so there's nothing meaningful to pick between beyond a stable choice.
  // confirm:true always, since the admin has already seen both folders/
  // titles side by side here — the generic folder-mismatch 409 confirmation
  // step (used by CandidatePanel's manual "Link Existing" search) is
  // redundant on top of that.
  const handleMerge = async () => {
    setMerging(true)
    setError(null)
    try {
      if (item.item_type === 'show') await api.mergeShow(item.kairos_id_a, item.kairos_id_b, true)
      else                            await api.mergeMovie(item.kairos_id_a, item.kairos_id_b, true)
      onResolved()
    } catch (e) {
      setError(e instanceof ApiError ? (e.body?.error || 'Unable to merge — please try again.') : 'Unable to merge — please try again.')
      setMerging(false)
    }
  }

  const handleDismiss = async () => {
    setDismissing(true)
    setError(null)
    try {
      await api.dismissDuplicate(item.candidate_id)
      onResolved()
    } catch {
      setError('Unable to dismiss — please try again.')
      setDismissing(false)
    }
  }

  return (
    <div className={styles.panelBody}>
      <div className={styles.detailHeader}>
        <div className={styles.detailHeaderTop}>
          <h2 className={styles.detailHeaderTitle}>
            Possible duplicate
          </h2>
          <div className={styles.reqDetailMeta}>{item.reason}</div>
        </div>
        <button onClick={onClose} className={styles.iconCloseBtn}>✕</button>
      </div>

      <div className={`${styles.panelBodyScroll} scrollbar-dark ${styles.dupDetailScroll}`}>
        <div className={styles.dupCompareRow}>
          {[
            { title: item.title_a, year: item.year_a, thumb: item.thumb_a, folder: item.folder_a },
            { title: item.title_b, year: item.year_b, thumb: item.thumb_b, folder: item.folder_b },
          ].map((side, i) => (
            <div key={i} className={styles.dupSide}>
              <div className={styles.dupSidePoster}>
                {side.thumb && (
                  <img src={mediaUrl(side.thumb)} alt="" className={styles.dupSidePosterImg}
                    onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
                  />
                )}
              </div>
              <div className={styles.dupSideTitle}>
                {side.title} {side.year ? <span className={styles.dupSideYear}>({side.year})</span> : null}
              </div>
              {side.folder && (
                <div title={side.folder} className={styles.dupSideFolder}>
                  {folderBaseName(side.folder)}
                </div>
              )}
            </div>
          ))}
        </div>

        {error && (
          <div className={styles.dupErrorBanner}>
            {error}
          </div>
        )}

        <div className={styles.dupActionsRow}>
          <button onClick={handleMerge} disabled={busy} className={`${styles.dupMergeBtn} ${busy ? styles.dupActionBtnBusy : ''}`}>{merging ? '…' : 'Merge'}</button>
          <button onClick={handleDismiss} disabled={busy} className={`${styles.dupDismissBtn} ${busy ? styles.dupActionBtnBusy : ''}`}>{dismissing ? '…' : 'Not a duplicate'}</button>
        </div>
      </div>
    </div>
  )
}

// ── Chapters tab ──────────────────────────────────────────────────────────────
// Visual inspection only — no accept/save action.

function fmtMs(ms: number): string {
  const total = Math.max(0, Math.floor(ms / 1000))
  const h  = Math.floor(total / 3600)
  const m  = Math.floor((total % 3600) / 60)
  const s  = total % 60
  const mm = h > 0 ? String(m).padStart(2, '0') : String(m)
  const ss = String(s).padStart(2, '0')
  return h > 0 ? `${h}:${mm}:${ss}` : `${mm}:${ss}`
}

const CHAPTER_TYPES: ChapterType[] = ['unclassified', 'pre_roll', 'intro', 'recap', 'ad_break', 'chapter', 'credits', 'post_credits', 'outro', 'next_time']

const CHAPTER_TYPE_LABEL: Record<ChapterType, string> = {
  unclassified: 'Unclassified', pre_roll: 'Pre-Roll', intro: 'Intro', recap: 'Recap',
  ad_break: 'Ad Break', chapter: 'Chapter', credits: 'Credits', post_credits: 'Post-Credits', outro: 'Outro',
  next_time: 'Next Time',
}

// A 10-way content-classification color lookup (not a small closed toggle
// state) — kept as a JS constant consumed via a targeted inline `color`
// style rather than 10 near-identical CSS modifier classes each setting one
// property. Same precedent as channel/'s BLOCK_META per-type colors.
const CHAPTER_TYPE_COLOR: Record<ChapterType, string> = {
  pre_roll:     'oklch(0.7 0.14 200)',
  intro:        'oklch(0.7 0.16 150)',
  recap:        'oklch(0.7 0.16 95)',
  ad_break:     'var(--hds-match-red)',
  credits:      'oklch(0.65 0.14 280)',
  post_credits: 'oklch(0.68 0.18 320)',
  outro:        'var(--hds-match-amber)',
  chapter:      'oklch(0.65 0.18 220)',
  unclassified: 'var(--hds-txt-3)',
  next_time:    'oklch(0.7 0.16 40)',
}

function ChaptersListPanel({
  items, total, loading, mediaType, chapterType, query, selected,
  onMediaTypeChange, onChapterTypeChange, onQueryChange, onSelect,
}: {
  items: ChapterReviewItem[]; total: number; loading: boolean
  mediaType: 'all'|'episode'|'movie'; chapterType: 'all'|ChapterType; query: string
  selected: ChapterReviewItem | null
  onMediaTypeChange: (t: 'all'|'episode'|'movie') => void
  onChapterTypeChange: (t: 'all'|ChapterType) => void
  onQueryChange: (q: string) => void
  onSelect: (item: ChapterReviewItem) => void
}) {
  return (
    <>
      <div className={styles.panelHeader}>
        <div className={styles.panelHeaderRow}>
          <span className={styles.panelTitle}>
            Chapters
          </span>
          <span className={styles.panelCount}>
            {total} item{total !== 1 ? 's' : ''}
          </span>
        </div>
        <input
          value={query}
          onChange={e => onQueryChange(e.target.value)}
          placeholder="Search title…"
          className={styles.chapterSearchInput}
        />
        <div className={styles.chapterFilterRow}>
          {(['all', 'episode', 'movie'] as const).map(t => (
            <button key={t} onClick={() => onMediaTypeChange(t)} className={`${styles.filterPill} ${mediaType === t ? styles.filterPillActive : ''}`}>
              {t === 'all' ? 'ALL' : t === 'episode' ? 'EPISODES' : 'MOVIES'}
            </button>
          ))}
        </div>
        <select
          value={chapterType}
          onChange={e => onChapterTypeChange(e.target.value as 'all'|ChapterType)}
          className={`${styles.selectInput} ${styles.selectInputFull}`}
        >
          <option value="all">All chapter types</option>
          {CHAPTER_TYPES.map(t => <option key={t} value={t}>{CHAPTER_TYPE_LABEL[t]}</option>)}
        </select>
      </div>

      <div className={`${styles.listScroll} scrollbar-dark`}>
        {loading ? (
          <div className={styles.skeletonListGap1}>
            {Array.from({ length: 8 }, (_, i) => (
              <div key={i} className={`hds-skeleton ${styles.skeletonRowSm}`} />
            ))}
          </div>
        ) : items.length === 0 ? (
          <div className={styles.emptyState}>
            No items match these filters.
          </div>
        ) : (
          items.map(item => {
            const isSelected = selected?.media_type === item.media_type && selected?.media_id === item.media_id
            const typeCounts = new Map<ChapterType, number>()
            for (const c of item.chapters) typeCounts.set(c.chapter_type, (typeCounts.get(c.chapter_type) ?? 0) + 1)
            return (
              <button
                key={item.media_type + item.media_id}
                onClick={() => onSelect(item)}
                className={`${styles.chapterListRow} ${isSelected ? styles.chapterListRowSelected : ''}`}
              >
                <div className={styles.chapterListTitle}>{item.title}</div>
                <div className={styles.chapterListMetaRow}>
                  <span className={styles.chapterListDuration}>
                    {fmtMs(item.duration_ms)}
                  </span>
                  {[...typeCounts.entries()].map(([t, n]) => (
                    <span key={t} className={styles.chapterTypeCountChip} style={{ color: CHAPTER_TYPE_COLOR[t] }}>{CHAPTER_TYPE_LABEL[t]} {n}</span>
                  ))}
                </div>
              </button>
            )
          })
        )}
      </div>
    </>
  )
}

function ChapterInspectorPanel({ item, onClose }: { item: ChapterReviewItem; onClose: () => void }) {
  const videoRef = useRef<HTMLVideoElement>(null)
  const target: PlaybackTarget = { kind: item.media_type, id: item.media_id }
  const session = usePlaybackSession(target)
  const [currentMs, setCurrentMs]   = useState(0)
  const [playerError, setPlayerError] = useState<string | null>(null)

  // Same buffered-check-then-reload fallback as PlayerPage's handleSeek.
  const seekTo = (ms: number) => {
    const video = videoRef.current
    if (!video) return
    const targetSec = ms / 1000
    let withinBuffer = false
    for (let i = 0; i < video.buffered.length; i++) {
      if (targetSec >= video.buffered.start(i) && targetSec <= video.buffered.end(i)) { withinBuffer = true; break }
    }
    if (withinBuffer) {
      video.currentTime = targetSec
      setCurrentMs(ms)
    } else {
      session.reload({ positionMs: ms })
    }
  }

  const sorted = [...item.chapters].sort((a, b) => a.position - b.position || a.start_ms - b.start_ms)
  const activeId = sorted.find(c => currentMs >= c.start_ms && currentMs < (c.end_ms || Infinity))?.chapter_id

  return (
    <div className={styles.panelBody}>
      <div className={styles.detailHeader}>
        <div className={styles.detailHeaderTop}>
          <h2 className={styles.detailHeaderTitle}>{item.title}</h2>
          <div className={styles.detailHeaderChips}>
            {item.year && <span className={styles.chip}>{item.year}</span>}
            <span className={styles.chip}>{item.media_type}</span>
            <span className={styles.chip}>{fmtMs(item.duration_ms)}</span>
            <span className={styles.chip}>{item.chapters.length} chapter{item.chapters.length !== 1 ? 's' : ''}</span>
          </div>
          {item.file_path && (
            <div
              title={item.file_path}
              className={styles.detailHeaderFolder}
            >
              <span className={styles.mutedLabel}>File: </span>{folderBaseName(item.file_path)}
            </div>
          )}
        </div>
        <button onClick={onClose} className={styles.iconCloseBtn}>✕</button>
      </div>

      <div className={styles.chapterPlayerWrap}>
        {session.loading && (
          <div className={styles.chapterPlayerOverlay}>
            <LoadingThrobber label="Starting playback…" />
          </div>
        )}
        {(session.error || playerError) && !session.loading && (
          <div className={styles.chapterPlayerError}>{session.error ?? playerError}</div>
        )}
        {!session.loading && !session.error && session.manifestUrl && (
          <VideoPlayer
            videoRef={videoRef}
            manifestUrl={session.manifestUrl}
            isLive={false}
            autoPlay={false}
            controls
            onTimeUpdate={ms => setCurrentMs(ms)}
            onEnded={() => {}}
            onError={setPlayerError}
          />
        )}
      </div>

      <div className={`${styles.panelBodyScroll} scrollbar-dark`}>
        {sorted.length === 0 ? (
          <div className={styles.chapterNoChaptersText}>No chapters.</div>
        ) : (
          <div className={styles.chapterRowStack}>
            {sorted.map(c => (
              <ChapterRow
                key={c.chapter_id}
                chapter={c}
                active={c.chapter_id === activeId}
                onSeekStart={() => seekTo(c.start_ms)}
                onSeekEnd={() => seekTo(Math.max(c.start_ms, c.end_ms - 500))}
              />
            ))}
          </div>
        )}
      </div>
    </div>
  )
}

function ChapterRow({
  chapter, active, onSeekStart, onSeekEnd,
}: {
  chapter: Chapter
  active: boolean
  onSeekStart: () => void
  onSeekEnd:   () => void
}) {
  const color = CHAPTER_TYPE_COLOR[chapter.chapter_type]
  return (
    <div className={`${styles.chapterRow} ${active ? styles.chapterRowActive : ''}`} style={{ borderLeftColor: color }}>
      <span className={styles.chapterTypeBadge} style={{ color }}>{CHAPTER_TYPE_LABEL[chapter.chapter_type].toUpperCase()}</span>
      <div className={styles.chapterRowInfo}>
        {chapter.title && (
          <div className={styles.chapterRowTitle}>{chapter.title}</div>
        )}
        <div className={styles.chapterRowTime}>
          {fmtMs(chapter.start_ms)} – {fmtMs(chapter.end_ms)} · {chapter.source}{chapter.locked ? ' · locked' : ''}
        </div>
      </div>
      <button onClick={onSeekStart} className={styles.chapterActionBtn}>▶ Start</button>
      <button onClick={onSeekEnd} className={styles.chapterActionBtn}>▶ End</button>
    </div>
  )
}

// ── Subtitles tab ────────────────────────────────────────────────────────────
// Read-only report over sidecar files kairos's sync-time content check (see
// kairos/src/source/SubtitleValidation.h) flagged as broken — e.g. matching
// the "<video>.<lang>.srt" naming convention but containing ~0 real cues, so
// nothing about the filename itself would ever catch it. One action:
// re-check a file after fixing/replacing it on disk, rather than waiting
// for the next full library sync.

function SubtitlesListPanel({
  items, loading, selected, onSelect,
}: {
  items: BrokenSubtitleItem[]; loading: boolean; selected: BrokenSubtitleItem | null
  onSelect: (item: BrokenSubtitleItem) => void
}) {
  return (
    <>
      <div className={styles.panelHeader}>
        <div className={styles.panelHeaderRow}>
          <span className={styles.panelTitle}>
            Broken Subtitles
          </span>
          <span className={styles.panelCount}>
            {items.length} item{items.length !== 1 ? 's' : ''}
          </span>
        </div>
      </div>

      <div className={`${styles.listScroll} scrollbar-dark`}>
        {loading ? (
          <div className={styles.skeletonListGap1}>
            {Array.from({ length: 8 }, (_, i) => (
              <div key={i} className={`hds-skeleton ${styles.skeletonRowSm}`} />
            ))}
          </div>
        ) : items.length === 0 ? (
          <div className={styles.emptyState}>
            No broken subtitle files found.<br />
            <span className={styles.emptyStateSub}>Sidecar files are checked during every library sync.</span>
          </div>
        ) : (
          items.map(item => {
            const isSelected = selected?.subtitle_id === item.subtitle_id
            return (
              <button
                key={item.subtitle_id}
                onClick={() => onSelect(item)}
                className={`${styles.chapterListRow} ${isSelected ? styles.chapterListRowSelected : ''}`}
              >
                <div className={styles.chapterListTitle}>{item.title || item.media_id}</div>
                <div className={styles.chapterListMetaRow}>
                  {item.language && <span className={styles.chapterListDuration}>{item.language.toUpperCase()}</span>}
                  {item.forced && <span className={styles.chapterTypeCountChip}>FORCED</span>}
                  {item.sdh && <span className={styles.chapterTypeCountChip}>SDH</span>}
                  <span className={styles.chapterListDuration}>{item.invalid_reason}</span>
                </div>
              </button>
            )
          })
        )}
      </div>
    </>
  )
}

function SubtitleInspectorPanel({ item, onClose, onChanged, onDeleted }: {
  item: BrokenSubtitleItem
  onClose: () => void
  onChanged: (updated: BrokenSubtitleItem) => void
  onDeleted: (id: string) => void
}) {
  const [rechecking, setRechecking] = useState(false)
  const [editing,    setEditing]    = useState(false)
  const [saving,     setSaving]     = useState(false)
  const [confirmDelete, setConfirmDelete] = useState(false)
  const [deleting,   setDeleting]   = useState(false)
  const [error,      setError]      = useState<string | null>(null)

  const [editLanguage, setEditLanguage] = useState(item.language)
  const [editForced,   setEditForced]   = useState(item.forced)
  const [editSdh,       setEditSdh]     = useState(item.sdh)

  const startEdit = () => {
    setEditLanguage(item.language)
    setEditForced(item.forced)
    setEditSdh(item.sdh)
    setError(null)
    setEditing(true)
  }

  const handleRecheck = async () => {
    setRechecking(true)
    setError(null)
    try {
      onChanged(await api.recheckSubtitle(item.subtitle_id))
    } catch (e: any) {
      setError(e.message ?? 'Failed to re-check this file.')
    } finally {
      setRechecking(false)
    }
  }

  const handleSaveEdit = async () => {
    setSaving(true)
    setError(null)
    try {
      onChanged(await api.updateSubtitle(item.subtitle_id, { language: editLanguage, forced: editForced, sdh: editSdh }))
      setEditing(false)
    } catch (e: any) {
      setError(e.message ?? 'Failed to save changes.')
    } finally {
      setSaving(false)
    }
  }

  const handleDelete = async () => {
    setDeleting(true)
    setError(null)
    try {
      await api.deleteSubtitle(item.subtitle_id)
      onDeleted(item.subtitle_id)
    } catch (e: any) {
      setError(e.message ?? 'Failed to delete this file.')
      setDeleting(false)
      setConfirmDelete(false)
    }
  }

  return (
    <div className={styles.detailPanel}>
      <div className={styles.detailHeader}>
        <div className={styles.detailHeaderTop}>
          <h2 className={styles.detailHeaderTitle}>{item.title || item.media_id}</h2>
          <div className={styles.detailHeaderFolder}>{item.file_path}</div>
        </div>
        <button onClick={onClose} className={styles.iconCloseBtn}>✕</button>
      </div>

      <div className={`${styles.listScroll} scrollbar-dark`} style={{ padding: 'var(--hds-space-7)' }}>
        {editing ? (
          <div className={styles.chapterRowStack}>
            <label className={styles.emptyStateSub}>
              Language (ISO 639-2)
              <input
                value={editLanguage}
                onChange={e => setEditLanguage(e.target.value)}
                placeholder="e.g. spa"
                className={styles.chapterSearchInput}
                style={{ marginTop: 'var(--hds-space-2)' }}
              />
            </label>
            <label className={styles.emptyStateSub} style={{ display: 'flex', alignItems: 'center', gap: 'var(--hds-space-3)' }}>
              <input type="checkbox" checked={editForced} onChange={e => setEditForced(e.target.checked)} />
              Forced
            </label>
            <label className={styles.emptyStateSub} style={{ display: 'flex', alignItems: 'center', gap: 'var(--hds-space-3)' }}>
              <input type="checkbox" checked={editSdh} onChange={e => setEditSdh(e.target.checked)} />
              SDH
            </label>
            <p className={styles.emptyStateSub}>
              Not permanent for a sidecar file: the next library sync re-derives this row from the
              filename and overwrites these fields if the file is still there.
            </p>
            <div style={{ display: 'flex', gap: 'var(--hds-space-3)' }}>
              <button onClick={handleSaveEdit} disabled={saving} className={styles.chapterActionBtn}>
                {saving ? 'Saving…' : 'Save'}
              </button>
              <button onClick={() => setEditing(false)} disabled={saving} className={styles.chapterActionBtn}>
                Cancel
              </button>
            </div>
          </div>
        ) : (
          <div className={styles.chapterRowStack}>
            <SubtitleStatRow label="Media type" value={item.media_type} />
            <SubtitleStatRow label="Language" value={item.language || 'unknown'} />
            <SubtitleStatRow label="Forced" value={item.forced ? 'yes' : 'no'} />
            <SubtitleStatRow label="SDH" value={item.sdh ? 'yes' : 'no'} />
            <SubtitleStatRow label="Source" value={item.source} />
            <SubtitleStatRow label="Reason" value={item.invalid_reason} />
          </div>
        )}

        {!editing && (
          <div style={{ display: 'flex', gap: 'var(--hds-space-3)', marginTop: 'var(--hds-space-5)', flexWrap: 'wrap' }}>
            <button onClick={handleRecheck} disabled={rechecking} className={styles.chapterActionBtn}>
              {rechecking ? 'Re-checking…' : '↻ Re-check now'}
            </button>
            <button onClick={startEdit} className={styles.chapterActionBtn}>
              ✎ Edit
            </button>
            {confirmDelete ? (
              <div className={styles.chapterRowStack} style={{ width: '100%' }}>
                <p className={styles.emptyStateSub} style={{ color: 'var(--hds-match-red)' }}>
                  Permanently deletes the actual file from your media library (not just this record),
                  so a subtitle downloader like Bazarr treats this language as missing again and fetches
                  a fresh copy. This cannot be undone. Delete "{item.file_path}"?
                </p>
                <div style={{ display: 'flex', gap: 'var(--hds-space-3)' }}>
                  <button
                    onClick={handleDelete}
                    disabled={deleting}
                    className={styles.chapterActionBtn}
                    style={{ borderColor: 'var(--hds-match-red)', color: 'var(--hds-match-red)' }}
                  >
                    {deleting ? 'Deleting…' : 'Yes, delete the file'}
                  </button>
                  <button onClick={() => setConfirmDelete(false)} disabled={deleting} className={styles.chapterActionBtn}>
                    Cancel
                  </button>
                </div>
              </div>
            ) : (
              <button onClick={() => setConfirmDelete(true)} className={styles.chapterActionBtn}>
                🗑 Delete file
              </button>
            )}
          </div>
        )}
        {error && <p className={styles.emptyStateSub} style={{ marginTop: 'var(--hds-space-3)' }}>{error}</p>}
      </div>
    </div>
  )
}

function SubtitleStatRow({ label, value }: { label: string; value: string }) {
  return (
    <div className={styles.chapterRow} style={{ borderLeftColor: 'var(--hds-line)' }}>
      <div className={styles.chapterRowInfo}>
        <div className={styles.chapterRowTitle}>{label}</div>
        <div className={styles.chapterRowTime}>{value}</div>
      </div>
    </div>
  )
}
