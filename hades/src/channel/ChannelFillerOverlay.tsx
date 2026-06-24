import { useState, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import type { Channel, FillerEntryAdvancement, FillerSelectionMode, Show, Movie, Playlist } from '../api/types'
import { BLOCK_META, FILLER_ADV_OPTS, FILLER_SEL_OPTS } from './constants'
import { inputStyle, filterInputStyle } from './styles'
import { api } from '../api/client'
import type { ChannelDetailStore } from './store'
import { ShowMediaTile, MediaTile, MediaInfoPanel, useDetailPanel, BrowserEmpty } from './BrowserTiles'

type FillerTab = 'shows' | 'movies' | 'playlists'

const ChannelFillerOverlay = observer(function ChannelFillerOverlay({ channelId, channel, store }: {
  channelId: string
  channel:   Channel
  store:     ChannelDetailStore
}) {
  const [tab,   setTab]   = useState<FillerTab>('shows')
  const [query, setQuery] = useState('')

  const [shows,          setShows]          = useState<Show[]>([])
  const [showsLoading,   setShowsLoading]   = useState(false)
  const [movies,         setMovies]         = useState<Movie[]>([])
  const [moviesLoading,  setMoviesLoading]  = useState(false)
  const [playlists,      setPlaylists]      = useState<Playlist[]>([])
  const [playlistLoading,setPlaylistLoading]= useState(false)

  const { infoItem, setInfoItem, infoDetail, infoSeasons, detailLoading } = useDetailPanel()

  const entries  = channel.default_filler_entries
  const selMode  = channel.default_filler_selection
  const showWeight = selMode === 'weighted'

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
      setPlaylistLoading(true)
      api.getPlaylists()
        .then(r => { if (!ctrl.signal.aborted) setPlaylists(r) })
        .catch(() => {})
        .finally(() => { if (!ctrl.signal.aborted) setPlaylistLoading(false) })
    }
    return () => ctrl.abort()
  }, [tab, query])

  const isAdded = (ct: string, cid: string, sf?: number) =>
    entries.some(e => e.content_type === ct && e.content_id === cid &&
      (sf === undefined ? true : (e.season_filter ?? undefined) === sf))

  const addFiller = (params: { content_type: string; content_id: string; title: string; season_filter?: number | null; include_specials?: boolean }) => {
    const sf = params.season_filter ?? undefined
    if (isAdded(params.content_type, params.content_id, sf as number | undefined)) return
    store.addChannelFiller(channelId, {
      content_type: params.content_type as any,
      content_id:   params.content_id,
      title:        params.title,
      advancement:  'sized',
      weight:       1,
      season_filter: sf,
    })
  }

  const isLoading = (tab === 'shows' && showsLoading) || (tab === 'movies' && moviesLoading) || (tab === 'playlists' && playlistLoading)

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
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 16, letterSpacing: '0.04em' }}>Channel Filler</span>
        <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>default filler for blocks without their own filler list</span>
        <div style={{ flex: 1 }} />
        <button
          onClick={() => { store.channelFillerOverlayOpen = false }}
          style={{ width: 34, height: 34, display: 'flex', alignItems: 'center', justifyContent: 'center', borderRadius: 9, border: '1px solid var(--hds-line-s)', background: 'transparent', color: 'var(--hds-txt-2)', fontSize: 18, cursor: 'pointer' }}
        >×</button>
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
              addLabel="ADD TO CHANNEL FILLER"
              onAdd={addFiller}
              onBack={() => setInfoItem(null)}
            />
          ) : (
            <>
              <div style={{ flexShrink: 0, padding: '10px 14px 0', borderBottom: '1px solid var(--hds-line-s)' }}>
                <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 7, padding: 3, width: 'fit-content', marginBottom: 8 }}>
                  {(['shows', 'movies', 'playlists'] as FillerTab[]).map(t => (
                    <button key={t} onClick={() => { setTab(t); setQuery(''); setInfoItem(null) }}
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
                {isLoading ? (
                  <div style={{ padding: '20px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
                ) : tab === 'shows' ? (
                  shows.length === 0 ? <BrowserEmpty /> : (
                    <div style={gridStyle}>
                      {shows.map(s => (
                        <ShowMediaTile key={s.show_id}
                          show={s}
                          isAdded={isAdded('show', s.show_id)}
                          onAdd={addFiller}
                          onInfoOpen={() => setInfoItem({ kind: 'show', id: s.show_id, seed: s })}
                        />
                      ))}
                    </div>
                  )
                ) : tab === 'movies' ? (
                  movies.length === 0 ? <BrowserEmpty /> : (
                    <div style={gridStyle}>
                      {movies.map(m => (
                        <MediaTile key={m.movie_id}
                          imgUrl={`/api/movies/${m.movie_id}/thumb`}
                          title={m.title}
                          sub={m.year ? String(m.year) : undefined}
                          badge={isAdded('movie', m.movie_id)}
                          onClick={() => setInfoItem({ kind: 'movie', id: m.movie_id, seed: m })}
                        />
                      ))}
                    </div>
                  )
                ) : (
                  playlists.length === 0 ? <BrowserEmpty hint="No playlists found." /> : (
                    <div style={gridStyle}>
                      {playlists.map(p => (
                        <MediaTile key={p.playlist_id}
                          title={p.title}
                          sub={`${p.item_count} items`}
                          placeholder="☰"
                          badge={isAdded('playlist', p.playlist_id)}
                          onClick={() => addFiller({ content_type: 'playlist', content_id: p.playlist_id, title: p.title })}
                        />
                      ))}
                    </div>
                  )
                )}
              </div>
            </>
          )}
        </div>

        {/* Right: filler entries */}
        <div style={{ flexShrink: 0, width: 380, borderLeft: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', background: 'oklch(0.15 0.018 288 / 0.55)', overflow: 'auto', padding: 18 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 15 }}>
            <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 11, letterSpacing: '0.14em', color: 'var(--hds-gold)' }}>CHANNEL FILLER</span>
            <span style={{ fontSize: 11, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{entries.length}</span>
          </div>

          {entries.length > 1 && (
            <div style={{ marginBottom: 12 }}>
              <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>SELECT BY</div>
              <select value={selMode} onChange={e => store.saveChannelFiller(channelId, { default_filler_selection: e.target.value as FillerSelectionMode })} style={inputStyle}>
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
                    <div style={{ fontSize: 9.5, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>
                      S{String(entry.season_filter).padStart(2, '0')} only
                    </div>
                  )}
                </div>
                <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>{entry.content_type}</span>
                <select
                  value={entry.advancement}
                  onChange={e => store.updateChannelFiller(channelId, entry.id, { advancement: e.target.value as FillerEntryAdvancement })}
                  style={{ ...filterInputStyle, width: 88 }}
                >
                  {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                </select>
                {showWeight && (
                  <input type="number" min={1} value={entry.weight}
                    onChange={e => store.updateChannelFiller(channelId, entry.id, { weight: Math.max(1, +e.target.value || 1) })}
                    style={{ ...filterInputStyle, width: 44, textAlign: 'center' }} title="Weight" />
                )}
                <button
                  onClick={() => store.removeChannelFiller(channelId, entry.id)}
                  style={{ width: 22, height: 22, border: 'none', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13, flexShrink: 0 }}
                >×</button>
              </div>
            ))}
            {entries.length === 0 && (
              <div style={{ padding: '16px 12px', textAlign: 'center', fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", border: '1px dashed var(--hds-line-s)', borderRadius: 8 }}>
                No filler configured for this channel
              </div>
            )}
          </div>
        </div>
      </div>

      {/* Footer */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 11, padding: '14px 22px', borderTop: '1px solid var(--hds-line-s)' }}>
        <span style={{ flex: 1, fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
          {entries.length > 0 ? `${entries.length} source${entries.length !== 1 ? 's' : ''} · ${selMode}` : 'No default channel filler configured'}
        </span>
        {store.channelFillerErr && (
          <span style={{ fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{store.channelFillerErr}</span>
        )}
        <button
          onClick={() => { store.channelFillerOverlayOpen = false }}
          style={{ padding: '11px 26px', borderRadius: 10, background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))', color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 14, letterSpacing: '0.04em', border: 'none', cursor: 'pointer' }}
        >Done</button>
      </div>
    </div>
    </div>
  )
})

export default ChannelFillerOverlay
