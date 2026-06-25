import { useState, useEffect, useRef } from 'react'
import { api } from '../api/client'
import { imageQueue } from './imageQueue'
import type { Show, Movie, ShowDetail, MovieDetail, EpisodeSearchResult, Playlist } from '../api/types'

const MAX_HOVER_SEASONS = 5

const seasonLabel = (s: {number: number; name: string}) =>
  s.name || `S${String(s.number).padStart(2, '0')}`

// ─── Shared types ─────────────────────────────────────────────────────────────

export type AddContentParams = {
  content_type: 'show' | 'movie' | 'episode' | 'playlist'
  content_id:   string
  title:        string
  season_filter?:    number | null
  include_specials?: boolean
}

export type InfoItem =
  | { kind: 'show';     id: string; seed: Show }
  | { kind: 'movie';    id: string; seed: Movie }
  | { kind: 'episode';  ep: EpisodeSearchResult }
  | { kind: 'playlist'; pl: Playlist }

// ─── Basic tile card ──────────────────────────────────────────────────────────

export function MediaTile({ imgUrl, title, sub, placeholder, badge, onDragStart, onDragEnd, onClick }: {
  imgUrl?:      string
  title:        string
  sub?:         string
  placeholder?: string
  badge?:       boolean
  onDragStart?: (e: React.DragEvent) => void
  onDragEnd?:   () => void
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
      draggable={!!onDragStart}
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

export function ShowMediaTile({ show, onAdd, onInfoOpen, onDragStart, onDragEnd, isAdded }: {
  show:         Show
  onAdd:        (params: AddContentParams) => void
  onInfoOpen:   () => void
  onDragStart?: (e: React.DragEvent) => void
  onDragEnd?:   () => void
  isAdded?:     boolean
}) {
  const [imgReady,       setImgReady]       = useState(false)
  const [hovering,       setHovering]       = useState(false)
  const [seasons,        setSeasons]        = useState<{number: number; name: string}[] | null>(null)
  const [seasonsLoading, setSeasonsLoading] = useState(false)
  const titleRef = useRef<HTMLSpanElement>(null)
  const imgUrl   = `/api/shows/${show.show_id}/thumb`

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
    onAdd({ content_type: 'show', content_id: show.show_id, season_filter, title, include_specials: include_specials || season_filter === null || season_filter === 0 })
  }

  const nonSpecial = (seasons ?? []).filter(s => s.number !== 0)
  const hasSpecials = (seasons ?? []).some(s => s.number === 0)
  const visible    = nonSpecial.slice(0, MAX_HOVER_SEASONS)
  const hasMore    = nonSpecial.length > MAX_HOVER_SEASONS

  return (
    <div
      draggable={!!onDragStart}
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

        {isAdded && !hovering && (
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
                <HoverSeasonBtn onClick={e => add(e, null, show.title, true)}>All</HoverSeasonBtn>
                {hasSpecials && <HoverSeasonBtn gold onClick={e => add(e, 0, `${show.title} S00`, true)}>S00</HoverSeasonBtn>}
                {visible.map(s => (
                  <HoverSeasonBtn key={s.number} onClick={e => add(e, s.number, `${show.title} ${seasonLabel(s)}`)}>
                    {seasonLabel(s)}
                  </HoverSeasonBtn>
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

function HoverSeasonBtn({ onClick, gold, children }: { onClick: (e: React.MouseEvent) => void; gold?: boolean; children: React.ReactNode }) {
  return (
    <button
      onClick={onClick}
      className="hds-season-btn"
      style={{ padding: '2px 4px', border: `1px solid ${gold ? 'oklch(0.55 0.12 58)' : 'var(--hds-line)'}`, borderRadius: 4, background: 'transparent', color: gold ? 'oklch(0.75 0.12 58)' : 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 9.5, cursor: 'pointer', textAlign: 'center', width: '100%' }}
    >{children}</button>
  )
}

// ─── Media info detail panel ──────────────────────────────────────────────────

export function MediaInfoPanel({ item, detail, seasons, detailLoading, onAdd, onBack, addLabel = 'ADD TO BLOCK', renderAdd }: {
  item:          InfoItem
  detail:        ShowDetail | MovieDetail | null
  seasons:       {number: number; name: string}[]
  detailLoading: boolean
  onAdd:         (params: AddContentParams) => void
  onBack:        () => void
  addLabel?:     string
  renderAdd?:    (item: InfoItem, seasons: {number: number; name: string}[], onAdd: (params: AddContentParams) => void) => React.ReactNode
}) {
  const add = (params: AddContentParams) => { onAdd(params); onBack() }

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

        {item.kind === 'show' && (() => {
          const s = item.seed
          const d = detail as ShowDetail | null
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

              {d?.overview ? <Overview text={d.overview} /> : detailLoading && <OverviewSkeleton />}

              <div style={{ marginTop: 12 }}>
                <DetailSectionLabel>{addLabel}</DetailSectionLabel>
                {renderAdd ? renderAdd(item, seasons, add) : (
                  <div style={{ display: 'flex', flexWrap: 'wrap', gap: 6, marginTop: 6 }}>
                    <AddBtn onClick={() => add({ content_type: 'show', content_id: s.show_id, season_filter: null, title: s.title, include_specials: true })}>Add All</AddBtn>
                    {seasons.some(sn => sn.number === 0) && <>
                      <AddBtn onClick={() => add({ content_type: 'show', content_id: s.show_id, season_filter: null, title: s.title, include_specials: false })}>No S00</AddBtn>
                      <AddBtn gold onClick={() => add({ content_type: 'show', content_id: s.show_id, season_filter: 0, title: `${s.title} S00`, include_specials: true })}>S00</AddBtn>
                    </>}
                    {seasons.filter(sn => sn.number !== 0).map(sn => (
                      <AddBtn key={sn.number} onClick={() => add({ content_type: 'show', content_id: s.show_id, season_filter: sn.number, title: `${s.title} ${seasonLabel(sn)}` })}>
                        {seasonLabel(sn)}
                      </AddBtn>
                    ))}
                    {detailLoading && seasons.length === 0 && (
                      <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', alignSelf: 'center' }}>loading seasons…</span>
                    )}
                  </div>
                )}
              </div>
            </>
          )
        })()}

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
              {d?.overview ? <Overview text={d.overview} /> : detailLoading && <OverviewSkeleton />}

              <div style={{ marginTop: 12 }}>
                <DetailSectionLabel>{addLabel}</DetailSectionLabel>
                {renderAdd ? renderAdd(item, [], add) : (
                  <div style={{ marginTop: 6 }}>
                    <AddBtn onClick={() => add({ content_type: 'movie', content_id: m.movie_id, title: m.title })}>Add Movie</AddBtn>
                  </div>
                )}
              </div>
            </>
          )
        })()}

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
                <DetailSectionLabel>{addLabel}</DetailSectionLabel>
                {renderAdd ? renderAdd(item, [], add) : (
                  <div style={{ marginTop: 6 }}>
                    <AddBtn onClick={() => add({ content_type: 'episode', content_id: ep.episode_id, title })}>Add Episode</AddBtn>
                  </div>
                )}
              </div>
            </>
          )
        })()}

        {item.kind === 'playlist' && (() => {
          const pl = item.pl
          return (
            <>
              <div style={{ fontSize: 15, fontWeight: 700, lineHeight: 1.3, marginBottom: 6 }}>{pl.title}</div>
              <div style={{ fontSize: 10.5, color: 'var(--hds-txt-3)', marginBottom: 3 }}>{pl.item_count} items · {pl.mode === 'show_collection' ? 'Show Collection' : 'In-Order'}</div>
              {pl.total_ms > 0 && <div style={{ fontSize: 10.5, color: 'var(--hds-txt-3)', marginBottom: 14 }}>{fmtMs(pl.total_ms)} total</div>}
              <div style={{ marginTop: 4 }}>
                <DetailSectionLabel>{addLabel}</DetailSectionLabel>
                {renderAdd ? renderAdd(item, [], add) : (
                  <div style={{ marginTop: 6 }}>
                    <AddBtn onClick={() => add({ content_type: 'playlist', content_id: pl.playlist_id, title: pl.title })}>Add Playlist</AddBtn>
                  </div>
                )}
              </div>
            </>
          )
        })()}

      </div>
    </div>
  )
}

// ─── Detail fetching hook ─────────────────────────────────────────────────────

export function useDetailPanel() {
  const [infoItem,     setInfoItem]     = useState<InfoItem | null>(null)
  const [infoDetail,   setInfoDetail]   = useState<ShowDetail | MovieDetail | null>(null)
  const [infoSeasons,  setInfoSeasons]  = useState<{number: number; name: string}[]>([])
  const [detailLoading,setDetailLoading]= useState(false)

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
      api.getShow(infoItem.id)
        .then(detail => {
          if (ctrl.signal.aborted) return
          setInfoDetail(detail)
          setInfoSeasons(detail.seasons)
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

  return { infoItem, setInfoItem, infoDetail, infoSeasons, detailLoading }
}

// ─── Utility components ───────────────────────────────────────────────────────

export function AddBtn({ onClick, gold, children }: { onClick: () => void; gold?: boolean; children: React.ReactNode }) {
  return (
    <button onClick={onClick} className="hds-season-btn" style={{ padding: '4px 10px', borderRadius: 5, border: `1px solid ${gold ? 'oklch(0.55 0.12 58)' : 'var(--hds-line)'}`, background: 'transparent', color: gold ? 'oklch(0.75 0.12 58)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer' }}>
      {children}
    </button>
  )
}

export function BrowserEmpty({ hint }: { hint?: string }) {
  return <div style={{ padding: '20px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>{hint ?? 'No results.'}</div>
}

export function LoadMoreSentinel({ loading, onVisible }: { loading: boolean; onVisible: () => void }) {
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

// ─── Private helpers ──────────────────────────────────────────────────────────

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

function DetailSectionLabel({ children }: { children: React.ReactNode }) {
  return <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)' }}>{children}</div>
}
