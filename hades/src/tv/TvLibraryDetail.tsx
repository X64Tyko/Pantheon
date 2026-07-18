import { useEffect, useState, type CSSProperties } from 'react'
import { useNavigate, useParams } from 'react-router-dom'
import { useMediaDetail } from '../components/media/useMediaDetail'
import { useScrollCollapse } from '../components/media/useScrollCollapse'
import { useElementHeight } from '../components/media/useElementHeight'
import { EpisodeShelf } from '../components/media/EpisodeShelf'
import { useFocusable } from '../nav/useFocusable'
import { useNavBack } from '../nav/back'
import { resolvePlayPath } from '../player/resolvePlayTarget'
import { peekDetailReturnTo, clearPendingDetailReturn } from './tvDetailNav'
import { useZoneManifest } from './useZoneManifest'
import styles from './TvLibraryDetail.module.css'

// The backdrop is a genuinely fixed layer — never scrolls. It's
// HERO_HEIGHT_CSS tall (shown in full on first paint) until the header
// below has actually locked at the top (`collapsed`, from
// useScrollCollapse's sentinel — geometry-accurate, not a guessed scroll
// distance), at which point it shrinks to match the header's own *measured*
// rendered height (`useElementHeight`) and grows back once scrolled back
// up. The poster/title/overview/Play/Back block itself never shrinks or
// changes style — it's a `position: sticky` element inside the scroll
// container, starting at its normal document position (overlapping the
// backdrop's lower edge by HERO_OVERLAP) and translating up with the
// scroll like anything else until it locks at the top, still overlaid on
// the backdrop throughout (its zIndex is higher). Sized larger than
// desktop's own HERO_HEIGHT_CSS (see MediaDetailHero.tsx) — this is a TV,
// not a phone; matches TV's own historical hero proportions rather than
// desktop's HomePage (a different screen).
const HERO_HEIGHT_CSS = 'max(52vh, 460px)'
const HERO_OVERLAP    = 60

export function TvLibraryDetail() {
  const navigate = useNavigate()
  const { type, id } = useParams<{ type: 'show' | 'movie'; id: string }>()
  // Wherever the card that opened this was clicked from (Home or Library) —
  // set right before navigate() by that page, read once here and held for
  // this whole visit. Falls back to Library for any other way of landing on
  // this route (e.g. a future deep link) — see tvDetailNav.ts.
  const [returnTo] = useState(() => peekDetailReturnTo())
  useNavBack(() => navigate(returnTo))

  // Safety net for leaving via something other than Back (e.g. Play) — the
  // legitimate Back path already consumes the pending entry on the
  // destination's own mount, before this cleanup runs; see tvDetailNav.ts.
  useEffect(() => () => clearPendingDetailReturn(), [])

  const {
    loading, detail, contentType, posterUrl, backdropUrl, title, year, overview, genres, rating,
    seasonsWithEpisodes, setFocusedEpisode,
  } = useMediaDetail({ id, content_type: type })

  // Which zones this screen renders — hero-backdrop/meta-block/genre-chips/
  // play-button/episode-shelves, per GET /api/tv/manifest's detail.zones.
  // This screen's structure is already fixed/1:1 with that vocabulary (no
  // genuinely variable composition to drive here, unlike Library's filters)
  // — presence-gating is what makes it a real manifest consumer rather than
  // hardcoded, without inventing new behavior. episode-shelves' showOnly is
  // already naturally respected: seasonsWithEpisodes is empty for movies.
  const { hasZone } = useZoneManifest('detail')

  const { scrollRef, sentinelRef, collapsed } = useScrollCollapse()
  const { ref: headerRef, height: headerHeight } = useElementHeight<HTMLDivElement>()

  const goPlay = async () => {
    if (!id || !contentType) return
    const path = await resolvePlayPath(contentType, id)
    if (path) navigate(path)
  }
  const goBack = () => navigate(returnTo)

  const play = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-detail-play', forceFocus: true, onEnterPress: goPlay })
  const back = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-detail-back', onEnterPress: goBack })

  if (loading) {
    return <div className={styles.loadingScreen}>
      <div className={`hds-skeleton ${styles.loadingSkeleton}`} />
    </div>
  }

  // The backdrop's height is a live-measured pixel value (collapsed header's
  // actual rendered height) and, when present, backdropUrl is an arbitrary
  // runtime image URL — neither can be expressed as a static CSS class, so
  // this one element keeps a real inline style for just those two dynamic
  // properties; everything else about it lives in TvLibraryDetail.module.css.
  const backdropStyle: CSSProperties = {
    height: collapsed && headerHeight > 0 ? headerHeight : HERO_HEIGHT_CSS,
    ...(backdropUrl ? {
      backgroundImage: `url(${backdropUrl})`,
      backgroundPosition: 'center',
      backgroundSize: 'cover',
      backgroundRepeat: 'no-repeat',
    } : {}),
  }

  return (
    <div className={styles.screen}>
      {/* Fixed backdrop — never scrolls; shrinks to the locked header's
          measured height once collapsed, back to full size once not. */}
      {hasZone('hero-backdrop') && (
        <div
          className={`${styles.backdrop} ${backdropUrl ? '' : styles.backdropFallback}`}
          style={backdropStyle}
        >
          <div className={styles.backdropScrimH} />
          <div className={styles.backdropScrimV} />
        </div>
      )}

      <div ref={scrollRef} className={`${styles.scrollContainer} scrollbar-dark`}>
        {/* Spacer — the sticky header's natural (unstuck) starting position,
            overlapping the fixed backdrop's lower edge by HERO_OVERLAP. */}
        <div className={styles.heroSpacer} />

        {/* Sentinel — same document position as the header's natural top
            edge, right before it. Once this scrolls out of view the header
            has nowhere left to go but stick (see useScrollCollapse). */}
        <div ref={sentinelRef} />

        {/* Sticky header — scrolls up with the page like normal content
            until it reaches the top, then locks there. Never changes size —
            just overlaid on the still-visible backdrop behind it throughout
            (zIndex 2 > backdrop's zIndex 1). The title/overview stay
            visible always, so retargeting them by hovering/focusing an
            episode further down the shelf list (see useMediaDetail's
            focusedEpisode) is still visible either way — the reason the
            hero used to be fully fixed no longer requires that, since the
            locked header serves the same purpose. */}
        <div ref={headerRef} className={styles.header}>
          <div className={styles.headerRow}>
            <div className={styles.poster}>
              {posterUrl && <img src={posterUrl} alt={title} className={styles.posterImg} />}
            </div>

            <div className={styles.infoCol}>
              <h1 className={styles.heroTitle}>{title}</h1>

              {hasZone('meta-block') && (
                <div className={styles.metaRow}>
                  {year && <span>{year}</span>}
                  {rating != null && <span className={styles.ratingText}>★ {rating.toFixed(1)}</span>}
                  <span className={styles.contentTypeText}>{contentType === 'show' ? 'series' : 'film'}</span>
                </div>
              )}

              {hasZone('genre-chips') && genres.length > 0 && (
                <div className={styles.genreRow}>
                  {genres.map(g => (
                    <span key={g} className={styles.genreChip}>{g}</span>
                  ))}
                </div>
              )}

              {overview && (
                <p className={styles.overview}>{overview}</p>
              )}

              <div className={styles.buttonRow}>
                {hasZone('play-button') && (
                  <button
                    ref={play.ref} data-tv-focused={play.focused}
                    onClick={goPlay}
                    className={styles.playButton}
                  >
                    <svg width="16" height="16" viewBox="0 0 14 14" fill="currentColor"><path d="M3 1.5v11l9-5.5-9-5.5z" /></svg>
                    Play
                  </button>
                )}
                <button
                  ref={back.ref} data-tv-focused={back.focused}
                  onClick={goBack}
                  className={styles.backButtonDetail}
                >Back</button>
              </div>
            </div>
          </div>
        </div>

        {/* Plain flow — no position/z-index, so it naturally paints *below*
            the fixed backdrop (a positioned element) wherever it scrolls
            into that region, i.e. behind it. */}
        {hasZone('episode-shelves') && seasonsWithEpisodes.length > 0 && (
          <div className={styles.episodesSection}>
            {seasonsWithEpisodes.map(s => (
              <EpisodeShelf
                key={`${s.number}-${s.episodes[0]?.episode_id ?? ''}`}
                seasonNumber={s.number} seasonName={s.name} episodes={s.episodes}
                onEpisodeHover={setFocusedEpisode} onEpisodeHoverEnd={() => setFocusedEpisode(null)}
              />
            ))}
          </div>
        )}

        {!detail && (
          <div className={styles.notFound}>
            Not found.
          </div>
        )}
      </div>
    </div>
  )
}
