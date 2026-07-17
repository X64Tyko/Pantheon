import { useState, useEffect, useRef } from 'react'
import { api, mediaUrl } from '../api/client'
import { imageQueue } from './imageQueue'
import { SectionLabel } from './SectionLabel'
import { fmtMs } from './utils'
import type { Show, Movie, ShowDetail, MovieDetail, EpisodeSearchResult, Playlist } from '../api/types'
import styles from './BrowserTiles.module.css'

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
      className={styles.tile}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-violet)'; scrollIn() }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line-s)'; scrollOut() }}
    >
      <div className={styles.thumbWrap}>
        {imgUrl && (
          <img src={imgReady ? imgUrl : ''} alt=""
            className={`${styles.thumbImg} ${imgReady ? styles.thumbImgReady : styles.thumbImgHidden}`} />
        )}
        {placeholder && !imgUrl && (
          <div className={styles.placeholderIcon}>{placeholder}</div>
        )}
        {badge && (
          <div className={styles.checkBadge}>✓</div>
        )}
      </div>
      <div className={styles.tileInfo}>
        <div className={styles.tileTitle}>
          <span ref={titleRef} className={styles.tileTitleInner}>{title}</span>
        </div>
        {sub && <div className={styles.tileSub}>{sub}</div>}
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
  const imgUrl   = mediaUrl(`/api/shows/${show.show_id}/thumb`)

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
      className={`${styles.tile} ${styles.tileHoverable} ${hovering ? styles.tileHoverableActive : ''}`}
      onMouseEnter={onMouseEnter}
      onMouseLeave={onMouseLeave}
    >
      <div className={styles.thumbWrap}>
        <img src={imgReady ? imgUrl : ''} alt=""
          className={`${styles.thumbImg} ${imgReady ? styles.thumbImgReady : styles.thumbImgHidden}`} />

        {isAdded && !hovering && (
          <div className={styles.checkBadge}>✓</div>
        )}

        {hovering && (
          <div
            className={styles.hoverOverlayShow}
            onClick={e => e.stopPropagation()}
          >
            {seasonsLoading ? (
              <span className={styles.hoverLoadingText}>loading…</span>
            ) : seasons !== null ? (
              <>
                <HoverSeasonBtn onClick={e => add(e, null, show.title, true)}>All Episodes</HoverSeasonBtn>
                {hasSpecials && <HoverSeasonBtn gold onClick={e => add(e, 0, `${show.title} S00`, true)}>S00 Only</HoverSeasonBtn>}
                {visible.map(s => (
                  <HoverSeasonBtn key={s.number} onClick={e => add(e, s.number, `${show.title} ${seasonLabel(s)}`)}>
                    {seasonLabel(s)}
                  </HoverSeasonBtn>
                ))}
                {hasMore && (
                  <button
                    onClick={e => { e.stopPropagation(); onInfoOpen() }}
                    className={styles.viewAllLink}
                  >view all →</button>
                )}
              </>
            ) : null}
          </div>
        )}
      </div>
      <div className={styles.tileInfo}>
        <div className={styles.tileTitle}>
          <span ref={titleRef} className={styles.tileTitleInner}>{show.title}</span>
        </div>
        {show.year && <div className={styles.tileSub}>{show.year}</div>}
      </div>
    </div>
  )
}

// ─── Movie tile with hover add ──────────────────────────────────────────────

export function MovieMediaTile({ movie, onAdd, onInfoOpen, onDragStart, onDragEnd, isAdded }: {
  movie:       Movie
  onAdd:       (params: AddContentParams) => void
  onInfoOpen:  () => void
  onDragStart?: (e: React.DragEvent) => void
  onDragEnd?:   () => void
  isAdded?:     boolean
}) {
  const [imgReady, setImgReady] = useState(false)
  const [hovering, setHovering] = useState(false)
  const titleRef = useRef<HTMLSpanElement>(null)
  const imgUrl   = mediaUrl(`/api/movies/${movie.movie_id}/thumb`)

  useEffect(() => {
    setImgReady(false)
    const ctrl = new AbortController()
    imageQueue.load(imgUrl, ctrl.signal).then(() => setImgReady(true)).catch(() => {})
    return () => ctrl.abort()
  }, [imgUrl])

  const onMouseEnter = () => {
    setHovering(true)
    if (titleRef.current) { const ov = titleRef.current.scrollHeight - 30; if (ov > 0) titleRef.current.style.transform = `translateY(-${ov}px)` }
  }
  const onMouseLeave = () => {
    setHovering(false)
    if (titleRef.current) titleRef.current.style.transform = ''
  }

  const add = (e: React.MouseEvent) => {
    e.stopPropagation()
    onAdd({ content_type: 'movie', content_id: movie.movie_id, title: movie.title })
  }

  return (
    <div
      draggable={!!onDragStart}
      onDragStart={onDragStart}
      onDragEnd={onDragEnd}
      onClick={onInfoOpen}
      className={`${styles.tile} ${styles.tileHoverable} ${hovering ? styles.tileHoverableActive : ''}`}
      onMouseEnter={onMouseEnter}
      onMouseLeave={onMouseLeave}
    >
      <div className={styles.thumbWrap}>
        <img src={imgReady ? imgUrl : ''} alt=""
          className={`${styles.thumbImg} ${imgReady ? styles.thumbImgReady : styles.thumbImgHidden}`} />

        {isAdded && !hovering && (
          <div className={styles.checkBadge}>✓</div>
        )}

        {hovering && (
          <div
            className={styles.hoverOverlayMovie}
            onClick={e => e.stopPropagation()}
          >
            <HoverSeasonBtn onClick={add}>Quick Add</HoverSeasonBtn>
            <button
              onClick={e => { e.stopPropagation(); onInfoOpen() }}
              className={styles.viewAllLink}
            >view details →</button>
          </div>
        )}
      </div>
      <div className={styles.tileInfo}>
        <div className={styles.tileTitle}>
          <span ref={titleRef} className={styles.tileTitleInner}>{movie.title}</span>
        </div>
        <div className={styles.tileSub}>{movie.year || fmtMs(movie.duration_ms)}</div>
      </div>
    </div>
  )
}

function HoverSeasonBtn({ onClick, gold, children }: { onClick: (e: React.MouseEvent) => void; gold?: boolean; children: React.ReactNode }) {
  return (
    <button
      onClick={onClick}
      className={`hds-season-btn ${styles.seasonBtn} ${gold ? styles.seasonBtnGold : ''}`}
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

  return (
    <div className={styles.infoPanelRoot}>
      <div className={styles.infoPanelHeader}>
        <button onClick={onBack} className={styles.backButton}>
          ← Back
        </button>
        {detailLoading && <span className={styles.loadingDetailsText}>loading details…</span>}
      </div>

      <div className={`${styles.infoPanelBody} scrollbar-dark`}>

        {item.kind === 'show' && (() => {
          const s = item.seed
          const d = detail as ShowDetail | null
          return (
            <>
              {d?.art && <Backdrop url={d.art} />}
              <div className={styles.infoHeaderRow}>
                <ThumbSlot url={mediaUrl(d?.thumb ?? `/api/shows/${s.show_id}/thumb`)} />
                <div className={styles.infoMinWidth0}>
                  <div className={styles.infoTitle}>{s.title}</div>
                  {s.year           && <div className={styles.infoYear}>{s.year}</div>}
                  {s.content_rating && s.content_rating !== '' && <RatingBadge rating={s.content_rating} />}
                  {d?.genres        && d.genres.length > 0 && <div className={styles.infoGenres}>{d.genres.join(', ')}</div>}
                  <div className={styles.infoMeta}>
                    {s.episode_count} episode{s.episode_count !== 1 ? 's' : ''}
                    {d?.status ? ` · ${d.status}` : ''}
                  </div>
                </div>
              </div>

              {d?.overview ? <Overview text={d.overview} /> : detailLoading && <OverviewSkeleton />}

              <div className={styles.addSectionSpacer}>
                <SectionLabel variant="tight">{addLabel}</SectionLabel>
                {renderAdd ? renderAdd(item, seasons, add) : (
                  <div className={styles.addBtnRow}>
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
                      <span className={styles.loadingSeasonsText}>loading seasons…</span>
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
              <div className={styles.infoHeaderRow}>
                <ThumbSlot url={mediaUrl(d?.thumb ?? `/api/movies/${m.movie_id}/thumb`)} />
                <div className={styles.infoMinWidth0}>
                  <div className={styles.infoTitle}>{m.title}</div>
                  {m.year           && <div className={styles.infoYear}>{m.year}</div>}
                  {m.content_rating && m.content_rating !== '' && <RatingBadge rating={m.content_rating} />}
                  {d?.genres        && d.genres.length > 0 && <div className={styles.infoGenres}>{d.genres.join(', ')}</div>}
                  <div className={styles.infoMeta}>
                    {fmtMs(m.duration_ms)}
                    {d?.director ? ` · Dir. ${d.director}` : ''}
                  </div>
                </div>
              </div>

              {d?.tagline && <p className={styles.tagline}>{d.tagline}</p>}
              {d?.overview ? <Overview text={d.overview} /> : detailLoading && <OverviewSkeleton />}

              <div className={styles.addSectionSpacer}>
                <SectionLabel variant="tight">{addLabel}</SectionLabel>
                {renderAdd ? renderAdd(item, [], add) : (
                  <div className={styles.addBtnRowSingle}>
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
              <div className={styles.episodeShowTitle}>{ep.show_title}</div>
              <div className={styles.episodeTitle}>{code} — {ep.title}</div>
              {ep.duration_ms > 0 && <div className={styles.episodeDuration}>{fmtMs(ep.duration_ms)}</div>}
              <div className={styles.mt4}>
                <SectionLabel variant="tight">{addLabel}</SectionLabel>
                {renderAdd ? renderAdd(item, [], add) : (
                  <div className={styles.addBtnRowSingle}>
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
              <div className={styles.playlistTitle}>{pl.title}</div>
              <div className={styles.playlistMeta}>{pl.item_count} items · {pl.mode === 'show_collection' ? 'Show Collection' : 'In-Order'}</div>
              {pl.total_ms > 0 && <div className={styles.playlistTotal}>{fmtMs(pl.total_ms)} total</div>}
              <div className={styles.mt4}>
                <SectionLabel variant="tight">{addLabel}</SectionLabel>
                {renderAdd ? renderAdd(item, [], add) : (
                  <div className={styles.addBtnRowSingle}>
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
    <button onClick={onClick} className={`hds-season-btn ${styles.addBtn} ${gold ? styles.addBtnGold : ''}`}>
      {children}
    </button>
  )
}

export function BrowserEmpty({ hint }: { hint?: string }) {
  return <div className={styles.browserEmpty}>{hint ?? 'No results.'}</div>
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
    <div ref={ref} className={styles.loadMoreSentinel}>
      {loading ? 'Loading…' : ''}
    </div>
  )
}

// ─── Private helpers ──────────────────────────────────────────────────────────

function Backdrop({ url }: { url: string }) {
  return <img src={url} alt="" className={styles.backdrop} onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
}

function ThumbSlot({ url }: { url: string }) {
  const [ready, setReady] = useState(false)
  return (
    <div className={styles.thumbSlot}>
      <img src={url} alt="" className={`${styles.thumbSlotImg} ${ready ? styles.thumbImgReady : styles.thumbImgHidden}`}
        onLoad={() => setReady(true)} onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
    </div>
  )
}

function RatingBadge({ rating }: { rating: string }) {
  return <span className={styles.ratingBadge}>{rating}</span>
}

function Overview({ text }: { text: string }) {
  return <p className={styles.overviewText}>{text}</p>
}

function OverviewSkeleton() {
  return (
    <div className={styles.overviewSkeletonWrap}>
      {[100, 92, 85, 60].map((w, i) => (
        <div key={i} className={styles.overviewSkeletonBar} style={{ width: `${w}%` }} />
      ))}
    </div>
  )
}

