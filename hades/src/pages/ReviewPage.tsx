import { observer } from 'mobx-react-lite'
import { makeAutoObservable, runInAction } from 'mobx'
import { useEffect, useState, useCallback } from 'react'
import { useAuth } from '../auth/AuthContext'
import { api } from '../api/client'
import type {
  ReviewQueueItem, ItemMatchCandidate,
  EpisodeGroup, GroupingCandidate, ShowGroupingResult,
  ArrLookupResult, ArrServiceOptions, ContentRequest, RequestStatus,
  ScraperSearchResult,
} from '../api/types'
import { MatchBadge } from '../components/media/MatchBadge'
import type { MatchStatus } from '../components/media/MatchBadge'
import { goldBtnStyle } from '../channel/styles'

// ── Shared chip style ─────────────────────────────────────────────────────────

const chip: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
  padding: '2px 8px', borderRadius: 10,
  border: '1px solid var(--hds-line-s)', color: 'var(--hds-txt-3)',
}

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

type Tab = 'queue' | 'groups' | 'requests'

// ── Page ─────────────────────────────────────────────────────────────────────

export default observer(function ReviewPage() {
  const { user } = useAuth()
  const [tab, setTab] = useState<Tab>('queue')

  // ── Queue state ──────────────────────────────────────────────────────────────
  const [queueItems,    setQueueItems]    = useState<ReviewQueueItem[]>([])
  const [queueTotal,    setQueueTotal]    = useState(0)
  const [queueLoading,  setQueueLoading]  = useState(true)
  const [queueFilter,   setQueueFilter]   = useState<'all'|'uncertain'|'unmatched'>('all')
  const [selectedQueue, setSelectedQueue] = useState<ReviewQueueItem | null>(null)
  const [matching,      setMatching]      = useState(false)

  const fetchQueue = useCallback(() => {
    setQueueLoading(true)
    api.getReviewQueue({ status: queueFilter, limit: 48 })
      .then(r => { setQueueItems(r.items); setQueueTotal(r.total) })
      .catch(() => {})
      .finally(() => setQueueLoading(false))
  }, [queueFilter])

  useEffect(() => { fetchQueue() }, [fetchQueue])

  const handleAccept = async (candidate_id: string) => {
    const kairosId = selectedQueue?.kairos_id
    await api.acceptCandidate(candidate_id)
    const updated = await api.getReviewQueue({ status: queueFilter, limit: 48 })
    setQueueItems(updated.items)
    setQueueTotal(updated.total)
    const refreshed = kairosId ? updated.items.find(i => i.kairos_id === kairosId) : null
    setSelectedQueue(refreshed ?? null)
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

  // ── Tab switch ───────────────────────────────────────────────────────────────
  const switchTab = (t: Tab) => {
    setTab(t)
    setSelectedQueue(null)
    setSelectedReq(null)
  }

  // ── Render ───────────────────────────────────────────────────────────────────
  return (
    <div style={{ display: 'flex', height: '100%', overflow: 'hidden', background: 'var(--hds-bg)' }}>
      {/* ── Left panel ─────────────────────────────────────────────────────── */}
      <div style={{
        width: 320, flexShrink: 0, height: '100%', overflow: 'hidden',
        display: 'flex', flexDirection: 'column',
        borderRight: '1px solid var(--hds-line)',
      }}>
        {/* Tab bar */}
        <div style={{
          display: 'flex', padding: '14px 16px 0', gap: 4,
          borderBottom: '1px solid var(--hds-line)', flexShrink: 0,
        }}>
          {([
            { key: 'queue',    label: 'Queue',    badge: null,                                                            admin: false },
            { key: 'groups',   label: 'Groups',   badge: groupsStore.pendingCount > 0 ? groupsStore.pendingCount : null, admin: false },
            { key: 'requests', label: 'Requests', badge: pendingReqCount > 0 ? pendingReqCount : null,                   admin: true  },
          ] as const).filter(t => !t.admin || user?.role === 'admin').map(({ key, label, badge }) => (
            <button
              key={key}
              onClick={() => switchTab(key as Tab)}
              style={{
                flex: 1, padding: '8px 0', borderRadius: '6px 6px 0 0',
                border: `1px solid ${tab === key ? 'var(--hds-line)' : 'transparent'}`,
                borderBottom: 'none',
                background: tab === key ? 'var(--hds-bg)' : 'transparent',
                color: tab === key ? 'var(--hds-txt)' : 'var(--hds-txt-3)',
                fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
                letterSpacing: '0.06em', cursor: 'pointer', position: 'relative',
                display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 6,
                marginBottom: tab === key ? -1 : 0,
              }}
            >
              {label.toUpperCase()}
              {badge !== null && (
                <span style={{
                  minWidth: 16, height: 16, borderRadius: 8, padding: '0 4px',
                  background: 'oklch(0.75 0.12 80 / 0.18)',
                  border: '1px solid var(--hds-match-amber)',
                  color: 'var(--hds-match-amber)',
                  fontSize: 9, fontWeight: 700,
                  display: 'flex', alignItems: 'center', justifyContent: 'center',
                }}>{badge > 99 ? '99+' : badge}</span>
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
              onMatched={() => { setSelectedQueue(null); fetchQueue() }}
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
    </div>
  )
})

// ── Shared ────────────────────────────────────────────────────────────────────

function EmptyHint({ children }: { children: React.ReactNode }) {
  return (
    <div style={{
      flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center',
      fontFamily: "'JetBrains Mono', monospace", fontSize: 12, color: 'var(--hds-txt-3)',
    }}>
      {children}
    </div>
  )
}

// ── Queue tab ─────────────────────────────────────────────────────────────────

function QueueListPanel({
  items, total, loading, filter, selected, matching,
  onFilterChange, onSelect, onTriggerMatch,
}: {
  items: ReviewQueueItem[]; total: number; loading: boolean
  filter: 'all'|'uncertain'|'unmatched'; selected: ReviewQueueItem | null; matching: boolean
  onFilterChange: (f: 'all'|'uncertain'|'unmatched') => void
  onSelect: (item: ReviewQueueItem) => void
  onTriggerMatch: () => void
}) {
  return (
    <>
      {/* Header */}
      <div style={{ padding: '12px 16px 10px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 8 }}>
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 14, fontWeight: 700, color: 'var(--hds-txt)' }}>
            Review Queue
          </span>
          <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
            {total} item{total !== 1 ? 's' : ''}
          </span>
        </div>
        <div style={{ display: 'flex', gap: 4 }}>
          {(['all', 'uncertain', 'unmatched'] as const).map(f => (
            <button key={f} onClick={() => onFilterChange(f)} style={{
              flex: 1, padding: '5px 0', borderRadius: 6,
              border: `1px solid ${filter === f ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
              background: filter === f ? 'oklch(0.55 0.14 292 / 0.15)' : 'transparent',
              color: filter === f ? 'var(--hds-violet)' : 'var(--hds-txt-3)',
              fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
              cursor: 'pointer', letterSpacing: '0.06em',
            }}>
              {f.toUpperCase()}
            </button>
          ))}
        </div>
      </div>

      {/* List */}
      <div style={{ flex: 1, overflowY: 'auto' }} className="scrollbar-dark">
        {loading ? (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 1 }}>
            {Array.from({ length: 8 }, (_, i) => (
              <div key={i} className="hds-skeleton" style={{ height: 60, margin: '1px 0' }} />
            ))}
          </div>
        ) : items.length === 0 ? (
          <div style={{
            padding: 32, textAlign: 'center',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
            color: 'var(--hds-txt-3)', lineHeight: 1.6,
          }}>
            Nothing in the queue.<br />
            <span style={{ color: 'var(--hds-txt-2)' }}>Run a match pass to find uncertain items.</span>
          </div>
        ) : (
          items.map(item => (
            <button
              key={item.kairos_id}
              onClick={() => onSelect(item)}
              style={{
                width: '100%', display: 'flex', alignItems: 'center', gap: 10,
                padding: '10px 16px', border: 'none', cursor: 'pointer', textAlign: 'left',
                background: selected?.kairos_id === item.kairos_id ? 'var(--hds-bg-3)' : 'transparent',
                borderBottom: '1px solid var(--hds-line-s)',
                borderLeft: `3px solid ${item.match_status === 'uncertain' ? 'var(--hds-match-amber)' : 'var(--hds-match-red)'}`,
                transition: 'background .1s',
              }}
            >
              {item.thumb && (
                <img
                  src={item.source_base_url + item.thumb}
                  alt=""
                  style={{ width: 32, height: 48, objectFit: 'cover', borderRadius: 4, flexShrink: 0 }}
                  onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
                />
              )}
              <div style={{ flex: 1, minWidth: 0 }}>
                <div style={{
                  fontFamily: "'Chakra Petch', sans-serif", fontSize: 12, fontWeight: 600,
                  color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                }}>{item.title}</div>
                <div style={{
                  fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)', marginTop: 3,
                }}>
                  {item.item_type}{item.year ? ` · ${item.year}` : ''}
                </div>
                <div style={{ marginTop: 4, display: 'flex', alignItems: 'center', gap: 5 }}>
                  <MatchBadge status={item.match_status as MatchStatus} score={item.match_score} size="sm" />
                  {item.match_score > 0 && (
                    <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)' }}>
                      {Math.round(item.match_score * 100)}%
                    </span>
                  )}
                </div>
              </div>
              <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)', flexShrink: 0 }}>
                {item.candidates.length}
              </span>
            </button>
          ))
        )}
      </div>

      {/* Run match button */}
      <div style={{ padding: '12px 16px', borderTop: '1px solid var(--hds-line)', flexShrink: 0 }}>
        <button
          onClick={onTriggerMatch}
          disabled={matching}
          style={{ ...goldBtnStyle, width: '100%', opacity: matching ? 0.5 : 1, cursor: matching ? 'not-allowed' : 'pointer' }}
        >
          {matching ? 'Running…' : 'Run Match Pass'}
        </button>
      </div>
    </>
  )
}

// ── Queue candidate panel ─────────────────────────────────────────────────────

function CandidatePanel({
  item, onAccept, onReject, onClose, onMatched,
}: {
  item:      ReviewQueueItem
  onAccept:  (id: string) => void
  onReject:  (id: string) => void
  onClose:   () => void
  onMatched: () => void
}) {
  const [searchMode,    setSearchMode]    = useState(false)
  const [searchQuery,   setSearchQuery]   = useState(item.title)
  const [searchResults, setSearchResults] = useState<ScraperSearchResult[]>([])
  const [searchLoading, setSearchLoading] = useState(false)
  const [matchingId,    setMatchingId]    = useState<string | null>(null)

  const runSearch = async (q: string) => {
    if (!q.trim()) return
    setSearchLoading(true)
    setSearchResults([])
    try {
      const r = await api.scraperSearch(q.trim(), item.item_type)
      setSearchResults(r.items)
    } catch {}
    setSearchLoading(false)
  }

  const handleManualMatch = async (result: ScraperSearchResult) => {
    const key = result.source + ':' + result.external_id
    setMatchingId(key)
    try {
      await api.manualMatch(item.kairos_id, {
        item_type:   item.item_type,
        source:      result.source,
        external_id: result.external_id,
        title:       result.title,
        year:        result.year,
        poster_url:  result.poster_url,
        overview:    result.overview,
      })
      onMatched()
    } catch {
      setMatchingId(null)
    }
  }

  const toggleSearch = () => {
    setSearchMode(m => !m)
    if (!searchMode) {
      setSearchResults([])
      setSearchQuery(item.title)
    }
  }

  return (
    <div style={{ flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
      {/* Header */}
      <div style={{
        padding: '16px 24px', borderBottom: '1px solid var(--hds-line)',
        display: 'flex', alignItems: 'center', gap: 12, flexShrink: 0,
      }}>
        <div style={{ flex: 1 }}>
          <h2 style={{
            fontFamily: "'Chakra Petch', sans-serif", fontSize: 18, fontWeight: 700,
            color: 'var(--hds-txt)', margin: '0 0 6px',
          }}>{item.title}</h2>
          <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
            {item.year && <span style={chip}>{item.year}</span>}
            <span style={chip}>{item.item_type}</span>
            <MatchBadge status={item.match_status as MatchStatus} score={item.match_score} />
          </div>
        </div>
        <button
          onClick={toggleSearch}
          style={{
            padding: '5px 12px', borderRadius: 6, cursor: 'pointer', fontSize: 10,
            fontFamily: "'JetBrains Mono', monospace", letterSpacing: '0.06em',
            border: `1px solid ${searchMode ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
            background: searchMode ? 'oklch(0.55 0.14 292 / 0.15)' : 'transparent',
            color: searchMode ? 'var(--hds-violet)' : 'var(--hds-txt-3)',
          }}
        >
          {searchMode ? 'Candidates' : 'Search'}
        </button>
        <button onClick={onClose} style={{
          background: 'none', border: 'none', cursor: 'pointer',
          color: 'var(--hds-txt-3)', fontSize: 20, lineHeight: 1,
        }}>✕</button>
      </div>

      {searchMode ? (
        /* ── Search panel ── */
        <div style={{ flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
          <div style={{ padding: '14px 24px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
            <div style={{ display: 'flex', gap: 8 }}>
              <input
                autoFocus
                value={searchQuery}
                onChange={e => setSearchQuery(e.target.value)}
                onKeyDown={e => e.key === 'Enter' && runSearch(searchQuery)}
                placeholder="Title or tmdb:12345 / tvdb:12345"
                style={{
                  flex: 1, padding: '7px 12px', borderRadius: 7,
                  border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
                  color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 12,
                  outline: 'none',
                }}
              />
              <button
                onClick={() => runSearch(searchQuery)}
                disabled={searchLoading || !searchQuery.trim()}
                style={{
                  padding: '7px 16px', borderRadius: 7, cursor: 'pointer',
                  border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
                  color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace",
                  fontSize: 11, opacity: (searchLoading || !searchQuery.trim()) ? 0.5 : 1,
                }}
              >
                {searchLoading ? '…' : 'Search'}
              </button>
            </div>
          </div>

          <div style={{ flex: 1, overflowY: 'auto', padding: '16px 24px' }} className="scrollbar-dark">
            {searchLoading ? (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                {Array.from({ length: 4 }, (_, i) => (
                  <div key={i} className="hds-skeleton" style={{ height: 90, borderRadius: 8 }} />
                ))}
              </div>
            ) : searchResults.length === 0 ? (
              <div style={{
                padding: '32px 0', textAlign: 'center',
                fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)', lineHeight: 1.6,
              }}>
                {searchQuery.trim() ? 'No results. Try a different title or use tmdb:ID / tvdb:ID.' : 'Enter a title or ID to search.'}
              </div>
            ) : (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
                {searchResults.map(r => {
                  const key       = r.source + ':' + r.external_id
                  const isMatching = matchingId === key
                  const srcColor  = r.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)'
                  return (
                    <div key={key} style={{
                      display: 'flex', gap: 0, borderRadius: 9,
                      border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)',
                      overflow: 'hidden', opacity: isMatching ? 0.6 : 1,
                    }}>
                      {r.poster_url && (
                        <img src={r.poster_url} alt=""
                          style={{ width: 60, height: 90, objectFit: 'cover', flexShrink: 0 }}
                          onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
                        />
                      )}
                      <div style={{ flex: 1, padding: '10px 14px', minWidth: 0 }}>
                        <div style={{ display: 'flex', alignItems: 'flex-start', gap: 8, marginBottom: 5 }}>
                          <div style={{
                            fontFamily: "'Chakra Petch', sans-serif", fontSize: 13, fontWeight: 600,
                            color: 'var(--hds-txt)', flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                          }}>{r.title}</div>
                          <button
                            onClick={() => handleManualMatch(r)}
                            disabled={isMatching}
                            style={{
                              flexShrink: 0, padding: '4px 12px', borderRadius: 5, cursor: isMatching ? 'not-allowed' : 'pointer',
                              border: '1px solid var(--hds-match-green)',
                              background: 'oklch(0.7 0.16 150 / 0.1)',
                              color: 'var(--hds-match-green)',
                              fontFamily: "'JetBrains Mono', monospace", fontSize: 10, fontWeight: 600,
                            }}
                          >
                            {isMatching ? '…' : 'Match'}
                          </button>
                        </div>
                        <div style={{ display: 'flex', gap: 6, marginBottom: 6, flexWrap: 'wrap' }}>
                          {r.year && <span style={chip}>{r.year}</span>}
                          <span style={{ ...chip, color: srcColor, borderColor: 'currentColor' }}>{r.source.toUpperCase()}</span>
                          <span style={{ ...chip, fontSize: 8 }}>ID: {r.external_id}</span>
                          {r.in_library && <span style={{ ...chip, color: 'oklch(0.7 0.16 150)', borderColor: 'currentColor' }}>in library</span>}
                        </div>
                        {r.overview && (
                          <p style={{
                            fontFamily: "'JetBrains Mono', monospace", fontSize: 10, lineHeight: 1.5,
                            color: 'var(--hds-txt-3)', margin: 0,
                            display: '-webkit-box', WebkitLineClamp: 2, WebkitBoxOrient: 'vertical', overflow: 'hidden',
                          }}>{r.overview}</p>
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
        <div style={{ flex: 1, overflowY: 'auto', padding: '20px 24px' }} className="scrollbar-dark">
          {item.candidates.length === 0 ? (
            <div style={{
              fontFamily: "'JetBrains Mono', monospace", fontSize: 12,
              color: 'var(--hds-txt-3)', padding: '32px 0',
            }}>
              No candidates found — use Search to find a match manually.
            </div>
          ) : (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
              <div style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
                color: 'var(--hds-txt-3)', letterSpacing: '0.08em', marginBottom: 4,
              }}>
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
  const scoreColor = pct >= 90 ? 'var(--hds-match-green)' : pct >= 70 ? 'var(--hds-match-amber)' : 'var(--hds-match-red)'
  const accepted   = candidate.accepted

  return (
    <div style={{
      borderRadius: 10,
      border: `1px solid ${accepted === true ? 'var(--hds-match-green)' : accepted === false ? 'oklch(0.35 0.04 285)' : 'var(--hds-line)'}`,
      background: accepted === true ? 'oklch(0.7 0.16 150 / 0.05)' : accepted === false ? 'oklch(0.35 0.04 285 / 0.05)' : 'var(--hds-bg-2)',
      overflow: 'hidden', opacity: accepted === false ? 0.45 : 1,
    }}>
      <div style={{ display: 'flex' }}>
        {candidate.poster_url && (
          <img src={candidate.poster_url} alt=""
            style={{ width: 80, height: 120, objectFit: 'cover', flexShrink: 0 }}
            onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
          />
        )}
        <div style={{ flex: 1, padding: '14px 16px', minWidth: 0 }}>
          <div style={{ display: 'flex', alignItems: 'flex-start', gap: 8, marginBottom: 6 }}>
            <div style={{
              fontFamily: "'Chakra Petch', sans-serif", fontSize: 14, fontWeight: 600,
              color: 'var(--hds-txt)', flex: 1,
            }}>{candidate.title}</div>
            <span style={{
              fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700,
              color: scoreColor, flexShrink: 0, paddingTop: 2,
            }}>{pct}%</span>
          </div>
          <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap', marginBottom: 8 }}>
            {candidate.year && <span style={chip}>{candidate.year}</span>}
            <span style={{
              ...chip,
              color: candidate.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)',
              borderColor: 'currentColor',
            }}>{candidate.source.toUpperCase()}</span>
            <span style={{ ...chip, fontSize: 8 }}>ID: {candidate.external_id}</span>
          </div>
          {candidate.overview && (
            <p style={{
              fontFamily: "'JetBrains Mono', monospace", fontSize: 10, lineHeight: 1.6,
              color: 'var(--hds-txt-3)', margin: '0 0 10px',
              display: '-webkit-box', WebkitLineClamp: 3, WebkitBoxOrient: 'vertical', overflow: 'hidden',
            }}>{candidate.overview}</p>
          )}
          {accepted === null && (
            <div style={{ display: 'flex', gap: 8 }}>
              <button onClick={onAccept} style={{
                flex: 1, padding: '6px 0', borderRadius: 6, cursor: 'pointer',
                border: '1px solid var(--hds-match-green)', background: 'oklch(0.7 0.16 150 / 0.12)',
                color: 'var(--hds-match-green)', fontFamily: "'JetBrains Mono', monospace",
                fontSize: 10, letterSpacing: '0.08em', fontWeight: 600,
              }}>Accept</button>
              <button onClick={onReject} style={{
                flex: 1, padding: '6px 0', borderRadius: 6, cursor: 'pointer',
                border: '1px solid var(--hds-line)', background: 'transparent',
                color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, letterSpacing: '0.08em',
              }}>Reject</button>
            </div>
          )}
          {accepted === true  && <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-match-green)', letterSpacing: '0.08em' }}>✓ Accepted</div>}
          {accepted === false && <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.08em' }}>Rejected</div>}
        </div>
      </div>
    </div>
  )
}

// ── Groups tab ────────────────────────────────────────────────────────────────

const GroupsListPanel = observer(function GroupsListPanel({ store }: { store: GroupsStore }) {
  return (
    <>
      <div style={{ padding: '12px 16px 10px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 14, fontWeight: 700, color: 'var(--hds-txt)' }}>
            Episode Groups
          </span>
          {store.pendingCount > 0 && (
            <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
              {store.pendingCount} pending
            </span>
          )}
        </div>
      </div>
      <div style={{ flex: 1, overflowY: 'auto' }} className="scrollbar-dark">
        {store.loading && store.shows.length === 0 ? (
          <div style={{ padding: '20px 16px', display: 'flex', alignItems: 'center', gap: 8, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11 }}>
            <span style={{ width: 6, height: 6, borderRadius: '50%', background: 'var(--hds-violet)', flexShrink: 0, animation: 'hds-pulse 2.6s ease-in-out infinite' }} />
            Scanning shows…
          </div>
        ) : store.shows.length === 0 ? (
          <div style={{ padding: 32, textAlign: 'center', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)', lineHeight: 1.6 }}>
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
                style={{
                  width: '100%', display: 'flex', alignItems: 'center', justifyContent: 'space-between',
                  gap: 8, padding: '9px 16px', border: 'none', cursor: 'pointer', textAlign: 'left',
                  background: isActive ? 'oklch(0.83 0.13 84 / 0.07)' : 'transparent',
                  borderBottom: '1px solid var(--hds-line-s)',
                  borderLeft: `3px solid ${isActive ? 'var(--hds-match-amber)' : 'transparent'}`,
                  color: isActive ? 'var(--hds-match-amber)' : 'var(--hds-txt-2)',
                  transition: 'background .1s',
                }}
              >
                <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 12, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', flex: 1 }}>
                  {s.show_title}
                </span>
                <span style={{
                  flexShrink: 0, fontFamily: "'JetBrains Mono', monospace", fontSize: 9, fontWeight: 700,
                  padding: '2px 6px', borderRadius: 6,
                  background: isActive ? 'oklch(0.83 0.13 84 / 0.15)' : 'var(--hds-bg-3)',
                  color: isActive ? 'var(--hds-match-amber)' : 'var(--hds-txt-3)',
                }}>{pending}</span>
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
    <div style={{ flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
      <div style={{
        padding: '16px 24px', borderBottom: '1px solid var(--hds-line)',
        display: 'flex', alignItems: 'center', justifyContent: 'space-between', flexShrink: 0,
      }}>
        <h2 style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 16, fontWeight: 700, color: 'var(--hds-txt)', margin: 0 }}>
          {show.show_title}
        </h2>
        {highConf.length > 1 && (
          <button
            disabled={store.saving}
            onClick={() => store.confirmAllHigh()}
            style={{
              padding: '6px 14px', borderRadius: 7, cursor: store.saving ? 'not-allowed' : 'pointer',
              border: '1px solid oklch(0.6 0.18 260 / 0.5)',
              background: 'oklch(0.55 0.18 260 / 0.1)', color: 'oklch(0.72 0.18 260)',
              fontFamily: "'JetBrains Mono', monospace", fontSize: 10, opacity: store.saving ? 0.5 : 1,
            }}
          >
            Confirm all high-confidence ({highConf.length})
          </button>
        )}
      </div>

      <div style={{ flex: 1, overflowY: 'auto', padding: '20px 24px' }} className="scrollbar-dark">
        {/* Confirmed groups */}
        <div style={{
          fontFamily: "'JetBrains Mono', monospace", fontSize: 9, letterSpacing: '0.14em',
          color: 'oklch(0.6 0.18 260 / 0.7)', marginBottom: 10, fontWeight: 700,
        }}>CONFIRMED GROUPS</div>
        {store.groupsLoading ? (
          <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)', paddingBottom: 16 }}>Loading…</div>
        ) : store.confirmedGroups.length === 0 ? (
          <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)', paddingBottom: 16 }}>No confirmed groups yet.</div>
        ) : (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 8, marginBottom: 24 }}>
            {store.confirmedGroups.map(g => (
              <div key={g.group_id} style={{
                borderRadius: 8, border: '1px solid oklch(0.6 0.18 150 / 0.25)',
                background: 'oklch(0.6 0.18 150 / 0.05)', padding: '10px 14px',
              }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
                  <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 12, fontWeight: 600, color: 'var(--hds-txt)', flex: 1 }}>{g.name}</span>
                  <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'oklch(0.65 0.16 150)' }}>{g.members.length} parts</span>
                  <button
                    disabled={store.saving}
                    onClick={() => store.deleteGroup(g.group_id)}
                    style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)', background: 'none', border: 'none', cursor: 'pointer', opacity: store.saving ? 0.4 : 1 }}
                  >Delete</button>
                </div>
                <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
                  {g.members.map(m => (
                    <div key={m.id} style={{ display: 'flex', gap: 10, fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                      <span style={{ width: 44, flexShrink: 0 }}>Part {m.part_num}</span>
                      <span style={{ width: 52, flexShrink: 0, color: 'var(--hds-txt-3)' }}>S{String(m.season).padStart(2,'0')}E{String(m.episode).padStart(2,'0')}</span>
                      <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{m.title}</span>
                    </div>
                  ))}
                </div>
              </div>
            ))}
          </div>
        )}

        {/* Candidates */}
        <div style={{
          fontFamily: "'JetBrains Mono', monospace", fontSize: 9, letterSpacing: '0.14em',
          color: 'oklch(0.6 0.18 260 / 0.7)', marginBottom: 10, fontWeight: 700,
        }}>DETECTED CANDIDATES</div>
        {unconfirmed.length === 0 ? (
          <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)' }}>All candidates already confirmed.</div>
        ) : (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
            {unconfirmed.sort((a, b) => b.confidence - a.confidence).map((c, i) => {
              const isHigh = c.confidence >= 80
              return (
                <div key={i} style={{
                  borderRadius: 8,
                  border: `1px solid ${isHigh ? 'oklch(0.6 0.18 260 / 0.4)' : 'oklch(0.4 0.05 260 / 0.25)'}`,
                  padding: '10px 14px',
                }}>
                  <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
                    <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 12, fontWeight: 600, color: 'var(--hds-txt)', flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{c.base_title}</span>
                    <span style={{
                      fontFamily: "'JetBrains Mono', monospace", fontSize: 9, fontWeight: 700,
                      padding: '2px 6px', borderRadius: 5, flexShrink: 0,
                      background: isHigh ? 'oklch(0.55 0.18 260 / 0.25)' : 'oklch(0.4 0.08 260 / 0.15)',
                      color:      isHigh ? 'oklch(0.75 0.18 260)'         : 'oklch(0.6 0.08 260)',
                    }}>{c.confidence}%</span>
                    {!c.adjacent && <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-match-amber)', flexShrink: 0 }}>non-adjacent</span>}
                    <button
                      disabled={store.saving}
                      onClick={() => store.confirmGroup(c)}
                      style={{
                        flexShrink: 0, padding: '3px 10px', borderRadius: 5, cursor: store.saving ? 'not-allowed' : 'pointer',
                        border: '1px solid oklch(0.6 0.18 260 / 0.5)',
                        background: 'transparent', color: 'oklch(0.72 0.18 260)',
                        fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
                        opacity: store.saving ? 0.4 : 1,
                      }}
                    >Confirm</button>
                  </div>
                  <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
                    {c.parts.map(p => (
                      <div key={p.episode_id} style={{ display: 'flex', gap: 10, fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                        <span style={{ width: 44, flexShrink: 0 }}>Part {p.part_num}</span>
                        <span style={{ width: 52, flexShrink: 0 }}>S{String(p.season).padStart(2,'0')}E{String(p.episode).padStart(2,'0')}</span>
                        <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{p.title}</span>
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

const STATUS_COLOR: Record<RequestStatus, string> = {
  pending:  'var(--hds-match-amber)',
  approved: 'oklch(0.7 0.16 150)',
  rejected: 'var(--hds-match-red)',
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
      <div style={{ padding: '12px 16px 10px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 8 }}>
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 14, fontWeight: 700, color: 'var(--hds-txt)' }}>
            Requests
          </span>
        </div>
        <div style={{ display: 'flex', gap: 4 }}>
          {(['pending', 'all', 'approved', 'rejected'] as const).map(f => (
            <button key={f} onClick={() => onFilterChange(f)} style={{
              flex: 1, padding: '5px 0', borderRadius: 6, cursor: 'pointer', fontSize: 9,
              fontFamily: "'JetBrains Mono', monospace", letterSpacing: '0.06em',
              border: `1px solid ${filter === f ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
              background: filter === f ? 'oklch(0.55 0.14 292 / 0.15)' : 'transparent',
              color: filter === f ? 'var(--hds-violet)' : 'var(--hds-txt-3)',
            }}>
              {f === 'all' ? 'ALL' : f.toUpperCase()}
              {f !== 'all' && (
                <span style={{ marginLeft: 4, opacity: 0.7 }}>
                  {allRequests.filter(r => r.status === f).length}
                </span>
              )}
            </button>
          ))}
        </div>
      </div>

      <div style={{ flex: 1, overflowY: 'auto' }} className="scrollbar-dark">
        {loading ? (
          <div style={{ padding: 16, display: 'flex', flexDirection: 'column', gap: 8 }}>
            {[...Array(5)].map((_, i) => <div key={i} className="hds-skeleton" style={{ height: 72, borderRadius: 8 }} />)}
          </div>
        ) : items.length === 0 ? (
          <div style={{ padding: 32, textAlign: 'center', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)' }}>
            {filter === 'pending' ? 'No pending requests' : `No ${filter} requests`}
          </div>
        ) : (
          <div style={{ padding: '8px 10px' }}>
            {items.map(r => {
              const srcColor = r.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)'
              const isSelected = selected?.request_id === r.request_id
              return (
                <div
                  key={r.request_id}
                  onClick={() => onSelect(r)}
                  style={{
                    display: 'flex', gap: 10, alignItems: 'center', padding: '9px 10px',
                    borderRadius: 8, cursor: 'pointer', marginBottom: 2,
                    background: isSelected ? 'oklch(0.55 0.14 292 / 0.1)' : 'transparent',
                    border: `1px solid ${isSelected ? 'var(--hds-violet)' : 'transparent'}`,
                    transition: 'background .1s',
                  }}
                >
                  <div style={{ width: 32, height: 48, borderRadius: 4, overflow: 'hidden', flexShrink: 0, background: 'var(--hds-bg-3)' }}>
                    {r.poster_url && <img src={r.poster_url} alt="" style={{ width: '100%', height: '100%', objectFit: 'cover' }} onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />}
                  </div>
                  <div style={{ flex: 1, minWidth: 0 }}>
                    <div style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 12, fontWeight: 600, color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{r.title}</div>
                    <div style={{ display: 'flex', gap: 5, marginTop: 4, alignItems: 'center' }}>
                      {r.year && <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)' }}>{r.year}</span>}
                      <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 8, color: srcColor, border: `1px solid ${srcColor}`, borderRadius: 3, padding: '1px 4px' }}>{r.source.toUpperCase()}</span>
                    </div>
                  </div>
                  <span style={{ flexShrink: 0, fontFamily: "'JetBrains Mono', monospace", fontSize: 8, letterSpacing: '0.08em', color: STATUS_COLOR[r.status] }}>
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
  const srcColor     = r.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)'

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
    } catch {
      setArrError(`Could not reach ${serviceLabel}.`)
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
    } catch {
      setArrError(`Failed to add to ${serviceLabel}.`)
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
    <div style={{ flex: 1, height: '100%', overflow: 'hidden', display: 'flex', flexDirection: 'column' }}>
      {/* Backdrop */}
      <div style={{ position: 'relative', height: 200, flexShrink: 0 }}>
        <div style={{
          position: 'absolute', inset: 0,
          background: r.poster_url
            ? `url(${r.poster_url}) center 20%/cover no-repeat`
            : 'linear-gradient(135deg, oklch(0.12 0.04 292), oklch(0.16 0.03 280))',
        }} />
        <div style={{ position: 'absolute', inset: 0, background: 'linear-gradient(to top, var(--hds-bg) 0%, oklch(0 0 0 / 0.4) 100%)' }} />
        <button onClick={onClose} style={{
          position: 'absolute', top: 12, right: 12, width: 28, height: 28, borderRadius: '50%',
          border: 'none', cursor: 'pointer', background: 'oklch(0 0 0 / 0.5)',
          color: 'oklch(0.8 0.01 285)', fontSize: 18, display: 'flex', alignItems: 'center', justifyContent: 'center',
        }}>×</button>
        {r.poster_url && (
          <img src={r.poster_url} alt="" style={{
            position: 'absolute', bottom: -32, left: 24,
            width: 60, height: 90, objectFit: 'cover', borderRadius: 6,
            boxShadow: '0 4px 20px oklch(0 0 0 / 0.5)',
          }} />
        )}
      </div>

      <div style={{ flex: 1, overflowY: 'auto', padding: r.poster_url ? '44px 24px 32px' : '20px 24px 32px', maxWidth: 560 }}>
        <div style={{ display: 'flex', alignItems: 'flex-start', justifyContent: 'space-between', gap: 12, marginBottom: 10 }}>
          <h2 style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 20, fontWeight: 700, color: 'var(--hds-txt)', margin: 0, lineHeight: 1.2 }}>
            {r.title}
          </h2>
          <span style={{
            flexShrink: 0, marginTop: 4,
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, letterSpacing: '0.08em',
            color: STATUS_COLOR[r.status], border: `1px solid ${STATUS_COLOR[r.status]}`,
            padding: '2px 8px', borderRadius: 10,
            background: `${STATUS_COLOR[r.status].replace(')', ' / 0.08)')}`,
          }}>{r.status.toUpperCase()}</span>
        </div>
        <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', marginBottom: 16 }}>
          {r.year && <span style={chip}>{r.year}</span>}
          <span style={chip}>{r.content_type}</span>
          <span style={{ ...chip, color: srcColor, borderColor: `${srcColor.replace(')', ' / 0.4)')}` }}>{r.source.toUpperCase()}</span>
        </div>
        <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)', marginBottom: 20 }}>
          Requested {new Date(r.created_at * 1000).toLocaleString()} · user {r.user_id.slice(0, 8)}
        </div>

        {r.status === 'pending' && (
          <div style={{ borderTop: '1px solid var(--hds-line-s)', paddingTop: 18 }}>
            <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.08em', marginBottom: 12 }}>
              {serviceLabel.toUpperCase()}
            </div>
            {arrStep === 'idle' && (
              <div style={{ display: 'flex', gap: 8 }}>
                <button onClick={handleApproveClick} style={{
                  flex: 2, padding: '9px 0', borderRadius: 8, cursor: 'pointer',
                  border: '1px solid oklch(0.7 0.16 150 / 0.6)', background: 'oklch(0.7 0.16 150 / 0.1)',
                  color: 'oklch(0.7 0.16 150)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 600,
                }}>Approve → {serviceLabel}</button>
                <button onClick={handleReject} disabled={rejecting} style={{
                  flex: 1, padding: '9px 0', borderRadius: 8, cursor: rejecting ? 'default' : 'pointer',
                  border: '1px solid var(--hds-match-red)', background: 'oklch(0.55 0.22 27 / 0.08)',
                  color: 'var(--hds-match-red)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
                  opacity: rejecting ? 0.5 : 1,
                }}>Reject</button>
              </div>
            )}
            {(arrStep === 'loading' || arrStep === 'adding') && (
              <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)', padding: '8px 0' }}>
                {arrStep === 'loading' ? `Looking up in ${serviceLabel}…` : `Adding to ${serviceLabel}…`}
              </div>
            )}
            {arrStep === 'form' && options && (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 11 }}>
                <label style={formLabelStyle}>
                  Quality Profile
                  <select value={qualityProfileId ?? ''} onChange={e => setQualityProfileId(Number(e.target.value))} style={selectStyle}>
                    {options.quality_profiles.map(p => <option key={p.id} value={p.id}>{p.name}</option>)}
                  </select>
                </label>
                <label style={formLabelStyle}>
                  Root Folder
                  <select value={rootFolder} onChange={e => setRootFolder(e.target.value)} style={selectStyle}>
                    {options.root_folders.map(f => <option key={f} value={f}>{f}</option>)}
                  </select>
                </label>
                <label style={{ display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-2)' }}>
                  <input type="checkbox" checked={searchOnAdd} onChange={e => setSearchOnAdd(e.target.checked)} style={{ accentColor: 'var(--hds-violet)', width: 14, height: 14 }} />
                  Search immediately
                </label>
                <div style={{ display: 'flex', gap: 8, marginTop: 2 }}>
                  <button onClick={() => setArrStep('idle')} style={{ flex: 1, padding: '8px 0', borderRadius: 7, cursor: 'pointer', border: '1px solid var(--hds-line)', background: 'transparent', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10 }}>Cancel</button>
                  <button onClick={handleConfirmApprove} style={{ flex: 2, padding: '8px 0', borderRadius: 7, cursor: 'pointer', border: '1px solid oklch(0.7 0.16 150 / 0.6)', background: 'oklch(0.7 0.16 150 / 0.1)', color: 'oklch(0.7 0.16 150)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, fontWeight: 600 }}>
                    Confirm → {serviceLabel}
                  </button>
                </div>
              </div>
            )}
            {arrStep === 'error' && (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                <div style={{ padding: '10px 14px', borderRadius: 8, border: '1px solid var(--hds-match-red)', background: 'oklch(0.55 0.22 27 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-match-red)', lineHeight: 1.5 }}>{arrError}</div>
                <button onClick={() => setArrStep('idle')} style={{ padding: '7px 0', borderRadius: 7, cursor: 'pointer', border: '1px solid var(--hds-line)', background: 'transparent', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10 }}>Try Again</button>
              </div>
            )}
          </div>
        )}
        {arrStep === 'done' && (
          <div style={{ padding: '12px 16px', borderRadius: 8, border: '1px solid oklch(0.7 0.16 150 / 0.4)', background: 'oklch(0.7 0.16 150 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'oklch(0.7 0.16 150)' }}>
            Approved — added to {serviceLabel}{searchOnAdd ? ', search queued' : ''}
          </div>
        )}
        {r.status === 'approved' && arrStep !== 'done' && (
          <div style={{ padding: '12px 16px', borderRadius: 8, border: '1px solid oklch(0.7 0.16 150 / 0.4)', background: 'oklch(0.7 0.16 150 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'oklch(0.7 0.16 150)' }}>Approved</div>
        )}
        {r.status === 'rejected' && (
          <div style={{ padding: '12px 16px', borderRadius: 8, border: '1px solid var(--hds-match-red)', background: 'oklch(0.55 0.22 27 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-match-red)' }}>Rejected</div>
        )}
      </div>
    </div>
  )
}

const formLabelStyle: React.CSSProperties = {
  display: 'flex', flexDirection: 'column', gap: 6,
  fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
  color: 'var(--hds-txt-3)', letterSpacing: '0.06em',
}

const selectStyle: React.CSSProperties = {
  padding: '7px 10px', borderRadius: 7,
  border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
  color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
  cursor: 'pointer', outline: 'none',
}
