import { type ReactNode } from 'react'
import type { ScraperSearchResult, VideoInfo } from '../../api/types'
import { EpisodeShelf } from './EpisodeShelf'
import { LanguageChips } from './LanguageChips'
import { useFocusable } from '../../nav/useFocusable'
import { useNavBack } from '../../nav/back'
import { folderBaseName, useMediaDetail, type MediaDetailResult } from './useMediaDetail'
import { useScrollCollapse } from './useScrollCollapse'
import { useElementHeight } from './useElementHeight'
import styles from './MediaDetailHero.module.css'

// The backdrop is a genuinely fixed layer — never scrolls. It's
// HERO_HEIGHT_CSS tall (matches HomePage's hero, see HomePage.tsx's height:
// '62vh', minHeight: 360, so both read as the same scale — and so it's
// shown in full on first paint, per design) until the header below has
// actually locked at the top (`collapsed`, from useScrollCollapse's
// sentinel — geometry-accurate, not a guessed scroll distance), at which
// point it shrinks to match the header's own *measured* rendered height
// (`useElementHeight` — varies per item, e.g. a long overview needs more
// room than a short one) and grows back once scrolled back up.
//
// The poster/title/overview block itself never shrinks or changes style on
// web — it's a `position: sticky` element *inside* the scroll container,
// starting at its normal document position (overlapping the backdrop's
// lower edge by HERO_OVERLAP) and translating up with the scroll like
// anything else until it locks at the top, still overlaid on the backdrop
// throughout (its zIndex is higher). `secondaryInfo` (languages, studio,
// folder, sources) and `playButton` live in that same sticky block, since
// they're part of the detail header, not "the rest of the page" — they
// lock and stay visible alongside poster/title instead of continuing to
// scroll underneath once locked. `actions` (match status, Fix Match, Push
// to Sources, Refresh Metadata, etc. — the heavier admin tooling) and the
// season shelves are plain, non-positioned flow *after* the sticky block,
// so they naturally paint *below* the fixed backdrop wherever they scroll
// into that region — visually passing behind it.
const HERO_HEIGHT_CSS = 'max(62vh, 360px)'
const HERO_OVERLAP    = 40

function formatVideoInfo(v: VideoInfo): string | null {
  if (!v.codec && !v.height) return null
  const parts: string[] = []
  if (v.height) parts.push(`${v.height}p`)
  if (v.codec)  parts.push(v.codec.toUpperCase())
  if (v.bit_depth && v.bit_depth !== 8) parts.push(`${v.bit_depth}-bit`)
  return parts.join(' · ')
}

interface MediaDetailHeroProps {
  id?:             string
  content_type?:   'show' | 'movie'
  discoverResult?: ScraperSearchResult
  onBack:          () => void
  /** Rendered inside the sticky header, right after secondaryInfo (studio/folder/sources) — stays locked alongside poster/title rather than scrolling with the admin tooling in `actions`. */
  playButton?:     ReactNode
  /** Heavier admin/library-management controls (match status, Fix Match, Push to Sources, etc.), rendered between the sticky header and the season shelves as plain scrolling content. Render-prop so it shares this component's own useMediaDetail() result. */
  actions?:        (media: MediaDetailResult) => ReactNode
  /** Admin/details tooling, rendered at the very bottom, after the season shelves. */
  afterShelves?:   ReactNode
}

export function MediaDetailHero({ id, content_type, discoverResult, onBack, playButton, actions, afterShelves }: MediaDetailHeroProps) {
  useNavBack(onBack) // Escape/Backspace closes the detail view, same contract as PlayerPage
  const media = useMediaDetail({ id, content_type, discoverResult })
  const {
    show, movie, loading, detail, contentType,
    posterUrl, backdropUrl, title, year, overview, genres, tags, rating,
    seasonsWithEpisodes, languages, videoInfo, folderName, fileName,
    setFocusedEpisode,
  } = media

  const { scrollRef, sentinelRef, collapsed } = useScrollCollapse()
  const { ref: headerRef, height: headerHeight } = useElementHeight<HTMLDivElement>()

  const sourceChipClass = discoverResult?.source === 'tmdb' ? styles.metaChipTmdb : styles.metaChipOther
  const requestApproved = discoverResult?.request_status === 'approved'

  const posterBox = (
    <div className={`hds-media-detail-poster ${styles.posterBox}`}>
      {posterUrl && (
        <img
          src={posterUrl} alt={title}
          className={styles.posterImg}
          onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
        />
      )}
    </div>
  )

  const primaryInfo = (
    <div className={styles.primaryInfo}>
      <div className={styles.titleRow}>
        <h2 className={styles.title}>{title}</h2>
        {detail?.locked && (
          <span className={`${styles.statusBadge} ${styles.statusBadgeLocked}`}>LOCKED</span>
        )}
        {detail?.skip_scraping && (
          <span className={`${styles.statusBadge} ${styles.statusBadgeScrapingOff}`}>SCRAPING OFF</span>
        )}
      </div>

      {/* Meta chips */}
      <div className={styles.metaChipRow}>
        {year && <span className={styles.metaChip}>{year}</span>}
        {detail?.content_rating && <span className={styles.metaChip}>{detail.content_rating}</span>}
        {rating != null && (
          <span className={`${styles.metaChip} ${styles.metaChipGold}`}>
            ★ {rating.toFixed(1)}
          </span>
        )}
        <span className={styles.metaChip}>{contentType === 'show' ? 'series' : 'film'}</span>
        {videoInfo && formatVideoInfo(videoInfo) && (
          <span className={styles.metaChip}>{formatVideoInfo(videoInfo)}</span>
        )}
        {discoverResult && (
          <span className={`${styles.metaChip} ${sourceChipClass}`}>{discoverResult.source.toUpperCase()}</span>
        )}
        {discoverResult?.in_library && (
          <span className={`${styles.metaChip} ${styles.metaChipGreen}`}>
            IN LIBRARY
          </span>
        )}
        {!discoverResult?.in_library && discoverResult?.request_status && (
          <span className={`${styles.metaChip} ${requestApproved ? styles.metaChipGreen : styles.metaChipAmber}`}>
            {discoverResult.request_status === 'approved' ? 'APPROVED' : discoverResult.request_status === 'rejected' ? 'REJECTED' : 'REQUESTED'}
          </span>
        )}
      </div>

      {/* Genres */}
      {genres.length > 0 && (
        <div className={styles.genreRow}>
          {genres.map(g => (
            <span key={g} className={styles.genreChip}>{g}</span>
          ))}
        </div>
      )}

      {/* Tags — scraper-derived keywords/themes (TMDB keywords, AniList tags,
          Wikidata main subject), distinct from genres: more numerous and more
          specific (e.g. "Christmas", "time travel"), so a lighter/smaller
          chip style keeps them from competing visually with genres. */}
      {tags.length > 0 && (
        <div className={styles.tagRow}>
          {tags.map(t => (
            <span key={t} className={styles.tagChip}>{t}</span>
          ))}
        </div>
      )}

      {/* Overview — swaps to the focused episode's when one is hovered/focused */}
      {overview && (
        <p className={styles.overview}>{overview}</p>
      )}
    </div>
  )

  const secondaryInfo = (
    <>
      {/* Languages */}
      <LanguageChips languages={languages} />

      {/* Movie: credits */}
      {movie && (
        <div className={styles.movieCreditsCol}>
          {movie.director && <MetaRow label="Director">{movie.director}</MetaRow>}
          {movie.studio   && <MetaRow label="Studio">{movie.studio}</MetaRow>}
          {movie.duration_ms > 0 && (
            <MetaRow label="Runtime">{Math.round(movie.duration_ms / 60000)} min</MetaRow>
          )}
          {/* Admin-facing "where does this live on disk" — shows don't
              get this (a collection of per-episode files, no single
              path), only movies have one file to point at. */}
          {fileName && (
            <MetaRow label="File">
              <span className={styles.breakAll}>
                {folderName && <>{folderName}/</>}{fileName}
              </span>
            </MetaRow>
          )}
        </div>
      )}
      {show?.studio && <MetaRow label="Studio">{show.studio}</MetaRow>}
      {/* Admin-facing "where does this live on disk" — common ancestor
          directory across all of the show's episode files (see
          ContentRepository::getShowFolderPath), the show-level analog
          of the movie "File" row above. */}
      {show?.folder_path && (
        <MetaRow label="Folder">
          <span className={styles.breakAll} title={show.folder_path}>
            {folderBaseName(show.folder_path)}
          </span>
        </MetaRow>
      )}
      {detail?.sources && detail.sources.length > 0 && (
        <MetaRow label="Sources">
          {detail.sources.map(s => s.display_name || s.source_type).join(', ')}
        </MetaRow>
      )}
    </>
  )

  const seasonShelves = seasonsWithEpisodes.length > 0 && (
    <div className={styles.seasonShelvesWrap}>
      {seasonsWithEpisodes.map(s => (
        <EpisodeShelf
          key={`${s.number}-${s.episodes[0]?.episode_id ?? ''}`}
          seasonNumber={s.number} seasonName={s.name} episodes={s.episodes}
          onEpisodeHover={setFocusedEpisode} onEpisodeHoverEnd={() => setFocusedEpisode(null)}
        />
      ))}
    </div>
  )

  // One implementation for every caller (LibraryPage, Home's detail view;
  // TvLibraryDetail has its own TV-shaped layout but the same idea, sized
  // for 10-foot viewing). See the HERO_HEIGHT_CSS comment above for the
  // full mechanism.
  return (
    <div className={styles.root}>
      {/* Fixed backdrop — never scrolls; shrinks to the locked header's
          measured height once collapsed, back to full size once not.
          Height and the backdrop image itself are both genuinely dynamic
          per-render values (a live DOM measurement, and an arbitrary image
          URL) so they stay targeted inline styles rather than static classes. */}
      <div
        className={`${styles.backdrop} ${loading && !discoverResult ? styles.backdropLoading : ''}`}
        style={{
          height: collapsed && headerHeight > 0 ? headerHeight : HERO_HEIGHT_CSS,
          background: backdropUrl
            ? `url(${backdropUrl}) center/cover no-repeat`
            : 'linear-gradient(135deg, var(--hds-backdrop-fallback-1) 0%, var(--hds-backdrop-fallback-2) 50%, var(--hds-backdrop-fallback-3) 100%)',
        }}
      >
        <div className={styles.backdropScrimH} />
        <div className={styles.backdropScrimV} />
      </div>

      {/* Pinned above everything, independent of scroll or collapse state. */}
      <div className={styles.pinnedBack}>
        <BackButton onClick={onBack} overlay />
      </div>

      <div ref={scrollRef} className={`${styles.scrollArea} scrollbar-dark`}>
        {/* Spacer — the sticky header's natural (unstuck) starting position,
            overlapping the fixed backdrop's lower edge by HERO_OVERLAP. */}
        <div style={{ height: `calc(${HERO_HEIGHT_CSS} - ${HERO_OVERLAP}px)` }} />

        {/* Sentinel — same document position as the header's natural top
            edge, right before it. Once this scrolls out of view the header
            has nowhere left to go but stick (see useScrollCollapse). */}
        <div ref={sentinelRef} />

        {/* Sticky header — scrolls up with the page like normal content
            until it reaches the top, then locks there. Never changes size —
            just overlaid on the still-visible backdrop behind it throughout
            (zIndex 2 > backdrop's zIndex 1), never on the page background.
            secondaryInfo (Studio, Folder, Sources, etc.) and playButton live
            *inside* this sticky block, not in the plain-flow content below —
            they're part of the detail header, not "the rest of the page,"
            so they lock and stay visible alongside poster/title instead of
            continuing to scroll underneath them once locked. */}
        <div
          ref={headerRef}
          className={`hds-media-detail-hero-container${collapsed ? ' hds-media-detail-hero-collapsed' : ''} ${styles.stickyHeader}`}
        >
          {loading && !discoverResult ? (
            <DetailSkeleton />
          ) : (
            <>
              <div className={`hds-media-detail-hero-row ${styles.heroRow}`}>
                {posterBox}
                {primaryInfo}
              </div>
              <div className={styles.alignUnderInfo}>
                <div className={styles.secondaryInfoCol}>
                  {secondaryInfo}
                  {playButton}
                </div>
              </div>
            </>
          )}
        </div>

        {/* Plain flow — no position/z-index, so it naturally paints *below*
            the fixed backdrop (a positioned element) wherever it scrolls
            into that region, i.e. behind it. Match status/Fix Match/Push to
            Sources/etc, season shelves, fix-match panel — genuinely "the
            rest of the page." */}
        <div className={styles.belowFold}>
          {!loading && actions && (
            <div className={styles.alignUnderInfo}>
              <div className={styles.actionsCol}>{actions(media)}</div>
            </div>
          )}
          {seasonShelves}
          {afterShelves}
        </div>
      </div>
    </div>
  )
}

function BackButton({ onClick, overlay }: { onClick: () => void; overlay?: boolean }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'detail-back', onEnterPress: onClick })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      className={`${styles.backButton} ${overlay ? styles.backButtonOverlay : ''}`}
    >
      <svg width="14" height="14" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round">
        <path d="M9 2L4 7l5 5" />
      </svg>
      Back
    </button>
  )
}

function MetaRow({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className={styles.metaRow}>
      <span className={styles.metaRowLabel}>{label}</span>
      {' · '}{children}
    </div>
  )
}

function DetailSkeleton() {
  return (
    <div className={styles.skeletonRow}>
      <div className={`hds-skeleton ${styles.skeletonPoster}`} />
      <div className={styles.skeletonLines}>
        {[240, 120, 90, 320, 280, 300, 220].map((w, i) => (
          <div key={i} className={`hds-skeleton ${styles.skeletonLine}`} style={{ width: w }} />
        ))}
      </div>
    </div>
  )
}
