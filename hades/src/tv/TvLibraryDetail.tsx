import {useEffect, useState, type CSSProperties, type ReactNode, type RefObject} from 'react'
import { useNavigate, useParams } from 'react-router-dom'
import {api} from '../api/client'
import { useMediaDetail } from '../components/media/useMediaDetail'
import { useScrollCollapse } from '../components/media/useScrollCollapse'
import { useElementHeight } from '../components/media/useElementHeight'
import { EpisodeShelf } from '../components/media/EpisodeShelf'
import { useFocusable } from '../nav/useFocusable'
import { useNavBack } from '../nav/back'
import {resolvePlayPath, resolvePlayTarget} from '../player/resolvePlayTarget'
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
// the backdrop throughout (its zIndex is higher). Reduced from an earlier
// max(52vh, 460px)/460px overlap — real-hardware feedback (a TV, not the
// dev-machine browser this was first tuned in) found that blocked almost
// all content above the fold with a visibly hard edge where it ended.
// Floor (320px) and overlap (40px) are real shared tokens now
// (--hds-tile-hero-height-tv/--hds-tile-hero-overlap-tv, index.css) — Android
// TV Detail reads the same two values via PantheonMetrics instead of its own
// independently-hardcoded 320.dp, so the two clients can't silently drift
// apart on this again. The 36vh responsive scale itself stays local — it's
// meaningful only for a resizable browser viewport, not a fixed-resolution
// TV/native surface (see the SDUI-manifest audit's own reasoning for why
// this kind of viewport-relative sizing isn't a good cross-platform token).
const HERO_HEIGHT_CSS = 'max(36vh, var(--hds-tile-hero-height-tv, 320px))'
const HERO_OVERLAP = 40 // mirrors --hds-tile-hero-overlap-tv — see .heroSpacer in the module CSS, the only place this JS constant's value actually needs to match

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
    const {hasZone, zone} = useZoneManifest('detail')
    // Which fields the meta-block zone renders (kairos v97) — falls back to
    // today's fixed set for a manifest that predates the `fields` key.
    const metaFields = zone('meta-block')?.fields?.length ? zone('meta-block')!.fields! : ['year', 'rating', 'content_type']
    // Which of play/play-from-beginning/watch-together the play-button zone
    // offers (kairos v105's `actions` list) — mirrors Android's
    // DetailViewModel.hasAction exactly: a zone that exists but predates
    // this field (actions undefined) defaults to showing all three; a
    // missing zone entirely shows none (callers already gate on hasZone
    // first, same as before this field existed).
    const hasAction = (action: string) => {
        const z = zone('play-button')
        if (!z) return false
        return z.actions ? z.actions.includes(action) : true
    }

  const { scrollRef, sentinelRef, collapsed } = useScrollCollapse()
  const { ref: headerRef, height: headerHeight } = useElementHeight<HTMLDivElement>()

  const goPlay = async () => {
    if (!id || !contentType) return
    const path = await resolvePlayPath(contentType, id)
    if (path) navigate(path)
  }
    // Mirrors Android's playFromBeginningTarget(): movie always restarts at 0;
    // show goes to the first episode of the first shelf as currently
    // displayed (respects aired-order interleaving the same way the shelves
    // themselves do), never resolve-play-target's resume logic.
    const goPlayFromBeginning = () => {
        if (!id || !contentType) return
        if (contentType === 'movie') {
            navigate(`/player/movie/${id}`);
            return
        }
        const first = seasonsWithEpisodes[0]?.episodes[0]
        if (first) navigate(`/player/episode/${first.episode_id}`)
    }
    const [watchTogetherLoading, setWatchTogetherLoading] = useState(false)
    const goWatchTogether = async () => {
        if (!id || !contentType) return
        setWatchTogetherLoading(true)
        try {
            const target = await resolvePlayTarget(contentType, id)
            if (!target) return
            const session = await api.createWatchTogether(target.kind, target.id)
            const t = target.positionMs > 0 ? `&t=${target.positionMs}` : ''
            navigate(`/player/${target.kind}/${target.id}?wt=${session.session_id}${t}`)
        } catch {
            // Best-effort, same as desktop web's WatchTogetherAction — a failed
            // create just leaves the button clickable again.
        } finally {
            setWatchTogetherLoading(false)
        }
    }
  const goBack = () => navigate(returnTo)

  const play = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-detail-play', forceFocus: true, onEnterPress: goPlay })
    const playFromBeginning = useFocusable<object, HTMLButtonElement>({
        focusKey: 'tv-detail-play-from-beginning',
        onEnterPress: goPlayFromBeginning
    })
    const watchTogether = useFocusable<object, HTMLButtonElement>({
        focusKey: 'tv-detail-watch-together',
        onEnterPress: goWatchTogether,
        focusable: !watchTogetherLoading
    })
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
        <div ref={headerRef} className={`${styles.header}${collapsed ? ` ${styles.headerCollapsed}` : ''}`}>
          <div className={styles.headerRow}>
            <div className={styles.poster}>
              {posterUrl && <img src={posterUrl} alt={title} className={styles.posterImg} />}
            </div>

            <div className={styles.infoCol}>
              <h1 className={styles.heroTitle}>{title}</h1>

              {hasZone('meta-block') && (
                <div className={styles.metaRow}>
                    {metaFields.map(field => {
                        if (field === 'year' && year) return <span key={field}>{year}</span>
                        if (field === 'rating' && rating != null) return <span key={field}
                                                                               className={styles.ratingText}>★ {rating.toFixed(1)}</span>
                        if (field === 'content_type') return <span key={field}
                                                                   className={styles.contentTypeText}>{contentType === 'show' ? 'series' : 'film'}</span>
                        return null
                    })}
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
                  {/* Each button independently gated on the zone's `actions`
                    list (kairos v105) — see hasAction above. Icon-only while
                    unfocused, expanding to icon+label on remote focus
                    (CollapsibleActionButton) so all three sit comfortably
                    inline instead of competing for row width, matching the
                    same treatment Android TV Detail got. */}
                  {hasZone('play-button') && hasAction('play') && (
                      <CollapsibleActionButton
                          focus={play} onClick={goPlay} filled
                          icon={<svg width="16" height="16" viewBox="0 0 14 14" fill="currentColor">
                              <path d="M3 1.5v11l9-5.5-9-5.5z"/>
                          </svg>}
                          label="Play"
                      />
                  )}
                  {hasZone('play-button') && hasAction('play-from-beginning') && (
                      <CollapsibleActionButton
                          focus={playFromBeginning} onClick={goPlayFromBeginning}
                          icon="↺" label="Play from Beginning"
                      />
                  )}
                  {hasZone('play-button') && hasAction('watch-together') && (
                      <CollapsibleActionButton
                          focus={watchTogether} onClick={goWatchTogether} disabled={watchTogetherLoading}
                          icon="👥" label={watchTogetherLoading ? 'Starting…' : 'Watch Together'}
                      />
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

// Play/Play from Beginning/Watch Together, collapsed to just `icon` while
// unfocused and expanding to icon+label on remote focus — see
// TvLibraryDetail.module.css's .actionButton/.actionLabel for the actual
// max-width/opacity transition (hds-transition-fast), and Android's
// CollapsibleActionButton (DetailScreen.kt) for the equivalent on that
// platform. `focus` is one of this screen's own useFocusable() results
// (play/playFromBeginning/watchTogether) so each button keeps its own
// stable focusKey/ref rather than this component creating a new one itself.
function CollapsibleActionButton({focus, icon, label, onClick, filled, disabled}: {
    focus: { ref: RefObject<HTMLButtonElement>; focused: boolean }
    icon: ReactNode
    label: string
    onClick: () => void
    filled?: boolean
    disabled?: boolean
}) {
    return (
        <button
            ref={focus.ref} data-tv-focused={focus.focused}
            onClick={onClick} disabled={disabled}
            className={`${styles.actionButton} ${filled ? styles.actionButtonFilled : ''} ${disabled ? styles.actionButtonDisabled : ''}`}
        >
            {icon}
            <span className={`${styles.actionLabel} ${focus.focused ? styles.actionLabelExpanded : ''}`}>{label}</span>
        </button>
    )
}
