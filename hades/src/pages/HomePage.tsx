import {useEffect, useRef, useState, type CSSProperties} from 'react'
import { useNavigate } from 'react-router-dom'
import { api, mediaUrl } from '../api/client'
import type {
    Show,
    Movie,
    MixedMediaItem,
    ShowDetail,
    MovieDetail,
    ScraperStats,
    WatchProgress,
    HomePlaylistShelf,
    WatchTogetherSession
} from '../api/types'
import { resolvePlayPath } from '../player/resolvePlayTarget'
import {joinWatchTogether} from '../player/watchTogetherApi'
import { MediaDetailHero } from '../components/media/MediaDetailHero'
import { LibraryDetailActions, PlayAction, WatchTogetherAction } from '../components/media/LibraryDetailActions'
import { getScrollPos, saveScrollPos } from '../hooks/scrollMemory'
import {useAuth} from '../auth/AuthContext'
import { ghostBtnStyle, goldBtnStyle, heroTextShadow } from '../channel/styles'
import { FocusContext } from '@noriginmedia/norigin-spatial-navigation'
import { useFocusable } from '../nav/useFocusable'
import { useTravelingFocus } from '../nav/useTravelingFocus'
import { TravelingFocusFrame } from '../nav/TravelingFocusFrame'
import { libraryStore } from '../stores/LibraryStore'
import { useCastSession } from '../cast/useCastSession'
import styles from './HomePage.module.css'

const HOME_FOCUS_KEY = 'HOME'

const SCROLL_KEY = 'home'

// ── Shelf definitions ────────────────────────────────────────────────────────
// One shelf = one filter-set (contentType + sort + an optional canon filter
// string — see components/media/filterSyntax.ts) plus a tile cap. "Continue
// in Library" (ShelfEndTile, via Shelves' continueInLibrary) applies this
// exact same filter-set to the Library page instead of just contentType/sort,
// so the tile always reproduces what the shelf itself was built from. None of
// today's shelves need extra criteria beyond contentType/sort (filter: ''),
// but a future shelf variant (e.g. genre- or library-scoped) is just adding a
// def here rather than new state + a bespoke fetch + bespoke tile wiring.
interface HomeShelfDef {
  title:       string
  contentType: 'show' | 'movie' | 'all'
  sort:        string
  filter:      string
  limit:       number
}

const HOME_SHELVES: Record<'recentShows' | 'recentMovies' | 'recentlyReleased' | 'recentlyAired', HomeShelfDef> = {
  recentShows:      { title: 'Recently Added Shows',  contentType: 'show',  sort: 'recently_added',    filter: '', limit: 24 },
  recentMovies:     { title: 'Recently Added Movies', contentType: 'movie', sort: 'recently_added',    filter: '', limit: 16 },
  recentlyReleased: { title: 'Recently Released',     contentType: 'movie', sort: 'recently_released', filter: '', limit: 16 },
  recentlyAired:    { title: 'Recently Aired',        contentType: 'show',  sort: 'recently_aired',    filter: '', limit: 16 },
}

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

function proxyMixedThumb(item: MixedMediaItem) {
    if (!item.thumb) return undefined
    return mediaUrl(`/api/${item.content_type}s/${item.id}/thumb`)
}

// MixedMediaItem -> ShelfEntry — a custom shelf renders every content_type
// (show/movie/episode) through the same Shelf/ShelfCard, so an episode tile
// just carries a directPlayPath (jump straight into playback, same as the
// built-in Recently Aired shelf — there's no episode detail page to open)
// plus its show title/S-E code for display, same convention as
// ContinueWatchingCard's title/subtitle split.
function mixedToShelfEntry(item: MixedMediaItem): ShelfEntry {
    if (item.content_type === 'episode') {
        return {
            id: item.id, title: item.title, thumb_url: proxyMixedThumb(item),
            content_type: 'episode', library_id: item.library_id,
            directPlayPath: `/player/episode/${item.id}`,
            episodeCode: `S${String(item.season ?? 0).padStart(2, '0')}E${String(item.episode ?? 0).padStart(2, '0')}`,
            showTitle: item.show_title,
        }
    }
    return {
        id: item.id, title: item.title, year: item.year,
        thumb_url: proxyMixedThumb(item), rating: item.audience_rating,
        content_type: item.content_type, library_id: item.library_id,
    }
}

// Show/Movie -> MixedMediaItem — lets a custom shelf's items be one uniform
// shape regardless of its smart_type, instead of splitting shelves by type.
function showToMixed(s: Show): MixedMediaItem {
    return {
        content_type: 'show', id: s.show_id, title: s.title, thumb: s.thumb, library_id: s.library_id,
        year: s.year, audience_rating: s.audience_rating,
        watched: s.watched_episode_count > 0, view_count: s.watched_episode_count, episode_count: s.episode_count,
    }
}

function movieToMixed(m: Movie): MixedMediaItem {
    return {
        content_type: 'movie', id: m.movie_id, title: m.title, thumb: m.thumb, library_id: m.library_id,
        year: m.year, audience_rating: m.audience_rating, watched: !!m.watched, view_count: m.view_count ?? 0,
    }
}

// ── Component ────────────────────────────────────────────────────────────────

export default function HomePage() {
  const navigate = useNavigate()
  const scrollContainerRef = useRef<HTMLDivElement>(null)
  const savedScrollY       = useRef(0)
  const restoredScrollRef  = useRef(false)
  const allItemsRef        = useRef<Map<string, Show | Movie>>(new Map())
  const castSession        = useCastSession()

  // Data
  const [recentShows,      setRecentShows]      = useState<Show[]>([])
  const [recentMovies,     setRecentMovies]     = useState<Movie[]>([])
  const [recentlyReleased, setRecentlyReleased] = useState<Movie[]>([])
  const [recentlyAired,    setRecentlyAired]    = useState<Show[]>([])
  const [continueWatching, setContinueWatching] = useState<WatchProgress[]>([])
  const [watchTogether,    setWatchTogether]     = useState<WatchTogetherSession[]>([])
  const [stats,            setStats]            = useState<ScraperStats | null>(null)
  const [loading,          setLoading]          = useState(true)
  const [libraryNames,     setLibraryNames]     = useState<Map<string, string>>(new Map())
  // User-defined shelves (smart playlists with show_on_home=true) — see the
  // data-load effect's second fetch wave below for why this is separate from
  // the built-in shelves' state (variable count, not known until the first
    // /api/home-playlists round trip resolves). One array in listHomeShelves'
    // home_order (not split by smart_type/smart_expand_episodes into separate
    // arrays) so a user's drag-ordering is never silently discarded across a
    // type boundary at render time; one uniform MixedMediaItem shape regardless
    // of a shelf's own smart_type (show/movie/mixed) or expand-to-episodes
    // setting — see showToMixed/movieToMixed/mixedToShelfEntry — rather than
    // splitting shelves by type.
    const [customShelves, setCustomShelves] = useState<{ shelf: HomePlaylistShelf; items: MixedMediaItem[] }[]>([])

  // Hero
  const heroCandidates    = useRef<(Show | Movie)[]>([])
  const heroIdx           = useRef(0)
  const hoverRestoreRef   = useRef<{ item: Show | Movie; detail: ShowDetail | MovieDetail | null; idx: number } | null>(null)
  const [heroItem,   setHeroItem]   = useState<Show | Movie | null>(null)
  const [heroDetail, setHeroDetail] = useState<ShowDetail | MovieDetail | null>(null)
  const [heroFading, setHeroFading] = useState(false)
  const heroIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null)
  // Continue Watching's episode entries — see HeroEpisodeOverride's comment.
  // Layered on top of heroItem/heroDetail rather than routed through them:
  // an episode hover never touches the rotating hero's own state, so ending
  // it is just clearing this back to null, no restore bookkeeping needed
  // (unlike a movie hover, which does go through heroItem/hoverRestoreRef).
  const [heroEpisode, setHeroEpisode] = useState<HeroEpisodeOverride | null>(null)

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
      api.getShows({ limit: HOME_SHELVES.recentShows.limit, sort: HOME_SHELVES.recentShows.sort, home: true, hideEmpty: true }),
      api.getMovies({ limit: HOME_SHELVES.recentMovies.limit, sort: HOME_SHELVES.recentMovies.sort, home: true, hideEmpty: true }),
      api.getMovies({ limit: HOME_SHELVES.recentlyReleased.limit, sort: HOME_SHELVES.recentlyReleased.sort, home: true, hideEmpty: true }).catch(() => ({ items: [] as Movie[], total: 0 })),
      api.getShows({ limit: HOME_SHELVES.recentlyAired.limit, sort: HOME_SHELVES.recentlyAired.sort, home: true, hideEmpty: true }).catch(() => ({ items: [] as Show[], total: 0 })),
      api.getScraperStats().catch(() => null),
      api.getWatchProgress().catch(() => []),
      api.getAllLibraries().catch(() => []),
      api.getHomePlaylists().catch(() => [] as HomePlaylistShelf[]),
      api.getActiveWatchTogether().catch(() => [] as WatchTogetherSession[]),
    ]).then(([sr, mr, rr, ra, st, cw, libs, homeShelves, wt]) => {
      setRecentShows(sr.items)
      setRecentMovies(mr.items)
      setRecentlyReleased(rr.items)
      setRecentlyAired(ra.items)
      setStats(st)
      setContinueWatching(cw)
      setWatchTogether(wt)
      setLibraryNames(new Map(libs.map(l => [l.library_id, l.display_name])))

      sr.items.forEach(s => allItemsRef.current.set(s.show_id, s))
      mr.items.forEach(m => allItemsRef.current.set(m.movie_id, m))
      rr.items.forEach(m => allItemsRef.current.set(m.movie_id, m))
      ra.items.forEach(s => allItemsRef.current.set(s.show_id, s))

      const withArt = [...sr.items.filter(s => s.art), ...mr.items.filter(m => m.art)]
      heroCandidates.current = withArt
      const first = withArt[0] ?? sr.items[0]
      if (first) { heroIdx.current = 0; setHeroItem(first); loadHeroDetail(first) }

      // Second wave: each custom shelf's items, fetched the same way the
        // built-in shelves above are — not known until the home-playlists
        // list itself resolves (already in home_order). Every shelf ends up
        // as MixedMediaItem[] regardless of its own smart_type/expand setting:
        // 'mixed' and show+smart_expand_episodes shelves go through the
        // unified index+hydrate flow (truly interleaved at the actual
        // displayed granularity — movie+episode, not movie+whole-show — see
        // kairos's MixedSort.h), plain show/movie shelves through
        // getShows/getMovies converted via showToMixed/movieToMixed.
      Promise.all(homeShelves.map(shelf => {
          if (shelf.smart_type === 'mixed' || (shelf.smart_type === 'show' && shelf.smart_expand_episodes)) {
              return api.getMixedMediaIndex({
                  sort: shelf.smart_sort, sort_dir: shelf.smart_sort_dir || undefined, filter: shelf.filter_expr,
                  home: true, hideEmpty: true, expandEpisodes: true,
                  includeMovies: shelf.smart_type === 'mixed',
              })
                  .then(idx => api.getMixedMediaTiles(idx.items.slice(0, shelf.home_tile_limit).map(e => ({
                      content_type: e.content_type,
                      id: e.id
                  }))))
                  .then(r => ({shelf, items: r.items}))
                  .catch(() => ({shelf, items: [] as MixedMediaItem[]}))
          }
        const params = { limit: shelf.home_tile_limit, sort: shelf.smart_sort, filter: shelf.filter_expr, home: true, hideEmpty: true }
        const req = shelf.smart_type === 'show' ? api.getShows(params) : api.getMovies(params)
          return req
              .then(r => ({
                  shelf,
                  items: (r.items as (Show | Movie)[]).map(item => isShow(item) ? showToMixed(item) : movieToMixed(item))
              }))
              .catch(() => ({shelf, items: [] as MixedMediaItem[]}))
      })).then(results => {
          setCustomShelves(results.filter(r => r.items.length > 0))
      })
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

    // Marks the currently-rotating hero as the one to restore on hover-end,
    // if nothing's already pending one (moving straight from card to card
    // shouldn't overwrite the *original* pre-hover hero with an intermediate
    // hovered one).
    const beginHeroOverride = () => {
    if (heroIntervalRef.current) clearInterval(heroIntervalRef.current)
    if (!hoverRestoreRef.current && heroItem) {
      hoverRestoreRef.current = { item: heroItem, detail: heroDetail, idx: heroIdx.current }
    }
    }

    const handleShelfHover = (item: ShelfEntry) => {
        if (detailOpen) return
        // Episode tiles (a custom smart-shelf's smart_expand_episodes items —
        // see mixedToShelfEntry) have no Show|Movie shape for heroItem to hold,
        // so they go through heroEpisode instead, fetched fresh the same way
        // Continue Watching's episode entries do.
        if (item.content_type === 'episode') {
            beginHeroOverride()
            api.getEpisodeBrief(item.id).then(ep => {
                setHeroEpisode({
                    title: ep.title,
                    metaLine: `${ep.show_title}  ·  S${String(ep.season).padStart(2, '0')}E${String(ep.episode).padStart(2, '0')}`,
                    overview: ep.overview,
                    backdropUrl: mediaUrl(`/api/episodes/${ep.episode_id}/thumb`),
                })
            }).catch(() => {
            })
            return
        }
        const full = allItemsRef.current.get(item.id)
        if (!full || !full.art) return
        beginHeroOverride()
        transitionHeroTo(full, null)
  }

  const handleShelfHoverEnd = () => {
      setHeroEpisode(null)
    if (hoverRestoreRef.current) {
      const { item, detail, idx } = hoverRestoreRef.current
      heroIdx.current = idx
      transitionHeroTo(item, detail)
      hoverRestoreRef.current = null
    }
    if (!detailOpen) startRotation()
  }

  // Continue Watching hover — movie entries reuse the exact same heroItem/
  // hoverRestoreRef swap as a regular shelf hover (fetching detail fresh
  // since a WatchProgress entry isn't in allItemsRef); episodes go through
  // heroEpisode instead (see its declaration's comment) rather than trying
  // to shoehorn an episode into the Show|Movie-shaped heroItem.
  const handleContinueWatchingHover = (cw: WatchProgress) => {
    if (detailOpen) return

    if (cw.content_type === 'movie') {
        beginHeroOverride()
      api.getMovie(cw.content_id).then(d => transitionHeroTo(d, d)).catch(() => {})
      return
    }

      beginHeroOverride()
    api.getEpisodeBrief(cw.content_id).then(ep => {
      setHeroEpisode({
        title:       ep.title,
        metaLine:    `${ep.show_title}  ·  S${String(ep.season).padStart(2, '0')}E${String(ep.episode).padStart(2, '0')}`,
        overview:    ep.overview,
        backdropUrl: mediaUrl(`/api/episodes/${ep.episode_id}/thumb`),
      })
    }).catch(() => {})
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
      // Episode tiles carry the parent show's library_id (see hydrateMixed),
      // so this purges those too, not just show/movie tiles.
      setCustomShelves(shelves => shelves.map(s => ({...s, items: s.items.filter(x => x.library_id !== libraryId)})))
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
    <div ref={homeRef} className={styles.pageRoot}>
      {/* Hero — a fixed, non-scrolling region; only the content below it scrolls.
          Plain flexbox, not JS-driven, so mouse-wheel/trackpad scrolling gets
          the same behavior as D-pad nav for free. Browse-mode only — the
          open detail view brings its own (MediaDetailHero's), so this whole
          region simply isn't rendered while one's open. */}
      {!detailOpen && (
        <div className={styles.heroWrap}>
          {loading ? (
            <div className={`hds-skeleton ${styles.heroSkeletonBox}`} />
          ) : heroItem ? (
            <HeroPanel
              item={heroItem}
              detail={heroDetail}
              episode={heroEpisode}
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
        // MediaDetailHero owns its own internal fixed-hero/scrolling-body
        // split while open (same as LibraryPage) — this container must stop
        // being a second scroller then, or the two fight each other.
        className={`${styles.scrollArea} ${detailOpen ? styles.scrollAreaFixed : styles.scrollAreaScrollable} scrollbar-dark`}
      >
        {/* Crossfading content area */}
        <div className={`${styles.crossfade} ${transitioning ? styles.crossfadeHidden : styles.crossfadeVisible} ${detailOpen ? styles.crossfadeDetailHeight : styles.crossfadeShelvesHeight}`}>
          {detailOpen && detailId && detailType ? (
            <MediaDetailHero
              id={detailId}
              content_type={detailType}
              onBack={closeDetail}
              playButton={<>
                <PlayAction id={detailId} content_type={detailType} />
                <WatchTogetherAction id={detailId} content_type={detailType} />
              </>}
              actions={media => <LibraryDetailActions id={detailId} content_type={detailType} media={media} />}
            />
          ) : (
            <>
              <Shelves
                loading={loading}
                recentShows={recentShows}
                recentMovies={recentMovies}
                recentlyReleased={recentlyReleased}
                recentlyAired={recentlyAired}
                continueWatching={continueWatching}
                watchTogether={watchTogether}
                onCloseWatchTogether={id => setWatchTogether(prev => prev.filter(s => s.session_id !== id))}
                customShelves={customShelves}
                onItemClick={openDetail}
                onNavigate={navigate}
                onItemHover={handleShelfHover}
                onRowLeave={handleShelfHoverEnd}
                onContinueWatchingHover={handleContinueWatchingHover}
                onContinueWatchingHoverEnd={handleShelfHoverEnd}
                libraryNames={libraryNames}
                onHideLibrary={hideLibrary}
              />
            </>
          )}
        </div>
      </div>
    </div>
    </FocusContext.Provider>
  )
}

// ── Hero panel ────────────────────────────────────────────────────────────────

// Continue Watching's episode entries mirror MediaDetailHero's own
// hover-an-episode swap (title/overview/backdrop swap to the episode's own;
// everything else about the "page" stays put) — the one difference is Home
// isn't already inside that show's own page the way the season shelves are,
// so there's no implicit "which show is this" context; metaLine fills that
// gap in the exact slot the year/rating/type row would otherwise occupy.
interface HeroEpisodeOverride {
  title:       string
  metaLine:    string
  overview:    string
  backdropUrl: string | null
}

function HeroPanel({
  item, detail, episode, fading, totalCandidates, currentIdx,
  reviewCount, onViewDetail, onPlay, onDotClick, onReviewClick,
  castAvailable, onCast,
}: {
  item:            Show | Movie
  detail:          ShowDetail | MovieDetail | null
  episode?:        HeroEpisodeOverride | null
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
  const backdrop = episode ? episode.backdropUrl : proxyArt(item)
  // Genuinely dynamic per-render value (an arbitrary image URL, or a fallback
  // gradient using the shared --hds-backdrop-fallback-* tokens when there's
  // no art) — kept as a targeted inline style rather than a static class for
  // that reason, same pattern as TvHome.tsx's hero backdrop.
  const heroBgStyle: CSSProperties = backdrop
    ? { backgroundImage: `url(${backdrop})`, backgroundPosition: 'center', backgroundSize: 'cover', backgroundRepeat: 'no-repeat' }
    : { background: 'linear-gradient(135deg, var(--hds-backdrop-fallback-1) 0%, var(--hds-backdrop-fallback-2) 50%, var(--hds-backdrop-fallback-3) 100%)' }

  const genres: string[] = !episode && detail && 'genres' in detail && Array.isArray(detail.genres)
    ? (detail.genres as string[]).slice(0, 4) : []
  const overview     = episode ? episode.overview : (detail?.overview ?? '')
  const displayTitle = episode ? episode.title : item.title
  const rating   = detail?.audience_rating ?? item.audience_rating

  const play      = useFocusable<object, HTMLButtonElement>({ focusKey: 'home-hero-play',   onEnterPress: onPlay })
  const viewDetail = useFocusable<object, HTMLButtonElement>({ focusKey: 'home-hero-detail', onEnterPress: onViewDetail })
  const castBtn    = useFocusable<object, HTMLButtonElement>({ focusKey: 'home-hero-cast',    onEnterPress: onCast, focusable: castAvailable })
  const review     = useFocusable<object, HTMLButtonElement>({ focusKey: 'home-hero-review', onEnterPress: onReviewClick, focusable: reviewCount > 0 })

  return (
    <div className={`${styles.heroRoot} ${fading ? styles.heroRootFading : ''}`} style={heroBgStyle}>
      {/* Side dim — for text legibility over the backdrop. */}
      <div className={styles.heroScrimH} />
      {/* Bottom fade-to-background — fully contained within this box (Hero is
          now its own fixed, non-scrolling flex region, not a normal-flow
          sibling of the scrollable content below it — nothing needs to
          bleed past its own edge to blend into the next element anymore). */}
      <div className={styles.heroScrimV} />

      {/* Text content — padding/maxWidth shrink on mobile via .hds-hero-text
          (index.css) — at 64px each side this leaves ~247px for the whole
          title/genres/overview/button column on a 375px phone. */}
      <div className={`hds-hero-text ${styles.heroTextBlock}`}>
        {genres.length > 0 && (
          <div className={styles.heroGenreRow}>
            {genres.map(g => (
              <span key={g} className={styles.heroGenreChip}>{g}</span>
            ))}
          </div>
        )}

        <h1 className={styles.heroTitle}>{displayTitle}</h1>

        <div className={styles.heroMetaRow}>
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
          <p className={styles.heroOverview}>{overview}</p>
        )}

        <div className={styles.heroButtonRow}>
          <button
            ref={play.ref} data-tv-focused={play.focused}
            style={goldBtnStyle}
            className={styles.heroPlayBtn}
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
          className={styles.heroCastButton}
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
          className={`${styles.heroReviewButton} ${castAvailable ? styles.heroReviewButtonWithCast : styles.heroReviewButtonNoCast}`}
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

function EmptyHero({ onGoToSources }: { onGoToSources: () => void }) {
  return (
    <div className={styles.emptyHero}>
      <div className={styles.emptyHeroText}>
        No content yet
      </div>
      <button style={ghostBtnStyle} onClick={onGoToSources}>Add a Source →</button>
    </div>
  )
}

// ── Shelves ───────────────────────────────────────────────────────────────────

function Shelves({
  loading, recentShows, recentMovies, recentlyReleased, recentlyAired, continueWatching,
                     watchTogether, onCloseWatchTogether,
  customShelves,
  onItemClick, onNavigate, onItemHover, onRowLeave,
  onContinueWatchingHover, onContinueWatchingHoverEnd,
  libraryNames, onHideLibrary,
}: {
  loading:          boolean
  recentShows:      Show[]
  recentMovies:     Movie[]
  recentlyReleased: Movie[]
  recentlyAired:    Show[]
  continueWatching: WatchProgress[]
  watchTogether:    WatchTogetherSession[]
    onCloseWatchTogether: (sessionId: string) => void
  // User-defined shelves — each one *is* a smart playlist with
  // show_on_home=true (see PlaylistPage.tsx's "Show on Home" toggle); there's
  // no separate shelf-definition concept. Variable length, fetched as a
  // second wave once HomePage knows how many exist (see its data-load effect).
    // One uniform MixedMediaItem shape regardless of the shelf's own
    // smart_type or expand-to-episodes setting — see
    // showToMixed/movieToMixed/mixedToShelfEntry.
    customShelves: { shelf: HomePlaylistShelf; items: MixedMediaItem[] }[]
  onItemClick:      (id: string, type: 'show' | 'movie') => void
  onNavigate:       (path: string) => void
    onItemHover: (item: ShelfEntry) => void
  onRowLeave:       () => void
  onContinueWatchingHover:    (item: WatchProgress) => void
  onContinueWatchingHoverEnd: () => void
  libraryNames:     Map<string, string>
  onHideLibrary:    (libraryId: string) => void
}) {
  // Applies a shelf's own filter-set (contentType + sort + filter — see
  // HOME_SHELVES) to the Library page before navigating there — e.g.
  // "Recently Added Movies" lands on Library showing movies only, sorted by
  // date added, with any of the shelf's own filter criteria already applied
  // and visible in the rule-builder panel. Mutating the store synchronously
  // before navigate() is safe: LibraryPage's mount effect (loadLibraries().
  // then(() => store.fetch())) reads store state fresh on arrival.
  const continueInLibrary = (shelf: HomeShelfDef) => () => {
    libraryStore.presetFilterFromString(shelf.contentType, shelf.sort, shelf.filter)
    onNavigate('/library')
  }

  return (
    <div className={styles.shelvesWrap}>

      {loading ? (
        <><ShelfSkeleton /><ShelfSkeleton /></>
      ) : (
        <>
          {watchTogether.length > 0 && (
              <WatchTogetherShelf items={watchTogether} onNavigate={onNavigate} onClose={onCloseWatchTogether}/>
          )}
          {continueWatching.length > 0 && (
            <ContinueWatchingShelf
              items={continueWatching} onNavigate={onNavigate}
              onItemHover={onContinueWatchingHover} onRowLeave={onContinueWatchingHoverEnd}
            />
          )}
          {recentShows.length > 0 && (
            <Shelf
              title={HOME_SHELVES.recentShows.title}
              items={recentShows.map(s => ({
                id: s.show_id, title: s.title, year: s.year,
                thumb_url: proxyThumb(s), rating: s.audience_rating,
                content_type: 'show' as const, library_id: s.library_id,
              }))}
              onItemClick={(id, type) => onItemClick(id, type)}
              onViewAll={continueInLibrary(HOME_SHELVES.recentShows)}
              onItemHover={onItemHover}
              onRowLeave={onRowLeave}
              libraryNames={libraryNames}
              onHideLibrary={onHideLibrary}
            />
          )}
          {recentMovies.length > 0 && (
            <Shelf
              title={HOME_SHELVES.recentMovies.title}
              items={recentMovies.map(m => ({
                id: m.movie_id, title: m.title, year: m.year,
                thumb_url: proxyThumb(m), rating: m.audience_rating,
                content_type: 'movie' as const, library_id: m.library_id,
              }))}
              onItemClick={(id, type) => onItemClick(id, type)}
              onViewAll={continueInLibrary(HOME_SHELVES.recentMovies)}
              onItemHover={onItemHover}
              onRowLeave={onRowLeave}
              libraryNames={libraryNames}
              onHideLibrary={onHideLibrary}
            />
          )}
          {recentlyReleased.length > 0 && (
            <Shelf
              title={HOME_SHELVES.recentlyReleased.title}
              items={recentlyReleased.map(m => ({
                id: m.movie_id, title: m.title, year: m.year,
                thumb_url: proxyThumb(m), rating: m.audience_rating,
                content_type: 'movie' as const, library_id: m.library_id,
              }))}
              onItemClick={(id, type) => onItemClick(id, type)}
              onViewAll={continueInLibrary(HOME_SHELVES.recentlyReleased)}
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
              title={HOME_SHELVES.recentlyAired.title}
              items={recentlyAired.filter(s => s.latest_episode).map(s => ({
                id: s.show_id, title: s.title, year: s.year,
                thumb_url: proxyThumb(s), rating: s.audience_rating,
                content_type: 'show' as const, library_id: s.library_id,
                directPlayPath: `/player/episode/${s.latest_episode!.episode_id}`,
              }))}
              onItemClick={(id, type) => onItemClick(id, type)}
              onNavigate={onNavigate}
              onViewAll={continueInLibrary(HOME_SHELVES.recentlyAired)}
              onItemHover={onItemHover}
              onRowLeave={onRowLeave}
              libraryNames={libraryNames}
              onHideLibrary={onHideLibrary}
            />
          )}
          {customShelves.map(({ shelf, items }) => (
            <Shelf
              key={shelf.playlist_id}
              title={shelf.title}
              items={items.map(mixedToShelfEntry)}
              onItemClick={(id, type) => onItemClick(id, type)}
              onNavigate={onNavigate}
              onViewAll={continueInLibrary({
                  title: shelf.title,
                  // The Library page's 'all' contentType is exactly "both
                  // shows and movies" — the same thing 'mixed' means here.
                  // A show+smart_expand_episodes shelf still lands on 'show'
                  // (Library has no standalone "episodes" content type of its
                  // own to flatten into).
                  contentType: shelf.smart_type === 'mixed' ? 'all' : shelf.smart_type,
                  sort: shelf.smart_sort, filter: shelf.filter_expr, limit: shelf.home_tile_limit,
              })}
              onItemHover={onItemHover}
              onRowLeave={onRowLeave}
              libraryNames={libraryNames}
              onHideLibrary={onHideLibrary}
            />
          ))}
        </>
      )}
    </div>
  )
}

// ── Shelf ────────────────────────────────────────────────────────────────────

interface ShelfEntry {
  id: string; title: string; year?: number
    thumb_url?: string;
    rating?: number;
    content_type: 'show' | 'movie' | 'episode'
  library_id?: string
    // Recently Aired (one tile per show) and any episode tile (mixedToShelfEntry)
    // both use this: when set, selecting the card jumps straight to playing
    // this episode instead of opening a detail view — episodes have no detail
    // page of their own to open.
  directPlayPath?: string
    // Episode tiles only — see mixedToShelfEntry. showTitle displays as the
    // card's primary title (an episode's own title, often "Episode 12", isn't
    // useful on its own) with the episode's own title as the subtitle instead,
    // same convention as ContinueWatchingCard.
    episodeCode?: string
    showTitle?: string
}

function Shelf({ title, items, onItemClick, onNavigate, onViewAll, onItemHover, onRowLeave, libraryNames, onHideLibrary }: {
  title:       string
  items:       ShelfEntry[]
  onItemClick: (id: string, type: 'show' | 'movie') => void
  onNavigate?: (path: string) => void
  onViewAll?:  () => void
    onItemHover?: (item: ShelfEntry) => void
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
      className={styles.shelfRowWrap}
      onMouseEnter={() => setShowArrows(true)}
      onMouseLeave={() => { setShowArrows(false); onRowLeave?.() }}
    >
      <div className={styles.shelfHeaderRow}>
        <span className={styles.shelfTitle}>{title}</span>
        {onViewAll && (
          <button onClick={onViewAll} className={styles.shelfViewAll}>View All →</button>
        )}
      </div>

      {showArrows && <ShelfArrow side="left"  onClick={() => scroll('left')} />}
      <div ref={rowRef} className={styles.shelfRowScroller}>
        <TravelingFocusFrame rect={travel.rect} active={travel.active} />
        <FocusContext.Provider value={rowFocusKey}>
        {items.map(item => (
          <ShelfCard
            key={item.id} item={item}
            onClick={() => {
                if (item.directPlayPath) {
                    onNavigate?.(item.directPlayPath);
                    return
                }
                if (item.content_type !== 'episode') onItemClick(item.id, item.content_type)
            }}
            onHover={() => onItemHover?.(item)}
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
      className={`${styles.tileBase} ${styles.shelfEndTile} ${active ? styles.shelfEndTileActive : ''}`}
    >
      <svg width="22" height="22" viewBox="0 0 22 22" fill="none" stroke="currentColor" strokeWidth="1.6">
        <rect x="3" y="3" width="7" height="7" rx="1.2" /><rect x="12" y="3" width="7" height="7" rx="1.2" />
        <rect x="3" y="12" width="7" height="7" rx="1.2" /><rect x="12" y="12" width="7" height="7" rx="1.2" />
      </svg>
      <span className={styles.shelfEndTileLabel}>
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
    const displayTitle = item.content_type === 'episode' ? (item.showTitle || item.title) : item.title

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
      className={`${styles.tileBase} ${styles.shelfCard} ${hovered || focused ? styles.shelfCardActive : ''}`}
    >
      <div className={styles.shelfCardPoster}>
        {showImg ? (
          <img
              src={item.thumb_url} alt={displayTitle} onError={() => setImgErr(true)}
            className={styles.shelfCardImg}
          />
        ) : (
          <span className={styles.shelfCardMonogram}>
            {displayTitle.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
          {item.episodeCode && (
              <span className={styles.cwEpBadge}>{item.episodeCode}</span>
          )}
        {libraryName && onHideLibrary && (hovered || focused) && (
          <button
            onClick={e => { e.stopPropagation(); setMenuOpen(o => !o) }}
            aria-label="More options"
            className={styles.shelfCardMenuBtn}
          >⋯</button>
        )}
        {menuOpen && libraryName && onHideLibrary && (
          <div
            onClick={e => e.stopPropagation()}
            className={styles.shelfCardMenu}
          >
            <button
              onClick={() => { onHideLibrary(); setMenuOpen(false) }}
              className={styles.shelfCardMenuItem}
            >Hide "{libraryName}" from Home</button>
          </div>
        )}
        {(hovered || focused) && (
          <div className={styles.shelfCardHoverOverlay}>
              <div className={styles.shelfCardHoverTitle}>{displayTitle}</div>
            {item.rating != null && (
              <div className={styles.shelfCardHoverRating}>★ {item.rating.toFixed(1)}</div>
            )}
          </div>
        )}
      </div>
      <div className={styles.shelfCardInfo}>
          <div className={styles.shelfCardTitle}>{displayTitle}</div>
        <div className={styles.shelfCardMeta}>
            {item.content_type === 'episode' ? item.title : <>{item.year}{item.year && ' · '}{item.content_type}</>}
        </div>
      </div>
    </div>
  )
}

// The literal Continue Watching feed — actual in-progress position_ms/
// resume behavior, hence its own card distinct from Shelf/ShelfCard's
// (which every other shelf, including custom per-episode ones, renders
// through instead — see mixedToShelfEntry). `id` disambiguates focusKeys
// from a regular shelf row that happens to show the same content.
function ContinueWatchingShelf({
                                   id = 'continue-watching',
                                   title = 'Continue Watching',
                                   items,
                                   onNavigate,
                                   onItemHover,
                                   onRowLeave,
                                   onViewAll
                               }: {
    id?: string; title?: string
  items: WatchProgress[]; onNavigate: (path: string) => void
  onItemHover?: (item: WatchProgress) => void; onRowLeave?: () => void
    onViewAll?: () => void
}) {
  const [showArrows, setShowArrows] = useState(false)
  const travel = useTravelingFocus()
  const { ref: rowRef, focusKey: rowFocusKey } = useFocusable<object, HTMLDivElement>({
      focusKey: `home-shelf-row-${id}`,
    trackChildren: true,
    isFocusBoundary: true,
    focusBoundaryDirections: ['left', 'right'],
  })
  const scroll = (d: 'left' | 'right') =>
    rowRef.current?.scrollBy({ left: d === 'right' ? 340 : -340, behavior: 'smooth' })

  return (
    <div
      className={styles.shelfRowWrap}
      onMouseEnter={() => setShowArrows(true)}
      onMouseLeave={() => { setShowArrows(false); onRowLeave?.() }}
    >
      <div className={styles.shelfHeaderRow}>
          <span className={styles.shelfTitle}>{title}</span>
      </div>

      {showArrows && <ShelfArrow side="left" onClick={() => scroll('left')} />}
      <div ref={rowRef} className={styles.shelfRowScroller}>
        <TravelingFocusFrame rect={travel.rect} active={travel.active} />
        <FocusContext.Provider value={rowFocusKey}>
        {items.map(p => (
          <ContinueWatchingCard
              key={`${p.content_type}:${p.content_id}`} shelfId={id} item={p} onNavigate={onNavigate}
            onActivate={travel.activate} onDeactivate={travel.deactivate}
            onHover={onItemHover}
          />
        ))}
            {/* No natural single filter for a mixed shows/movies/episodes feed
            — lands on an unfiltered Library unless the caller has a more
            specific place to send "View All" (e.g. a shelf backed by a
            real smart playlist's own filter). */}
        <ShelfEndTile
            focusKey={`home-shelf-end-${id}`} onClick={() => onViewAll ? onViewAll() : onNavigate('/library')}
          onActivate={travel.activate} onDeactivate={travel.deactivate}
        />
        </FocusContext.Provider>
      </div>
      {showArrows && <ShelfArrow side="right" onClick={() => scroll('right')} />}
    </div>
  )
}

function ContinueWatchingCard({shelfId, item, onNavigate, onActivate, onDeactivate, onHover}: {
    shelfId: string
  item: WatchProgress; onNavigate: (path: string) => void
  onActivate: (el: HTMLElement | null) => void; onDeactivate: () => void
  onHover?: (item: WatchProgress) => void
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
      focusKey: `home-cw-card-${shelfId}-${item.content_type}-${item.content_id}`,
    onEnterPress: go,
    onFocus: () => { onActivate(ref.current); onHover?.(item) }, onBlur: onDeactivate,
  })

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={go}
      onMouseEnter={() => { setHovered(true); onActivate(ref.current); onHover?.(item) }}
      onMouseLeave={() => { setHovered(false); onDeactivate() }}
      className={`${styles.tileBase} ${styles.shelfCard} ${hovered || focused ? styles.shelfCardActive : ''}`}
    >
      <div className={styles.shelfCardPoster}>
        {thumbPath && !imgErr ? (
          <img
            src={mediaUrl(thumbPath)} alt={item.title} onError={() => setImgErr(true)}
            className={styles.shelfCardImg}
          />
        ) : (
          <span className={styles.shelfCardMonogram}>
            {item.title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
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
        <div className={styles.shelfCardTitle}>{item.content_type === 'episode' ? (item.show_title || item.title) : item.title}</div>
        {item.content_type === 'episode' && (
          <div className={styles.shelfCardMeta}>{item.title}</div>
        )}
      </div>
    </div>
  )
}

// ── Watch Together ───────────────────────────────────────────────────────────

function WatchTogetherShelf({items, onNavigate, onClose}: {
    items: WatchTogetherSession[]; onNavigate: (path: string) => void; onClose: (sessionId: string) => void
}) {
  const [showArrows, setShowArrows] = useState(false)
  const travel = useTravelingFocus()
  const { ref: rowRef, focusKey: rowFocusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: 'home-shelf-row-watch-together',
    trackChildren: true,
    isFocusBoundary: true,
    focusBoundaryDirections: ['left', 'right'],
  })
  const scroll = (d: 'left' | 'right') =>
    rowRef.current?.scrollBy({ left: d === 'right' ? 340 : -340, behavior: 'smooth' })

  return (
    <div
      className={styles.shelfRowWrap}
      onMouseEnter={() => setShowArrows(true)}
      onMouseLeave={() => setShowArrows(false)}
    >
      <div className={styles.shelfHeaderRow}>
        <span className={styles.shelfTitle}>Watch Together</span>
      </div>

      {showArrows && <ShelfArrow side="left" onClick={() => scroll('left')} />}
      <div ref={rowRef} className={styles.shelfRowScroller}>
        <TravelingFocusFrame rect={travel.rect} active={travel.active} />
        <FocusContext.Provider value={rowFocusKey}>
        {items.map(s => (
          <WatchTogetherCard
              key={s.session_id} item={s} onNavigate={onNavigate} onClose={onClose}
            onActivate={travel.activate} onDeactivate={travel.deactivate}
          />
        ))}
        </FocusContext.Provider>
      </div>
      {showArrows && <ShelfArrow side="right" onClick={() => scroll('right')} />}
    </div>
  )
}

function WatchTogetherCard({item, onNavigate, onClose, onActivate, onDeactivate}: {
    item: WatchTogetherSession; onNavigate: (path: string) => void; onClose: (sessionId: string) => void
  onActivate: (el: HTMLElement | null) => void; onDeactivate: () => void
}) {
    const {user} = useAuth()
    const canClose = user?.user_id === item.host_user_id || user?.role === 'admin'
  const [hovered, setHovered] = useState(false)
  const [imgErr,  setImgErr]  = useState(false)
    const [closing, setClosing] = useState(false)
  const thumbPath = item.content_type === 'movie'
    ? `/api/movies/${item.content_id}/thumb`
    : item.show_id ? `/api/shows/${item.show_id}/thumb` : ''
  const epCode = item.content_type === 'episode'
    ? `S${String(item.season ?? 0).padStart(2, '0')}E${String(item.episode ?? 0).padStart(2, '0')}`
    : undefined

    const close = async (e: React.MouseEvent) => {
        e.stopPropagation()
        setClosing(true)
        try {
            await api.closeWatchTogether(item.session_id);
            onClose(item.session_id)
        } catch {
            setClosing(false)
        }
    }

    // join() returns the host's current position along with the usual
    // membership write, so the new VOD session can start there (via
    // startPositionSec/hls.js's own startPosition) instead of at 0 and relying
    // on a later sync correction, which doesn't reliably catch up (see
    // PlayerPage.tsx).
    const go = async () => {
        let t = ''
        try {
            const joined = await joinWatchTogether(item.session_id)
            if (joined.position_ms > 0) t = `&t=${Math.round(joined.position_ms)}`
        } catch { /* fall back to position 0 — the SSE sync tick still corrects it, just slower */
        }
        onNavigate(`/player/${item.content_type}/${item.content_id}?wt=${item.session_id}${t}`)
  }

  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `home-wt-card-${item.session_id}`,
    onEnterPress: go,
    onFocus: () => { onActivate(ref.current) }, onBlur: onDeactivate,
  })

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={go}
      onMouseEnter={() => { setHovered(true); onActivate(ref.current) }}
      onMouseLeave={() => { setHovered(false); onDeactivate() }}
      className={`${styles.tileBase} ${styles.shelfCard} ${hovered || focused ? styles.shelfCardActive : ''}`}
    >
        {canClose && (hovered || focused) && (
            <button
                onClick={close} disabled={closing}
                title="End Watch Together for everyone"
                className={styles.wtCloseBtn}
            >×</button>
        )}
      <div className={styles.shelfCardPoster}>
        {thumbPath && !imgErr ? (
          <img
            src={mediaUrl(thumbPath)} alt={item.title} onError={() => setImgErr(true)}
            className={styles.shelfCardImg}
          />
        ) : (
          <span className={styles.shelfCardMonogram}>
            {item.title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
        {epCode && <span className={styles.cwEpBadge}>{epCode}</span>}
        <span className={styles.wtLiveBadge}>{item.member_count} watching</span>
      </div>
      <div className={styles.shelfCardInfo}>
        <div className={styles.shelfCardTitle}>{item.content_type === 'episode' ? (item.show_title || item.title) : item.title}</div>
        <div className={styles.shelfCardMeta}>Hosted by {item.host_username}</div>
      </div>
    </div>
  )
}

function ShelfArrow({ side, onClick }: { side: 'left' | 'right'; onClick: () => void }) {
  return (
    <button onClick={onClick} className={`${styles.shelfArrow} ${side === 'left' ? styles.shelfArrowLeft : styles.shelfArrowRight}`}>
      {side === 'left' ? '‹' : '›'}
    </button>
  )
}


// ── Skeletons ─────────────────────────────────────────────────────────────────

function ShelfSkeleton() {
  return (
    <div className={styles.skeletonWrap}>
      <div className={`hds-skeleton ${styles.skeletonTitle}`} />
      <div className={styles.skeletonRow}>
        {Array.from({ length: 7 }, (_, i) => (
          <div key={i} className={`hds-skeleton ${styles.skeletonCard}`} />
        ))}
      </div>
    </div>
  )
}
