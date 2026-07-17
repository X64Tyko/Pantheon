import { useState, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import type { FillerEntryAdvancement, Show, Movie, Playlist } from '../api/types'
import { FILLER_ADV_OPTS } from './constants'
import type { ChannelDetailStore } from './store'
import { api } from '../api/client'
import styles from './FillerPanel.module.css'
import shared from './sharedStyles.module.css'

// ─── Filler entry row ─────────────────────────────────────────────────────────

export function FillerEntryRow({ entry, showWeight, onAdvancement, onWeight, onRemove }: {
  entry:         { id: number; content_id?: string; title?: string; advancement: FillerEntryAdvancement; weight: number; season_filter?: number }
  showWeight:    boolean
  onAdvancement: (a: FillerEntryAdvancement) => void
  onWeight:      (w: number) => void
  onRemove:      () => void
}) {
  return (
    <div className={styles.entryRow}>
      <span className={`${styles.entryDot} ${styles.fillerDot}`} />
      <div className={styles.entryInfo}>
        <span className={styles.entryTitle}>{entry.title || entry.content_id}</span>
        {entry.season_filter !== undefined && (
          <span className={styles.entrySeasonTag}>
            S{String(entry.season_filter).padStart(2, '0')} only
          </span>
        )}
      </div>
      <select
        value={entry.advancement}
        onChange={e => onAdvancement(e.target.value as FillerEntryAdvancement)}
        className={`${shared.filterInput} ${styles.entryAdvancementSelect}`}
      >
        {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
      </select>
      {showWeight && (
        <input
          type="number" min={1} value={entry.weight}
          onChange={e => onWeight(Math.max(1, +e.target.value || 1))}
          className={`${shared.filterInput} ${styles.entryWeightInput}`}
          title="Weight"
        />
      )}
      <button onClick={onRemove} className={styles.entryRemoveBtn}>×</button>
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
      <div className={styles.pickerTabRow}>
        {(['shows', 'movies', 'playlists'] as FillerTab[]).map(t => (
          <button key={t} onClick={() => { setTab(t); setQuery('') }}
            className={`${styles.pickerTab} ${tab === t ? styles.pickerTabActive : ''}`}>
            {t}
          </button>
        ))}
      </div>
      {tab !== 'playlists' && (
        <input
          value={query}
          onChange={e => setQuery(e.target.value)}
          placeholder="Search…"
          className={`${shared.input} ${styles.pickerSearch}`}
        />
      )}
      <div className={`${styles.pickerListBox} scrollbar-dark`}>
        {loading ? (
          <div className={styles.pickerEmptyText}>Loading…</div>
        ) : items.length === 0 ? (
          <div className={styles.pickerEmptyText}>No results.</div>
        ) : items.map(item => (
          <div key={item.content_id}
            onClick={() => onSelect(item)}
            className={styles.pickerItemRow}
          >
            <span className={`${styles.entryDot} ${styles.fillerDot}`} />
            <span className={styles.pickerItemTitle}>{item.title}</span>
            <span className={styles.pickerItemPlus}>+</span>
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

  if (loading) return <span className={styles.seasonPickerLoading}>Loading seasons…</span>

  return (
    <div className={styles.seasonPickerRow}>
      <button className={`hds-season-btn ${styles.seasonBtn} ${selected === undefined ? styles.seasonBtnActive : ''}`} onClick={() => onSelect(undefined)}>All</button>
      {seasons.filter(s => s.number !== 0).map(s => (
        <button key={s.number} className={`hds-season-btn ${styles.seasonBtn} ${selected === s.number ? styles.seasonBtnActive : ''}`} onClick={() => onSelect(s.number)}>
          {s.name || `S${String(s.number).padStart(2, '0')}`}
        </button>
      ))}
      {seasons.some(s => s.number === 0) && (
        <button className={`hds-season-btn ${styles.seasonBtn} ${selected === 0 ? styles.seasonBtnActive : ''}`} onClick={() => onSelect(0)}>S00</button>
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
    <div className={styles.addPanelRoot}>
      {armed ? (
        <div>
          <div className={styles.armedRow}>
            <span className={styles.armedTitle}>
              <span className={styles.armedTitlePrefix}>Selected: </span>{armed.title}
              {seasonFilter !== undefined && <span className={styles.armedSeasonTag}> · S{String(seasonFilter).padStart(2,'0')}</span>}
            </span>
            <button onClick={() => { setArmed(null); setSeasonFilter(undefined) }}
              className={styles.armedClearBtn}>✕</button>
          </div>

          {armed.content_type === 'show' && (
            <div className={styles.seasonFilterBlock}>
              <div className={styles.seasonFilterLabel}>SEASON FILTER</div>
              <SeasonPicker showId={armed.content_id} selected={seasonFilter} onSelect={setSeasonFilter} />
            </div>
          )}

          <div className={styles.addRow}>
            <select value={advancement} onChange={e => setAdvancement(e.target.value as FillerEntryAdvancement)} className={`${shared.filterInput} ${styles.addAdvancementSelect}`}>
              {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
            {showWeight && (
              <input type="number" min={1} value={weight} onChange={e => setWeight(Math.max(1, +e.target.value || 1))} className={`${shared.filterInput} ${styles.addWeightInput}`} title="Weight" placeholder="Wt" />
            )}
            <button
              onClick={() => {
                store.addBlockFiller(channelId, { ...armed, advancement, weight, season_filter: armed.content_type === 'show' ? seasonFilter : undefined })
                setArmed(null)
                setSeasonFilter(undefined)
              }}
              disabled={store.fillerSaving}
              className={styles.addBtn}
            >{store.fillerSaving ? '…' : 'Add'}</button>
          </div>
        </div>
      ) : (
        <FillerContentPicker onSelect={handleSelect} />
      )}
    </div>
  )
})
