import { useState, useEffect, useRef } from 'react'
import { observer } from 'mobx-react-lite'
import { api } from '../api/client'
import { inputStyle, filterInputStyle } from './styles'
import { imageQueue } from './imageQueue'
import type { BumperContentType, BumperMode, ChannelBumper, EpisodeSearchResult, Playlist, Show } from '../api/types'
import type { ChannelDetailStore } from './store'

type BumperTab = 'shows' | 'playlists' | 'episodes'
type Armed = { content_type: BumperContentType; content_id: string; title: string }

const ChannelBumperOverlay = observer(function ChannelBumperOverlay({ channelId, store }: {
  channelId: string
  store:     ChannelDetailStore
}) {
  const [tab,      setTab]      = useState<BumperTab>('shows')
  const [q,        setQ]        = useState('')
  const [sFilter,  setSFilter]  = useState('')
  const [shows,    setShows]    = useState<Show[]>([])
  const [lists,    setLists]    = useState<Playlist[]>([])
  const [eps,      setEps]      = useState<EpisodeSearchResult[]>([])
  const [loading,  setLoading]  = useState(false)
  const [armed,    setArmed]    = useState<Armed | null>(null)

  const [bumpers,   setBumpers]   = useState<ChannelBumper[]>([])
  const [bumperErr, setBumperErr] = useState('')
  const [saving,    setSaving]    = useState(false)

  const [newMode, setNewMode] = useState<BumperMode>('between')
  const [newN,    setNewN]    = useState(3)

  const [drillShow,    setDrillShow]    = useState<Show | null>(null)
  const [drillSeasons, setDrillSeasons] = useState<number[]>([])
  const [drillEps,     setDrillEps]     = useState<any[]>([])
  const [drillSeason,  setDrillSeason]  = useState<number | null>(null)
  const [drillLoading, setDrillLoading] = useState(false)

  useEffect(() => {
    api.getBumpers(channelId).then(setBumpers).catch(() => {})
  }, [channelId])

  useEffect(() => {
    if (drillShow) return
    const ctrl = new AbortController()
    setLoading(true)
    const season = sFilter.trim() !== '' ? parseInt(sFilter, 10) : undefined
    const p =
      tab === 'shows'     ? api.getShows({ limit: 80, q: q || undefined }).then(r => { if (!ctrl.signal.aborted) { setShows(r.items); setLoading(false) } }) :
      tab === 'playlists' ? api.getPlaylists().then(r => { if (!ctrl.signal.aborted) { setLists(r); setLoading(false) } }) :
      api.searchEpisodes({ q: q || undefined, season: Number.isFinite(season) ? season : undefined, limit: 40 }).then(r => { if (!ctrl.signal.aborted) { setEps(r.items); setLoading(false) } })
    p.catch(() => { if (!ctrl.signal.aborted) setLoading(false) })
    return () => ctrl.abort()
  }, [tab, q, sFilter, drillShow])

  useEffect(() => {
    if (!drillShow) return
    setDrillLoading(true)
    api.getEpisodes(drillShow.show_id, drillSeason !== null ? drillSeason : undefined)
      .then(r => setDrillEps(r as any[]))
      .catch(() => {})
      .finally(() => setDrillLoading(false))
  }, [drillShow, drillSeason])

  const drillInto = (show: Show) => {
    setDrillShow(show)
    setArmed(null)
    setDrillSeason(null)
    setDrillEps([])
    setDrillSeasons([])
    setDrillLoading(true)
    api.getShowSeasons(show.show_id)
      .then(r => setDrillSeasons(r.seasons))
      .catch(() => {})
  }

  async function addBumper() {
    if (!armed) return
    setSaving(true); setBumperErr('')
    try {
      const b = await api.createBumper(channelId, { content_type: armed.content_type, content_id: armed.content_id, mode: newMode, every_n: newN })
      setBumpers(prev => [...prev, b as ChannelBumper])
      setArmed(null)
    } catch (e: any) { setBumperErr(e.message) }
    finally { setSaving(false) }
  }

  async function deleteBumper(id: number) {
    try {
      await api.deleteBumper(channelId, id)
      setBumpers(prev => prev.filter(b => b.id !== id))
    } catch (e: any) { setBumperErr(e.message) }
  }

  const armedHint = armed ? `"${armed.title}" ready` : drillShow ? 'Click an episode or add show' : 'Click a tile to select'

  const gridStyle: React.CSSProperties = {
    display: 'grid',
    gridTemplateColumns: 'repeat(auto-fill, minmax(140px, 1fr))',
    gap: 14, padding: '14px 18px', alignContent: 'start',
  }

  const sBtnStyle = (active: boolean): React.CSSProperties => ({
    padding: '3px 7px', borderRadius: 5,
    border: `1px solid ${active ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
    background: active ? 'oklch(0.55 0.14 292 / 0.18)' : 'transparent',
    color: active ? 'var(--hds-violet)' : 'var(--hds-txt-2)',
    fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer',
  })

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

        {/* Left: tile grid or drill-down */}
        <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>

          {drillShow ? (
            <>
              {/* Drill header */}
              <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 10, padding: '10px 14px', borderBottom: '1px solid var(--hds-line-s)' }}>
                <button onClick={() => { setDrillShow(null); setArmed(null) }}
                  style={{ display: 'flex', alignItems: 'center', gap: 5, padding: '4px 9px', border: '1px solid var(--hds-line)', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer' }}>← Back</button>
                <img src={`/api/shows/${drillShow.show_id}/thumb`} alt=""
                  style={{ width: 28, height: 42, objectFit: 'cover', borderRadius: 3, flexShrink: 0 }}
                  onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
                <div style={{ flex: 1, minWidth: 0 }}>
                  <div style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 13, fontWeight: 700, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{drillShow.title}</div>
                  {drillShow.year && <div style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>{drillShow.year}</div>}
                </div>
                <button
                  onClick={() => setArmed(a => a?.content_id === drillShow.show_id && a?.content_type === 'show' ? null : { content_type: 'show', content_id: drillShow.show_id, title: drillShow.title })}
                  style={{ padding: '4px 10px', border: `1px solid ${armed?.content_id === drillShow.show_id && armed?.content_type === 'show' ? 'var(--hds-violet)' : 'var(--hds-line)'}`, borderRadius: 6, background: armed?.content_id === drillShow.show_id && armed?.content_type === 'show' ? 'oklch(0.55 0.14 292 / 0.15)' : 'transparent', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer', flexShrink: 0 }}
                >+ Add Show</button>
              </div>

              {/* Season navigation */}
              <div style={{ flexShrink: 0, padding: '10px 14px', borderBottom: '1px solid var(--hds-line-s)' }}>
                <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 7, fontFamily: "'JetBrains Mono', monospace" }}>BROWSE BY SEASON</div>
                {drillSeasons.length === 0 ? (
                  <span style={{ fontSize: 11, color: 'var(--hds-txt-3)' }}>Loading…</span>
                ) : (
                  <div style={{ display: 'flex', flexWrap: 'wrap', gap: 5 }}>
                    <button style={sBtnStyle(drillSeason === null)} onClick={() => setDrillSeason(null)}>All</button>
                    {drillSeasons.map(s => (
                      <button key={s} style={sBtnStyle(drillSeason === s)} onClick={() => setDrillSeason(s)}>
                        S{String(s).padStart(2, '0')}
                      </button>
                    ))}
                  </div>
                )}
              </div>

              {/* Episode list */}
              <div style={{ flex: 1, overflow: 'auto' }} className="scrollbar-dark">
                {drillLoading ? (
                  <div style={{ padding: 14, color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
                ) : drillEps.length === 0 ? (
                  <div style={{ padding: 14, color: 'var(--hds-txt-3)', fontSize: 12 }}>No episodes.</div>
                ) : drillEps.map((ep: any) => {
                  const isArmed = armed?.content_type === 'episode' && armed?.content_id === ep.episode_id
                  const title   = `${drillShow.title} S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}`
                  return (
                    <div key={ep.episode_id}
                      onClick={() => setArmed(isArmed ? null : { content_type: 'episode', content_id: ep.episode_id, title })}
                      style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '7px 14px', borderBottom: '1px solid var(--hds-line-s)', cursor: 'pointer', background: isArmed ? 'oklch(0.55 0.14 292 / 0.12)' : 'transparent' }}
                      onMouseEnter={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)' }}
                      onMouseLeave={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.background = 'transparent' }}
                    >
                      <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0, minWidth: 52 }}>
                        S{String(ep.season).padStart(2,'0')}E{String(ep.episode).padStart(2,'0')}
                      </span>
                      <span style={{ flex: 1, fontSize: 11.5, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{ep.title}</span>
                      {ep.duration_ms ? <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>{Math.round(ep.duration_ms / 60000)}m</span> : null}
                      {isArmed && <span style={{ fontSize: 10, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>selected</span>}
                    </div>
                  )
                })}
              </div>
            </>
          ) : (
            <>
              {/* Tabs + search */}
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
                </div>
              </div>

              <div style={{ flex: 1, overflow: 'auto' }} className="scrollbar-dark">
                {loading ? (
                  <div style={{ padding: '18px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
                ) : tab === 'shows' ? (
                  shows.length === 0
                    ? <div style={{ padding: '18px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>No results.</div>
                    : <div style={gridStyle}>
                        {shows.map(s => (
                          <BumperTile key={s.show_id}
                            imgUrl={`/api/shows/${s.show_id}/thumb`}
                            title={s.title} sub={s.year ? String(s.year) : undefined}
                            isArmed={armed?.content_id === s.show_id && armed?.content_type === 'show'}
                            isAdded={bumpers.some(b => b.content_type === 'show' && b.content_id === s.show_id)}
                            drillable onClick={() => drillInto(s)} />
                        ))}
                      </div>
                ) : tab === 'playlists' ? (
                  lists.length === 0
                    ? <div style={{ padding: '18px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>No playlists.</div>
                    : <div style={gridStyle}>
                        {lists.map(p => (
                          <BumperTile key={p.playlist_id}
                            title={p.title} sub={`${p.item_count} items`} placeholder="☰"
                            isArmed={armed?.content_id === p.playlist_id && armed?.content_type === 'playlist'}
                            isAdded={bumpers.some(b => b.content_type === 'playlist' && b.content_id === p.playlist_id)}
                            onClick={() => setArmed(a => a?.content_id === p.playlist_id && a?.content_type === 'playlist' ? null : { content_type: 'playlist', content_id: p.playlist_id, title: p.title })} />
                        ))}
                      </div>
                ) : (
                  eps.length === 0
                    ? <div style={{ padding: '18px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>Type to search episodes.</div>
                    : <div style={{ display: 'flex', flexDirection: 'column' }}>
                        {eps.map(ep => {
                          const isArmed = armed?.content_type === 'episode' && armed?.content_id === ep.episode_id
                          const title   = `${ep.show_title} S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}`
                          return (
                            <div key={ep.episode_id}
                              onClick={() => setArmed(isArmed ? null : { content_type: 'episode', content_id: ep.episode_id, title })}
                              style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '7px 14px', borderBottom: '1px solid var(--hds-line-s)', cursor: 'pointer', background: isArmed ? 'oklch(0.55 0.14 292 / 0.12)' : 'transparent' }}
                              onMouseEnter={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)' }}
                              onMouseLeave={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.background = 'transparent' }}
                            >
                              <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{ep.show_title} · S{String(ep.season).padStart(2,'0')}E{String(ep.episode).padStart(2,'0')}</span>
                              <span style={{ flex: 1, fontSize: 11.5 }}>{ep.title}</span>
                              {isArmed && <span style={{ fontSize: 10, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>selected</span>}
                            </div>
                          )
                        })}
                      </div>
                )}
              </div>
            </>
          )}
        </div>

        {/* Right: bumpers panel */}
        <div style={{ flexShrink: 0, width: 380, borderLeft: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', background: 'oklch(0.15 0.018 288 / 0.55)', overflow: 'auto', padding: 18 }} className="scrollbar-dark">
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 15 }}>
            <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 11, letterSpacing: '0.14em', color: 'var(--hds-gold)' }}>BUMPERS</span>
            <span style={{ fontSize: 11, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{bumpers.length}</span>
            <div style={{ flex: 1 }} />
            <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{armedHint}</span>
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

          {armed && (
            <div style={{ padding: '11px 12px', background: 'oklch(0.16 0.016 286)', border: '1px solid var(--hds-line)', borderRadius: 9, marginBottom: 10 }}>
              <div style={{ fontSize: 12, color: 'var(--hds-txt)', marginBottom: 9, fontWeight: 500, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{armed.title}</div>
              <div style={{ display: 'flex', gap: 6, alignItems: 'center', flexWrap: 'wrap', marginBottom: 8 }}>
                <select value={newMode} onChange={e => setNewMode(e.target.value as BumperMode)} style={{ ...filterInputStyle, flex: '1 1 110px' }}>
                  <option value="between">Between programs</option>
                  <option value="filler">During filler</option>
                </select>
                <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
                  <span style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>Every</span>
                  <input type="number" min={1} value={newN} onChange={e => setNewN(Math.max(1, +e.target.value || 1))} style={{ ...filterInputStyle, width: 48, textAlign: 'center' }} />
                  <span style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>progs</span>
                </div>
              </div>
              <div style={{ display: 'flex', gap: 6 }}>
                <button onClick={addBumper} disabled={saving}
                  style={{ padding: '6px 14px', border: 'none', borderRadius: 6, background: 'var(--hds-violet)', color: 'oklch(0.15 0.02 286)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700, cursor: 'pointer', opacity: saving ? 0.5 : 1 }}>
                  {saving ? '…' : 'Add bumper'}
                </button>
                <button onClick={() => setArmed(null)}
                  style={{ padding: '6px 10px', border: '1px solid var(--hds-line)', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer' }}>
                  Cancel
                </button>
              </div>
              {bumperErr && <div style={{ marginTop: 7, fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{bumperErr}</div>}
            </div>
          )}
        </div>
      </div>

      {/* Footer */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 11, padding: '14px 22px', borderTop: '1px solid var(--hds-line-s)' }}>
        <span style={{ flex: 1, fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
          {bumpers.length > 0
            ? `${bumpers.length} bumper${bumpers.length !== 1 ? 's' : ''} · click a tile to add more`
            : 'No channel bumpers configured'}
        </span>
        {bumperErr && !armed && (
          <span style={{ fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{bumperErr}</span>
        )}
        <button onClick={() => { store.channelBumperOverlayOpen = false }}
          style={{ padding: '11px 26px', borderRadius: 10, background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))', color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 14, letterSpacing: '0.04em', border: 'none', cursor: 'pointer' }}>Done</button>
      </div>
    </div>
    </div>
  )
})

// ─── Bumper tile ──────────────────────────────────────────────────────────────

function BumperTile({ imgUrl, title, sub, placeholder, isArmed, isAdded, drillable, onClick }: {
  imgUrl?:     string
  title:       string
  sub?:        string
  placeholder?: string
  isArmed:     boolean
  isAdded?:    boolean
  drillable?:  boolean
  onClick:     () => void
}) {
  const [imgReady, setImgReady] = useState(false)
  const titleRef = useRef<HTMLSpanElement>(null)

  useEffect(() => {
    if (!imgUrl) return
    setImgReady(false)
    const ctrl = new AbortController()
    imageQueue.load(imgUrl, ctrl.signal).then(() => setImgReady(true)).catch(() => {})
    return () => ctrl.abort()
  }, [imgUrl])

  const scrollIn  = () => { if (titleRef.current) { const ov = titleRef.current.scrollHeight - 30; if (ov > 0) titleRef.current.style.transform = `translateY(-${ov}px)` } }
  const scrollOut = () => { if (titleRef.current) titleRef.current.style.transform = '' }

  return (
    <div onClick={onClick}
      style={{ cursor: 'pointer', borderRadius: 9, overflow: 'hidden', border: `1px solid ${isArmed ? 'var(--hds-violet)' : 'var(--hds-line-s)'}`, background: 'var(--hds-bg-2)', boxShadow: isArmed ? '0 0 0 1px var(--hds-violet)' : 'none', transition: 'border-color .1s' }}
      onMouseEnter={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-violet)'; scrollIn() }}
      onMouseLeave={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line-s)'; scrollOut() }}
    >
      <div style={{ width: '100%', aspectRatio: '2/3', background: 'var(--hds-bg-3)', position: 'relative', display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden' }}>
        {imgUrl && (
          <img src={imgReady ? imgUrl : ''} alt=""
            style={{ position: 'absolute', inset: 0, width: '100%', height: '100%', objectFit: 'cover', opacity: imgReady ? 1 : 0, transition: 'opacity .2s' }}
            onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
        )}
        {!imgUrl && placeholder && (
          <span style={{ fontSize: 22, opacity: 0.3, position: 'relative' }}>{placeholder}</span>
        )}
        {isAdded && (
          <div style={{ position: 'absolute', top: 6, right: 6, width: 18, height: 18, borderRadius: '50%', background: 'var(--hds-violet)', display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 9, color: '#fff', fontWeight: 700 }}>✓</div>
        )}
        {drillable && (
          <div style={{ position: 'absolute', bottom: 5, right: 5, fontSize: 10, color: 'oklch(0.88 0.08 292)', background: 'oklch(0.18 0.03 290 / 0.75)', padding: '1px 5px', borderRadius: 3, fontFamily: "'JetBrains Mono', monospace" }}>›</div>
        )}
        {isArmed && (
          <div style={{ position: 'absolute', inset: 0, background: 'oklch(0.55 0.14 292 / 0.3)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
            <span style={{ fontSize: 11, color: 'oklch(0.9 0.1 292)', fontFamily: "'JetBrains Mono', monospace" }}>selected</span>
          </div>
        )}
      </div>
      <div style={{ padding: '5px 7px 7px' }}>
        <div style={{ fontSize: 11, fontWeight: 600, lineHeight: 1.3, height: 30, overflow: 'hidden' }}>
          <span ref={titleRef} style={{ display: 'block', transition: 'transform 0.35s ease' }}>{title}</span>
        </div>
        {sub && <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 2 }}>{sub}</div>}
      </div>
    </div>
  )
}

export default ChannelBumperOverlay
