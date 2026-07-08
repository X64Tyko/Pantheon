import { useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { FocusContext } from '@noriginmedia/norigin-spatial-navigation'
import { useCastSession } from '../cast/useCastSession'
import { startVodPlayback, stopVodPlayback } from '../player/playbackApi'
import { api, mediaUrl } from '../api/client'
import type { Show, Movie, ShowDetail, MovieDetail, WatchProgress } from '../api/types'
import { resolvePlayTarget } from '../player/resolvePlayTarget'
import { useFocusable } from '../nav/useFocusable'
import { useTravelingFocus } from '../nav/useTravelingFocus'
import { TravelingFocusFrame } from '../nav/TravelingFocusFrame'
import { TvGuideSection } from './TvGuideSection'
import { ghostBtnStyle, goldBtnStyle, heroTextShadow } from '../channel/styles'

const HOME_FOCUS_KEY = 'TV_HOME'

function isShow(item: Show | Movie): item is Show { return 'show_id' in item }
function thumbUrl(item: Show | Movie) {
  if (!item.thumb) return undefined
  return mediaUrl(isShow(item) ? `/api/shows/${item.show_id}/thumb` : `/api/movies/${item.movie_id}/thumb`)
}
function artUrl(item: Show | Movie) {
  if (!item.art) return undefined
  return mediaUrl(isShow(item) ? `/api/shows/${item.show_id}/art` : `/api/movies/${item.movie_id}/art`)
}

export function TvHome() {
  const navigate = useNavigate()
  const castSession = useCastSession()
  // Session id of whatever this hero panel last cast — stopped when a new
  // item is cast or the component unmounts, mirroring usePlaybackSession's
  // own prevSession teardown (this page owns the session directly instead of
  // going through that hook since, unlike PlayerPage, it never navigates
  // away and needs to survive across repeated Play presses).
  const castVodSessionRef = useRef<string | null>(null)
  const allItemsRef = useRef<Map<string, Show | Movie>>(new Map())
  const guideRef = useRef<HTMLDivElement>(null)

  const [recentShows,      setRecentShows]      = useState<Show[]>([])
  const [recentMovies,     setRecentMovies]     = useState<Movie[]>([])
  const [recentlyReleased, setRecentlyReleased] = useState<Movie[]>([])
  const [recentlyAired,    setRecentlyAired]    = useState<Show[]>([])
  const [continueWatching, setContinueWatching] = useState<WatchProgress[]>([])
  const [loading, setLoading] = useState(true)

  // Hero — same rotate/hover-swap model as the desktop HomePage, minus the
  // inline detail overlay (View Details is a real /tv/library/* route here).
  const heroCandidates  = useRef<(Show | Movie)[]>([])
  const heroIdx         = useRef(0)
  const hoverRestoreRef = useRef<{ item: Show | Movie; detail: ShowDetail | MovieDetail | null; idx: number } | null>(null)
  const heroIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null)
  const [heroItem,   setHeroItem]   = useState<Show | Movie | null>(null)
  const [heroDetail, setHeroDetail] = useState<ShowDetail | MovieDetail | null>(null)
  const [heroFading, setHeroFading] = useState(false)

  const loadHeroDetail = (item: Show | Movie) => {
    const p = isShow(item) ? api.getShow(item.show_id) : api.getMovie(item.movie_id)
    p.then(d => setHeroDetail(d)).catch(() => {})
  }

  const transitionHeroTo = (item: Show | Movie, detail: ShowDetail | MovieDetail | null = null) => {
    setHeroFading(true)
    setTimeout(() => {
      setHeroItem(item)
      setHeroDetail(detail)
      setHeroFading(false)
      if (!detail) loadHeroDetail(item)
    }, 260)
  }

  const goToHero = (idx: number) => {
    const candidates = heroCandidates.current
    if (!candidates.length) return
    transitionHeroTo(candidates[idx % candidates.length])
    heroIdx.current = idx % candidates.length
  }

  const startRotation = () => {
    if (heroIntervalRef.current) clearInterval(heroIntervalRef.current)
    heroIntervalRef.current = setInterval(() => goToHero(heroIdx.current + 1), 9000)
  }

  const handleShelfFocus = (id: string) => {
    const item = allItemsRef.current.get(id)
    if (!item || !item.art) return
    if (heroIntervalRef.current) clearInterval(heroIntervalRef.current)
    if (!hoverRestoreRef.current && heroItem) {
      hoverRestoreRef.current = { item: heroItem, detail: heroDetail, idx: heroIdx.current }
    }
    transitionHeroTo(item, null)
  }

  const handleShelfBlur = () => {
    if (hoverRestoreRef.current) {
      const { item, detail, idx } = hoverRestoreRef.current
      heroIdx.current = idx
      transitionHeroTo(item, detail)
      hoverRestoreRef.current = null
    }
    startRotation()
  }

  useEffect(() => {
    Promise.all([
      api.getShows({ limit: 16, sort: 'recently_added' }),
      api.getMovies({ limit: 16, sort: 'recently_added' }),
      api.getMovies({ limit: 16, sort: 'recently_released' }).catch(() => ({ items: [] as Movie[], total: 0 })),
      api.getShows({ limit: 16, sort: 'recently_aired' }).catch(() => ({ items: [] as Show[], total: 0 })),
      api.getWatchProgress().catch(() => []),
    ]).then(([sr, mr, rr, ra, cw]) => {
      setRecentShows(sr.items)
      setRecentMovies(mr.items)
      setRecentlyReleased(rr.items)
      setRecentlyAired(ra.items)
      setContinueWatching(cw)

      sr.items.forEach(s => allItemsRef.current.set(s.show_id, s))
      mr.items.forEach(m => allItemsRef.current.set(m.movie_id, m))
      rr.items.forEach(m => allItemsRef.current.set(m.movie_id, m))
      ra.items.forEach(s => allItemsRef.current.set(s.show_id, s))

      const withArt = [...sr.items.filter(s => s.art), ...mr.items.filter(m => m.art)]
      heroCandidates.current = withArt
      const first = withArt[0] ?? sr.items[0]
      if (first) { heroIdx.current = 0; setHeroItem(first); loadHeroDetail(first) }
    }).finally(() => setLoading(false))
  }, [])

  useEffect(() => {
    if (heroItem) startRotation()
    return () => { if (heroIntervalRef.current) clearInterval(heroIntervalRef.current) }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [heroItem?.art])

  useEffect(() => () => {
    if (castVodSessionRef.current) stopVodPlayback(castVodSessionRef.current)
  }, [])

  const { ref: homeRef, focusKey: homeFocusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: HOME_FOCUS_KEY, trackChildren: true, saveLastFocusedChild: true,
    preferredChildFocusKey: 'tv-hero-play',
  })

  const goToLibrary = (id: string, type: 'show' | 'movie') => navigate(`/tv/library/${type}/${id}`)

  return (
    <FocusContext.Provider value={homeFocusKey}>
    <div ref={homeRef} style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
      <div style={{ flexShrink: 0 }}>
        {loading ? (
          <div className="hds-skeleton" style={{ height: '36vh', minHeight: 280 }} />
        ) : heroItem ? (
          <TvHeroPanel
            item={heroItem}
            detail={heroDetail}
            fading={heroFading}
            totalCandidates={heroCandidates.current.length}
            currentIdx={heroIdx.current}
            onViewDetail={() => goToLibrary(isShow(heroItem) ? heroItem.show_id : heroItem.movie_id, isShow(heroItem) ? 'show' : 'movie')}
            onPlay={async () => {
              const id = isShow(heroItem) ? heroItem.show_id : heroItem.movie_id
              const type = isShow(heroItem) ? 'show' : 'movie'
              const target = await resolvePlayTarget(type, id)
              if (!target) return

              if (castSession.connected) {
                // Already connected to a receiver — load onto it directly and
                // stay on this screen (same D-pad grid) rather than jumping to
                // Hades' own local /player UI, which nothing is watching here.
                if (castVodSessionRef.current) stopVodPlayback(castVodSessionRef.current)
                const vod = await startVodPlayback({
                  content_type: target.kind,
                  content_id:   target.id,
                  position_ms:  target.positionMs,
                })
                castVodSessionRef.current = vod.session_id
                await castSession.load({
                  manifestUrl: vod.manifest_url,
                  isLive: false,
                  currentMs: target.positionMs,
                  metadata: {
                    title: vod.title,
                    imageUrl: artUrl(heroItem),
                    seriesTitle: isShow(heroItem) ? heroItem.title : undefined,
                  },
                  route: { contentType: target.kind, contentId: target.id },
                })
                return
              }

              navigate(target.positionMs > 0
                ? `/player/${target.kind}/${target.id}?t=${target.positionMs}`
                : `/player/${target.kind}/${target.id}`)
            }}
            onCast={async () => {
              // Connect only — no media load. The user keeps browsing here;
              // pressing Play (above) is what actually sends something to
              // the now-connected receiver.
              if (castSession.connected) return
              try { await cast.framework.CastContext.getInstance().requestSession() }
              catch (err) {
                if (err !== 'cancel') console.error('Cast requestSession() failed:', err)
              }
            }}
            castAvailable={castSession.available}
            onDotClick={i => { startRotation(); goToHero(i) }}
          />
        ) : (
          <div style={{
            height: '36vh', minHeight: 280, display: 'flex', alignItems: 'center', justifyContent: 'center',
            background: 'linear-gradient(135deg, oklch(0.12 0.04 292), oklch(0.16 0.03 280))',
            fontFamily: "'Chakra Petch', sans-serif", fontSize: 18, color: 'var(--hds-txt-3)',
          }}>No content yet</div>
        )}
      </div>

        <div style={{
          display: 'flex', alignItems: 'center', gap: 10,
          padding: '8px 48px 0', flexShrink: 0,
        }}>
          <LibraryButton onClick={() => navigate('/tv/library')} />
          <GuideButton onClick={() => guideRef.current?.scrollIntoView({ behavior: 'smooth' })} />
        </div>

      <div style={{ flex: 1, minHeight: 0, overflowY: 'auto' }} className="scrollbar-dark">
        {loading ? (
          <div style={{ padding: '24px 48px', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>Loading…</div>
        ) : (
          <>
            {continueWatching.length > 0 && (
              <TvContinueWatchingShelf items={continueWatching} onNavigate={navigate} />
            )}
            {recentShows.length > 0 && (
              <TvShelf
                title="Recently Added Shows"
                items={recentShows.map(s => ({
                  key: s.show_id, title: s.title, year: s.year, rating: s.audience_rating,
                  thumb_url: thumbUrl(s), onClick: () => goToLibrary(s.show_id, 'show'), onFocus: () => handleShelfFocus(s.show_id),
                }))}
                onBlur={handleShelfBlur}
                endTile={{ focusKey: 'tv-shelf-end-shows', onClick: () => navigate('/tv/library') }}
              />
            )}
            {recentMovies.length > 0 && (
              <TvShelf
                title="Recently Added Movies"
                items={recentMovies.map(m => ({
                  key: m.movie_id, title: m.title, year: m.year, rating: m.audience_rating,
                  thumb_url: thumbUrl(m), onClick: () => goToLibrary(m.movie_id, 'movie'), onFocus: () => handleShelfFocus(m.movie_id),
                }))}
                onBlur={handleShelfBlur}
                endTile={{ focusKey: 'tv-shelf-end-movies', onClick: () => navigate('/tv/library') }}
              />
            )}
            {recentlyReleased.length > 0 && (
              <TvShelf
                title="Recently Released"
                items={recentlyReleased.map(m => ({
                  key: m.movie_id, title: m.title, year: m.year, rating: m.audience_rating,
                  thumb_url: thumbUrl(m), onClick: () => goToLibrary(m.movie_id, 'movie'), onFocus: () => handleShelfFocus(m.movie_id),
                }))}
                onBlur={handleShelfBlur}
                endTile={{ focusKey: 'tv-shelf-end-released', onClick: () => navigate('/tv/library') }}
              />
            )}
            {recentlyAired.filter(s => s.latest_episode).length > 0 && (
              <TvShelf
                title="Recently Aired"
                items={recentlyAired.filter(s => s.latest_episode).map(s => ({
                  key: s.show_id, title: s.title, year: s.year, rating: s.audience_rating,
                  thumb_url: thumbUrl(s), onClick: () => navigate(`/player/episode/${s.latest_episode!.episode_id}`),
                  onFocus: () => handleShelfFocus(s.show_id),
                }))}
                onBlur={handleShelfBlur}
                endTile={{ focusKey: 'tv-shelf-end-aired', onClick: () => navigate('/tv/library') }}
              />
            )}

            <div ref={guideRef} style={{ padding: '8px 0 64px' }}>
              <TvGuideSection />
            </div>
          </>
        )}
      </div>
    </div>
    </FocusContext.Provider>
  )
}

function LibraryButton({ onClick }: { onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-quickaction-library', onEnterPress: onClick })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      style={{
        display: 'flex', alignItems: 'center', gap: 10, cursor: 'pointer',
        padding: '11px 22px', borderRadius: 10,
        border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)', color: 'var(--hds-txt)',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 14,
      }}
    >
      <svg width="16" height="16" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4">
        <rect x="1.5" y="1.5" width="4.5" height="11" rx="1" /><rect x="8" y="1.5" width="4.5" height="11" rx="1" />
      </svg>
      Library
    </button>
  )
}

function GuideButton({ onClick }: { onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-quickaction-guide', onEnterPress: onClick })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      style={{
        display: 'flex', alignItems: 'center', gap: 10, cursor: 'pointer',
        padding: '11px 22px', borderRadius: 10,
        border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)', color: 'var(--hds-txt)',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 14,
      }}
    >
      <svg width="16" height="16" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4">
        <rect x="1.5" y="2.5" width="11" height="9" rx="1.5" /><path d="M4 11.5h6" />
      </svg>
      Guide
    </button>
  )
}

// ── Hero ─────────────────────────────────────────────────────────────────────

function TvHeroPanel({ item, detail, fading, totalCandidates, currentIdx, onViewDetail, onPlay, onCast, castAvailable, onDotClick }: {
  item: Show | Movie
  detail: ShowDetail | MovieDetail | null
  fading: boolean
  totalCandidates: number
  currentIdx: number
  onViewDetail: () => void
  onPlay: () => void
  onCast: () => void
  castAvailable: boolean
  onDotClick: (i: number) => void
}) {
  const backdrop = artUrl(item)
  const bg = backdrop
    ? `url(${backdrop}) center/cover no-repeat`
    : 'linear-gradient(135deg, oklch(0.12 0.04 292) 0%, oklch(0.18 0.06 270) 50%, oklch(0.14 0.03 280) 100%)'

  const genres: string[] = detail && 'genres' in detail && Array.isArray(detail.genres) ? (detail.genres as string[]).slice(0, 4) : []
  const overview = detail?.overview ?? ''
  const rating   = detail?.audience_rating ?? item.audience_rating

  const play       = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-hero-play',   onEnterPress: onPlay,       forceFocus: true })
  const viewDetail = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-hero-detail', onEnterPress: onViewDetail })
  const castBtn    = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-hero-cast',   onEnterPress: onCast,       focusable: castAvailable })

  return (
    <div style={{
      position: 'relative', height: '36vh', minHeight: 280,
      background: bg, flexShrink: 0,
      opacity: fading ? 0 : 1, transition: 'opacity .26s ease',
      overflow: 'hidden',
    }}>
      <div style={{
        position: 'absolute', inset: 0,
        background: 'linear-gradient(to right, oklch(0 0 0 / 0.88) 0%, oklch(0 0 0 / 0.42) 52%, transparent 100%)',
      }} />
      <div style={{
        position: 'absolute', left: 0, right: 0, bottom: 0, height: '30%',
        background: 'linear-gradient(to top, var(--hds-bg) 0%, transparent 100%)',
        pointerEvents: 'none',
      }} />

      <div style={{ position: 'absolute', bottom: 0, left: 0, right: 0, padding: '0 48px 32px', maxWidth: 780 }}>
        {genres.length > 0 && (
          <div style={{ display: 'flex', gap: 8, marginBottom: 14, flexWrap: 'wrap' }}>
            {genres.map(g => (
              <span key={g} style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
                padding: '3px 10px', borderRadius: 12,
                background: 'var(--hds-glass)', border: '1px solid var(--hds-glass-border)',
                color: 'var(--hds-txt-2)', letterSpacing: '0.06em',
              }}>{g}</span>
            ))}
          </div>
        )}

        <h1 style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 34, fontWeight: 700,
          color: 'oklch(1 0 0)', margin: 0, lineHeight: 1.1, letterSpacing: '-0.02em',
          textShadow: heroTextShadow,
        }}>{item.title}</h1>

        <div style={{
          display: 'flex', alignItems: 'center', gap: 16, marginTop: 10, marginBottom: 14,
          fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-2)',
          textShadow: heroTextShadow,
        }}>
          {item.year && <span>{item.year}</span>}
          {rating != null && <span style={{ color: 'var(--hds-gold)' }}>★ {rating.toFixed(1)}</span>}
          <span style={{ opacity: 0.5 }}>{'show_id' in item ? 'series' : 'film'}</span>
        </div>

        {overview && (
          <p style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 12, lineHeight: 1.6,
            color: 'oklch(0.75 0.01 285)', margin: '0 0 22px',
            display: '-webkit-box', WebkitLineClamp: 2, WebkitBoxOrient: 'vertical', overflow: 'hidden',
            textShadow: heroTextShadow,
          }}>{overview}</p>
        )}

        <div style={{ display: 'flex', gap: 12 }}>
          <button
            ref={play.ref} data-tv-focused={play.focused}
            onClick={onPlay}
            style={{ ...goldBtnStyle, display: 'flex', alignItems: 'center', gap: 8 }}
          >
            <svg width="12" height="12" viewBox="0 0 14 14" fill="currentColor"><path d="M3 1.5v11l9-5.5-9-5.5z" /></svg>
            Play
          </button>
          <button
            ref={viewDetail.ref} data-tv-focused={viewDetail.focused}
            onClick={onViewDetail}
            style={{ ...ghostBtnStyle, backdropFilter: 'blur(8px)' }}
          >View Details</button>
        </div>
      </div>

      {/* Cast — fixed top-right, same anchor as the desktop HomePage hero. */}
      {castAvailable && (
        <button
          ref={castBtn.ref} data-tv-focused={castBtn.focused}
          onClick={onCast}
          aria-label="Cast"
          style={{
            position: 'absolute', top: 18, right: 24, zIndex: 3,
            display: 'flex', alignItems: 'center', justifyContent: 'center',
            width: 34, height: 34, borderRadius: '50%',
            border: '1px solid var(--hds-glass-border)',
            background: 'var(--hds-glass)', backdropFilter: 'blur(8px)',
            color: 'oklch(0.92 0.01 285)', cursor: 'pointer',
          }}
        >
          <svg width="15" height="15" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.4">
            <rect x="2" y="2.5" width="14" height="10" rx="1.2" />
            <path d="M2 15.5a4 4 0 0 0-2-3.5M2 12.5a1.5 1.5 0 0 1 1.5 1.5" fill="none" strokeLinecap="round" />
            <circle cx="2.3" cy="15.5" r="0.9" fill="currentColor" stroke="none" />
          </svg>
        </button>
      )}

      {totalCandidates > 1 && (
        <div style={{ position: 'absolute', bottom: 20, right: 48, display: 'flex', gap: 6, zIndex: 2 }}>
          {Array.from({ length: Math.min(totalCandidates, 8) }, (_, i) => (
            <button key={i} onClick={() => onDotClick(i)} style={{
              width: i === currentIdx ? 18 : 6, height: 6, borderRadius: 3,
              border: 'none', cursor: 'pointer', padding: 0,
              background: i === currentIdx ? 'var(--hds-gold)' : 'oklch(1 0 0 / 0.3)',
              transition: 'width .2s, background .2s',
            }} />
          ))}
        </div>
      )}
    </div>
  )
}

// ── Shelves ──────────────────────────────────────────────────────────────────

// useFocusable's scrollIntoView runs on each card's own root element (see
// nav/useFocusable.ts) with no idea that a shelf title sits above its row —
// 'center'-aligning just that element can land the title (and the next
// shelf's, below) right at the clipped edge of the scroll container. Native
// scrollIntoView respects scroll-margin on the element being scrolled to, so
// giving every focusable row item the same top margin (sized to a shelf
// title's rendered height: 17-18px font + up to 12px bottom padding, plus a
// little slack) reserves room for its title without any custom scroll math.
const SHELF_TITLE_SCROLL_MARGIN = 44

interface TvShelfEntry {
  key: string; title: string; year?: number; rating?: number
  thumb_url?: string; onClick: () => void; onFocus?: () => void
}

function TvShelf({ title, items, onBlur, endTile }: {
  title: string; items: TvShelfEntry[]; onBlur?: () => void
  endTile?: { focusKey: string; onClick: () => void }
}) {
  const travel = useTravelingFocus()
  // isFocusBoundary traps left/right within the row — see HomePage's Shelf
  // for why (arrowing off the row edge was otherwise falling back to a
  // nearest-neighbor search across the whole page).
  const { ref: rowRef, focusKey: rowFocusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: `tv-shelf-row-${title}`,
    trackChildren: true,
    isFocusBoundary: true,
    focusBoundaryDirections: ['left', 'right'],
  })
  return (
    <div style={{ marginBottom: 16 }} onMouseLeave={onBlur}>
      <div style={{
        fontFamily: "'Chakra Petch', sans-serif", fontSize: 17, fontWeight: 600,
        color: 'var(--hds-txt)', padding: '0 48px 10px',
      }}>{title}</div>
      <div ref={rowRef} style={{
        display: 'flex', gap: 16, overflowX: 'auto', overflowY: 'hidden',
        padding: '2px 48px 10px', scrollbarWidth: 'none', position: 'relative',
      }}>
        <TravelingFocusFrame rect={travel.rect} active={travel.active} />
        <FocusContext.Provider value={rowFocusKey}>
        {items.map(item => (
          <div key={item.key} style={{ flexShrink: 0, width: 125 }}>
            <TvShelfCard {...item} onBlur={onBlur} onActivate={travel.activate} onDeactivate={travel.deactivate} />
          </div>
        ))}
        {endTile && (
          <TvShelfEndTile
            focusKey={endTile.focusKey} onClick={endTile.onClick} onBlur={onBlur}
            onActivate={travel.activate} onDeactivate={travel.deactivate}
          />
        )}
        </FocusContext.Provider>
      </div>
    </div>
  )
}

function TvShelfCard({ title, year, rating, thumb_url, onClick, onFocus, onBlur, onActivate, onDeactivate }: TvShelfEntry & {
  onBlur?: () => void; onActivate: (el: HTMLElement | null) => void; onDeactivate: () => void
}) {
  const [hovered, setHovered] = useState(false)
  const [imgErr,  setImgErr]  = useState(false)
  const showImg = thumb_url && !imgErr
  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `tv-shelf-card-${title}-${year ?? ''}`, onEnterPress: onClick,
    onFocus: () => { onFocus?.(); onActivate(ref.current) },
    onBlur: onDeactivate,
  })
  const active = hovered || focused

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      onMouseEnter={() => { setHovered(true); onFocus?.(); onActivate(ref.current) }}
      onMouseLeave={() => { setHovered(false); onBlur?.(); onDeactivate() }}
      style={{
        borderRadius: 10, overflow: 'hidden', cursor: 'pointer',
        transform: active ? 'translateY(-4px)' : 'none',
        transition: 'transform 0.15s cubic-bezier(0.2,0,0.2,1)',
        scrollMarginTop: SHELF_TITLE_SCROLL_MARGIN,
      }}
    >
      <div style={{
        aspectRatio: '2/3', width: '100%', position: 'relative',
        background: 'linear-gradient(135deg, oklch(0.18 0.03 287), oklch(0.13 0.02 285))',
        display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
      }}>
        {showImg ? (
          <img src={thumb_url} alt={title} onError={() => setImgErr(true)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
        ) : (
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 24, color: 'var(--hds-violet)', opacity: 0.4 }}>
            {title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
        {active && (
          <div style={{
            position: 'absolute', bottom: 0, left: 0, right: 0,
            background: 'linear-gradient(to top, oklch(0 0 0 / 0.85), transparent)',
            padding: '28px 10px 10px',
          }}>
            <div style={{
              fontFamily: "'Chakra Petch', sans-serif", fontSize: 13, fontWeight: 600,
              color: '#fff', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
            }}>{title}</div>
            {rating != null && (
              <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-gold)', marginTop: 2 }}>★ {rating.toFixed(1)}</div>
            )}
          </div>
        )}
      </div>
      <div style={{ padding: '6px 4px 2px' }}>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 13, fontWeight: 600,
          color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }}>{title}</div>
        {year && <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 1 }}>{year}</div>}
      </div>
    </div>
  )
}

// Trailing tile — "Continue in Library" — mirrors HomePage's ShelfEndTile.
function TvShelfEndTile({ focusKey, onClick, onBlur, onActivate, onDeactivate }: {
  focusKey: string; onClick: () => void; onBlur?: () => void
  onActivate: (el: HTMLElement | null) => void; onDeactivate: () => void
}) {
  const [hovered, setHovered] = useState(false)
  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey, onEnterPress: onClick,
    onFocus: () => onActivate(ref.current), onBlur: onDeactivate,
  })
  const active = hovered || focused

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      onMouseEnter={() => { setHovered(true); onActivate(ref.current) }}
      onMouseLeave={() => { setHovered(false); onBlur?.(); onDeactivate() }}
      style={{
        flexShrink: 0, width: 125, aspectRatio: '2/3', borderRadius: 10, cursor: 'pointer',
        display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 12,
        border: '1px dashed var(--hds-line)', background: 'var(--hds-bg-2)', color: 'var(--hds-txt-3)',
        transform: active ? 'translateY(-4px)' : 'none',
        transition: 'transform 0.15s cubic-bezier(0.2,0,0.2,1)',
        textAlign: 'center', padding: '0 12px',
        scrollMarginTop: SHELF_TITLE_SCROLL_MARGIN,
      }}
    >
      <svg width="26" height="26" viewBox="0 0 22 22" fill="none" stroke="currentColor" strokeWidth="1.6">
        <rect x="3" y="3" width="7" height="7" rx="1.2" /><rect x="12" y="3" width="7" height="7" rx="1.2" />
        <rect x="3" y="12" width="7" height="7" rx="1.2" /><rect x="12" y="12" width="7" height="7" rx="1.2" />
      </svg>
      <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 12, letterSpacing: '0.04em', lineHeight: 1.4 }}>
        Continue in Library
      </span>
    </div>
  )
}

// ── Continue Watching ────────────────────────────────────────────────────────

function TvContinueWatchingShelf({ items, onNavigate }: { items: WatchProgress[]; onNavigate: (path: string) => void }) {
  const travel = useTravelingFocus()
  const { ref: rowRef, focusKey: rowFocusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: 'tv-shelf-row-continue-watching',
    trackChildren: true,
    isFocusBoundary: true,
    focusBoundaryDirections: ['left', 'right'],
  })
  return (
    <div style={{ marginBottom: 20 }}>
      <div style={{
        fontFamily: "'Chakra Petch', sans-serif", fontSize: 18, fontWeight: 600,
        color: 'var(--hds-txt)', padding: '0 48px 12px',
      }}>Continue Watching</div>
      <div ref={rowRef} style={{
        display: 'flex', gap: 16, overflowX: 'auto', overflowY: 'hidden',
        padding: '4px 48px 12px', scrollbarWidth: 'none', position: 'relative',
      }}>
        <TravelingFocusFrame rect={travel.rect} active={travel.active} />
        <FocusContext.Provider value={rowFocusKey}>
        {items.map(p => (
          <div key={`${p.content_type}:${p.content_id}`} style={{ flexShrink: 0, width: 125 }}>
            <TvContinueWatchingCard item={p} onNavigate={onNavigate} onActivate={travel.activate} onDeactivate={travel.deactivate} />
          </div>
        ))}
        <TvShelfEndTile
          focusKey="tv-shelf-end-continue-watching" onClick={() => onNavigate('/tv/library')}
          onActivate={travel.activate} onDeactivate={travel.deactivate}
        />
        </FocusContext.Provider>
      </div>
    </div>
  )
}

function TvContinueWatchingCard({ item, onNavigate, onActivate, onDeactivate }: {
  item: WatchProgress; onNavigate: (path: string) => void
  onActivate: (el: HTMLElement | null) => void; onDeactivate: () => void
}) {
  const [hovered, setHovered] = useState(false)
  const [imgErr,  setImgErr]  = useState(false)
  const thumbPath = item.content_type === 'movie'
    ? `/api/movies/${item.content_id}/thumb`
    : item.show_id ? `/api/shows/${item.show_id}/thumb` : ''
  const progress = item.duration_ms > 0 ? Math.min(1, item.position_ms / item.duration_ms) : 0
  const epCode = item.content_type === 'episode'
    ? `S${String(item.season ?? 0).padStart(2, '0')}E${String(item.episode ?? 0).padStart(2, '0')}`
    : undefined
  const title = item.content_type === 'episode' ? (item.show_title || item.title) : item.title

  const go = () => onNavigate(`/player/${item.content_type}/${item.content_id}?t=${item.position_ms}`)
  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `tv-cw-card-${item.content_type}-${item.content_id}`, onEnterPress: go,
    onFocus: () => onActivate(ref.current), onBlur: onDeactivate,
  })
  const active = hovered || focused

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={go}
      onMouseEnter={() => { setHovered(true); onActivate(ref.current) }}
      onMouseLeave={() => { setHovered(false); onDeactivate() }}
      style={{
        borderRadius: 10, overflow: 'hidden', cursor: 'pointer',
        transform: active ? 'translateY(-4px)' : 'none',
        transition: 'transform 0.15s cubic-bezier(0.2,0,0.2,1)',
        scrollMarginTop: SHELF_TITLE_SCROLL_MARGIN,
      }}
    >
      <div style={{
        aspectRatio: '2/3', width: '100%', position: 'relative',
        background: 'linear-gradient(135deg, oklch(0.18 0.03 287), oklch(0.13 0.02 285))',
        display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
      }}>
        {thumbPath && !imgErr ? (
          <img src={mediaUrl(thumbPath)} alt={title} onError={() => setImgErr(true)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
        ) : (
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 24, color: 'var(--hds-violet)', opacity: 0.4 }}>
            {title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
        {epCode && (
          <span style={{
            position: 'absolute', top: 8, left: 8,
            background: 'oklch(0 0 0 / 0.6)', borderRadius: 5, padding: '3px 7px',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'oklch(0.9 0.01 285)',
          }}>{epCode}</span>
        )}
        <div style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 4, background: 'oklch(0 0 0 / 0.5)' }}>
          <div style={{ height: '100%', width: `${progress * 100}%`, background: 'var(--hds-violet)' }} />
        </div>
      </div>
      <div style={{ padding: '6px 4px 2px' }}>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 13, fontWeight: 600,
          color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }}>{title}</div>
        {item.content_type === 'episode' && (
          <div style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 1,
            overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
          }}>{item.title}</div>
        )}
      </div>
    </div>
  )
}
