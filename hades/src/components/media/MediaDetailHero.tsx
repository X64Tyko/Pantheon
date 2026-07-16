import { type ReactNode } from 'react'
import type { ScraperSearchResult, VideoInfo } from '../../api/types'
import { EpisodeShelf } from './EpisodeShelf'
import { LanguageChips } from './LanguageChips'
import { useFocusable } from '../../nav/useFocusable'
import { useNavBack } from '../../nav/back'
import { folderBaseName, useMediaDetail, type MediaDetailResult } from './useMediaDetail'
import { heroTextShadow } from '../../channel/styles'
import { useScrollCollapse } from './useScrollCollapse'
import { useElementHeight } from './useElementHeight'

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

const metaChip: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
  padding: '3px 9px', borderRadius: 10,
  border: '1px solid var(--hds-line-s)', color: 'var(--hds-txt-3)',
}

export function MediaDetailHero({ id, content_type, discoverResult, onBack, playButton, actions, afterShelves }: MediaDetailHeroProps) {
  useNavBack(onBack) // Escape/Backspace closes the detail view, same contract as PlayerPage
  const media = useMediaDetail({ id, content_type, discoverResult })
  const {
    show, movie, loading, detail, contentType,
    posterUrl, backdropUrl, title, year, overview, genres, rating,
    seasonsWithEpisodes, languages, videoInfo, folderName, fileName,
    setFocusedEpisode,
  } = media

  const { scrollRef, sentinelRef, collapsed } = useScrollCollapse()
  const { ref: headerRef, height: headerHeight } = useElementHeight<HTMLDivElement>()

  const srcColor = discoverResult?.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)'

  const posterBox = (
    <div className="hds-media-detail-poster" style={{
      width: 170, height: 255, borderRadius: 10, overflow: 'hidden', flexShrink: 0,
      background: 'var(--hds-bg-3)', boxShadow: '0 8px 32px oklch(0 0 0 / 0.5)',
    }}>
      {posterUrl && (
        <img
          src={posterUrl} alt={title}
          style={{ width: '100%', height: '100%', objectFit: 'cover' }}
          onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
        />
      )}
    </div>
  )

  const primaryInfo = (
    <div style={{ flex: 1, minWidth: 0 }}>
      <div style={{ display: 'flex', alignItems: 'flex-start', gap: 10, marginBottom: 10 }}>
        <h2 style={{
          fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 27,
          color: 'var(--hds-txt)', margin: 0, flex: 1, lineHeight: 1.15,
          textShadow: heroTextShadow,
        }}>{title}</h2>
        {detail?.locked && (
          <span style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, padding: '2px 8px',
            borderRadius: 8, background: 'oklch(0.55 0.14 292 / 0.2)',
            border: '1px solid oklch(0.7 0.13 287 / 0.4)', color: 'var(--hds-violet)',
            flexShrink: 0, marginTop: 5,
          }}>LOCKED</span>
        )}
        {detail?.skip_scraping && (
          <span style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, padding: '2px 8px',
            borderRadius: 8, background: 'oklch(0.3 0.01 286 / 0.5)',
            border: '1px solid oklch(0.4 0.01 286 / 0.6)', color: 'var(--hds-txt-3)',
            flexShrink: 0, marginTop: 5,
          }}>SCRAPING OFF</span>
        )}
      </div>

      {/* Meta chips */}
      <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', marginBottom: 14 }}>
        {year && <span style={metaChip}>{year}</span>}
        {detail?.content_rating && <span style={metaChip}>{detail.content_rating}</span>}
        {rating != null && (
          <span style={{ ...metaChip, color: 'var(--hds-gold)', borderColor: 'oklch(0.83 0.13 84 / 0.4)' }}>
            ★ {rating.toFixed(1)}
          </span>
        )}
        <span style={metaChip}>{contentType === 'show' ? 'series' : 'film'}</span>
        {videoInfo && formatVideoInfo(videoInfo) && (
          <span style={metaChip}>{formatVideoInfo(videoInfo)}</span>
        )}
        {discoverResult && (
          <span style={{
            ...metaChip, color: srcColor,
            borderColor: discoverResult.source === 'tmdb' ? 'oklch(0.65 0.18 220 / 0.4)' : 'oklch(0.65 0.12 280 / 0.4)',
          }}>{discoverResult.source.toUpperCase()}</span>
        )}
        {discoverResult?.in_library && (
          <span style={{ ...metaChip, color: 'oklch(0.7 0.16 150)', borderColor: 'oklch(0.7 0.16 150 / 0.4)' }}>
            IN LIBRARY
          </span>
        )}
        {!discoverResult?.in_library && discoverResult?.request_status && (
          <span style={{
            ...metaChip,
            color:       discoverResult.request_status === 'approved' ? 'oklch(0.7 0.16 150)' : 'oklch(0.78 0.15 84)',
            borderColor: discoverResult.request_status === 'approved' ? 'oklch(0.7 0.16 150 / 0.4)' : 'oklch(0.78 0.15 84 / 0.4)',
          }}>
            {discoverResult.request_status === 'approved' ? 'APPROVED' : discoverResult.request_status === 'rejected' ? 'REJECTED' : 'REQUESTED'}
          </span>
        )}
      </div>

      {/* Genres */}
      {genres.length > 0 && (
        <div style={{ display: 'flex', gap: 7, flexWrap: 'wrap', marginBottom: 16 }}>
          {genres.map(g => (
            <span key={g} style={{
              fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
              padding: '3px 10px', borderRadius: 12,
              background: 'var(--hds-glass)', border: '1px solid var(--hds-glass-border)',
              color: 'var(--hds-txt-2)', letterSpacing: '0.05em',
            }}>{g}</span>
          ))}
        </div>
      )}

      {/* Overview — swaps to the focused episode's when one is hovered/focused */}
      {overview && (
        <p style={{
          fontFamily: "'JetBrains Mono', monospace", fontSize: 11.5, lineHeight: 1.75,
          color: 'var(--hds-txt-2)', margin: '0 0 16px', maxWidth: 760,
          textShadow: heroTextShadow,
        }}>{overview}</p>
      )}
    </div>
  )

  const secondaryInfo = (
    <>
      {/* Languages */}
      <LanguageChips languages={languages} />

      {/* Movie: credits */}
      {movie && (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 6, marginBottom: 6 }}>
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
              <span style={{ wordBreak: 'break-all' }}>
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
          <span style={{ wordBreak: 'break-all' }} title={show.folder_path}>
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
    <div style={{ marginTop: 32 }}>
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
    <div style={{ position: 'relative', height: '100%', overflow: 'hidden' }}>
      {/* Fixed backdrop — never scrolls; shrinks to the locked header's
          measured height once collapsed, back to full size once not. */}
      <div style={{
        position: 'absolute', top: 0, left: 0, right: 0, zIndex: 1,
        height: collapsed && headerHeight > 0 ? headerHeight : HERO_HEIGHT_CSS,
        background: backdropUrl
          ? `url(${backdropUrl}) center/cover no-repeat`
          : 'linear-gradient(135deg, oklch(0.12 0.04 292) 0%, oklch(0.18 0.06 270) 50%, oklch(0.14 0.03 280) 100%)',
        opacity: loading && !discoverResult ? 0.6 : 1,
        transition: 'height .3s ease, opacity .3s ease',
      }}>
        <div style={{
          position: 'absolute', inset: 0,
          background: 'linear-gradient(to right, oklch(0 0 0 / 0.75) 0%, oklch(0 0 0 / 0.3) 55%, transparent 100%)',
        }} />
        <div style={{
          position: 'absolute', inset: 0,
          background: 'linear-gradient(to top, var(--hds-bg) 0%, transparent 46%)',
        }} />
      </div>

      {/* Pinned above everything, independent of scroll or collapse state. */}
      <div style={{ position: 'absolute', top: 18, left: 24, zIndex: 20 }}>
        <BackButton onClick={onBack} overlay />
      </div>

      <div ref={scrollRef} style={{ position: 'relative', height: '100%', overflowY: 'auto' }} className="scrollbar-dark">
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
          className={`hds-media-detail-hero-container${collapsed ? ' hds-media-detail-hero-collapsed' : ''}`}
          style={{ position: 'sticky', top: 0, zIndex: 2, padding: '0 48px 24px' }}
        >
          {loading && !discoverResult ? (
            <DetailSkeleton />
          ) : (
            <>
              <div className="hds-media-detail-hero-row" style={{ display: 'flex', gap: 36, alignItems: 'flex-start', maxWidth: 1200 }}>
                {posterBox}
                {primaryInfo}
              </div>
              <div style={{ maxWidth: 1200, paddingLeft: 206 /* align under the info column above, not the poster */ }}>
                <div style={{ maxWidth: 760, display: 'flex', flexDirection: 'column', gap: 10 }}>
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
        <div style={{ padding: '0 48px 48px' }}>
          {!loading && actions && (
            <div style={{ maxWidth: 1200, paddingLeft: 206 /* align under the info column above, not the poster */ }}>
              <div style={{ maxWidth: 760 }}>{actions(media)}</div>
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
      style={{
        display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 11, letterSpacing: '0.06em',
        transition: 'color .12s, background .12s',
        ...(overlay
          ? {
              marginLeft: 24, // positioning handled by the pinned wrapper in MediaDetailHero
              padding: '7px 14px 7px 10px', borderRadius: 20,
              border: '1px solid var(--hds-glass-border)', background: 'var(--hds-glass)',
              backdropFilter: 'blur(8px)', color: 'oklch(0.92 0.01 285)',
            }
          : {
              background: 'none', border: 'none', padding: '18px 0 20px', color: 'var(--hds-txt-3)',
            }),
      }}
      onMouseEnter={e => (e.currentTarget.style.color = overlay ? '#fff' : 'var(--hds-txt)')}
      onMouseLeave={e => (e.currentTarget.style.color = overlay ? 'oklch(0.92 0.01 285)' : 'var(--hds-txt-3)')}
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
    <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
      <span style={{ color: 'var(--hds-txt-2)' }}>{label}</span>
      {' · '}{children}
    </div>
  )
}

function DetailSkeleton() {
  return (
    <div style={{ display: 'flex', gap: 32, paddingTop: 20 }}>
      <div className="hds-skeleton" style={{ width: 170, height: 255, borderRadius: 10, flexShrink: 0 }} />
      <div style={{ flex: 1, display: 'flex', flexDirection: 'column', gap: 12, maxWidth: 600 }}>
        {[240, 120, 90, 320, 280, 300, 220].map((w, i) => (
          <div key={i} className="hds-skeleton" style={{ height: 14, borderRadius: 4, width: w }} />
        ))}
      </div>
    </div>
  )
}
