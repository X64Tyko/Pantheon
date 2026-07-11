import { lazy, Suspense, useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { api, mediaUrl } from '../api/client'
import type { Show, Movie, ShowDetail, MovieDetail, ScraperStats, WatchProgress } from '../api/types'
import { resolvePlayPath } from '../player/resolvePlayTarget'
import { MediaDetailHero } from '../components/media/MediaDetailHero'
import { LibraryDetailActions } from '../components/media/LibraryDetailActions'
import { getScrollPos, saveScrollPos } from '../hooks/scrollMemory'
import { ghostBtnStyle, goldBtnStyle, heroTextShadow } from '../channel/styles'
import { FocusContext } from '@noriginmedia/norigin-spatial-navigation'
import { useFocusable } from '../nav/useFocusable'
import { useTravelingFocus } from '../nav/useTravelingFocus'
import { TravelingFocusFrame } from '../nav/TravelingFocusFrame'
import { libraryStore } from '../stores/LibraryStore'
import { useCastSession } from '../cast/useCastSession'

const HOME_FOCUS_KEY = 'HOME'

// Lazy: pulls in hls.js (via the Guide's live preview) that most page loads don't need.
const GuidePage = lazy(() => import('../guide/GuidePage').then(m => ({ default: m.GuidePage })))

const SCROLL_KEY = 'home'

// ── Helpers ──────────────────────────────────────────────────────────────────

function isShow(item: Show | Movie): item is Show { return 'show_id' in item }

function proxyThumb(item: Show | Movie) {
  if (!item.thumb) return undefined
  return mediaUrl(isShow(item) ? `/api/shows/${item.show_id}/thumb` : `/api/movies/${item.movie_id}/thumb`)
}
function proxyArt(item: Show | Movie) {
  if (!item.art) return undefined
  return mediaUrl(isShow(item) ? `/api/shows/${item.show_id}/art` : `/api/movies/${item.movie_id}/art`)
}

// ── Component ────────────────────────────────────────────────────────────────

export default function HomePage() {
  const navigate = useNavigate()
  const scrollContainerRef = useRef<HTMLDivElement>(null)
  const savedScrollY       = useRef(0)
  const restoredScrollRef  = useRef(false)
  const allItemsRef        = useRef<Map<string, Show | Movie>>(new Map())
  const guideRef           = useRef<HTMLDivElement>(null)
  const castSession        = useCastSession()

  // Data
  const [recentShows,      setRecentShows]      = useState<Show[]>([])
  const [recentMovies,     setRecentMovies]     = useState<Movie[]>([])
  const [recentlyReleased, setRecentlyReleased] = useState<Movie[]>([])
  const [recentlyAired,    setRecentlyAired]    = useState<Show[]>([])
  const [continueWatching, setContinueWatching] = useState<WatchProgress[]>([])
  const [stats,            setStats]            = useState<ScraperStats | null>(null)
  const [loading,          setLoading]          = useState(true)
  const [libraryNames,     setLibraryNames]     = useState<Map<string, string>>(new Map())

  // Hero
  const heroCandidates    = useRef<(Show | Movie)[]>([])
  const heroIdx           = useRef(0)
  const hoverRestoreRef   = useRef<{ item: Show | Movie; detail: ShowDetail | MovieDetail | null; idx: number } | null>(null)
  const [heroItem,   setHeroItem]   = useState<Show | Movie | null>(null)
  const [heroDetail, setHeroDetail] = useState<ShowDetail | MovieDetail | null>(null)
  const [heroFading, setHeroFading] = useState(false)
  const heroIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null)

  // Detail view
  const [detailOpen, setDetailOpen] = useState(false)
  const [detailId,   setDetailId]   = useState<string | null>(null)
  const [detailType, setDetailType] = useState<'show' | 'movie' | null>(null)

  // Crossfade
  const [transitioning, setTransitioning] = useState(false)

  // ── Hero helpers ────────────────────────────────────────────────────────────

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

  // ── Data load ───────────────────────────────────────────────────────────────

  useEffect(() => {
    setLoading(true)
    Promise.all([
      api.getShows({ limit: 24, sort: 'recently_added' }),
      api.getMovies({ limit: 16, sort: 'recently_added' }),
      api.getMovies({ limit: 16, sort: 'recently_released' }).catch(() => ({ items: [] as Movie[], total: 0 })),
      api.getShows({ limit: 16, sort: 'recently_aired' }).catch(() => ({ items: [] as Show[], total: 0 })),
      api.getScraperStats().catch(() => null),
      api.getWatchProgress().catch(() => []),
      api.getAllLibraries().catch(() => []),
    ]).then(([sr, mr, rr, ra, st, cw, libs]) => {
      setRecentShows(sr.items)
      setRecentMovies(mr.items)
      setRecentlyReleased(rr.items)
      setRecentlyAired(ra.items)
      setStats(st)
      setContinueWatching(cw)
      setLibraryNames(new Map(libs.map(l => [l.library_id, l.display_name])))

      sr.items.forEach(s => allItemsRef.current.set(s.show_id, s))
      mr.items.forEach(m => allItemsRef.current.set(m.movie_id, m))
      rr.items.forEach(m => allItemsRef.current.set(m.movie_id, m))
      ra.items.forEach(s => allItemsRef.current.set(s.show_id, s))

      const withArt = [...sr.items.filter(s => s.art), ...mr.items.filter(m => m.art)]
      heroCandidates.current = withArt
      const first = withArt[0] ?? sr.items[0]
      if (first) { heroIdx.current = 0; setHeroItem(first); loadHeroDetail(first) }
    }).finally(() => {
      setLoading(false)
      if (!restoredScrollRef.current) {
        restoredScrollRef.current = true
        setTimeout(() => scrollContainerRef.current?.scrollTo({ top: getScrollPos(SCROLL_KEY) }), 32)
      }
    })
  }, [])

  useEffect(() => {
    if (heroItem && !detailOpen) startRotation()
    return () => { if (heroIntervalRef.current) clearInterval(heroIntervalRef.current) }
  }, [heroItem?.art, detailOpen]) // eslint-disable-line react-hooks/exhaustive-deps

  // ── Detail open / close ─────────────────────────────────────────────────────

  // Home's own rotating hero (HeroPanel) is browse-mode only — the open
  // detail view is MediaDetailHero's own pinned hero instead (same component
  // Library uses, so the two never drift out of sync with each other again),
  // so opening/closing a detail view no longer touches HeroPanel's state at
  // all: it just stops being rendered, and resumes wherever it left off.
  const openDetail = (id: string, type: 'show' | 'movie') => {
    if (heroIntervalRef.current) clearInterval(heroIntervalRef.current)
    savedScrollY.current = scrollContainerRef.current?.scrollTop ?? 0
    // The shelf row unmounts on detail-open without ever firing its own
    // mouseleave, so a hover-swap in progress would otherwise be orphaned.
    hoverRestoreRef.current = null

    // Crossfade shelves → detail
    setTransitioning(true)
    setTimeout(() => {
      setDetailId(id)
      setDetailType(type)
      setDetailOpen(true)
      setTransitioning(false)
      scrollContainerRef.current?.scrollTo({ top: 0 })
    }, 200)
  }

  const closeDetail = () => {
    // Crossfade detail → shelves
    setTransitioning(true)
    setTimeout(() => {
      setDetailOpen(false)
      setDetailId(null)
      setDetailType(null)
      setTransitioning(false)
      // Restore scroll after content mounts
      setTimeout(() => {
        scrollContainerRef.current?.scrollTo({ top: savedScrollY.current })
      }, 32)
    }, 200)
  }

  // ── Shelf hover → hero swap ──────────────────────────────────────────────────
  // Row-level leave (not per-card) restores, so moving between adjacent cards
  // in the same shelf doesn't flicker restore-then-reswap on every card.

  const handleShelfHover = (id: string) => {
    if (detailOpen) return
    const item = allItemsRef.current.get(id)
    if (!item || !item.art) return
    if (heroIntervalRef.current) clearInterval(heroIntervalRef.current)
    if (!hoverRestoreRef.current && heroItem) {
      hoverRestoreRef.current = { item: heroItem, detail: heroDetail, idx: heroIdx.current }
    }
    transitionHeroTo(item, null)
  }

  const handleShelfHoverEnd = () => {
    if (hoverRestoreRef.current) {
      const { item, detail, idx } = hoverRestoreRef.current
      heroIdx.current = idx
      transitionHeroTo(item, detail)
      hoverRestoreRef.current = null
    }
    if (!detailOpen) startRotation()
  }

  // Per-card "Hide from Home" shortcut — flips the whole library's
  // show_on_home flag (see Settings → Sources for the same toggle), then
  // optimistically drops every currently-loaded item from that library out
  // of every shelf, not just the one the card was clicked from.
  const hideLibrary = async (libraryId: string) => {
    await api.setLibraryShowOnHome(libraryId, false)
    setRecentShows(s => s.filter(x => x.library_id !== libraryId))
    setRecentMovies(m => m.filter(x => x.library_id !== libraryId))
    setRecentlyReleased(m => m.filter(x => x.library_id !== libraryId))
    setRecentlyAired(s => s.filter(x => x.library_id !== libraryId))
  }

  const needsReview = stats ? stats.uncertain + stats.unmatched : 0

  // Entering Home fresh focuses the hero Play button; returning (nav away
  // and back) restores wherever focus last was, via the library's own
  // lastFocusedChildKey → preferredChildFocusKey → nearest-to-origin
  // precedence (see getNextFocusKey in the library source) — no manual
  // "remember position" bookkeeping needed here.
  const { ref: homeRef, focusKey: homeFocusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: HOME_FOCUS_KEY,
    trackChildren: true,
    saveLastFocusedChild: true,
    preferredChildFocusKey: 'home-hero-play',
  })

  return (
    <FocusContext.Provider value={homeFocusKey}>
    <div ref={homeRef} style={{ height: '100%', overflow: 'hidden', display: 'flex', flexDirection: 'column', background: 'var(--hds-bg)' }}>
      {/* Hero — a fixed, non-scrolling region; only the content below it scrolls.
          Plain flexbox, not JS-driven, so mouse-wheel/trackpad scrolling gets
          the same behavior as D-pad nav for free. Browse-mode only — the
          open detail view brings its own (MediaDetailHero's), so this whole
          region simply isn't rendered while one's open. */}
      {!detailOpen && (
        <div style={{ flexShrink: 0 }}>
          {loading ? (
            <div className="hds-skeleton" style={{ height: '62vh', minHeight: 360 }} />
          ) : heroItem ? (
            <HeroPanel
              item={heroItem}
              detail={heroDetail}
              fading={heroFading}
              totalCandidates={heroCandidates.current.length}
              currentIdx={heroIdx.current}
              reviewCount={needsReview}
              onViewDetail={() => openDetail(
                isShow(heroItem) ? heroItem.show_id : heroItem.movie_id,
                isShow(heroItem) ? 'show' : 'movie',
              )}
              onPlay={async () => {
                const path = await resolvePlayPath(
                  isShow(heroItem) ? 'show' : 'movie',
                  isShow(heroItem) ? heroItem.show_id : heroItem.movie_id,
                )
                if (path) navigate(path)
              }}
              onDotClick={i => { startRotation(); goToHero(i) }}
              onReviewClick={() => navigate('/review')}
              castAvailable={castSession.available}
              onCast={async () => {
                // Whichever item is currently the hero — rotation or a shelf
                // hover both already retarget it, and Play already follows the
                // same item, so Cast mirrors that "current state" for free
                // rather than needing its own tracking.
                if (!castSession.connected) {
                  try { await cast.framework.CastContext.getInstance().requestSession() }
                  catch (err) {
                    // chrome.cast.ErrorCode.CANCEL is the normal "closed the
                    // device picker" case — anything else (e.g. invalid/
                    // unregistered App ID, no eligible receiver) is a real
                    // failure that was previously silent here.
                    if (err !== 'cancel') console.error('Cast requestSession() failed:', err)
                    return
                  }
                }
                // PlayerPage's own useCastSession() picks up the now-connected
                // session on mount and loads this content into it — same as if
                // Cast were pressed mid-local-playback, just starting already
                // connected. No cast-session/VOD-lifecycle duplication needed here.
                const path = await resolvePlayPath(
                  isShow(heroItem) ? 'show' : 'movie',
                  isShow(heroItem) ? heroItem.show_id : heroItem.movie_id,
                )
                if (path) navigate(path)
              }}
            />
          ) : (
            <EmptyHero onGoToSources={() => navigate('/sources')} />
          )}
        </div>
      )}

      <div
        ref={scrollContainerRef}
        onScroll={e => saveScrollPos(SCROLL_KEY, e.currentTarget.scrollTop)}
        style={{
          flex: 1, minHeight: 0,
          // MediaDetailHero owns its own internal fixed-hero/scrolling-body
          // split while open (same as LibraryPage) — this container must stop
          // being a second scroller then, or the two fight each other.
          overflowY: detailOpen ? 'hidden' : 'auto',
        }}
        className="scrollbar-dark"
      >
        {/* Crossfading content area */}
        <div style={{
          opacity:    transitioning ? 0 : 1,
          transition: 'opacity .2s ease',
          position: 'relative',
          height: detailOpen ? '100%' : undefined,
          minHeight: detailOpen ? undefined : '100%',
          background: 'var(--hds-bg)',
        }}>
          {detailOpen && detailId && detailType ? (
            <MediaDetailHero
              id={detailId}
              content_type={detailType}
              onBack={closeDetail}
              actions={media => <LibraryDetailActions id={detailId} content_type={detailType} media={media} />}
            />
          ) : (
            <>
              <QuickActionsRow onGuideClick={() => guideRef.current?.scrollIntoView({ behavior: 'smooth' })} />
              <Shelves
                loading={loading}
                recentShows={recentShows}
                recentMovies={recentMovies}
                recentlyReleased={recentlyReleased}
                recentlyAired={recentlyAired}
                continueWatching={continueWatching}
                onItemClick={openDetail}
                onNavigate={navigate}
                onItemHover={handleShelfHover}
                onRowLeave={handleShelfHoverEnd}
                libraryNames={libraryNames}
                onHideLibrary={hideLibrary}
              />
              <div ref={guideRef} style={{ padding: '0 24px 48px' }}>
                <Suspense fallback={null}><GuidePage /></Suspense>
              </div>
            </>
          )}
        </div>
      </div>
    </div>
    </FocusContext.Provider>
  )
}

// ── Hero panel ────────────────────────────────────────────────────────────────

function HeroPanel({
  item, detail, fading, totalCandidates, currentIdx,
  reviewCount, onViewDetail, onPlay, onDotClick, onReviewClick,
  castAvailable, onCast,
}: {
  item:            Show | Movie
  detail:          ShowDetail | MovieDetail | null
  fading:          boolean
  totalCandidates: number
  currentIdx:      number
  reviewCount:     number
  onViewDetail:    () => void
  onPlay:          () => void
  onDotClick:      (i: number) => void
  onReviewClick:   () => void
  // SDK-loaded gate for the Cast button, same as PlayerControls' cast?.available
  // — hidden entirely rather than shown-but-disabled when the SDK isn't usable.
  castAvailable:   boolean
  onCast:          () => void
}) {
  // Browse-mode only (the open detail view is MediaDetailHero's own pinned
  // hero instead) — heroCandidates is already pre-filtered to items that
  // have art (see the useEffect that builds it), so item.art is reliably set here.
  const backdrop = proxyArt(item)
  const bg = backdrop
    ? `url(${backdrop}) center/cover no-repeat`
    : 'linear-gradient(135deg, oklch(0.12 0.04 292) 0%, oklch(0.18 0.06 270) 50%, oklch(0.14 0.03 280) 100%)'

  const genres: string[] = detail && 'genres' in detail && Array.isArray(detail.genres)
    ? (detail.genres as string[]).slice(0, 4) : []
  const overview = detail?.overview ?? ''
  const rating   = detail?.audience_rating ?? item.audience_rating

  const play      = useFocusable<object, HTMLButtonElement>({ focusKey: 'home-hero-play',   onEnterPress: onPlay })
  const viewDetail = useFocusable<object, HTMLButtonElement>({ focusKey: 'home-hero-detail', onEnterPress: onViewDetail })
  const castBtn    = useFocusable<object, HTMLButtonElement>({ focusKey: 'home-hero-cast',    onEnterPress: onCast, focusable: castAvailable })
  const review     = useFocusable<object, HTMLButtonElement>({ focusKey: 'home-hero-review', onEnterPress: onReviewClick, focusable: reviewCount > 0 })

  return (
    <div style={{
      position: 'relative', height: '62vh', minHeight: 360,
      background: bg, flexShrink: 0,
      opacity: fading ? 0 : 1, transition: 'opacity .26s ease',
      overflow: 'hidden',
    }}>
      {/* Side dim — for text legibility over the backdrop. */}
      <div style={{
        position: 'absolute', inset: 0,
        background: 'linear-gradient(to right, oklch(0 0 0 / 0.88) 0%, oklch(0 0 0 / 0.42) 52%, transparent 100%)',
      }} />
      {/* Bottom fade-to-background — fully contained within this box (Hero is
          now its own fixed, non-scrolling flex region, not a normal-flow
          sibling of the scrollable content below it — nothing needs to
          bleed past its own edge to blend into the next element anymore). */}
      <div style={{
        position: 'absolute', left: 0, right: 0, bottom: 0, height: '30%',
        background: 'linear-gradient(to top, var(--hds-bg) 0%, transparent 100%)',
        pointerEvents: 'none',
      }} />

      {/* Text content — padding/maxWidth shrink on mobile via .hds-hero-text
          (index.css) — at 64px each side this leaves ~247px for the whole
          title/genres/overview/button column on a 375px phone. */}
      <div className="hds-hero-text" style={{
        position: 'absolute', bottom: 0, left: 0, right: 0, padding: '0 64px 56px', maxWidth: 640,
      }}>
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

        <div style={{ display: 'flex', gap: 10, flexWrap: 'wrap' }}>
          <button
            ref={play.ref} data-tv-focused={play.focused}
            style={{ ...goldBtnStyle, display: 'flex', alignItems: 'center', gap: 8 }}
            onClick={onPlay}
          >
            <svg width="12" height="12" viewBox="0 0 14 14" fill="currentColor"><path d="M3 1.5v11l9-5.5-9-5.5z" /></svg>
            Play
          </button>
          <button ref={viewDetail.ref} data-tv-focused={viewDetail.focused} style={ghostBtnStyle} onClick={onViewDetail}>View Details</button>
        </div>
      </div>

      {/* Cast — fixed top-right, not inline with Play/View Details; every
          window it's available in (here and the player) anchors it the same
          way rather than mixing it into the row of content actions. */}
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

      {/* Review notification pill — stacks below Cast when both are visible
          so they share the same top-right corner without overlapping. */}
      {reviewCount > 0 && (
        <button
          ref={review.ref} data-tv-focused={review.focused}
          onClick={onReviewClick}
          style={{
            position: 'absolute', top: castAvailable ? 60 : 18, right: 24, zIndex: 3,
            display: 'flex', alignItems: 'center', gap: 7,
            padding: '5px 12px', borderRadius: 20,
            border: '1px solid var(--hds-match-amber)',
            background: 'oklch(0.75 0.12 80 / 0.12)',
            backdropFilter: 'blur(8px)',
            color: 'var(--hds-match-amber)',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
            cursor: 'pointer', letterSpacing: '0.06em',
          }}
        >
          <svg width="10" height="10" viewBox="0 0 10 10" fill="currentColor">
            <path d="M5 1L9.33 8.5H.67L5 1z" />
            <rect x="4.5" y="4" width="1" height="2.5" fill="var(--hds-bg)" />
            <rect x="4.5" y="7" width="1" height="1" fill="var(--hds-bg)" />
          </svg>
          {reviewCount} need{reviewCount === 1 ? 's' : ''} review
        </button>
      )}

      {/* Rotation dots */}
      {totalCandidates > 1 && (
        <div style={{ position: 'absolute', bottom: 20, right: 24, display: 'flex', gap: 6, zIndex: 2 }}>
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

function EmptyHero({ onGoToSources }: { onGoToSources: () => void }) {
  return (
    <div style={{
      height: '62vh', minHeight: 360, flexShrink: 0,
      display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 16,
      background: 'linear-gradient(135deg, oklch(0.12 0.04 292), oklch(0.16 0.03 280))',
    }}>
      <div style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 15, color: 'var(--hds-txt-3)' }}>
        No content yet
      </div>
      <button style={ghostBtnStyle} onClick={onGoToSources}>Add a Source →</button>
    </div>
  )
}

// ── Quick actions ────────────────────────────────────────────────────────────
// A home for jump-to shortcuts — Guide today, more later (user-customizable).

function QuickActionsRow({ onGuideClick }: { onGuideClick: () => void }) {
  const guide = useFocusable<object, HTMLButtonElement>({ focusKey: 'home-quickaction-guide', onEnterPress: onGuideClick })

  // Hero is its own fixed flex region now (see HomePage's top-level layout)
  // — it no longer bleeds into this area via negative margin, so this just
  // needs normal breathing room, not extra clearance for a pull-up hack.
  return (
    <div style={{ padding: '20px 24px 8px' }}>
      <div style={{
        display: 'flex', gap: 10, padding: '14px 18px', borderRadius: 12,
        background: 'var(--hds-glass)', backdropFilter: 'blur(8px)', border: '1px solid var(--hds-glass-border)',
      }}>
        <button ref={guide.ref} data-tv-focused={guide.focused} onClick={onGuideClick} style={{
          display: 'flex', alignItems: 'center', gap: 8, padding: '9px 16px', borderRadius: 8, cursor: 'pointer',
          border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)', color: 'var(--hds-txt)',
          fontFamily: "'JetBrains Mono', monospace", fontSize: 12,
        }}>
          <svg width="14" height="14" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4">
            <rect x="1.5" y="2.5" width="11" height="9" rx="1.5" /><path d="M4 11.5h6" />
          </svg>
          Guide
        </button>
      </div>
    </div>
  )
}

// ── Shelves ───────────────────────────────────────────────────────────────────

function Shelves({
  loading, recentShows, recentMovies, recentlyReleased, recentlyAired, continueWatching,
  onItemClick, onNavigate, onItemHover, onRowLeave, libraryNames, onHideLibrary,
}: {
  loading:          boolean
  recentShows:      Show[]
  recentMovies:     Movie[]
  recentlyReleased: Movie[]
  recentlyAired:    Show[]
  continueWatching: WatchProgress[]
  onItemClick:      (id: string, type: 'show' | 'movie') => void
  onNavigate:       (path: string) => void
  onItemHover:      (id: string) => void
  onRowLeave:       () => void
  libraryNames:     Map<string, string>
  onHideLibrary:    (libraryId: string) => void
}) {
  // Applies a shelf's implicit filter/sort to the Library page before
  // navigating there — e.g. "Recently Added Movies" lands on Library showing
  // movies only, sorted by date added. Mutating the store synchronously
  // before navigate() is safe: LibraryPage's mount effect (loadLibraries().
  // then(() => store.fetch())) reads store state fresh on arrival.
  const continueInLibrary = (contentType: 'show' | 'movie' | 'all', sort: string) => () => {
    libraryStore.setContentType(contentType)
    libraryStore.setSort(sort)
    onNavigate('/library')
  }

  return (
    <div style={{ padding: '20px 0 48px' }}>

      {loading ? (
        <><ShelfSkeleton /><ShelfSkeleton /></>
      ) : (
        <>
          {continueWatching.length > 0 && (
            <ContinueWatchingShelf items={continueWatching} onNavigate={onNavigate} />
          )}
          {recentShows.length > 0 && (
            <Shelf
              title="Recently Added Shows"
              items={recentShows.map(s => ({
                id: s.show_id, title: s.title, year: s.year,
                thumb_url: proxyThumb(s), rating: s.audience_rating,
                content_type: 'show' as const, library_id: s.library_id,
              }))}
              onItemClick={(id, type) => onItemClick(id, type)}
              onViewAll={continueInLibrary('show', 'recently_added')}
              onItemHover={onItemHover}
              onRowLeave={onRowLeave}
              libraryNames={libraryNames}
              onHideLibrary={onHideLibrary}
            />
          )}
          {recentMovies.length > 0 && (
            <Shelf
              title="Recently Added Movies"
              items={recentMovies.map(m => ({
                id: m.movie_id, title: m.title, year: m.year,
                thumb_url: proxyThumb(m), rating: m.audience_rating,
                content_type: 'movie' as const, library_id: m.library_id,
              }))}
              onItemClick={(id, type) => onItemClick(id, type)}
              onViewAll={continueInLibrary('movie', 'recently_added')}
              onItemHover={onItemHover}
              onRowLeave={onRowLeave}
              libraryNames={libraryNames}
              onHideLibrary={onHideLibrary}
            />
          )}
          {recentlyReleased.length > 0 && (
            <Shelf
              title="Recently Released"
              items={recentlyReleased.map(m => ({
                id: m.movie_id, title: m.title, year: m.year,
                thumb_url: proxyThumb(m), rating: m.audience_rating,
                content_type: 'movie' as const, library_id: m.library_id,
              }))}
              onItemClick={(id, type) => onItemClick(id, type)}
              onViewAll={continueInLibrary('movie', 'recently_released')}
              onItemHover={onItemHover}
              onRowLeave={onRowLeave}
              libraryNames={libraryNames}
              onHideLibrary={onHideLibrary}
            />
          )}
          {/* Only shows the backend actually resolved a jump-to episode for
              (see latest_episode enrichment, ContentService.cpp) — a show
              with no valid aired episode has nothing for this shelf's
              "select it and it starts playing" behavior to do. */}
          {recentlyAired.filter(s => s.latest_episode).length > 0 && (
            <Shelf
              title="Recently Aired"
              items={recentlyAired.filter(s => s.latest_episode).map(s => ({
                id: s.show_id, title: s.title, year: s.year,
                thumb_url: proxyThumb(s), rating: s.audience_rating,
                content_type: 'show' as const, library_id: s.library_id,
                directPlayPath: `/player/episode/${s.latest_episode!.episode_id}`,
              }))}
              onItemClick={(id, type) => onItemClick(id, type)}
              onNavigate={onNavigate}
              onViewAll={continueInLibrary('show', 'recently_aired')}
              onItemHover={onItemHover}
              onRowLeave={onRowLeave}
              libraryNames={libraryNames}
              onHideLibrary={onHideLibrary}
            />
          )}
        </>
      )}
    </div>
  )
}

// ── Shelf ────────────────────────────────────────────────────────────────────

interface ShelfEntry {
  id: string; title: string; year?: number
  thumb_url?: string; rating?: number; content_type: 'show' | 'movie'
  library_id?: string
  // Recently Aired only: when set, selecting the card jumps straight to
  // playing this episode instead of opening the show's detail view (unlike
  // a typical "recently aired episodes" list, this shelf is one tile per
  // show — the tile itself carries which specific episode to play).
  directPlayPath?: string
}

function Shelf({ title, items, onItemClick, onNavigate, onViewAll, onItemHover, onRowLeave, libraryNames, onHideLibrary }: {
  title:       string
  items:       ShelfEntry[]
  onItemClick: (id: string, type: 'show' | 'movie') => void
  onNavigate?: (path: string) => void
  onViewAll?:  () => void
  onItemHover?: (id: string) => void
  onRowLeave?:  () => void
  libraryNames: Map<string, string>
  onHideLibrary: (libraryId: string) => void
}) {
  const [showArrows, setShowArrows] = useState(false)
  const travel = useTravelingFocus()
  // isFocusBoundary traps left/right within the row so arrowing off the
  // first/last card can't fall back to the library's nearest-neighbor search
  // across the whole page (which is what was launching focus into unrelated
  // shelves several rows away) — up/down stay open to move between shelves.
  const { ref: rowRef, focusKey: rowFocusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: `home-shelf-row-${title}`,
    trackChildren: true,
    isFocusBoundary: true,
    focusBoundaryDirections: ['left', 'right'],
  })
  const scroll = (d: 'left' | 'right') =>
    rowRef.current?.scrollBy({ left: d === 'right' ? 340 : -340, behavior: 'smooth' })

  return (
    <div
      style={{ padding: '0 0 8px', marginBottom: 24, position: 'relative' }}
      onMouseEnter={() => setShowArrows(true)}
      onMouseLeave={() => { setShowArrows(false); onRowLeave?.() }}
    >
      <div style={{
        display: 'flex', alignItems: 'baseline', justifyContent: 'space-between',
        padding: '0 24px', marginBottom: 14,
      }}>
        <span style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 14, fontWeight: 600,
          color: 'var(--hds-txt)', letterSpacing: '0.03em',
        }}>{title}</span>
        {onViewAll && (
          <button onClick={onViewAll} style={{
            background: 'none', border: 'none', cursor: 'pointer',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
            color: 'var(--hds-txt-3)', letterSpacing: '0.06em',
          }}>View All →</button>
        )}
      </div>

      {showArrows && <ShelfArrow side="left"  onClick={() => scroll('left')} />}
      <div ref={rowRef} style={{
        display: 'flex', gap: 12, overflowX: 'auto', overflowY: 'hidden',
        padding: '8px 24px', scrollbarWidth: 'none', position: 'relative',
      }}>
        <TravelingFocusFrame rect={travel.rect} active={travel.active} />
        <FocusContext.Provider value={rowFocusKey}>
        {items.map(item => (
          <ShelfCard
            key={item.id} item={item}
            onClick={() => item.directPlayPath ? onNavigate?.(item.directPlayPath) : onItemClick(item.id, item.content_type)}
            onHover={() => onItemHover?.(item.id)}
            onActivate={travel.activate}
            onDeactivate={travel.deactivate}
            libraryName={item.library_id ? libraryNames.get(item.library_id) : undefined}
            onHideLibrary={item.library_id ? () => onHideLibrary(item.library_id!) : undefined}
          />
        ))}
        {onViewAll && (
          <ShelfEndTile
            focusKey={`home-shelf-end-${title}`} onClick={onViewAll}
            onActivate={travel.activate} onDeactivate={travel.deactivate}
          />
        )}
        </FocusContext.Provider>
      </div>
      {showArrows && <ShelfArrow side="right" onClick={() => scroll('right')} />}
    </div>
  )
}

// Trailing tile at the end of every shelf's row — "Continue in Library"
// lands on the Library page pre-filtered/sorted to match the shelf (see each
// shelf's onViewAll closure in Shelves/HomePage). Lives in the same
// card-focus flow as ShelfCard, so it's D-pad reachable (right-arrow from
// the last real card) unlike the header's mouse-only "View All →" link.
function ShelfEndTile({ focusKey, onClick, onActivate, onDeactivate }: {
  focusKey: string; onClick: () => void
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
      onMouseLeave={() => { setHovered(false); onDeactivate() }}
      style={{
        flexShrink: 0, width: 130, aspectRatio: '2/3', borderRadius: 10, cursor: 'pointer',
        display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 10,
        border: '1px dashed var(--hds-line)', background: 'var(--hds-bg-2)', color: 'var(--hds-txt-3)',
        transform: active ? 'translateY(-4px)' : 'none',
        transition: 'transform .18s cubic-bezier(0.22,1,0.36,1)',
        textAlign: 'center', padding: '0 16px',
      }}
    >
      <svg width="22" height="22" viewBox="0 0 22 22" fill="none" stroke="currentColor" strokeWidth="1.6">
        <rect x="3" y="3" width="7" height="7" rx="1.2" /><rect x="12" y="3" width="7" height="7" rx="1.2" />
        <rect x="3" y="12" width="7" height="7" rx="1.2" /><rect x="12" y="12" width="7" height="7" rx="1.2" />
      </svg>
      <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, letterSpacing: '0.04em', lineHeight: 1.4 }}>
        Continue in Library
      </span>
    </div>
  )
}

function ShelfCard({ item, onClick, onHover, onActivate, onDeactivate, libraryName, onHideLibrary }: {
  item: ShelfEntry; onClick: () => void; onHover?: () => void
  onActivate: (el: HTMLElement | null) => void; onDeactivate: () => void
  libraryName?: string; onHideLibrary?: () => void
}) {
  const [hovered, setHovered] = useState(false)
  const [imgErr,  setImgErr]  = useState(false)
  const [menuOpen, setMenuOpen] = useState(false)
  const showImg = item.thumb_url && !imgErr

  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `home-shelf-card-${item.content_type}-${item.id}`,
    onEnterPress: onClick,
    onFocus: () => { onHover?.(); onActivate(ref.current) },
    onBlur: onDeactivate,
  })

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      onMouseEnter={() => { setHovered(true); onHover?.(); onActivate(ref.current) }}
      onMouseLeave={() => { setHovered(false); setMenuOpen(false); onDeactivate() }}
      style={{
        flexShrink: 0, width: 130, borderRadius: 10, overflow: 'hidden', cursor: 'pointer',
        transform: hovered || focused ? 'translateY(-4px)' : 'none',
        transition: 'transform .18s cubic-bezier(0.22,1,0.36,1)',
      }}
    >
      <div style={{
        aspectRatio: '2/3', width: '100%', position: 'relative',
        background: 'linear-gradient(135deg, oklch(0.18 0.03 287), oklch(0.13 0.02 285))',
        display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
      }}>
        {showImg ? (
          <img
            src={item.thumb_url} alt={item.title} onError={() => setImgErr(true)}
            style={{ width: '100%', height: '100%', objectFit: 'cover' }}
          />
        ) : (
          <span style={{
            fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700,
            fontSize: 26, color: 'var(--hds-violet)', opacity: 0.4,
          }}>
            {item.title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
        {libraryName && onHideLibrary && (hovered || focused) && (
          <button
            onClick={e => { e.stopPropagation(); setMenuOpen(o => !o) }}
            aria-label="More options"
            style={{
              position: 'absolute', top: 6, right: 6, zIndex: 2,
              width: 22, height: 22, borderRadius: 6, cursor: 'pointer',
              display: 'flex', alignItems: 'center', justifyContent: 'center',
              border: 'none', background: 'oklch(0 0 0 / 0.55)', color: '#fff', fontSize: 13, lineHeight: 1,
            }}
          >⋯</button>
        )}
        {menuOpen && libraryName && onHideLibrary && (
          <div
            onClick={e => e.stopPropagation()}
            style={{
              position: 'absolute', top: 30, right: 6, zIndex: 3, width: 150,
              borderRadius: 8, border: '1px solid var(--hds-glass-border)',
              background: 'var(--hds-bg-2)', boxShadow: '0 8px 24px oklch(0 0 0 / 0.5)',
              overflow: 'hidden',
            }}
          >
            <button
              onClick={() => { onHideLibrary(); setMenuOpen(false) }}
              style={{
                display: 'block', width: '100%', padding: '8px 10px', textAlign: 'left',
                border: 'none', background: 'none', cursor: 'pointer', color: 'var(--hds-txt-2)',
                fontFamily: "'JetBrains Mono', monospace", fontSize: 10, lineHeight: 1.4,
              }}
            >Hide "{libraryName}" from Home</button>
          </div>
        )}
        {(hovered || focused) && (
          <div style={{
            position: 'absolute', bottom: 0, left: 0, right: 0,
            background: 'linear-gradient(to top, oklch(0 0 0 / 0.85), transparent)',
            padding: '24px 10px 10px',
          }}>
            <div style={{
              fontFamily: "'Chakra Petch', sans-serif", fontSize: 10, fontWeight: 600,
              color: '#fff', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
            }}>{item.title}</div>
            {item.rating != null && (
              <div style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
                color: 'var(--hds-gold)', marginTop: 2,
              }}>★ {item.rating.toFixed(1)}</div>
            )}
          </div>
        )}
      </div>
      <div style={{ padding: '8px 4px 4px' }}>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, fontWeight: 600,
          color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }}>{item.title}</div>
        <div style={{
          fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)', marginTop: 2,
        }}>
          {item.year}{item.year && ' · '}{item.content_type}
        </div>
      </div>
    </div>
  )
}

function ContinueWatchingShelf({ items, onNavigate }: { items: WatchProgress[]; onNavigate: (path: string) => void }) {
  const [showArrows, setShowArrows] = useState(false)
  const travel = useTravelingFocus()
  const { ref: rowRef, focusKey: rowFocusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: 'home-shelf-row-continue-watching',
    trackChildren: true,
    isFocusBoundary: true,
    focusBoundaryDirections: ['left', 'right'],
  })
  const scroll = (d: 'left' | 'right') =>
    rowRef.current?.scrollBy({ left: d === 'right' ? 340 : -340, behavior: 'smooth' })

  return (
    <div
      style={{ padding: '0 0 8px', marginBottom: 24, position: 'relative' }}
      onMouseEnter={() => setShowArrows(true)}
      onMouseLeave={() => setShowArrows(false)}
    >
      <div style={{ display: 'flex', alignItems: 'baseline', padding: '0 24px', marginBottom: 14 }}>
        <span style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 14, fontWeight: 600,
          color: 'var(--hds-txt)', letterSpacing: '0.03em',
        }}>Continue Watching</span>
      </div>

      {showArrows && <ShelfArrow side="left" onClick={() => scroll('left')} />}
      <div ref={rowRef} style={{
        display: 'flex', gap: 12, overflowX: 'auto', overflowY: 'hidden',
        padding: '8px 24px', scrollbarWidth: 'none', position: 'relative',
      }}>
        <TravelingFocusFrame rect={travel.rect} active={travel.active} />
        <FocusContext.Provider value={rowFocusKey}>
        {items.map(p => (
          <ContinueWatchingCard
            key={`${p.content_type}:${p.content_id}`} item={p} onNavigate={onNavigate}
            onActivate={travel.activate} onDeactivate={travel.deactivate}
          />
        ))}
        {/* No natural single filter for Continue Watching (mixed shows/movies/
            episodes) — lands on an unfiltered Library, same as the other
            shelves' "View All" did before this feature. */}
        <ShelfEndTile
          focusKey="home-shelf-end-continue-watching" onClick={() => onNavigate('/library')}
          onActivate={travel.activate} onDeactivate={travel.deactivate}
        />
        </FocusContext.Provider>
      </div>
      {showArrows && <ShelfArrow side="right" onClick={() => scroll('right')} />}
    </div>
  )
}

function ContinueWatchingCard({ item, onNavigate, onActivate, onDeactivate }: {
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

  const go = () => onNavigate(`/player/${item.content_type}/${item.content_id}?t=${item.position_ms}`)

  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `home-cw-card-${item.content_type}-${item.content_id}`,
    onEnterPress: go,
    onFocus: () => onActivate(ref.current), onBlur: onDeactivate,
  })

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={go}
      onMouseEnter={() => { setHovered(true); onActivate(ref.current) }}
      onMouseLeave={() => { setHovered(false); onDeactivate() }}
      style={{
        flexShrink: 0, width: 130, borderRadius: 10, overflow: 'hidden', cursor: 'pointer',
        transform: hovered || focused ? 'translateY(-4px)' : 'none',
        transition: 'transform .18s cubic-bezier(0.22,1,0.36,1)',
      }}
    >
      <div style={{
        aspectRatio: '2/3', width: '100%', position: 'relative',
        background: 'linear-gradient(135deg, oklch(0.18 0.03 287), oklch(0.13 0.02 285))',
        display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
      }}>
        {thumbPath && !imgErr ? (
          <img
            src={mediaUrl(thumbPath)} alt={item.title} onError={() => setImgErr(true)}
            style={{ width: '100%', height: '100%', objectFit: 'cover' }}
          />
        ) : (
          <span style={{
            fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700,
            fontSize: 26, color: 'var(--hds-violet)', opacity: 0.4,
          }}>
            {item.title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
        {epCode && (
          <span style={{
            position: 'absolute', top: 6, left: 6,
            background: 'oklch(0 0 0 / 0.6)', borderRadius: 4, padding: '2px 6px',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'oklch(0.9 0.01 285)',
          }}>{epCode}</span>
        )}
        {item.up_next && (
          <span style={{
            position: 'absolute', top: 6, right: 6,
            background: 'var(--hds-violet)', borderRadius: 4, padding: '2px 6px',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, fontWeight: 700,
            letterSpacing: '0.04em', color: 'oklch(0.13 0.02 285)',
          }}>UP NEXT</span>
        )}
        {!item.up_next && (
          <div style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 3, background: 'oklch(0 0 0 / 0.5)' }}>
            <div style={{ height: '100%', width: `${progress * 100}%`, background: 'var(--hds-violet)' }} />
          </div>
        )}
      </div>
      <div style={{ padding: '8px 4px 4px' }}>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, fontWeight: 600,
          color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }}>{item.content_type === 'episode' ? (item.show_title || item.title) : item.title}</div>
        {item.content_type === 'episode' && (
          <div style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)', marginTop: 2,
            overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
          }}>{item.title}</div>
        )}
      </div>
    </div>
  )
}

function ShelfArrow({ side, onClick }: { side: 'left' | 'right'; onClick: () => void }) {
  return (
    <button onClick={onClick} style={{
      position: 'absolute', top: '50%', transform: 'translateY(-50%)',
      left: side === 'left' ? 4 : undefined, right: side === 'right' ? 4 : undefined,
      zIndex: 2, width: 36, height: 36, borderRadius: '50%', cursor: 'pointer',
      background: 'var(--hds-glass)', backdropFilter: 'blur(8px)',
      border: '1px solid var(--hds-glass-border)', color: 'var(--hds-txt)', fontSize: 22,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
    }}>
      {side === 'left' ? '‹' : '›'}
    </button>
  )
}


// ── Skeletons ─────────────────────────────────────────────────────────────────

function ShelfSkeleton() {
  return (
    <div style={{ marginBottom: 32 }}>
      <div className="hds-skeleton" style={{ height: 14, width: 180, borderRadius: 4, margin: '0 24px 14px' }} />
      <div style={{ display: 'flex', gap: 12, padding: '8px 24px' }}>
        {Array.from({ length: 7 }, (_, i) => (
          <div key={i} className="hds-skeleton" style={{ width: 130, aspectRatio: '2/3', borderRadius: 10, flexShrink: 0 }} />
        ))}
      </div>
    </div>
  )
}
