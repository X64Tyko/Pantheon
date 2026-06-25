import { useState, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import type { FillerEntryAdvancement, Show, Movie, Playlist } from '../api/types'
import { BLOCK_META, FILLER_ADV_OPTS } from './constants'
import { filterInputStyle, inputStyle } from './styles'
import type { ChannelDetailStore } from './store'
import { api } from '../api/client'

// ─── Filler entry row ─────────────────────────────────────────────────────────

export function FillerEntryRow({ entry, showWeight, onAdvancement, onWeight, onRemove }: {
  entry:         { id: number; content_id?: string; title?: string; advancement: FillerEntryAdvancement; weight: number; season_filter?: number }
  showWeight:    boolean
  onAdvancement: (a: FillerEntryAdvancement) => void
  onWeight:      (w: number) => void
  onRemove:      () => void
}) {
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '8px 10px', background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)', borderRadius: 8 }}>
      <span style={{ width: 7, height: 7, borderRadius: 2, background: BLOCK_META.filler.edge, flexShrink: 0 }} />
      <div style={{ flex: 1, minWidth: 0 }}>
        <span style={{ fontSize: 12, fontWeight: 500, display: 'block', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{entry.title || entry.content_id}</span>
        {entry.season_filter !== undefined && (
          <span style={{ fontSize: 9.5, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>
            S{String(entry.season_filter).padStart(2, '0')} only
          </span>
        )}
      </div>
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

// ─── Shared mini content picker for filler ────────────────────────────────────

type FillerTab  = 'shows' | 'movies' | 'playlists'
type FillerItem = { content_type: 'show' | 'movie' | 'playlist'; content_id: string; title: string; season_filter?: number }

export function FillerContentPicker({ onSelect }: { onSelect: (item: FillerItem) => void }) {
  const [tab,      setTab]      = useState<FillerTab>('shows')
  const [query,    setQuery]    = useState('')
  const [shows,    setShows]    = useState<Show[]>([])
  const [movies,   setMovies]   = useState<Movie[]>([])
  const [lists,    setLists]    = useState<Playlist[]>([])
  const [loading,  setLoading]  = useState(false)

  useEffect(() => {
    const ctrl = new AbortController()
    setLoading(true)
    if (tab === 'shows') {
      api.getShows({ limit: 100, q: query || undefined })
        .then(r => { if (!ctrl.signal.aborted) { setShows(r.items); setLoading(false) } })
        .catch(() => { if (!ctrl.signal.aborted) setLoading(false) })
    } else if (tab === 'movies') {
      api.getMovies({ limit: 100, q: query || undefined })
        .then(r => { if (!ctrl.signal.aborted) { setMovies(r.items); setLoading(false) } })
        .catch(() => { if (!ctrl.signal.aborted) setLoading(false) })
    } else {
      api.getPlaylists()
        .then(r => { if (!ctrl.signal.aborted) { setLists(r); setLoading(false) } })
        .catch(() => { if (!ctrl.signal.aborted) setLoading(false) })
    }
    return () => ctrl.abort()
  }, [tab, query])

  const items: FillerItem[] = tab === 'shows'
    ? shows.map(s  => ({ content_type: 'show'     as const, content_id: s.show_id,     title: s.title }))
    : tab === 'movies'
    ? movies.map(m => ({ content_type: 'movie'    as const, content_id: m.movie_id,    title: m.title }))
    : lists.map(p  => ({ content_type: 'playlist' as const, content_id: p.playlist_id, title: p.title }))

  return (
    <div>
      <div style={{ display: 'flex', gap: 3, marginBottom: 6 }}>
        {(['shows', 'movies', 'playlists'] as FillerTab[]).map(t => (
          <button key={t} onClick={() => { setTab(t); setQuery('') }}
            style={{ padding: '3px 8px', border: 'none', borderRadius: 4, background: tab === t ? 'var(--hds-violet)' : 'var(--hds-bg-3)', color: tab === t ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 9.5, cursor: 'pointer', textTransform: 'capitalize' }}>
            {t}
          </button>
        ))}
      </div>
      {tab !== 'playlists' && (
        <input
          value={query}
          onChange={e => setQuery(e.target.value)}
          placeholder="Search…"
          style={{ ...inputStyle, width: '100%', fontSize: 11, padding: '5px 8px', marginBottom: 5, boxSizing: 'border-box' as const }}
        />
      )}
      <div style={{ maxHeight: 140, overflow: 'auto', border: '1px solid var(--hds-line-s)', borderRadius: 7 }} className="scrollbar-dark">
        {loading ? (
          <div style={{ padding: '10px 12px', fontSize: 11, color: 'var(--hds-txt-3)' }}>Loading…</div>
        ) : items.length === 0 ? (
          <div style={{ padding: '10px 12px', fontSize: 11, color: 'var(--hds-txt-3)' }}>No results.</div>
        ) : items.map(item => (
          <div key={item.content_id}
            onClick={() => onSelect(item)}
            style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '6px 10px', cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)' }}
            onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)'}
            onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.background = ''}
          >
            <span style={{ width: 6, height: 6, borderRadius: 2, background: BLOCK_META.filler.edge, flexShrink: 0 }} />
            <span style={{ fontSize: 11.5, flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{item.title}</span>
            <span style={{ fontSize: 9.5, color: 'var(--hds-violet)' }}>+</span>
          </div>
        ))}
      </div>
    </div>
  )
}

// ─── Season picker (inline, compact) ─────────────────────────────────────────

function SeasonPicker({ showId, selected, onSelect }: {
  showId:   string
  selected: number | undefined
  onSelect: (sf: number | undefined) => void
}) {
  const [seasons,  setSeasons]  = useState<{number: number; name: string}[]>([])
  const [loading,  setLoading]  = useState(true)

  useEffect(() => {
    setLoading(true)
    api.getShowSeasons(showId)
      .then(r => setSeasons(r.seasons))
      .catch(() => {})
      .finally(() => setLoading(false))
  }, [showId])

  const btnStyle = (active: boolean): React.CSSProperties => ({
    padding: '2px 6px', borderRadius: 4,
    border: `1px solid ${active ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
    background: active ? 'oklch(0.55 0.14 292 / 0.18)' : 'transparent',
    color: active ? 'var(--hds-violet)' : 'var(--hds-txt-3)',
    fontFamily: "'JetBrains Mono', monospace", fontSize: 9.5, cursor: 'pointer',
  })

  if (loading) return <span style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>Loading seasons…</span>

  return (
    <div style={{ display: 'flex', flexWrap: 'wrap', gap: 4 }}>
      <button className="hds-season-btn" style={btnStyle(selected === undefined)} onClick={() => onSelect(undefined)}>All</button>
      {seasons.filter(s => s.number !== 0).map(s => (
        <button key={s.number} className="hds-season-btn" style={btnStyle(selected === s.number)} onClick={() => onSelect(s.number)}>
          {s.name || `S${String(s.number).padStart(2, '0')}`}
        </button>
      ))}
      {seasons.some(s => s.number === 0) && (
        <button className="hds-season-btn" style={btnStyle(selected === 0)} onClick={() => onSelect(0)}>S00</button>
      )}
    </div>
  )
}

// ─── Filler add panel (sidebar block editor) ──────────────────────────────────

export const FillerAddPanel = observer(function FillerAddPanel({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const [armed,        setArmed]        = useState<FillerItem | null>(null)
  const [seasonFilter, setSeasonFilter] = useState<number | undefined>(undefined)
  const [advancement,  setAdvancement]  = useState<FillerEntryAdvancement>('sized')
  const [weight,       setWeight]       = useState(1)
  const showWeight = store.draft.filler_selection === 'weighted'

  const handleSelect = (item: FillerItem) => {
    setArmed(item)
    setSeasonFilter(undefined)
  }

  return (
    <div style={{ marginTop: 10, padding: '11px 12px', background: 'oklch(0.16 0.016 286)', border: '1px solid var(--hds-line)', borderRadius: 9 }}>
      {armed ? (
        <div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}>
            <span style={{ fontSize: 11, color: 'var(--hds-txt-2)', flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
              <span style={{ color: 'var(--hds-txt-3)' }}>Selected: </span>{armed.title}
              {seasonFilter !== undefined && <span style={{ color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}> · S{String(seasonFilter).padStart(2,'0')}</span>}
            </span>
            <button onClick={() => { setArmed(null); setSeasonFilter(undefined) }}
              style={{ width: 18, height: 18, border: 'none', borderRadius: 4, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 12, flexShrink: 0 }}>✕</button>
          </div>

          {armed.content_type === 'show' && (
            <div style={{ marginBottom: 8 }}>
              <div style={{ fontSize: 9, letterSpacing: '0.14em', color: 'var(--hds-txt-3)', marginBottom: 4, fontFamily: "'JetBrains Mono', monospace" }}>SEASON FILTER</div>
              <SeasonPicker showId={armed.content_id} selected={seasonFilter} onSelect={setSeasonFilter} />
            </div>
          )}

          <div style={{ display: 'flex', gap: 6, alignItems: 'center', flexWrap: 'wrap' as const }}>
            <select value={advancement} onChange={e => setAdvancement(e.target.value as FillerEntryAdvancement)} style={{ ...filterInputStyle, flex: 1 }}>
              {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
            {showWeight && (
              <input type="number" min={1} value={weight} onChange={e => setWeight(Math.max(1, +e.target.value || 1))} style={{ ...filterInputStyle, width: 48 }} title="Weight" placeholder="Wt" />
            )}
            <button
              onClick={() => {
                store.addBlockFiller(channelId, { ...armed, advancement, weight, season_filter: armed.content_type === 'show' ? seasonFilter : undefined })
                setArmed(null)
                setSeasonFilter(undefined)
              }}
              disabled={store.fillerSaving}
              style={{ padding: '5px 12px', border: 'none', borderRadius: 6, background: 'var(--hds-violet)', color: 'oklch(0.15 0.02 286)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700, cursor: 'pointer' }}
            >{store.fillerSaving ? '…' : 'Add'}</button>
          </div>
        </div>
      ) : (
        <FillerContentPicker onSelect={handleSelect} />
      )}
    </div>
  )
})
