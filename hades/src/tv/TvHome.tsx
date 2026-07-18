import { useEffect, useRef, useState, type CSSProperties } from 'react'
import { useNavigate } from 'react-router-dom'
import { FocusContext, setFocus, doesFocusableExist } from '@noriginmedia/norigin-spatial-navigation'
import { useCastSession } from '../cast/useCastSession'
import { startVodPlayback, stopVodPlayback } from '../player/playbackApi'
import { api, mediaUrl } from '../api/client'
import type { Show, Movie, ShowDetail, MovieDetail, WatchProgress } from '../api/types'
import { resolvePlayTarget } from '../player/resolvePlayTarget'
import { useFocusable } from '../nav/useFocusable'
import { useTravelingFocus } from '../nav/useTravelingFocus'
import { TravelingFocusFrame } from '../nav/TravelingFocusFrame'
import { TvGuideSection } from './TvGuideSection'
import { rememberDetailReturn, consumeReturnFocusKey } from './tvDetailNav'
import { useHomeManifest, toClientParams } from './useHomeManifest'
import type { TvHomeRow } from '../api/types'
import styles from './TvHome.module.css'
import sharedStyles from '../channel/sharedStyles.module.css'

const HOME_FOCUS_KEY = 'TV_HOME'
const TV_HOME_PATH = '/tv'

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

  // Set only when this mount is the remote's Back arriving from a shelf
  // card's (or the hero's View Details) Detail route — consumed once so a
  // normal fresh landing on Home still gets the usual hero-Play autofocus.
  // See tvDetailNav.ts.
  const [restoreFocusKey] = useState(() => consumeReturnFocusKey(TV_HOME_PATH))

  // Hero — same rotate/hover-swap model as the desktop HomePage, minus the
  // inline detail overlay (View Details is a real /tv/library/* route here).
  const heroCandidates  = useRef<(Show | Movie)[]>([])
  const heroIdx         = useRef(0)
  const hoverRestoreRef = useRef<{ item: Show | Movie; detail: ShowDetail | MovieDetail | null; idx: number } | null>(null)
  const heroIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null)
  const [heroItem,   setHeroItem]   = useState<Show | Movie | null>(null)
  const [heroDetail, setHeroDetail] = useState<ShowDetail | MovieDetail | null>(null)
  // Continue Watching's episode entries — mirrors HomePage.tsx's heroEpisode
  // exactly, see its comment there.
  const [heroEpisode, setHeroEpisode] = useState<HeroEpisodeOverride | null>(null)
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

  // Mirrors HomePage.tsx's handleContinueWatchingHover/HoverEnd — see there
  // for why episodes go through heroEpisode instead of heroItem.
  const handleContinueWatchingFocus = (cw: WatchProgress) => {
    if (heroIntervalRef.current) clearInterval(heroIntervalRef.current)

    if (cw.content_type === 'movie') {
      setHeroEpisode(null)
      if (!hoverRestoreRef.current && heroItem) {
        hoverRestoreRef.current = { item: heroItem, detail: heroDetail, idx: heroIdx.current }
      }
      api.getMovie(cw.content_id).then(d => transitionHeroTo(d, d)).catch(() => {})
      return
    }

    api.getEpisodeBrief(cw.content_id).then(ep => {
      setHeroEpisode({
        title:       ep.title,
        metaLine:    `${ep.show_title}  ·  S${String(ep.season).padStart(2, '0')}E${String(ep.episode).padStart(2, '0')}`,
        overview:    ep.overview,
        backdropUrl: mediaUrl(`/api/episodes/${ep.episode_id}/thumb`),
      })
    }).catch(() => {})
  }

  const handleContinueWatchingBlur = () => {
    setHeroEpisode(null)
    handleShelfBlur()
  }

  // Home's manifest — which rows exist, in what order, fed by what query,
  // with what per-item action on select. See useHomeManifest.ts. Everything
  // below reacts to this instead of hardcoding shelf definitions, so a
  // server-side change to e.g. recent-aired's sort/filter, or its removal
  // entirely, needs zero changes here.
  const { rows: manifestRows, loading: manifestLoading } = useHomeManifest()
  const findRow = (id: string) => manifestRows.find(r => r.id === id)

  useEffect(() => {
    if (manifestLoading) return

    const showsRow    = findRow('recent-shows')
    const moviesRow    = findRow('recent-movies')
    const releasedRow = findRow('recent-released')
    const airedRow    = findRow('recent-aired')
    const cwRow       = findRow('continue-watching')

    Promise.all([
      showsRow    ? api.getShows(toClientParams(showsRow.dataSource?.params))     : Promise.resolve({ items: [] as Show[], total: 0 }),
      moviesRow   ? api.getMovies(toClientParams(moviesRow.dataSource?.params))   : Promise.resolve({ items: [] as Movie[], total: 0 }),
      releasedRow ? api.getMovies(toClientParams(releasedRow.dataSource?.params)).catch(() => ({ items: [] as Movie[], total: 0 })) : Promise.resolve({ items: [] as Movie[], total: 0 }),
      airedRow    ? api.getShows(toClientParams(airedRow.dataSource?.params)).catch(() => ({ items: [] as Show[], total: 0 })) : Promise.resolve({ items: [] as Show[], total: 0 }),
      cwRow       ? api.getWatchProgress().catch(() => [] as WatchProgress[]) : Promise.resolve([] as WatchProgress[]),
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
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [manifestLoading])

  // Only ever applies once — see the identical guard in TvLibrary.tsx. Falls
  // back to the hero Play button (the same target its own suppressed
  // forceFocus would have landed on) if the remembered item isn't actually
  // on Home this time — e.g. it aged out of the top-16 "recently added"
  // list between opening Detail and coming back.
  const appliedHomeRestoreRef = useRef(false)
  useEffect(() => {
    if (!restoreFocusKey || appliedHomeRestoreRef.current || loading) return
    appliedHomeRestoreRef.current = true
    setFocus(doesFocusableExist(restoreFocusKey) ? restoreFocusKey : 'tv-hero-play')
  }, [restoreFocusKey, loading])

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

  // focusKey defaults to the hero's own "View Details" button — its callers
  // (onViewDetail below) don't sit inside a shelf card and have no other
  // natural key to restore. Shelf cards pass their own tvShelfCardFocusKey.
  const goToLibrary = (id: string, type: 'show' | 'movie', focusKey = 'tv-hero-detail') => {
    rememberDetailReturn({ returnTo: TV_HOME_PATH, focusKey })
    navigate(`/tv/library/${type}/${id}`)
  }

  // The two tile actions a generic (non-continue-watching) shelf's manifest
  // row can declare — 'play-latest-episode' (Recently Aired only, and only
  // when this particular show actually has one) or the open-detail default.
  // Any other itemAction on a plain shelf row is a manifest/client vocabulary
  // drift, not a state this should silently render around.
  const resolveShelfItemOnClick = (row: TvHomeRow, item: Show | Movie): () => void => {
    const id   = isShow(item) ? item.show_id : item.movie_id
    const type: 'show' | 'movie' = isShow(item) ? 'show' : 'movie'
    if (row.itemAction === 'play-latest-episode' && isShow(item) && item.latest_episode) {
      const episodeId = item.latest_episode.episode_id
      return () => navigate(`/player/episode/${episodeId}`)
    }
    return () => goToLibrary(id, type, tvShelfCardFocusKey(row.title ?? row.id, id))
  }

  // recent-aired's real item list also filters to shows that actually have a
  // latest_episode — same filter the pre-manifest hardcoded version applied.
  const rowItems: Record<string, (Show | Movie)[]> = {
    'recent-shows':    recentShows,
    'recent-movies':   recentMovies,
    'recent-released': recentlyReleased,
    'recent-aired':    recentlyAired.filter(s => s.latest_episode),
  }

  return (
    <FocusContext.Provider value={homeFocusKey}>
    <div ref={homeRef} className={styles.homeContainer}>
      <div className={styles.heroWrap}>
        {loading ? (
          <div className={`hds-skeleton ${styles.heroSkeleton}`} />
        ) : heroItem ? (
          <TvHeroPanel
            item={heroItem}
            detail={heroDetail}
            episode={heroEpisode}
            fading={heroFading}
            totalCandidates={heroCandidates.current.length}
            currentIdx={heroIdx.current}
            autoFocusPlay={!restoreFocusKey}
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
          <div className={styles.heroEmpty}>No content yet</div>
        )}
      </div>

        <div className={styles.quickActionRow}>
          <LibraryButton onClick={() => navigate('/tv/library')} />
          <GuideButton onClick={() => guideRef.current?.scrollIntoView({ behavior: 'smooth' })} />
        </div>

      <div className={`${styles.shelvesScroll} scrollbar-dark`}>
        {loading || manifestLoading ? (
          <div className={styles.loadingText}>Loading…</div>
        ) : (
          <>
            {manifestRows.filter(row => row.type !== 'hero').map(row => {
              if (row.type === 'guide') {
                return (
                  <div key={row.id} ref={guideRef} className={styles.guideWrap}>
                    <TvGuideSection />
                  </div>
                )
              }

              if (row.id === 'continue-watching') {
                return continueWatching.length > 0 ? (
                  <TvContinueWatchingShelf
                    key={row.id}
                    items={continueWatching} onNavigate={navigate}
                    onItemFocus={handleContinueWatchingFocus} onRowBlur={handleContinueWatchingBlur}
                  />
                ) : null
              }

              const items = rowItems[row.id] ?? []
              if (items.length === 0) return null
              const title = row.title ?? row.id
              return (
                <TvShelf
                  key={row.id}
                  title={title}
                  items={items.map(item => ({
                    key: isShow(item) ? item.show_id : item.movie_id,
                    id: isShow(item) ? item.show_id : item.movie_id,
                    title: item.title, year: item.year, rating: item.audience_rating,
                    thumb_url: thumbUrl(item),
                    onClick: resolveShelfItemOnClick(row, item),
                    onFocus: () => handleShelfFocus(isShow(item) ? item.show_id : item.movie_id),
                  }))}
                  onBlur={handleShelfBlur}
                  endTile={row.endTile ? { focusKey: `tv-shelf-end-${row.id}`, onClick: () => navigate('/tv/library') } : undefined}
                />
              )
            })}
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
      className={styles.quickActionButton}
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
      className={styles.quickActionButton}
    >
      <svg width="16" height="16" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4">
        <rect x="1.5" y="2.5" width="11" height="9" rx="1.5" /><path d="M4 11.5h6" />
      </svg>
      Guide
    </button>
  )
}

// ── Hero ─────────────────────────────────────────────────────────────────────

// Mirrors HomePage.tsx's HeroEpisodeOverride exactly — see that file's
// comment for why this exists (Continue Watching's episode entries mirror
// MediaDetailHero's own hover-an-episode swap, plus a metaLine since Home
// isn't already inside that show's own page).
interface HeroEpisodeOverride {
  title:       string
  metaLine:    string
  overview:    string
  backdropUrl: string | null
}

function TvHeroPanel({ item, detail, episode, fading, totalCandidates, currentIdx, autoFocusPlay, onViewDetail, onPlay, onCast, castAvailable, onDotClick }: {
  item: Show | Movie
  detail: ShowDetail | MovieDetail | null
  episode?: HeroEpisodeOverride | null
  fading: boolean
  totalCandidates: number
  currentIdx: number
  // False while TvHome is restoring focus to a specific shelf/hero-detail
  // card on the way back from Detail — otherwise this button's own
  // forceFocus would win the race and steal it back every time.
  autoFocusPlay: boolean
  onViewDetail: () => void
  onPlay: () => void
  onCast: () => void
  castAvailable: boolean
  onDotClick: (i: number) => void
}) {
  const backdrop = episode ? episode.backdropUrl : artUrl(item)
  // Backdrop is a genuinely dynamic runtime value (an arbitrary image URL, or
  // a fallback gradient using the shared --hds-backdrop-fallback-* tokens
  // when there's no art) — kept as a targeted inline style rather than a
  // static class for that reason, same pattern as TvLibraryDetail's backdrop.
  const heroBgStyle: CSSProperties = backdrop
    ? { backgroundImage: `url(${backdrop})`, backgroundPosition: 'center', backgroundSize: 'cover', backgroundRepeat: 'no-repeat' }
    : { background: 'linear-gradient(135deg, var(--hds-backdrop-fallback-1) 0%, var(--hds-backdrop-fallback-2) 50%, var(--hds-backdrop-fallback-3) 100%)' }

  const genres: string[] = !episode && detail && 'genres' in detail && Array.isArray(detail.genres) ? (detail.genres as string[]).slice(0, 4) : []
  const overview     = episode ? episode.overview : (detail?.overview ?? '')
  const displayTitle = episode ? episode.title : item.title
  const rating   = detail?.audience_rating ?? item.audience_rating

  const play       = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-hero-play',   onEnterPress: onPlay,       forceFocus: autoFocusPlay })
  const viewDetail = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-hero-detail', onEnterPress: onViewDetail })
  const castBtn    = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-hero-cast',   onEnterPress: onCast,       focusable: castAvailable })

  return (
    <div className={`${styles.heroRoot} ${fading ? styles.heroRootFading : ''}`} style={heroBgStyle}>
      <div className={styles.heroScrimH} />
      <div className={styles.heroScrimV} />

      <div className={styles.heroTextBlock}>
        {genres.length > 0 && (
          <div className={styles.heroGenreRow}>
            {genres.map(g => (
              <span key={g} className={styles.heroGenreChip}>{g}</span>
            ))}
          </div>
        )}

        <h1 className={`${styles.heroTitle} ${styles.textShadowHero}`}>{displayTitle}</h1>

        <div className={`${styles.heroMetaRow} ${styles.textShadowHero}`}>
          {episode ? (
            <span>{episode.metaLine}</span>
          ) : (
            <>
              {item.year && <span>{item.year}</span>}
              {rating != null && <span className={styles.heroRatingText}>★ {rating.toFixed(1)}</span>}
              <span className={styles.heroContentTypeText}>{'show_id' in item ? 'series' : 'film'}</span>
            </>
          )}
        </div>

        {overview && (
          <p className={`${styles.heroOverview} ${styles.textShadowHero}`}>{overview}</p>
        )}

        <div className={styles.heroButtonRow}>
          <button
            ref={play.ref} data-tv-focused={play.focused}
            onClick={onPlay}
            className={`${sharedStyles.goldBtn} ${styles.heroPlayBtn}`}
          >
            <svg width="12" height="12" viewBox="0 0 14 14" fill="currentColor"><path d="M3 1.5v11l9-5.5-9-5.5z" /></svg>
            Play
          </button>
          <button
            ref={viewDetail.ref} data-tv-focused={viewDetail.focused}
            onClick={onViewDetail}
            className={`${sharedStyles.ghostBtn} ${styles.heroDetailBtn}`}
          >View Details</button>
        </div>
      </div>

      {/* Cast — fixed top-right, same anchor as the desktop HomePage hero. */}
      {castAvailable && (
        <button
          ref={castBtn.ref} data-tv-focused={castBtn.focused}
          onClick={onCast}
          aria-label="Cast"
          className={styles.heroCastButton}
        >
          <svg width="15" height="15" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.4">
            <rect x="2" y="2.5" width="14" height="10" rx="1.2" />
            <path d="M2 15.5a4 4 0 0 0-2-3.5M2 12.5a1.5 1.5 0 0 1 1.5 1.5" fill="none" strokeLinecap="round" />
            <circle cx="2.3" cy="15.5" r="0.9" fill="currentColor" stroke="none" />
          </svg>
        </button>
      )}

      {totalCandidates > 1 && (
        <div className={styles.heroDots}>
          {Array.from({ length: Math.min(totalCandidates, 8) }, (_, i) => (
            <button
              key={i} onClick={() => onDotClick(i)}
              className={`${styles.heroDot} ${i === currentIdx ? styles.heroDotActive : ''}`}
            />
          ))}
        </div>
      )}
    </div>
  )
}

// ── Shelves ──────────────────────────────────────────────────────────────────

interface TvShelfEntry {
  key: string; id: string; title: string; year?: number; rating?: number
  thumb_url?: string; onClick: () => void; onFocus?: () => void
}

// Shared by TvShelfCard (to register) and TvHome's onClick handlers (to
// remember, before navigating to Detail, which key to restore focus to on
// the way back — see tvDetailNav.ts). Scoped by shelf title as well as id:
// the same movie can legitimately appear in more than one shelf (e.g.
// Recently Added and Recently Released), and focus keys must be unique.
function tvShelfCardFocusKey(shelfTitle: string, id: string) {
  return `tv-shelf-card-${shelfTitle}-${id}`
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
    <div className={styles.shelfWrap} onMouseLeave={onBlur}>
      <div className={styles.shelfTitle}>{title}</div>
      <div ref={rowRef} className={styles.shelfRow}>
        <TravelingFocusFrame rect={travel.rect} active={travel.active} />
        <FocusContext.Provider value={rowFocusKey}>
        {items.map(item => (
          <div key={item.key} className={styles.shelfCardWrap}>
            <TvShelfCard {...item} shelfTitle={title} onBlur={onBlur} onActivate={travel.activate} onDeactivate={travel.deactivate} />
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

function TvShelfCard({ id, title, year, rating, thumb_url, shelfTitle, onClick, onFocus, onBlur, onActivate, onDeactivate }: TvShelfEntry & {
  shelfTitle: string; onBlur?: () => void; onActivate: (el: HTMLElement | null) => void; onDeactivate: () => void
}) {
  const [hovered, setHovered] = useState(false)
  const [imgErr,  setImgErr]  = useState(false)
  const showImg = thumb_url && !imgErr
  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: tvShelfCardFocusKey(shelfTitle, id), onEnterPress: onClick,
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
      className={`${styles.shelfCard} ${active ? styles.shelfCardActive : ''}`}
    >
      <div className={styles.shelfCardPoster}>
        {showImg ? (
          <img src={thumb_url} alt={title} onError={() => setImgErr(true)} className={styles.shelfCardImg} />
        ) : (
          <span className={styles.shelfCardMonogram}>
            {title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
        {active && (
          <div className={styles.shelfCardHoverOverlay}>
            <div className={styles.shelfCardHoverTitle}>{title}</div>
            {rating != null && (
              <div className={styles.shelfCardHoverRating}>★ {rating.toFixed(1)}</div>
            )}
          </div>
        )}
      </div>
      <div className={styles.shelfCardInfo}>
        <div className={styles.shelfCardTitle}>{title}</div>
        {year && <div className={styles.shelfCardYear}>{year}</div>}
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
      className={`${styles.endTile} ${active ? styles.endTileActive : ''}`}
    >
      <svg width="26" height="26" viewBox="0 0 22 22" fill="none" stroke="currentColor" strokeWidth="1.6">
        <rect x="3" y="3" width="7" height="7" rx="1.2" /><rect x="12" y="3" width="7" height="7" rx="1.2" />
        <rect x="3" y="12" width="7" height="7" rx="1.2" /><rect x="12" y="12" width="7" height="7" rx="1.2" />
      </svg>
      <span className={styles.endTileLabel}>
        Continue in Library
      </span>
    </div>
  )
}

// ── Continue Watching ────────────────────────────────────────────────────────

function TvContinueWatchingShelf({ items, onNavigate, onItemFocus, onRowBlur }: {
  items: WatchProgress[]; onNavigate: (path: string) => void
  onItemFocus?: (item: WatchProgress) => void; onRowBlur?: () => void
}) {
  const travel = useTravelingFocus()
  const { ref: rowRef, focusKey: rowFocusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: 'tv-shelf-row-continue-watching',
    trackChildren: true,
    isFocusBoundary: true,
    focusBoundaryDirections: ['left', 'right'],
  })
  return (
    <div className={styles.shelfWrap} onMouseLeave={onRowBlur}>
      <div className={styles.shelfTitle}>Continue Watching</div>
      <div ref={rowRef} className={styles.continueWatchingRow}>
        <TravelingFocusFrame rect={travel.rect} active={travel.active} />
        <FocusContext.Provider value={rowFocusKey}>
        {items.map(p => (
          <div key={`${p.content_type}:${p.content_id}`} className={styles.shelfCardWrap}>
            <TvContinueWatchingCard
              item={p} onNavigate={onNavigate} onActivate={travel.activate} onDeactivate={travel.deactivate}
              onFocus={onItemFocus} onBlur={onRowBlur}
            />
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

function TvContinueWatchingCard({ item, onNavigate, onActivate, onDeactivate, onFocus, onBlur }: {
  item: WatchProgress; onNavigate: (path: string) => void
  onActivate: (el: HTMLElement | null) => void; onDeactivate: () => void
  onFocus?: (item: WatchProgress) => void; onBlur?: () => void
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
    onFocus: () => { onActivate(ref.current); onFocus?.(item) }, onBlur: onDeactivate,
  })
  const active = hovered || focused

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={go}
      onMouseEnter={() => { setHovered(true); onActivate(ref.current); onFocus?.(item) }}
      onMouseLeave={() => { setHovered(false); onDeactivate(); onBlur?.() }}
      className={`${styles.shelfCard} ${active ? styles.shelfCardActive : ''}`}
    >
      <div className={styles.shelfCardPoster}>
        {thumbPath && !imgErr ? (
          <img src={mediaUrl(thumbPath)} alt={title} onError={() => setImgErr(true)} className={styles.shelfCardImg} />
        ) : (
          <span className={styles.shelfCardMonogram}>
            {title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
        {epCode && (
          <span className={styles.cwEpBadge}>{epCode}</span>
        )}
        {item.up_next && (
          <span className={styles.cwUpNextBadge}>UP NEXT</span>
        )}
        {!item.up_next && (
          <div className={styles.cwProgressTrack}>
            <div className={styles.cwProgressFill} style={{ width: `${progress * 100}%` }} />
          </div>
        )}
      </div>
      <div className={styles.shelfCardInfo}>
        <div className={styles.shelfCardTitle}>{title}</div>
        {item.content_type === 'episode' && (
          <div className={styles.cwSubtitle}>{item.title}</div>
        )}
      </div>
    </div>
  )
}
