import { useState, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import { api } from '../api/client'
import { inputStyle, filterInputStyle } from './styles'
import type { BumperContentType, BumperMode, ChannelBumper, EpisodeSearchResult, Playlist, Show } from '../api/types'
import type { ChannelDetailStore } from './store'
import { MediaTile, MediaInfoPanel, useDetailPanel, BrowserEmpty, LoadMoreSentinel } from './BrowserTiles'
import type { InfoItem, AddContentParams } from './BrowserTiles'

type BumperTab = 'shows' | 'playlists' | 'episodes'

const ChannelBumperOverlay = observer(function ChannelBumperOverlay({ channelId, store }: {
  channelId: string
  store:     ChannelDetailStore
}) {
  const [tab,          setTab]          = useState<BumperTab>('shows')
  const [q,            setQ]            = useState('')
  const [sFilter,      setSFilter]      = useState('')
  const [epsDurMax,    setEpsDurMax]    = useState('')
  const [shows,        setShows]        = useState<Show[]>([])
  const [showsTotal,   setShowsTotal]   = useState(0)
  const [showsLoadMore,setShowsLoadMore]= useState(false)
  const [lists,        setLists]        = useState<Playlist[]>([])
  const [eps,          setEps]          = useState<EpisodeSearchResult[]>([])
  const [epsHasMore,   setEpsHasMore]   = useState(false)
  const [epsLoadMore,  setEpsLoadMore]  = useState(false)
  const [loading,      setLoading]      = useState(false)

  const [bumpers,   setBumpers]   = useState<ChannelBumper[]>([])
  const [bumperErr, setBumperErr] = useState('')
  const [saving,    setSaving]    = useState(false)

  const [pendingMode, setPendingMode] = useState<BumperMode>('between')
  const [pendingN,    setPendingN]    = useState(3)

  const { infoItem, setInfoItem, infoDetail, infoSeasons, detailLoading } = useDetailPanel()

  useEffect(() => {
    api.getBumpers(channelId).then(setBumpers).catch(() => {})
  }, [channelId])

  useEffect(() => {
    if (infoItem) return
    const ctrl = new AbortController()
    setLoading(true)
    const season = sFilter.trim() !== '' ? parseInt(sFilter, 10) : undefined
    let p: Promise<void>
    if (tab === 'shows') {
      setShowsTotal(0)
      p = api.getShows({ limit: 80, q: q || undefined }).then(r => { if (!ctrl.signal.aborted) { setShows(r.items); setShowsTotal(r.total); setLoading(false) } })
    } else if (tab === 'playlists') {
      p = api.getPlaylists().then(r => { if (!ctrl.signal.aborted) { setLists(r); setLoading(false) } })
    } else {
      setEpsHasMore(false)
      p = api.searchEpisodes({ q: q || undefined, season: Number.isFinite(season) ? season : undefined, limit: 40 }).then(r => { if (!ctrl.signal.aborted) { setEps(r.items); setEpsHasMore(r.items.length >= 40); setLoading(false) } })
    }
    p.catch(() => { if (!ctrl.signal.aborted) setLoading(false) })
    return () => ctrl.abort()
  }, [tab, q, sFilter, infoItem])

  const loadMoreShows = () => {
    if (showsLoadMore || shows.length >= showsTotal) return
    setShowsLoadMore(true)
    api.getShows({ limit: 80, offset: shows.length, q: q || undefined })
      .then(r => { setShows(s => [...s, ...r.items]); setShowsTotal(r.total) })
      .catch(() => {})
      .finally(() => setShowsLoadMore(false))
  }

  const loadMoreEps = () => {
    if (epsLoadMore || !epsHasMore) return
    setEpsLoadMore(true)
    const season = sFilter.trim() !== '' ? parseInt(sFilter, 10) : undefined
    api.searchEpisodes({ q: q || undefined, season: Number.isFinite(season) ? season : undefined, limit: 40, offset: eps.length })
      .then(r => { setEps(e => [...e, ...r.items]); setEpsHasMore(r.items.length >= 40) })
      .catch(() => {})
      .finally(() => setEpsLoadMore(false))
  }

  async function addBumper(item: AddContentParams) {
    setSaving(true); setBumperErr('')
    try {
      const b = await api.createBumper(channelId, { content_type: item.content_type as BumperContentType, content_id: item.content_id, mode: pendingMode, every_n: pendingN })
      setBumpers(prev => [...prev, b as ChannelBumper])
      setInfoItem(null)
    } catch (e: any) { setBumperErr(e.message) }
    finally { setSaving(false) }
  }

  async function deleteBumper(id: number) {
    try {
      await api.deleteBumper(channelId, id)
      setBumpers(prev => prev.filter(b => b.id !== id))
    } catch (e: any) { setBumperErr(e.message) }
  }

  const renderBumperAdd = (item: InfoItem, _seasons: {number: number; name: string}[], _onAdd: (p: AddContentParams) => void) => {
    const ct    = item.kind === 'show' ? 'show' : item.kind === 'movie' ? 'movie' : item.kind === 'episode' ? 'episode' : 'playlist'
    const cid   = item.kind === 'show' ? item.id : item.kind === 'movie' ? item.id : item.kind === 'episode' ? item.ep.episode_id : item.pl.playlist_id
    const title = item.kind === 'show' ? item.seed.title : item.kind === 'movie' ? item.seed.title : item.kind === 'episode' ? item.ep.title : item.pl.title

    return (
      <div style={{ marginTop: 6 }}>
        <div style={{ display: 'flex', gap: 6, marginBottom: 8, flexWrap: 'wrap' }}>
          <select value={pendingMode} onChange={e => setPendingMode(e.target.value as BumperMode)} style={{ ...filterInputStyle, flex: '1 1 110px' }}>
            <option value="between">Between programs</option>
            <option value="filler">During filler</option>
          </select>
          <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
            <span style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>Every</span>
            <input type="number" min={1} value={pendingN} onChange={e => setPendingN(Math.max(1, +e.target.value || 1))}
              style={{ ...filterInputStyle, width: 48, textAlign: 'center' }} />
            <span style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>progs</span>
          </div>
        </div>
        <button
          onClick={() => addBumper({ content_type: ct as any, content_id: cid, title })}
          disabled={saving}
          style={{ padding: '6px 14px', border: 'none', borderRadius: 6, background: 'var(--hds-violet)', color: 'oklch(0.15 0.02 286)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700, cursor: 'pointer', opacity: saving ? 0.5 : 1 }}
        >
          {saving ? '…' : 'Add Bumper'}
        </button>
        {bumperErr && <div style={{ marginTop: 6, fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{bumperErr}</div>}
      </div>
    )
  }

  const gridStyle: React.CSSProperties = {
    display: 'grid',
    gridTemplateColumns: 'repeat(auto-fill, minmax(128px, 1fr))',
    gap: 10, padding: 14, alignContent: 'start',
  }

  return (
    <div style={{ position: 'fixed', inset: 0, zIndex: 200, display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'oklch(0.08 0.015 286 / 0.85)' }}>
    <div style={{ position: 'relative', display: 'flex', flexDirection: 'column', width: 'min(98vw, 1340px)', height: '92vh', background: 'linear-gradient(168deg, oklch(0.18 0.024 290 / 0.98), oklch(0.13 0.018 288 / 0.99))', borderRadius: 14, border: '1px solid var(--hds-line)', boxShadow: '0 32px 80px -16px rgba(0,0,0,0.8)', overflow: 'hidden' }}>
      <div style={{ position: 'absolute', top: 0, left: 0, right: 0, height: 1, background: 'linear-gradient(90deg, transparent, oklch(0.78 0.15 292 / 0.7) 40%, oklch(0.85 0.13 84 / 0.5) 75%, transparent)', pointerEvents: 'none' }} />

      {/* Header */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 14, padding: '16px 22px 14px', borderBottom: '1px solid var(--hds-line-s)' }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 16, letterSpacing: '0.04em' }}>Channel Bumpers</span>
        <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>injected between or during filler content</span>
        <div style={{ flex: 1 }} />
        <button onClick={() => { store.channelBumperOverlayOpen = false }}
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
              addLabel="ADD BUMPER"
              onAdd={() => {}}
              onBack={() => { setInfoItem(null); setBumperErr('') }}
              renderAdd={renderBumperAdd}
            />
          ) : (
            <>
              <div style={{ flexShrink: 0, padding: '10px 14px 0', borderBottom: '1px solid var(--hds-line-s)' }}>
                <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 7, padding: 3, width: 'fit-content', marginBottom: 8 }}>
                  {(['shows', 'playlists', 'episodes'] as BumperTab[]).map(t => (
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
                    <input type="number" min={0} value={sFilter} onChange={e => setSFilter(e.target.value)} placeholder="S#"
                      style={{ ...inputStyle, width: 52, fontSize: 11 }} />
                  )}
                  {tab === 'episodes' && (
                    <input type="number" min={1} value={epsDurMax} onChange={e => setEpsDurMax(e.target.value)} placeholder="≤m"
                      title="Max duration in minutes"
                      style={{ ...inputStyle, width: 60, fontSize: 11, padding: '6px 9px' }} />
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
                            badge={bumpers.some(b => b.content_type === 'show' && b.content_id === s.show_id)}
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
                          badge={bumpers.some(b => b.content_type === 'playlist' && b.content_id === p.playlist_id)}
                          onClick={() => setInfoItem({ kind: 'playlist', pl: p })}
                        />
                      ))}
                    </div>
                  )
                ) : (() => {
                  const maxMs = epsDurMax.trim() !== '' ? parseInt(epsDurMax) * 60_000 : undefined
                  const filtered = eps.filter(ep => maxMs === undefined || ep.duration_ms === 0 || ep.duration_ms <= maxMs)
                  return filtered.length === 0 ? <BrowserEmpty hint={eps.length === 0 ? 'Type to search episodes.' : 'No episodes match the duration filter.'} /> : (
                    <>
                      <div style={gridStyle}>
                        {filtered.map(ep => (
                          <MediaTile key={ep.episode_id}
                            imgUrl={`/api/shows/${ep.show_id}/thumb`}
                            title={`S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}`}
                            sub={ep.show_title}
                            badge={bumpers.some(b => b.content_type === 'episode' && b.content_id === ep.episode_id)}
                            onClick={() => setInfoItem({ kind: 'episode', ep })}
                          />
                        ))}
                      </div>
                      {epsHasMore && <LoadMoreSentinel loading={epsLoadMore} onVisible={loadMoreEps} />}
                    </>
                  )
                })()}
              </div>
            </>
          )}
        </div>

        {/* Right: bumpers list */}
        <div style={{ flexShrink: 0, width: 380, borderLeft: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', background: 'oklch(0.15 0.018 288 / 0.55)', overflow: 'auto', padding: 18 }} className="scrollbar-dark">
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 15 }}>
            <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 11, letterSpacing: '0.14em', color: 'var(--hds-gold)' }}>BUMPERS</span>
            <span style={{ fontSize: 11, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{bumpers.length}</span>
          </div>

          <div style={{ display: 'flex', flexDirection: 'column', gap: 7, marginBottom: 14 }}>
            {bumpers.map(b => (
              <div key={b.id} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '9px 11px', background: 'oklch(0.21 0.024 290 / 0.5)', border: '1px solid var(--hds-line-s)', borderRadius: 9 }}>
                <span style={{ fontSize: 9.5, padding: '2px 6px', borderRadius: 3, background: 'var(--hds-bg)', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>{b.content_type}</span>
                <span style={{ flex: 1, fontSize: 12, fontWeight: 500, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{b.title || b.content_id}</span>
                <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>{b.mode} / {b.every_n}</span>
                <button onClick={() => deleteBumper(b.id)}
                  style={{ width: 22, height: 22, border: 'none', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13, flexShrink: 0 }}>×</button>
              </div>
            ))}
            {bumpers.length === 0 && (
              <div style={{ padding: '16px 12px', textAlign: 'center', fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", border: '1px dashed var(--hds-line-s)', borderRadius: 8 }}>
                No bumpers configured for this channel
              </div>
            )}
          </div>
        </div>
      </div>

      {/* Footer */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 11, padding: '14px 22px', borderTop: '1px solid var(--hds-line-s)' }}>
        <span style={{ flex: 1, fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
          {bumpers.length > 0
            ? `${bumpers.length} bumper${bumpers.length !== 1 ? 's' : ''} · click a tile to add more`
            : 'No channel bumpers configured'}
        </span>
        {bumperErr && !infoItem && (
          <span style={{ fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{bumperErr}</span>
        )}
        <button onClick={() => { store.channelBumperOverlayOpen = false }}
          style={{ padding: '11px 26px', borderRadius: 10, background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))', color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 14, letterSpacing: '0.04em', border: 'none', cursor: 'pointer' }}
          className="hds-btn-gold">Done</button>
      </div>
    </div>
    </div>
  )
})

export default ChannelBumperOverlay
