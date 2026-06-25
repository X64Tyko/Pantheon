import { useState, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import { api } from '../api/client'
import { inputStyle } from './styles'
import type { ChannelDetailStore } from './store'
import type { Show, EpisodeSearchResult, Playlist } from '../api/types'
import { MediaTile, MediaInfoPanel, useDetailPanel, BrowserEmpty, LoadMoreSentinel } from './BrowserTiles'
import type { InfoItem, AddContentParams } from './BrowserTiles'

type BumperSlotKey = 'intro' | 'outro' | 'interstitial'
type BumperPickerTab = 'shows' | 'playlists' | 'episodes'

const SLOT_META: Record<BumperSlotKey, { label: string; hint: string }> = {
  intro:        { label: 'INTRO',        hint: 'Plays once before the first content item.' },
  outro:        { label: 'OUTRO',        hint: 'Plays after the last content item when the program count is hit.' },
  interstitial: { label: 'INTERSTITIAL', hint: 'Plays between show transitions.' },
}

function slotContentType(store: ChannelDetailStore, slot: BumperSlotKey): string {
  if (slot === 'intro')        return store.draft.intro_content_type        ?? ''
  if (slot === 'outro')        return store.draft.outro_content_type        ?? ''
  if (slot === 'interstitial') return store.draft.interstitial_content_type ?? ''
  return ''
}

function slotContentId(store: ChannelDetailStore, slot: BumperSlotKey): string {
  if (slot === 'intro')        return store.draft.intro_content_id        ?? ''
  if (slot === 'outro')        return store.draft.outro_content_id        ?? ''
  if (slot === 'interstitial') return store.draft.interstitial_content_id ?? ''
  return ''
}

function setSlot(store: ChannelDetailStore, slot: BumperSlotKey, ct: string, cid: string) {
  if (slot === 'intro')        { store.setDraft('intro_content_type' as any, ct);        store.setDraft('intro_content_id' as any, cid) }
  if (slot === 'outro')        { store.setDraft('outro_content_type' as any, ct);        store.setDraft('outro_content_id' as any, cid) }
  if (slot === 'interstitial') { store.setDraft('interstitial_content_type' as any, ct); store.setDraft('interstitial_content_id' as any, cid) }
}

function clearSlot(store: ChannelDetailStore, slot: BumperSlotKey) {
  setSlot(store, slot, '', '')
}

const BumperOverlay = observer(function BumperOverlay({ store }: { store: ChannelDetailStore }) {
  const [tab,     setTab]     = useState<BumperPickerTab>('shows')
  const [q,       setQ]       = useState('')
  const [sFilter, setSFilter] = useState('')
  const [shows,          setShows]          = useState<Show[]>([])
  const [showsTotal,     setShowsTotal]     = useState(0)
  const [showsLoadMore,  setShowsLoadMore]  = useState(false)
  const [lists,          setLists]          = useState<Playlist[]>([])
  const [eps,            setEps]            = useState<EpisodeSearchResult[]>([])
  const [epsHasMore,     setEpsHasMore]     = useState(false)
  const [epsLoadingMore, setEpsLoadingMore] = useState(false)
  const [loading,        setLoading]        = useState(false)
  const [epsDurationMax, setEpsDurationMax] = useState('')
  const [dragItem,       setDragItem]       = useState<AddContentParams | null>(null)
  const [armed,          setArmed]          = useState<AddContentParams | null>(null)

  const { infoItem, setInfoItem, infoDetail, infoSeasons, detailLoading } = useDetailPanel()

  useEffect(() => {
    setLoading(true)
    const season = sFilter.trim() !== '' ? parseInt(sFilter, 10) : undefined
    if (tab === 'shows') {
      setShowsTotal(0)
      api.getShows({ limit: 80, q: q || undefined })
        .then(r => { setShows(r.items); setShowsTotal(r.total); setLoading(false) })
        .catch(() => setLoading(false))
    } else if (tab === 'playlists') {
      api.getPlaylists().then(r => { setLists(r); setLoading(false) }).catch(() => setLoading(false))
    } else {
      setEpsHasMore(false)
      api.searchEpisodes({ q: q || undefined, season: Number.isFinite(season) ? season : undefined, limit: 40 })
        .then(r => { setEps(r.items); setEpsHasMore(r.items.length >= 40); setLoading(false) })
        .catch(() => setLoading(false))
    }
  }, [tab, q, sFilter])

  const loadMoreShows = () => {
    if (showsLoadMore || shows.length >= showsTotal) return
    setShowsLoadMore(true)
    api.getShows({ limit: 80, offset: shows.length, q: q || undefined })
      .then(r => { setShows(s => [...s, ...r.items]); setShowsTotal(r.total) })
      .catch(() => {})
      .finally(() => setShowsLoadMore(false))
  }

  const loadMoreEps = () => {
    if (epsLoadingMore || !epsHasMore) return
    setEpsLoadingMore(true)
    const season = sFilter.trim() !== '' ? parseInt(sFilter, 10) : undefined
    api.searchEpisodes({ q: q || undefined, season: Number.isFinite(season) ? season : undefined, limit: 40, offset: eps.length })
      .then(r => { setEps(e => [...e, ...r.items]); setEpsHasMore(r.items.length >= 40) })
      .catch(() => {})
      .finally(() => setEpsLoadingMore(false))
  }

  const assignToSlot = (slot: BumperSlotKey, item: AddContentParams) => {
    setSlot(store, slot, item.content_type, item.content_id)
    setArmed(null)
    setInfoItem(null)
  }

  const armItem = (params: AddContentParams) => {
    setArmed(a => a?.content_id === params.content_id && a?.content_type === params.content_type ? null : params)
  }

  const renderSlotAssign = (_item: InfoItem, _seasons: number[], _onAdd: (p: AddContentParams) => void) => {
    const source = infoItem
    if (!source) return null
    const ct  = source.kind === 'show' ? 'show' : source.kind === 'movie' ? 'movie' : source.kind === 'episode' ? 'episode' : 'playlist'
    const cid = source.kind === 'show' ? source.id : source.kind === 'movie' ? source.id : source.kind === 'episode' ? source.ep.episode_id : source.pl.playlist_id
    const title = source.kind === 'show' ? source.seed.title : source.kind === 'movie' ? source.seed.title : source.kind === 'episode' ? source.ep.title : source.pl.title
    const item: AddContentParams = { content_type: ct as any, content_id: cid, title }
    return (
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 6, marginTop: 6 }}>
        {(['intro', 'outro', 'interstitial'] as BumperSlotKey[]).map(slot => (
          <button key={slot} onClick={() => assignToSlot(slot, item)}
            style={{ padding: '4px 10px', borderRadius: 5, border: '1px solid var(--hds-line)', background: 'transparent', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer', textTransform: 'capitalize' }}>
            {slot}
          </button>
        ))}
      </div>
    )
  }

  const gridStyle: React.CSSProperties = {
    display: 'grid',
    gridTemplateColumns: 'repeat(auto-fill, minmax(128px, 1fr))',
    gap: 10, padding: 14, alignContent: 'start',
  }

  const configuredCount = (['intro', 'outro', 'interstitial'] as BumperSlotKey[]).filter(s => slotContentId(store, s) !== '').length

  return (
    <div style={{ position: 'absolute', inset: 0, zIndex: 82, display: 'flex', flexDirection: 'column', background: 'linear-gradient(168deg, oklch(0.18 0.024 290 / 0.96), oklch(0.13 0.018 288 / 0.98))', backdropFilter: 'blur(26px)' }}>
      <div style={{ position: 'absolute', top: 0, left: 0, right: 0, height: 1, background: 'linear-gradient(90deg, transparent, oklch(0.78 0.15 292 / 0.7) 40%, oklch(0.85 0.13 84 / 0.5) 75%, transparent)', pointerEvents: 'none' }} />

      {/* Header */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 14, padding: '16px 22px 14px', borderBottom: '1px solid var(--hds-line-s)' }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 16, letterSpacing: '0.04em' }}>Bumpers</span>
        <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>click a tile to inspect · assign to slot from detail panel or via arm + click</span>
        <div style={{ flex: 1 }} />
        <button onClick={() => { store.bumperOverlayOpen = false }}
          style={{ width: 34, height: 34, display: 'flex', alignItems: 'center', justifyContent: 'center', borderRadius: 9, border: '1px solid var(--hds-line-s)', background: 'transparent', color: 'var(--hds-txt-2)', fontSize: 18, cursor: 'pointer' }}>×</button>
      </div>

      {/* Body */}
      <div style={{ flex: 1, minHeight: 0, display: 'flex' }}>

        {/* Left: tile browser */}
        <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
          {infoItem ? (
            <MediaInfoPanel
              item={infoItem}
              detail={infoDetail}
              seasons={infoSeasons}
              detailLoading={detailLoading}
              addLabel="ASSIGN TO SLOT"
              onAdd={armItem}
              onBack={() => setInfoItem(null)}
              renderAdd={renderSlotAssign}
            />
          ) : (
            <>
              <div style={{ flexShrink: 0, padding: '10px 14px 0', borderBottom: '1px solid var(--hds-line-s)' }}>
                <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 7, padding: 3, width: 'fit-content', marginBottom: 8 }}>
                  {(['shows', 'playlists', 'episodes'] as BumperPickerTab[]).map(t => (
                    <button key={t} onClick={() => { setTab(t); setQ(''); setSFilter('') }}
                      style={{ padding: '4px 12px', border: 'none', borderRadius: 5, background: tab === t ? 'var(--hds-violet)' : 'transparent', color: tab === t ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer', textTransform: 'capitalize' }}>
                      {t}
                    </button>
                  ))}
                </div>
                <div style={{ display: 'flex', gap: 6, marginBottom: 10 }}>
                  <input value={q} onChange={e => setQ(e.target.value)} placeholder="Search…"
                    style={{ ...inputStyle, flex: 1, fontSize: 11.5, padding: '6px 9px' }} />
                  {tab === 'episodes' && (
                    <>
                      <input type="number" min={0} value={sFilter} onChange={e => setSFilter(e.target.value)} placeholder="S#"
                        style={{ ...inputStyle, width: 48, fontSize: 11 }} />
                      <input type="number" min={1} value={epsDurationMax} onChange={e => setEpsDurationMax(e.target.value)} placeholder="≤m"
                        title="Max duration in minutes"
                        style={{ ...inputStyle, width: 52, fontSize: 11 }} />
                    </>
                  )}
                </div>
              </div>

              <div style={{ flex: 1, overflow: 'auto' }} className="scrollbar-dark">
                {loading ? (
                  <div style={{ padding: '20px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
                ) : tab === 'shows' ? (
                  shows.length === 0 ? <BrowserEmpty /> : (
                    <>
                      <div style={gridStyle}>
                        {shows.map(s => (
                          <MediaTile key={s.show_id}
                            imgUrl={`/api/shows/${s.show_id}/thumb`}
                            title={s.title}
                            sub={s.year ? String(s.year) : undefined}
                            badge={(['intro', 'outro', 'interstitial'] as BumperSlotKey[]).some(slot => slotContentType(store, slot) === 'show' && slotContentId(store, slot) === s.show_id)}
                            onClick={() => setInfoItem({ kind: 'show', id: s.show_id, seed: s })}
                          />
                        ))}
                      </div>
                      {shows.length < showsTotal && <LoadMoreSentinel loading={showsLoadMore} onVisible={loadMoreShows} />}
                    </>
                  )
                ) : tab === 'playlists' ? (
                  lists.length === 0 ? <BrowserEmpty hint="No playlists." /> : (
                    <div style={gridStyle}>
                      {lists.map(p => (
                        <MediaTile key={p.playlist_id}
                          title={p.title} sub={`${p.item_count} items`} placeholder="☰"
                          badge={(['intro', 'outro', 'interstitial'] as BumperSlotKey[]).some(slot => slotContentType(store, slot) === 'playlist' && slotContentId(store, slot) === p.playlist_id)}
                          onClick={() => setInfoItem({ kind: 'playlist', pl: p })}
                        />
                      ))}
                    </div>
                  )
                ) : (() => {
                  const maxMs = epsDurationMax.trim() !== '' ? parseInt(epsDurationMax) * 60_000 : undefined
                  const filteredEps = eps.filter(ep => maxMs === undefined || ep.duration_ms === 0 || ep.duration_ms <= maxMs)
                  return filteredEps.length === 0 ? <BrowserEmpty hint={eps.length === 0 ? 'Type to search episodes.' : 'No episodes match the duration filter.'} /> : (
                    <>
                      <div style={gridStyle}>
                        {filteredEps.map(ep => (
                          <MediaTile key={ep.episode_id}
                            imgUrl={`/api/shows/${ep.show_id}/thumb`}
                            title={`S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}`}
                            sub={ep.show_title}
                            badge={(['intro', 'outro', 'interstitial'] as BumperSlotKey[]).some(slot => slotContentType(store, slot) === 'episode' && slotContentId(store, slot) === ep.episode_id)}
                            onClick={() => setInfoItem({ kind: 'episode', ep })}
                          />
                        ))}
                      </div>
                      {epsHasMore && <LoadMoreSentinel loading={epsLoadingMore} onVisible={loadMoreEps} />}
                    </>
                  )
                })()}
              </div>
            </>
          )}
        </div>

        {/* Right: slots */}
        <div style={{ flexShrink: 0, width: 380, borderLeft: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', background: 'oklch(0.15 0.018 288 / 0.55)', overflow: 'auto', padding: 18 }} className="scrollbar-dark">
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 15 }}>
            <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 11, letterSpacing: '0.14em', color: 'var(--hds-gold)' }}>SLOTS</span>
            {armed && (
              <span style={{ fontSize: 10, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", marginLeft: 'auto' }}>← click a slot to assign</span>
            )}
          </div>

          <div style={{ display: 'flex', flexDirection: 'column', gap: 13 }}>
            {(['intro', 'outro', 'interstitial'] as BumperSlotKey[]).map(slot => {
              const meta  = SLOT_META[slot]
              const ct    = slotContentType(store, slot)
              const cid   = slotContentId(store, slot)
              const hasCt = ct !== '' && cid !== ''

              const onDragOver  = (e: React.DragEvent) => { e.preventDefault(); (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-violet)' }
              const onDragLeave = (e: React.DragEvent) => { (e.currentTarget as HTMLDivElement).style.borderColor = hasCt ? 'var(--hds-line)' : 'var(--hds-line-s)' }
              const onDrop      = (e: React.DragEvent) => {
                e.preventDefault()
                if (dragItem) { assignToSlot(slot, dragItem); setDragItem(null) }
                ;(e.currentTarget as HTMLDivElement).style.borderColor = ''
              }
              const onClick = () => { if (armed) assignToSlot(slot, armed) }

              return (
                <div key={slot}>
                  <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
                    <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 10, letterSpacing: '0.14em', color: 'var(--hds-txt-2)' }}>{meta.label}</span>
                    <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{meta.hint}</span>
                  </div>
                  <div
                    onClick={onClick}
                    onDragOver={onDragOver}
                    onDragLeave={onDragLeave}
                    onDrop={onDrop}
                    style={{
                      display: 'flex', alignItems: 'center', gap: 8, padding: '9px 11px',
                      borderRadius: 9, border: `1px solid ${hasCt ? 'var(--hds-line)' : 'var(--hds-line-s)'}`,
                      background: hasCt ? 'oklch(0.21 0.024 290 / 0.4)' : 'oklch(0.17 0.018 288 / 0.35)',
                      cursor: armed ? 'pointer' : 'default', minHeight: 40, transition: 'border-color .12s',
                    }}
                  >
                    {hasCt ? (
                      <>
                        <span style={{ fontSize: 9.5, padding: '2px 6px', borderRadius: 3, background: 'var(--hds-bg)', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>{ct}</span>
                        <span style={{ flex: 1, fontSize: 12, color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{cid}</span>
                        <button
                          onClick={e => { e.stopPropagation(); clearSlot(store, slot) }}
                          style={{ width: 22, height: 22, flexShrink: 0, display: 'flex', alignItems: 'center', justifyContent: 'center', borderRadius: 6, background: 'transparent', border: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13 }}
                        >×</button>
                      </>
                    ) : (
                      <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
                        {armed ? '← Click to assign' : '— empty —'}
                      </span>
                    )}
                  </div>
                  {slot === 'interstitial' && (
                    <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginTop: 7 }}>
                      <span style={{ fontSize: 10.5, color: 'var(--hds-txt-3)' }}>Fire every</span>
                      <input
                        type="number" min={1} value={store.draft.interstitial_every_n ?? 1}
                        onChange={e => store.setDraft('interstitial_every_n' as any, Math.max(1, Number(e.target.value)))}
                        style={{ ...inputStyle, width: 56, padding: '4px 7px', fontSize: 11 }}
                      />
                      <span style={{ fontSize: 10.5, color: 'var(--hds-txt-3)' }}>show transition(s)</span>
                    </div>
                  )}
                </div>
              )
            })}
          </div>
        </div>
      </div>

      {/* Footer */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 11, padding: '14px 22px', borderTop: '1px solid var(--hds-line-s)' }}>
        <span style={{ flex: 1, fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
          {configuredCount} slot{configuredCount !== 1 ? 's' : ''} configured
        </span>
        <button onClick={() => { store.bumperOverlayOpen = false }}
          style={{ padding: '11px 26px', borderRadius: 10, background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))', color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 14, letterSpacing: '0.04em', border: 'none', cursor: 'pointer' }}
          className="hds-btn-gold">Done</button>
      </div>
    </div>
  )
})

export default BumperOverlay
