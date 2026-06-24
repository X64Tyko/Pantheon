import { useState, useEffect, useRef } from 'react'
import { observer } from 'mobx-react-lite'
import { BLOCK_META, FILLER_ADV_OPTS, FILLER_SEL_OPTS } from './constants'
import { inputStyle, filterInputStyle } from './styles'
import { imageQueue } from './imageQueue'
import { api } from '../api/client'
import type { Episode, FillerEntryAdvancement, FillerSelectionMode, Show, Movie, Playlist } from '../api/types'
import type { ChannelDetailStore } from './store'

type FillerTab = 'shows' | 'movies' | 'playlists'

type Armed =
  | { content_type: 'show';     content_id: string; title: string; season_filter?: number }
  | { content_type: 'movie';    content_id: string; title: string }
  | { content_type: 'playlist'; content_id: string; title: string }

const FillerOverlay = observer(function FillerOverlay({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const [tab,      setTab]      = useState<FillerTab>('shows')
  const [query,    setQuery]    = useState('')
  const [armed,    setArmed]    = useState<Armed | null>(null)
  const [newAdv,   setNewAdv]   = useState<FillerEntryAdvancement>('sized')
  const [dragId,   setDragId]   = useState<string | null>(null)

  const [drillShow,    setDrillShow]    = useState<Show | null>(null)
  const [drillSeasons, setDrillSeasons] = useState<number[]>([])
  const [drillEps,     setDrillEps]     = useState<Episode[]>([])
  const [drillLoading, setDrillLoading] = useState(false)

  const [shows,          setShows]          = useState<Show[]>([])
  const [showsLoading,   setShowsLoading]   = useState(false)
  const [movies,         setMovies]         = useState<Movie[]>([])
  const [moviesLoading,  setMoviesLoading]  = useState(false)
  const [playlists,      setPlaylists]      = useState<Playlist[]>([])
  const [playlistsLoading, setPlaylistsLoading] = useState(false)

  const d       = store.draft
  const entries = store.draftFillerEntries
  const showWeight = d.filler_selection === 'weighted'

  // load on tab/query change
  useEffect(() => {
    const ctrl = new AbortController()
    if (tab === 'shows') {
      setShowsLoading(true)
      api.getShows({ limit: 100, q: query || undefined })
        .then(r => { if (!ctrl.signal.aborted) setShows(r.items) })
        .catch(() => {})
        .finally(() => { if (!ctrl.signal.aborted) setShowsLoading(false) })
    } else if (tab === 'movies') {
      setMoviesLoading(true)
      api.getMovies({ limit: 100, q: query || undefined })
        .then(r => { if (!ctrl.signal.aborted) setMovies(r.items) })
        .catch(() => {})
        .finally(() => { if (!ctrl.signal.aborted) setMoviesLoading(false) })
    } else {
      setPlaylistsLoading(true)
      api.getPlaylists()
        .then(r => { if (!ctrl.signal.aborted) setPlaylists(r) })
        .catch(() => {})
        .finally(() => { if (!ctrl.signal.aborted) setPlaylistsLoading(false) })
    }
    return () => ctrl.abort()
  }, [tab, query])

  const drillInto = (show: Show) => {
    setDrillShow(show)
    setArmed(null)
    setDrillSeasons([])
    setDrillEps([])
    setDrillLoading(true)
    Promise.all([
      api.getShowSeasons(show.show_id).then(r => setDrillSeasons(r.seasons)),
      api.getEpisodes(show.show_id).then(r => setDrillEps(r)),
    ]).catch(() => {}).finally(() => setDrillLoading(false))
  }

  const armSeason = (show: Show, sf: number | undefined) => {
    const label = sf === undefined ? show.title : `${show.title} — S${String(sf).padStart(2, '0')}`
    setArmed({ content_type: 'show', content_id: show.show_id, title: label, season_filter: sf })
  }

  const addArmed = () => {
    if (!armed) return
    const sf = (armed as any).season_filter as number | undefined
    const already = entries.some(e => e.content_id === armed.content_id && e.content_type === armed.content_type && (e.season_filter ?? undefined) === sf)
    if (!already) {
      store.addBlockFiller(channelId, { content_type: armed.content_type, content_id: armed.content_id, title: armed.title, advancement: newAdv, weight: 1, season_filter: sf })
    }
    setArmed(null)
  }

  const isAdded = (ct: string, cid: string, sf?: number) =>
    entries.some(e => e.content_type === ct && e.content_id === cid && (sf === undefined ? true : (e.season_filter ?? undefined) === sf))

  const armedHint = armed
    ? `"${armed.title}" ready — click Add below`
    : drillShow ? 'Select a season to add as filler' : 'Click a show tile to browse seasons'

  const sBtnStyle = (active: boolean): React.CSSProperties => ({
    padding: '3px 7px', borderRadius: 5,
    border: `1px solid ${active ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
    background: active ? 'oklch(0.55 0.14 292 / 0.18)' : 'transparent',
    color: active ? 'var(--hds-violet)' : 'var(--hds-txt-2)',
    fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer',
  })

  const gridStyle: React.CSSProperties = {
    display: 'grid',
    gridTemplateColumns: 'repeat(auto-fill, minmax(128px, 1fr))',
    gap: 10, padding: 14, alignContent: 'start',
  }

  return (
    <div style={{ position: 'absolute', inset: 0, zIndex: 82, display: 'flex', flexDirection: 'column', background: 'linear-gradient(168deg, oklch(0.18 0.024 290 / 0.96), oklch(0.13 0.018 288 / 0.98))', backdropFilter: 'blur(26px)' }}>
      <div style={{ position: 'absolute', top: 0, left: 0, right: 0, height: 1, background: 'linear-gradient(90deg, transparent, oklch(0.78 0.15 292 / 0.7) 40%, oklch(0.85 0.13 84 / 0.5) 75%, transparent)', pointerEvents: 'none' }} />

      {/* Header */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 14, padding: '16px 22px 14px', borderBottom: '1px solid var(--hds-line-s)' }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 16, letterSpacing: '0.04em' }}>Filler &amp; Fallback</span>
        <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>select content to fill dead air in this block</span>
        <div style={{ flex: 1 }} />
        <button onClick={() => { store.fillerOverlayOpen = false }}
          style={{ width: 34, height: 34, display: 'flex', alignItems: 'center', justifyContent: 'center', borderRadius: 9, border: '1px solid var(--hds-line-s)', background: 'transparent', color: 'var(--hds-txt-2)', fontSize: 18, cursor: 'pointer' }}>×</button>
      </div>

      {/* Body */}
      <div style={{ flex: 1, minHeight: 0, display: 'flex' }}>

        {/* Tile grid / drill panel */}
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
                <div>
                  <div style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 13, fontWeight: 700 }}>{drillShow.title}</div>
                  {drillShow.year && <div style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>{drillShow.year}</div>}
                </div>
              </div>
              {/* Season buttons */}
              <div style={{ flexShrink: 0, padding: '10px 14px', borderBottom: '1px solid var(--hds-line-s)' }}>
                <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 7, fontFamily: "'JetBrains Mono', monospace" }}>ADD BY SEASON</div>
                {drillLoading ? (
                  <span style={{ fontSize: 11, color: 'var(--hds-txt-3)' }}>Loading…</span>
                ) : (
                  <div style={{ display: 'flex', flexWrap: 'wrap', gap: 5 }}>
                    <button style={sBtnStyle(armed?.content_id === drillShow.show_id && (armed as any).season_filter === undefined)} onClick={() => armSeason(drillShow, undefined)}>Add All</button>
                    {drillSeasons.includes(0) && <button style={sBtnStyle(armed?.content_id === drillShow.show_id && (armed as any).season_filter === 0)} onClick={() => armSeason(drillShow, 0)}>S00</button>}
                    {drillSeasons.includes(0) && <button style={sBtnStyle(false)} onClick={() => setArmed({ content_type: 'show', content_id: drillShow.show_id, title: drillShow.title })}>Non-Special</button>}
                    {drillSeasons.filter(s => s !== 0).map(s => (
                      <button key={s} style={sBtnStyle(armed?.content_id === drillShow.show_id && (armed as any).season_filter === s)} onClick={() => armSeason(drillShow, s)}>S{String(s).padStart(2, '0')}</button>
                    ))}
                  </div>
                )}
              </div>
              {/* Episode list */}
              <div style={{ flex: 1, overflow: 'auto' }} className="scrollbar-dark">
                <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', padding: '10px 14px 4px', fontFamily: "'JetBrains Mono', monospace" }}>EPISODES</div>
                {drillEps.map(ep => (
                  <div key={ep.episode_id} style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '6px 14px', borderBottom: '1px solid var(--hds-line-s)' }}>
                    <span style={{ width: 7, height: 7, borderRadius: 2, background: BLOCK_META.episode.edge, flexShrink: 0 }} />
                    <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>S{String(ep.season).padStart(2,'0')}E{String(ep.episode).padStart(2,'0')}</span>
                    <span style={{ flex: 1, fontSize: 11.5, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{ep.title}</span>
                    <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>{ep.duration_ms ? `${Math.round(ep.duration_ms / 60000)}m` : ''}</span>
                  </div>
                ))}
              </div>
            </>
          ) : (
            <>
          {/* Tabs + search */}
          <div style={{ flexShrink: 0, padding: '10px 14px 0', borderBottom: '1px solid var(--hds-line-s)' }}>
            <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 7, padding: 3, width: 'fit-content', marginBottom: 8 }}>
              {(['shows', 'movies', 'playlists'] as FillerTab[]).map(t => (
                <button key={t} onClick={() => { setTab(t); setArmed(null); setQuery('') }}
                  style={{ padding: '4px 12px', border: 'none', borderRadius: 5, background: tab === t ? 'var(--hds-violet)' : 'transparent', color: tab === t ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer', textTransform: 'capitalize' }}>
                  {t}
                </button>
              ))}
            </div>
            {tab !== 'playlists' && (
              <div style={{ marginBottom: 10 }}>
                <input value={query} onChange={e => setQuery(e.target.value)} placeholder="Search…"
                  style={{ ...inputStyle, width: '100%', fontSize: 11.5, padding: '6px 9px', boxSizing: 'border-box' }} />
              </div>
            )}
          </div>

          <div style={{ flex: 1, overflow: 'auto' }} className="scrollbar-dark">
            {(tab === 'shows' && showsLoading) || (tab === 'movies' && moviesLoading) || (tab === 'playlists' && playlistsLoading) ? (
              <div style={{ padding: '20px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
            ) : tab === 'shows' ? (
              shows.length === 0 ? <Empty /> : (
                <div style={gridStyle}>
                  {shows.map(s => (
                    <FillerTile key={s.show_id}
                      imgUrl={`/api/shows/${s.show_id}/thumb`}
                      title={s.title}
                      sub={s.year ? String(s.year) : undefined}
                      isArmed={false}
                      isAdded={isAdded('show', s.show_id)}
                      isDragged={dragId === s.show_id}
                      drillable
                      onDragStart={() => setDragId(s.show_id)}
                      onDragEnd={() => setDragId(null)}
                      onClick={() => drillInto(s)}
                    />
                  ))}
                </div>
              )
            ) : tab === 'movies' ? (
              movies.length === 0 ? <Empty /> : (
                <div style={gridStyle}>
                  {movies.map(m => (
                    <FillerTile key={m.movie_id}
                      imgUrl={`/api/movies/${m.movie_id}/thumb`}
                      title={m.title}
                      sub={m.year ? String(m.year) : undefined}
                      isArmed={armed?.content_id === m.movie_id && armed?.content_type === 'movie'}
                      isAdded={isAdded('movie', m.movie_id)}
                      isDragged={dragId === m.movie_id}
                      onDragStart={() => setDragId(m.movie_id)}
                      onDragEnd={() => setDragId(null)}
                      onClick={() => setArmed(a => a?.content_id === m.movie_id && a?.content_type === 'movie' ? null : { content_type: 'movie', content_id: m.movie_id, title: m.title })}
                    />
                  ))}
                </div>
              )
            ) : (
              playlists.length === 0 ? <Empty hint="No playlists found." /> : (
                <div style={gridStyle}>
                  {playlists.map(p => (
                    <FillerTile key={p.playlist_id}
                      title={p.title}
                      sub={`${p.item_count} items`}
                      placeholder="☰"
                      isArmed={armed?.content_id === p.playlist_id && armed?.content_type === 'playlist'}
                      isAdded={isAdded('playlist', p.playlist_id)}
                      isDragged={dragId === p.playlist_id}
                      onDragStart={() => setDragId(p.playlist_id)}
                      onDragEnd={() => setDragId(null)}
                      onClick={() => setArmed(a => a?.content_id === p.playlist_id && a?.content_type === 'playlist' ? null : { content_type: 'playlist', content_id: p.playlist_id, title: p.title })}
                    />
                  ))}
                </div>
              )
            )}
          </div>
            </>
          )}
        </div>

        {/* Right: entries panel */}
        <div style={{ flexShrink: 0, width: 380, borderLeft: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', background: 'oklch(0.15 0.018 288 / 0.55)', overflow: 'auto', padding: 18 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 15 }}>
            <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 11, letterSpacing: '0.14em', color: 'var(--hds-gold)' }}>BLOCK FILLER</span>
            <span style={{ fontSize: 11, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{entries.length}</span>
            <div style={{ flex: 1 }} />
            <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{armedHint}</span>
          </div>

          {entries.length > 1 && (
            <div style={{ marginBottom: 12 }}>
              <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>SELECT BY</div>
              <select value={d.filler_selection} onChange={e => store.setDraft('filler_selection', e.target.value as FillerSelectionMode)} style={inputStyle}>
                {FILLER_SEL_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
              </select>
            </div>
          )}

          <div style={{ display: 'flex', flexDirection: 'column', gap: 7, marginBottom: 14 }}>
            {entries.map(entry => (
              <div key={entry.id} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '9px 11px', background: 'oklch(0.21 0.024 290 / 0.5)', border: '1px solid var(--hds-line-s)', borderRadius: 9 }}>
                <span style={{ width: 7, height: 7, borderRadius: 2, background: BLOCK_META.filler.edge, flexShrink: 0 }} />
                <div style={{ flex: 1, minWidth: 0 }}>
                  <div style={{ fontSize: 12, fontWeight: 500, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{entry.title || entry.content_id}</div>
                  {entry.season_filter !== undefined && (
                    <div style={{ fontSize: 9.5, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>S{String(entry.season_filter).padStart(2, '0')} only</div>
                  )}
                </div>
                <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>{entry.content_type}</span>
                <select
                  value={entry.advancement}
                  onChange={e => store.updateBlockFiller(channelId, store.editing?.block_id ?? '', entry.id, { advancement: e.target.value as FillerEntryAdvancement })}
                  style={{ ...filterInputStyle, width: 88 }}
                >
                  {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                </select>
                {showWeight && (
                  <input type="number" min={1} value={entry.weight}
                    onChange={e => store.updateBlockFiller(channelId, store.editing?.block_id ?? '', entry.id, { weight: Math.max(1, +e.target.value || 1) })}
                    style={{ ...filterInputStyle, width: 44, textAlign: 'center' }} title="Weight" />
                )}
                <button onClick={() => store.removeBlockFiller(channelId, store.editing?.block_id ?? '', entry.id)}
                  style={{ width: 22, height: 22, border: 'none', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13, flexShrink: 0 }}>×</button>
              </div>
            ))}

            {entries.length === 0 && (
              <div style={{ padding: '16px 12px', textAlign: 'center', fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", border: '1px dashed var(--hds-line-s)', borderRadius: 8 }}>
                No filler — channel default will be used
              </div>
            )}
          </div>

          {/* Add selected */}
          {armed && (
            <div style={{ padding: '11px 12px', background: 'oklch(0.16 0.016 286)', border: '1px solid var(--hds-line)', borderRadius: 9, marginBottom: 10 }}>
              <div style={{ fontSize: 12, color: 'var(--hds-txt)', marginBottom: 8, fontWeight: 500 }}>{armed.title}</div>
              <div style={{ display: 'flex', gap: 7, alignItems: 'center', flexWrap: 'wrap' }}>
                <select value={newAdv} onChange={e => setNewAdv(e.target.value as FillerEntryAdvancement)} style={{ ...filterInputStyle, flex: '1 1 100px' }}>
                  {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                </select>
                <button
                  onClick={addArmed}
                  disabled={isAdded(armed.content_type, armed.content_id, (armed as any).season_filter)}
                  style={{ padding: '6px 14px', border: 'none', borderRadius: 6, background: 'var(--hds-violet)', color: 'oklch(0.15 0.02 286)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700, cursor: 'pointer', opacity: isAdded(armed.content_type, armed.content_id, (armed as any).season_filter) ? 0.4 : 1 }}
                >
                  {isAdded(armed.content_type, armed.content_id, (armed as any).season_filter) ? 'Already added' : 'Add to filler'}
                </button>
              </div>
            </div>
          )}

          {/* Inter-filler toggle */}
          <button
            onClick={() => store.setDraft('inter_filler', !d.inter_filler)}
            style={{
              display: 'flex', alignItems: 'center', justifyContent: 'space-between', width: '100%',
              padding: '8px 10px', border: `1px solid ${d.inter_filler ? 'oklch(0.7 0.12 84 / 0.5)' : 'var(--hds-line)'}`,
              borderRadius: 7, background: d.inter_filler ? 'oklch(0.55 0.1 84 / 0.12)' : 'var(--hds-bg-3)',
              cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace", marginTop: 'auto',
            }}
          >
            <span style={{ fontSize: 11, color: 'var(--hds-txt-2)' }}>Filler between programs</span>
            <span style={{ fontSize: 10, fontWeight: 700, letterSpacing: '0.08em', color: d.inter_filler ? 'var(--hds-gold)' : 'var(--hds-txt-3)' }}>{d.inter_filler ? 'ON' : 'OFF'}</span>
          </button>
          <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 6, lineHeight: 1.55 }}>
            When off, filler only fills leftover time at end of block. When on, also fills between programs.
          </div>
        </div>
      </div>

      {/* Footer */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 11, padding: '14px 22px', borderTop: '1px solid var(--hds-line-s)' }}>
        <span style={{ flex: 1, fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
          {entries.length > 0 ? `${entries.length} filler source${entries.length !== 1 ? 's' : ''} · ${d.filler_selection}` : 'Channel default filler will be used'}
        </span>
        <button
          onClick={() => { store.fillerOverlayOpen = false }}
          style={{ padding: '11px 26px', borderRadius: 10, background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))', color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 14, letterSpacing: '0.04em', border: 'none', cursor: 'pointer' }}
        >Done</button>
      </div>
    </div>
  )
})

// ─── Filler tile card ─────────────────────────────────────────────────────────

function FillerTile({ imgUrl, title, sub, placeholder, isArmed, isAdded, isDragged, drillable, onDragStart, onDragEnd, onClick }: {
  imgUrl?:     string
  title:       string
  sub?:        string
  placeholder?: string
  isArmed:     boolean
  isAdded:     boolean
  isDragged:   boolean
  drillable?:  boolean
  onDragStart: () => void
  onDragEnd:   () => void
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
    <div
      draggable
      onDragStart={onDragStart}
      onDragEnd={onDragEnd}
      onClick={onClick}
      style={{
        cursor: 'pointer', borderRadius: 8, overflow: 'hidden',
        border: `1px solid ${isArmed ? 'var(--hds-violet)' : 'var(--hds-line-s)'}`,
        background: 'var(--hds-bg-2)',
        boxShadow: isArmed ? '0 0 0 1px var(--hds-violet)' : 'none',
        opacity: isDragged ? 0.5 : 1, transition: 'border-color .1s',
      }}
      onMouseEnter={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-violet)'; scrollIn() }}
      onMouseLeave={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line-s)'; scrollOut() }}
    >
      <div style={{ width: '100%', aspectRatio: '2/3', background: 'var(--hds-bg-3)', position: 'relative', overflow: 'hidden' }}>
        {imgUrl ? (
          <img src={imgReady ? imgUrl : ''} alt=""
            style={{ position: 'absolute', inset: 0, width: '100%', height: '100%', objectFit: 'cover', opacity: imgReady ? 1 : 0, transition: 'opacity .2s' }} />
        ) : placeholder ? (
          <div style={{ position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 32, opacity: 0.3 }}>{placeholder}</div>
        ) : null}
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
        <div style={{ fontSize: 11, fontWeight: 600, lineHeight: 1.35, height: 30, overflow: 'hidden' }}>
          <span ref={titleRef} style={{ display: 'block', transition: 'transform 0.35s ease' }}>{title}</span>
        </div>
        {sub && <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 2 }}>{sub}</div>}
      </div>
    </div>
  )
}

function Empty({ hint }: { hint?: string }) {
  return <div style={{ padding: '20px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>{hint ?? 'No results.'}</div>
}

export default FillerOverlay
