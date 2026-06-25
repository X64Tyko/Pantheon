import { useState, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import { BLOCK_META, FILLER_ADV_OPTS, FILLER_SEL_OPTS } from './constants'
import { inputStyle, filterInputStyle } from './styles'
import { api } from '../api/client'
import type { FillerEntryAdvancement, FillerSelectionMode, Show, Movie, Playlist, EpisodeSearchResult } from '../api/types'
import type { ChannelDetailStore } from './store'
import { ShowMediaTile, MediaTile, MediaInfoPanel, useDetailPanel, BrowserEmpty, LoadMoreSentinel } from './BrowserTiles'
import type { InfoItem } from './BrowserTiles'

type FillerTab = 'shows' | 'movies' | 'episodes' | 'playlists'

const FillerOverlay = observer(function FillerOverlay({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const [tab,         setTab]         = useState<FillerTab>('shows')
  const [query,       setQuery]       = useState('')
  const [maxDur,      setMaxDur]      = useState('')
  const [sFilter,     setSFilter]     = useState('')

  const [shows,            setShows]            = useState<Show[]>([])
  const [showsLoading,     setShowsLoading]     = useState(false)
  const [showsTotal,       setShowsTotal]       = useState(0)
  const [showsLoadingMore, setShowsLoadingMore] = useState(false)
  const [movies,           setMovies]           = useState<Movie[]>([])
  const [moviesLoading,    setMoviesLoading]    = useState(false)
  const [moviesTotal,      setMoviesTotal]      = useState(0)
  const [moviesLoadingMore,setMoviesLoadingMore]= useState(false)
  const [eps,              setEps]              = useState<EpisodeSearchResult[]>([])
  const [epsLoading,       setEpsLoading]       = useState(false)
  const [epsHasMore,       setEpsHasMore]       = useState(false)
  const [epsLoadingMore,   setEpsLoadingMore]   = useState(false)
  const [playlists,        setPlaylists]        = useState<Playlist[]>([])
  const [playlistsLoading, setPlaylistsLoading] = useState(false)

  const { infoItem, setInfoItem, infoDetail, infoSeasons, detailLoading } = useDetailPanel()

  const d       = store.draft
  const entries = store.draftFillerEntries
  const showWeight = d.filler_selection === 'weighted'

  useEffect(() => {
    const ctrl = new AbortController()
    if (tab === 'shows') {
      setShowsLoading(true); setShowsTotal(0)
      api.getShows({ limit: 80, q: query || undefined })
        .then(r => { if (!ctrl.signal.aborted) { setShows(r.items); setShowsTotal(r.total) } })
        .catch(() => {})
        .finally(() => { if (!ctrl.signal.aborted) setShowsLoading(false) })
    } else if (tab === 'movies') {
      setMoviesLoading(true); setMoviesTotal(0)
      api.getMovies({ limit: 80, q: query || undefined })
        .then(r => { if (!ctrl.signal.aborted) { setMovies(r.items); setMoviesTotal(r.total) } })
        .catch(() => {})
        .finally(() => { if (!ctrl.signal.aborted) setMoviesLoading(false) })
    } else if (tab === 'episodes') {
      setEpsLoading(true); setEpsHasMore(false)
      const season = sFilter.trim() !== '' ? parseInt(sFilter, 10) : undefined
      api.searchEpisodes({ q: query || undefined, season: Number.isFinite(season) ? season : undefined, limit: 40 })
        .then(r => { if (!ctrl.signal.aborted) { setEps(r.items); setEpsHasMore(r.items.length >= 40) } })
        .catch(() => {})
        .finally(() => { if (!ctrl.signal.aborted) setEpsLoading(false) })
    } else {
      setPlaylistsLoading(true)
      api.getPlaylists()
        .then(r => { if (!ctrl.signal.aborted) setPlaylists(r) })
        .catch(() => {})
        .finally(() => { if (!ctrl.signal.aborted) setPlaylistsLoading(false) })
    }
    return () => ctrl.abort()
  }, [tab, query, sFilter])

  const loadMoreShows = () => {
    if (showsLoadingMore || shows.length >= showsTotal) return
    setShowsLoadingMore(true)
    api.getShows({ limit: 80, offset: shows.length, q: query || undefined })
      .then(r => { setShows(s => [...s, ...r.items]); setShowsTotal(r.total) })
      .catch(() => {})
      .finally(() => setShowsLoadingMore(false))
  }

  const loadMoreMovies = () => {
    if (moviesLoadingMore || movies.length >= moviesTotal) return
    setMoviesLoadingMore(true)
    api.getMovies({ limit: 80, offset: movies.length, q: query || undefined })
      .then(r => { setMovies(m => [...m, ...r.items]); setMoviesTotal(r.total) })
      .catch(() => {})
      .finally(() => setMoviesLoadingMore(false))
  }

  const loadMoreEps = () => {
    if (epsLoadingMore || !epsHasMore) return
    setEpsLoadingMore(true)
    const season = sFilter.trim() !== '' ? parseInt(sFilter, 10) : undefined
    api.searchEpisodes({ q: query || undefined, season: Number.isFinite(season) ? season : undefined, limit: 40, offset: eps.length })
      .then(r => { setEps(e => [...e, ...r.items]); setEpsHasMore(r.items.length >= 40) })
      .catch(() => {})
      .finally(() => setEpsLoadingMore(false))
  }

  const isAdded = (ct: string, cid: string, sf?: number) =>
    entries.some(e => e.content_type === ct && e.content_id === cid &&
      (sf === undefined ? true : (e.season_filter ?? undefined) === sf))

  const addFiller = (params: { content_type: string; content_id: string; title: string; season_filter?: number | null; include_specials?: boolean }) => {
    const sf = params.season_filter ?? undefined
    if (isAdded(params.content_type, params.content_id, sf as number | undefined)) return
    store.addBlockFiller(channelId, {
      content_type: params.content_type as any,
      content_id:   params.content_id,
      title:        params.title,
      advancement:  'sized',
      weight:       1,
      season_filter: sf,
    })
  }

  const isLoading = (tab === 'shows' && showsLoading) || (tab === 'movies' && moviesLoading) || (tab === 'playlists' && playlistsLoading)

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

        {/* Left: tile browser */}
        <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
          {infoItem ? (
            <MediaInfoPanel
              item={infoItem}
              detail={infoDetail}
              seasons={infoSeasons}
              detailLoading={detailLoading}
              addLabel="ADD TO FILLER"
              onAdd={addFiller}
              onBack={() => setInfoItem(null)}
            />
          ) : (
            <>
              <div style={{ flexShrink: 0, padding: '10px 14px 0', borderBottom: '1px solid var(--hds-line-s)' }}>
                <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 7, padding: 3, width: 'fit-content', marginBottom: 8 }}>
                  {(['shows', 'movies', 'episodes', 'playlists'] as FillerTab[]).map(t => (
                    <button key={t} onClick={() => { setTab(t); setQuery(''); setSFilter(''); setInfoItem(null) }}
                      style={{ padding: '4px 12px', border: 'none', borderRadius: 5, background: tab === t ? 'var(--hds-violet)' : 'transparent', color: tab === t ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer', textTransform: 'capitalize' }}>
                      {t}
                    </button>
                  ))}
                </div>
                <div style={{ marginBottom: 10, display: 'flex', gap: 6 }}>
                  {tab !== 'playlists' && (
                    <input value={query} onChange={e => setQuery(e.target.value)} placeholder="Search…"
                      style={{ ...inputStyle, flex: 1, fontSize: 11.5, padding: '6px 9px' }} />
                  )}
                  {tab === 'playlists' && <div style={{ flex: 1 }} />}
                  {tab === 'episodes' && (
                    <input type="number" min={0} value={sFilter} onChange={e => setSFilter(e.target.value)}
                      placeholder="S#" title="Filter by season"
                      style={{ ...inputStyle, width: 52, fontSize: 11, padding: '6px 9px' }} />
                  )}
                  {(tab === 'movies' || tab === 'episodes' || tab === 'playlists') && (
                    <input type="number" min={1} value={maxDur} onChange={e => setMaxDur(e.target.value)}
                      placeholder="≤m" title="Max duration in minutes"
                      style={{ ...inputStyle, width: 60, fontSize: 11, padding: '6px 9px' }} />
                  )}
                </div>
              </div>
              <div style={{ flex: 1, overflow: 'auto' }} className="scrollbar-dark">
                {isLoading ? (
                  <div style={{ padding: '20px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
                ) : tab === 'shows' ? (
                  shows.length === 0 ? <BrowserEmpty /> : (
                    <>
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
                      {shows.length < showsTotal && <LoadMoreSentinel loading={showsLoadingMore} onVisible={loadMoreShows} />}
                    </>
                  )
                ) : tab === 'movies' ? (() => {
                  const maxMs = maxDur.trim() !== '' ? parseInt(maxDur) * 60_000 : undefined
                  const filtered = maxMs !== undefined ? movies.filter(m => m.duration_ms === 0 || m.duration_ms <= maxMs) : movies
                  return filtered.length === 0 ? <BrowserEmpty hint={movies.length === 0 ? undefined : 'No movies match the duration filter.'} /> : (
                    <>
                      <div style={gridStyle}>
                        {filtered.map(m => (
                          <MediaTile key={m.movie_id}
                            imgUrl={`/api/movies/${m.movie_id}/thumb`}
                            title={m.title}
                            sub={m.year ? String(m.year) : undefined}
                            badge={isAdded('movie', m.movie_id)}
                            onClick={() => setInfoItem({ kind: 'movie', id: m.movie_id, seed: m })}
                          />
                        ))}
                      </div>
                      {movies.length < moviesTotal && <LoadMoreSentinel loading={moviesLoadingMore} onVisible={loadMoreMovies} />}
                    </>
                  )
                })() : tab === 'episodes' ? (() => {
                  const maxMs = maxDur.trim() !== '' ? parseInt(maxDur) * 60_000 : undefined
                  const filtered = eps.filter(ep => maxMs === undefined || ep.duration_ms === 0 || ep.duration_ms <= maxMs)
                  return filtered.length === 0 ? <BrowserEmpty hint={eps.length === 0 ? 'Type to search episodes.' : 'No episodes match the filter.'} /> : (
                    <>
                      <div style={gridStyle}>
                        {filtered.map(ep => (
                          <MediaTile key={ep.episode_id}
                            imgUrl={`/api/shows/${ep.show_id}/thumb`}
                            title={`S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}`}
                            sub={ep.show_title}
                            badge={isAdded('episode', ep.episode_id)}
                            onClick={() => addFiller({
                              content_type: 'episode',
                              content_id: ep.episode_id,
                              title: `${ep.show_title} S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}`,
                            })}
                          />
                        ))}
                      </div>
                      {epsHasMore && <LoadMoreSentinel loading={epsLoadingMore} onVisible={loadMoreEps} />}
                    </>
                  )
                })() : (() => {
                  const maxMs = maxDur.trim() !== '' ? parseInt(maxDur) * 60_000 : undefined
                  const filtered = maxMs !== undefined ? playlists.filter(p => p.total_ms === 0 || p.total_ms <= maxMs) : playlists
                  return filtered.length === 0 ? <BrowserEmpty hint={playlists.length === 0 ? 'No playlists found.' : 'No playlists match the duration filter.'} /> : (
                    <div style={gridStyle}>
                      {filtered.map(p => (
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
                })()}
              </div>
            </>
          )}
        </div>

        {/* Right: filler entries */}
        <div style={{ flexShrink: 0, width: 380, borderLeft: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', background: 'oklch(0.15 0.018 288 / 0.55)', overflow: 'auto', padding: 18 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 15 }}>
            <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 11, letterSpacing: '0.14em', color: 'var(--hds-gold)' }}>BLOCK FILLER</span>
            <span style={{ fontSize: 11, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{entries.length}</span>
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
          className="hds-btn-gold"
        >Done</button>
      </div>
    </div>
  )
})

export default FillerOverlay
