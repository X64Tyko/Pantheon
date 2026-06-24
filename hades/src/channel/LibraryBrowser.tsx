import { useState, useEffect, useRef } from 'react'
import { observer } from 'mobx-react-lite'
import { runInAction } from 'mobx'
import type { PickerTab } from './types'
import { availablePickerTabs } from './utils'
import { inputStyle } from './styles'
import { FilterSection } from '../components/PickerFilters'
import { api } from '../api/client'
import { imageQueue } from './imageQueue'
import { HelpTip, HelpSection, GifSlot } from './HelpTip'
import type { ChannelDetailStore } from './store'
import type { Show, Movie, ShowDetail, MovieDetail, EpisodeSearchResult, Playlist } from '../api/types'

const MAX_HOVER_SEASONS = 5

const TAB_LABELS: Record<PickerTab, string> = {
  shows: 'Shows', movies: 'Movies', episodes: 'Episodes', playlists: 'Playlists',
}

// Seed = minimal data already in the store from the picker query (no extra fetch needed).
type InfoItem =
  | { kind: 'show';     id: string; seed: Show }
  | { kind: 'movie';    id: string; seed: Movie }
  | { kind: 'episode';  ep: EpisodeSearchResult }
  | { kind: 'playlist'; pl: Playlist }

// ─── Main component ───────────────────────────────────────────────────────────

export const LibraryBrowser = observer(function LibraryBrowser({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const [infoItem,    setInfoItem]    = useState<InfoItem | null>(null)
  const [infoDetail,  setInfoDetail]  = useState<ShowDetail | MovieDetail | null>(null)
  const [infoSeasons, setInfoSeasons] = useState<number[]>([])
  const [detailLoading, setDetailLoading] = useState(false)

  const tabs = availablePickerTabs(store.draft.block_type)

  // Async-load rich detail after snapping to DB data immediately.
  useEffect(() => {
    setInfoDetail(null)
    setInfoSeasons([])
    if (!infoItem || (infoItem.kind !== 'show' && infoItem.kind !== 'movie')) {
      setDetailLoading(false)
      return
    }
    setDetailLoading(true)
    const ctrl = new AbortController()
    if (infoItem.kind === 'show') {
      Promise.all([api.getShow(infoItem.id), api.getShowSeasons(infoItem.id)])
        .then(([detail, { seasons }]) => {
          if (ctrl.signal.aborted) return
          setInfoDetail(detail)
          setInfoSeasons(seasons)
          setDetailLoading(false)
        })
        .catch(() => { if (!ctrl.signal.aborted) setDetailLoading(false) })
    } else {
      api.getMovie(infoItem.id)
        .then(d => { if (!ctrl.signal.aborted) { setInfoDetail(d); setDetailLoading(false) } })
        .catch(() => { if (!ctrl.signal.aborted) setDetailLoading(false) })
    }
    return () => ctrl.abort()
  }, [infoItem])

  const showFilters = !infoItem && (store.pickerTab === 'shows' || store.pickerTab === 'movies')
  const libType     = store.pickerTab === 'movies' ? 'movie' : 'show'
  const filteredLibs = store.allLibraries.filter(l => l.library_type === libType || l.library_type === 'mixed')

  if (tabs.length === 0) {
    return (
      <div style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'var(--hds-txt-3)', fontSize: 12, padding: 20, textAlign: 'center' }}>
        Use the Filler section to add filler lists to this block.
      </div>
    )
  }

  return (
    <div style={{ flex: 1, display: 'flex', flexDirection: 'column', minHeight: 0, background: 'oklch(0.13 0.015 286)' }}>
      {/* Header: tabs + search (hidden when info panel is open) */}
      {!infoItem && (
        <div style={{ flexShrink: 0, borderBottom: '1px solid var(--hds-line-s)' }}>
          <div style={{ padding: '10px 14px 8px' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}>
              <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 7, padding: 3 }}>
                {tabs.map(t => (
                  <button key={t} onClick={() => store.setPickerTab(t)}
                    style={{ padding: '4px 12px', border: 'none', borderRadius: 5, background: store.pickerTab === t ? 'var(--hds-violet)' : 'transparent', color: store.pickerTab === t ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer' }}>
                    {TAB_LABELS[t]}
                  </button>
                ))}
              </div>
              <HelpTip title="Using the Library Browser" tip="Drag, click, and filter your library" down>
                <HelpSection title="Drag to Add">
                  Grab any tile and drop it onto the CONTENT list in the left panel. A dashed violet drop zone appears when you hover over a valid target. Shows, movies, individual episodes, and playlists can all be dragged in.
                  <GifSlot label="Dragging a show tile onto the Content list" />
                </HelpSection>
                <HelpSection title="Click to Inspect">
                  Clicking a tile opens the info panel with the full synopsis, season list, runtime, and content rating. From there you can add the entire show, individual seasons, or just Season 00 (specials only).
                  <GifSlot label="Clicking a tile to open the info panel and add a specific season" />
                </HelpSection>
                <HelpSection title="Filters">
                  The Filters toggle (below the search bar) opens a rule builder. Add rules to filter by genre, year, content rating, network, library, and more. Rules combine with <b style={{ color: 'var(--hds-txt)' }}>ALL</b> (every rule must match) or <b style={{ color: 'var(--hds-txt)' }}>ANY</b> (at least one must match). Active rules are shown in violet on the Filters toggle.
                  <GifSlot label="Building filter rules to narrow the library by genre and year" />
                </HelpSection>
              </HelpTip>
            </div>
            {store.pickerTab !== 'playlists' && (
              <div style={{ display: 'flex', gap: 6 }}>
                <input value={store.pickerQuery} onChange={e => store.setPickerQuery(e.target.value)} placeholder="Search…" style={{ ...inputStyle, flex: 1, fontSize: 11.5, padding: '6px 9px' }} />
                {store.pickerTab === 'episodes' && (
                  <input type="number" min={0} value={store.pickerSeasonFilter} onChange={e => store.setPickerSeasonFilter(e.target.value)} placeholder="S#" title="Filter by season" style={{ ...inputStyle, width: 48, fontSize: 11, padding: '5px 6px' }} />
                )}
              </div>
            )}
          </div>
          {showFilters && (
            <FilterSection
              rulesOpen={store.filterRulesOpen}
              filterMatch={store.filterMatch}
              filterRules={store.filterRules}
              filteredLibs={filteredLibs}
              onToggleOpen={() => runInAction(() => { store.filterRulesOpen = !store.filterRulesOpen })}
              onSetMatch={m => store.setFilterMatch(m)}
              onAddRule={() => store.addFilterRule()}
              onUpdateRule={(id, patch) => store.updateFilterRule(id, patch)}
              onRemoveRule={id => store.removeFilterRule(id)}
            />
          )}
        </div>
      )}

      {infoItem ? (
        <MediaInfoPanel
          item={infoItem}
          detail={infoDetail}
          seasons={infoSeasons}
          detailLoading={detailLoading}
          channelId={channelId}
          store={store}
          onBack={() => setInfoItem(null)}
        />
      ) : (
        <TileGrid store={store} channelId={channelId} onSelect={setInfoItem} />
      )}
    </div>
  )
})

// ─── Tile grid ────────────────────────────────────────────────────────────────

const TileGrid = observer(function TileGrid({ store, channelId, onSelect }: { store: ChannelDetailStore; channelId: string; onSelect: (item: InfoItem) => void }) {
  if (store.pickerLoading) {
    return <div style={{ padding: '20px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
  }

  const startDrag = (e: React.DragEvent, content_type: 'show' | 'movie' | 'episode' | 'playlist', content_id: string, title: string) => {
    e.dataTransfer.effectAllowed = 'copy'
    runInAction(() => { store.dragContent = { content_type, content_id, title } })
  }
  const endDrag = () => runInAction(() => { store.dragContent = null })

  const gridStyle: React.CSSProperties = {
    display: 'grid',
    gridTemplateColumns: 'repeat(auto-fill, minmax(128px, 1fr))',
    gap: 10,
    padding: 14,
    alignContent: 'start',
  }

  if (store.pickerTab === 'shows') {
    const items = store.pickerShows
    if (items.length === 0) return <Empty />
    return (
      <div style={{ overflow: 'auto', flex: 1 }} className="scrollbar-dark">
        <div style={gridStyle}>
          {items.map(s => (
            <ShowTile key={s.show_id}
              show={s}
              store={store}
              channelId={channelId}
              onDragStart={e => startDrag(e, 'show', s.show_id, s.title)}
              onDragEnd={endDrag}
              onInfoOpen={() => onSelect({ kind: 'show', id: s.show_id, seed: s })}
            />
          ))}
        </div>
        {items.length < store.pickerTotal && (
          <LoadMoreSentinel loading={store.pickerLoadingMore} onVisible={() => store.loadMorePicker()} />
        )}
      </div>
    )
  }

  if (store.pickerTab === 'movies') {
    const items = store.pickerMovies
    if (items.length === 0) return <Empty />
    return (
      <div style={{ overflow: 'auto', flex: 1 }} className="scrollbar-dark">
        <div style={gridStyle}>
          {items.map(m => {
            const alreadyAdded = store.draftContent.some(c => c.content_type === 'movie' && c.content_id === m.movie_id)
            return (
              <Tile key={m.movie_id}
                imgUrl={`/api/movies/${m.movie_id}/thumb`}
                title={m.title}
                sub={m.year ? String(m.year) : undefined}
                badge={alreadyAdded}
                onDragStart={e => startDrag(e, 'movie', m.movie_id, m.title)}
                onDragEnd={endDrag}
                onClick={() => onSelect({ kind: 'movie', id: m.movie_id, seed: m })}
              />
            )
          })}
        </div>
        {items.length < store.pickerTotal && (
          <LoadMoreSentinel loading={store.pickerLoadingMore} onVisible={() => store.loadMorePicker()} />
        )}
      </div>
    )
  }

  if (store.pickerTab === 'episodes') {
    const items = store.pickerEpisodes
    if (items.length === 0) return <Empty hint="Type to search episodes." />
    return (
      <div style={{ ...gridStyle, overflow: 'auto', flex: 1 }} className="scrollbar-dark">
        {items.map(ep => {
          const code  = `S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')}`
          const title = `${ep.show_title} ${code} — ${ep.title}`
          return (
            <Tile key={ep.episode_id}
              imgUrl={`/api/shows/${ep.show_id}/thumb`}
              title={`${code} — ${ep.title}`}
              sub={ep.show_title}
              onDragStart={e => startDrag(e, 'episode', ep.episode_id, title)}
              onDragEnd={endDrag}
              onClick={() => onSelect({ kind: 'episode', ep })}
            />
          )
        })}
      </div>
    )
  }

  if (store.pickerTab === 'playlists') {
    const items = store.pickerPlaylists
    if (items.length === 0) return <Empty />
    return (
      <div style={{ ...gridStyle, overflow: 'auto', flex: 1 }} className="scrollbar-dark">
        {items.map(p => {
          const alreadyAdded = store.draftContent.some(c => c.content_type === 'playlist' && c.content_id === p.playlist_id)
          return (
            <Tile key={p.playlist_id}
              title={p.title}
              sub={`${p.item_count} items`}
              placeholder="☰"
              badge={alreadyAdded}
              onDragStart={e => startDrag(e, 'playlist', p.playlist_id, p.title)}
              onDragEnd={endDrag}
              onClick={() => onSelect({ kind: 'playlist', pl: p })}
            />
          )
        })}
      </div>
    )
  }

  return null
})

// ─── Tile card ────────────────────────────────────────────────────────────────

function Tile({ imgUrl, title, sub, placeholder, badge, onDragStart, onDragEnd, onClick }: {
  imgUrl?:      string
  title:        string
  sub?:         string
  placeholder?: string
  badge?:       boolean
  onDragStart:  (e: React.DragEvent) => void
  onDragEnd:    () => void
  onClick:      () => void
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
      style={{ cursor: 'pointer', borderRadius: 8, overflow: 'hidden', border: '1px solid var(--hds-line-s)', background: 'var(--hds-bg-2)', transition: 'border-color .1s' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-violet)'; scrollIn() }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line-s)'; scrollOut() }}
    >
      <div style={{ width: '100%', aspectRatio: '2/3', background: 'var(--hds-bg-3)', position: 'relative', overflow: 'hidden' }}>
        {imgUrl && (
          <img src={imgReady ? imgUrl : ''} alt=""
            style={{ position: 'absolute', inset: 0, width: '100%', height: '100%', objectFit: 'cover', opacity: imgReady ? 1 : 0, transition: 'opacity .2s' }} />
        )}
        {placeholder && !imgUrl && (
          <div style={{ position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 32, opacity: 0.3 }}>{placeholder}</div>
        )}
        {badge && (
          <div style={{ position: 'absolute', top: 5, right: 5, width: 18, height: 18, borderRadius: '50%', background: 'var(--hds-violet)', display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 9, color: '#fff', fontWeight: 700 }}>✓</div>
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

// ─── Show tile with hover season overlay ─────────────────────────────────────

function ShowTile({ show, store, channelId, onDragStart, onDragEnd, onInfoOpen }: {
  show:        Show
  store:       ChannelDetailStore
  channelId:   string
  onDragStart: (e: React.DragEvent) => void
  onDragEnd:   () => void
  onInfoOpen:  () => void
}) {
  const [imgReady,        setImgReady]        = useState(false)
  const [hovering,        setHovering]        = useState(false)
  const [seasons,         setSeasons]         = useState<number[] | null>(null)
  const [seasonsLoading,  setSeasonsLoading]  = useState(false)
  const titleRef = useRef<HTMLSpanElement>(null)
  const imgUrl   = `/api/shows/${show.show_id}/thumb`

  const alreadyAdded = store.draftContent.some(c => c.content_type === 'show' && c.content_id === show.show_id)

  useEffect(() => {
    setImgReady(false)
    const ctrl = new AbortController()
    imageQueue.load(imgUrl, ctrl.signal).then(() => setImgReady(true)).catch(() => {})
    return () => ctrl.abort()
  }, [imgUrl])

  const onMouseEnter = () => {
    setHovering(true)
    if (titleRef.current) { const ov = titleRef.current.scrollHeight - 30; if (ov > 0) titleRef.current.style.transform = `translateY(-${ov}px)` }
    if (seasons === null && !seasonsLoading) {
      setSeasonsLoading(true)
      api.getShowSeasons(show.show_id)
        .then(({ seasons: s }) => setSeasons(s))
        .catch(() => setSeasons([]))
        .finally(() => setSeasonsLoading(false))
    }
  }
  const onMouseLeave = () => {
    setHovering(false)
    if (titleRef.current) titleRef.current.style.transform = ''
  }

  const add = (e: React.MouseEvent, season_filter: number | null, title: string, include_specials = false) => {
    e.stopPropagation()
    store.addContent(channelId, { content_type: 'show', content_id: show.show_id, season_filter, title, include_specials: include_specials || season_filter === null || season_filter === 0 })
  }

  const nonSpecial = (seasons ?? []).filter(s => s !== 0)
  const hasSpecials = (seasons ?? []).includes(0)
  const visible     = nonSpecial.slice(0, MAX_HOVER_SEASONS)
  const hasMore     = nonSpecial.length > MAX_HOVER_SEASONS

  return (
    <div
      draggable
      onDragStart={onDragStart}
      onDragEnd={onDragEnd}
      onClick={onInfoOpen}
      style={{ cursor: 'pointer', borderRadius: 8, overflow: 'hidden', border: `1px solid ${hovering ? 'var(--hds-violet)' : 'var(--hds-line-s)'}`, background: 'var(--hds-bg-2)', transition: 'border-color .1s', position: 'relative' }}
      onMouseEnter={onMouseEnter}
      onMouseLeave={onMouseLeave}
    >
      <div style={{ width: '100%', aspectRatio: '2/3', background: 'var(--hds-bg-3)', position: 'relative', overflow: 'hidden' }}>
        <img src={imgReady ? imgUrl : ''} alt=""
          style={{ position: 'absolute', inset: 0, width: '100%', height: '100%', objectFit: 'cover', opacity: imgReady ? 1 : 0, transition: 'opacity .2s' }} />

        {alreadyAdded && !hovering && (
          <div style={{ position: 'absolute', top: 5, right: 5, width: 18, height: 18, borderRadius: '50%', background: 'var(--hds-violet)', display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 9, color: '#fff', fontWeight: 700 }}>✓</div>
        )}

        {hovering && (
          <div
            style={{ position: 'absolute', inset: 0, background: 'oklch(0.1 0.02 286 / 0.9)', backdropFilter: 'blur(3px)', display: 'flex', flexDirection: 'column', alignItems: 'stretch', justifyContent: 'center', gap: 3, padding: '8px 6px' }}
            onClick={e => e.stopPropagation()}
          >
            {seasonsLoading ? (
              <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', textAlign: 'center', fontFamily: "'JetBrains Mono', monospace" }}>loading…</span>
            ) : seasons !== null ? (
              <>
                <SeasonBtn onClick={e => add(e, null, show.title, true)}>All</SeasonBtn>
                {hasSpecials && <SeasonBtn gold onClick={e => add(e, 0, `${show.title} S00`, true)}>S00</SeasonBtn>}
                {visible.map(s => (
                  <SeasonBtn key={s} onClick={e => add(e, s, `${show.title} S${String(s).padStart(2,'0')}`)}>S{String(s).padStart(2,'0')}</SeasonBtn>
                ))}
                {hasMore && (
                  <button
                    onClick={e => { e.stopPropagation(); onInfoOpen() }}
                    style={{ padding: '2px 4px', border: 'none', borderRadius: 4, background: 'transparent', color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", fontSize: 8.5, cursor: 'pointer', textAlign: 'center' }}
                  >view all →</button>
                )}
              </>
            ) : null}
          </div>
        )}
      </div>
      <div style={{ padding: '5px 7px 7px' }}>
        <div style={{ fontSize: 11, fontWeight: 600, lineHeight: 1.35, height: 30, overflow: 'hidden' }}>
          <span ref={titleRef} style={{ display: 'block', transition: 'transform 0.35s ease' }}>{show.title}</span>
        </div>
        {show.year && <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 2 }}>{show.year}</div>}
      </div>
    </div>
  )
}

function SeasonBtn({ onClick, gold, children }: { onClick: (e: React.MouseEvent) => void; gold?: boolean; children: React.ReactNode }) {
  return (
    <button
      onClick={onClick}
      style={{ padding: '2px 4px', border: `1px solid ${gold ? 'oklch(0.55 0.12 58)' : 'var(--hds-line)'}`, borderRadius: 4, background: 'transparent', color: gold ? 'oklch(0.75 0.12 58)' : 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 9.5, cursor: 'pointer', textAlign: 'center', width: '100%' }}
    >{children}</button>
  )
}

// ─── Media info panel ─────────────────────────────────────────────────────────

function MediaInfoPanel({ item, detail, seasons, detailLoading, channelId, store, onBack }: {
  item:          InfoItem
  detail:        ShowDetail | MovieDetail | null
  seasons:       number[]
  detailLoading: boolean
  channelId:     string
  store:         ChannelDetailStore
  onBack:        () => void
}) {
  const add = (content: Parameters<typeof store.addContent>[1]) => {
    store.addContent(channelId, content)
    onBack()
  }

  const fmtMs = (ms: number) => {
    const m = Math.round(ms / 60000)
    const h = Math.floor(m / 60)
    return h > 0 ? `${h}h ${m % 60}m` : `${m}m`
  }

  return (
    <div style={{ flex: 1, display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      <div style={{ padding: '10px 14px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0, display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <button onClick={onBack} style={{ background: 'transparent', border: 'none', cursor: 'pointer', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, padding: 0 }}>
          ← Back
        </button>
        {detailLoading && <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', opacity: 0.7 }}>loading details…</span>}
      </div>

      <div style={{ flex: 1, overflow: 'auto', padding: '14px 14px 20px' }} className="scrollbar-dark">

        {/* ─ Show ─ */}
        {item.kind === 'show' && (() => {
          const s  = item.seed
          const d  = detail as ShowDetail | null
          return (
            <>
              {d?.art && <Backdrop url={d.art} />}
              <div style={{ display: 'flex', gap: 12, marginBottom: 12, alignItems: 'flex-start' }}>
                <ThumbSlot url={d?.thumb ?? `/api/shows/${s.show_id}/thumb`} />
                <div style={{ minWidth: 0 }}>
                  <div style={{ fontSize: 15, fontWeight: 700, lineHeight: 1.3, marginBottom: 5 }}>{s.title}</div>
                  {s.year           && <div style={{ fontSize: 11, color: 'var(--hds-txt-3)', marginBottom: 3 }}>{s.year}</div>}
                  {s.content_rating && s.content_rating !== '' && <RatingBadge rating={s.content_rating} />}
                  {d?.genres        && d.genres.length > 0 && <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 3, marginBottom: 3 }}>{d.genres.join(', ')}</div>}
                  <div style={{ fontSize: 10.5, color: 'var(--hds-txt-2)', marginTop: 3 }}>
                    {s.episode_count} episode{s.episode_count !== 1 ? 's' : ''}
                    {d?.status ? ` · ${d.status}` : ''}
                  </div>
                </div>
              </div>

              {d?.overview
                ? <Overview text={d.overview} />
                : detailLoading && <OverviewSkeleton />
              }

              <div style={{ marginTop: 12 }}>
                <SectionLabel>ADD TO BLOCK</SectionLabel>
                <div style={{ display: 'flex', flexWrap: 'wrap', gap: 6, marginTop: 6 }}>
                  <AddBtn onClick={() => add({ content_type: 'show', content_id: s.show_id, season_filter: null, title: s.title, include_specials: true })}>Add All</AddBtn>
                  {seasons.includes(0) && <>
                    <AddBtn onClick={() => add({ content_type: 'show', content_id: s.show_id, season_filter: null, title: s.title, include_specials: false })}>No S00</AddBtn>
                    <AddBtn gold onClick={() => add({ content_type: 'show', content_id: s.show_id, season_filter: 0, title: `${s.title} S00`, include_specials: true })}>S00</AddBtn>
                  </>}
                  {seasons.filter(n => n !== 0).map(n => (
                    <AddBtn key={n} onClick={() => add({ content_type: 'show', content_id: s.show_id, season_filter: n, title: `${s.title} S${String(n).padStart(2,'0')}` })}>
                      S{String(n).padStart(2,'0')}
                    </AddBtn>
                  ))}
                  {detailLoading && seasons.length === 0 && (
                    <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', alignSelf: 'center' }}>loading seasons…</span>
                  )}
                </div>
              </div>
            </>
          )
        })()}

        {/* ─ Movie ─ */}
        {item.kind === 'movie' && (() => {
          const m = item.seed
          const d = detail as MovieDetail | null
          return (
            <>
              {d?.art && <Backdrop url={d.art} />}
              <div style={{ display: 'flex', gap: 12, marginBottom: 12, alignItems: 'flex-start' }}>
                <ThumbSlot url={d?.thumb ?? `/api/movies/${m.movie_id}/thumb`} />
                <div style={{ minWidth: 0 }}>
                  <div style={{ fontSize: 15, fontWeight: 700, lineHeight: 1.3, marginBottom: 5 }}>{m.title}</div>
                  {m.year           && <div style={{ fontSize: 11, color: 'var(--hds-txt-3)', marginBottom: 3 }}>{m.year}</div>}
                  {m.content_rating && m.content_rating !== '' && <RatingBadge rating={m.content_rating} />}
                  {d?.genres        && d.genres.length > 0 && <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 3, marginBottom: 3 }}>{d.genres.join(', ')}</div>}
                  <div style={{ fontSize: 10.5, color: 'var(--hds-txt-2)', marginTop: 3 }}>
                    {fmtMs(m.duration_ms)}
                    {d?.director ? ` · Dir. ${d.director}` : ''}
                  </div>
                </div>
              </div>

              {d?.tagline && <p style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontStyle: 'italic', margin: '0 0 8px' }}>{d.tagline}</p>}
              {d?.overview
                ? <Overview text={d.overview} />
                : detailLoading && <OverviewSkeleton />
              }

              <div style={{ marginTop: 12 }}>
                <SectionLabel>ADD TO BLOCK</SectionLabel>
                <div style={{ marginTop: 6 }}>
                  <AddBtn onClick={() => add({ content_type: 'movie', content_id: m.movie_id, title: m.title })}>Add Movie</AddBtn>
                </div>
              </div>
            </>
          )
        })()}

        {/* ─ Episode ─ */}
        {item.kind === 'episode' && (() => {
          const ep    = item.ep
          const code  = `S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')}`
          const title = `${ep.show_title} ${code} — ${ep.title}`
          return (
            <>
              <div style={{ fontSize: 11, color: 'var(--hds-txt-3)', marginBottom: 3 }}>{ep.show_title}</div>
              <div style={{ fontSize: 15, fontWeight: 700, lineHeight: 1.3, marginBottom: 8 }}>{code} — {ep.title}</div>
              {ep.duration_ms > 0 && <div style={{ fontSize: 10.5, color: 'var(--hds-txt-3)', marginBottom: 14 }}>{fmtMs(ep.duration_ms)}</div>}
              <div style={{ marginTop: 4 }}>
                <SectionLabel>ADD TO BLOCK</SectionLabel>
                <div style={{ marginTop: 6 }}>
                  <AddBtn onClick={() => add({ content_type: 'episode', content_id: ep.episode_id, title })}>Add Episode</AddBtn>
                </div>
              </div>
            </>
          )
        })()}

        {/* ─ Playlist ─ */}
        {item.kind === 'playlist' && (() => {
          const pl = item.pl
          return (
            <>
              <div style={{ fontSize: 15, fontWeight: 700, lineHeight: 1.3, marginBottom: 6 }}>{pl.title}</div>
              <div style={{ fontSize: 10.5, color: 'var(--hds-txt-3)', marginBottom: 3 }}>{pl.item_count} items · {pl.mode === 'show_collection' ? 'Show Collection' : 'In-Order'}</div>
              {pl.total_ms > 0 && <div style={{ fontSize: 10.5, color: 'var(--hds-txt-3)', marginBottom: 14 }}>{fmtMs(pl.total_ms)} total</div>}
              <div style={{ marginTop: 4 }}>
                <SectionLabel>ADD TO BLOCK</SectionLabel>
                <div style={{ marginTop: 6 }}>
                  <AddBtn onClick={() => add({ content_type: 'playlist', content_id: pl.playlist_id, title: pl.title })}>Add Playlist</AddBtn>
                </div>
              </div>
            </>
          )
        })()}

      </div>
    </div>
  )
}

// ─── Small sub-components ─────────────────────────────────────────────────────

function Backdrop({ url }: { url: string }) {
  return <img src={url} alt="" style={{ width: '100%', height: 130, objectFit: 'cover', borderRadius: 8, marginBottom: 12, display: 'block' }} onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
}

function ThumbSlot({ url }: { url: string }) {
  const [ready, setReady] = useState(false)
  return (
    <div style={{ width: 72, height: 108, borderRadius: 6, flexShrink: 0, background: 'var(--hds-bg-3)', overflow: 'hidden', position: 'relative' }}>
      <img src={url} alt="" style={{ width: '100%', height: '100%', objectFit: 'cover', opacity: ready ? 1 : 0, transition: 'opacity .2s', display: 'block' }}
        onLoad={() => setReady(true)} onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
    </div>
  )
}

function RatingBadge({ rating }: { rating: string }) {
  return <span style={{ fontSize: 10, padding: '1px 5px', border: '1px solid var(--hds-line)', borderRadius: 3, color: 'var(--hds-txt-3)', display: 'inline-block', marginBottom: 4 }}>{rating}</span>
}

function Overview({ text }: { text: string }) {
  return <p style={{ fontSize: 11.5, color: 'var(--hds-txt-2)', lineHeight: 1.65, margin: '0 0 14px' }}>{text}</p>
}

function OverviewSkeleton() {
  return (
    <div style={{ marginBottom: 14 }}>
      {[100, 92, 85, 60].map((w, i) => (
        <div key={i} style={{ height: 10, borderRadius: 3, background: 'var(--hds-bg-3)', marginBottom: 6, width: `${w}%`, opacity: 0.6 }} />
      ))}
    </div>
  )
}

function SectionLabel({ children }: { children: React.ReactNode }) {
  return <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)' }}>{children}</div>
}

function AddBtn({ onClick, gold, children }: { onClick: () => void; gold?: boolean; children: React.ReactNode }) {
  return (
    <button onClick={onClick} style={{ padding: '4px 10px', borderRadius: 5, border: `1px solid ${gold ? 'oklch(0.55 0.12 58)' : 'var(--hds-line)'}`, background: 'transparent', color: gold ? 'oklch(0.75 0.12 58)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer' }}>
      {children}
    </button>
  )
}

function LoadMoreSentinel({ loading, onVisible }: { loading: boolean; onVisible: () => void }) {
  const ref   = useRef<HTMLDivElement>(null)
  const cbRef = useRef(onVisible)
  useEffect(() => { cbRef.current = onVisible })
  useEffect(() => {
    const el = ref.current
    if (!el) return
    const obs = new IntersectionObserver(
      ([entry]) => { if (entry.isIntersecting) cbRef.current() },
      { rootMargin: '120px' }
    )
    obs.observe(el)
    return () => obs.disconnect()
  }, [])
  return (
    <div ref={ref} style={{ padding: '10px 0 14px', textAlign: 'center', fontSize: 11, color: 'var(--hds-txt-3)' }}>
      {loading ? 'Loading…' : ''}
    </div>
  )
}

function Empty({ hint }: { hint?: string }) {
  return <div style={{ padding: '20px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>{hint ?? 'No results.'}</div>
}
