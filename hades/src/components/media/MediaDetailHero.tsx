import { type ReactNode } from 'react'
import type { ScraperSearchResult, VideoInfo } from '../../api/types'
import { EpisodeShelf } from './EpisodeShelf'
import { LanguageChips } from './LanguageChips'
import { useFocusable } from '../../nav/useFocusable'
import { useNavBack } from '../../nav/back'
import { folderBaseName, useMediaDetail, type MediaDetailResult } from './useMediaDetail'
import { heroTextShadow } from '../../channel/styles'
import { useScrollCollapse } from './useScrollCollapse'

// Backdrop height and how far the expanded header overlaps into its lower
// edge (same composition the fixed-height hero used to have). The collapse
// threshold is set to match HERO_HEIGHT - HERO_OVERLAP exactly, so the
// header finishes shrinking right as it reaches the top of the viewport and
// locks there, instead of snapping before or lagging after.
const HERO_HEIGHT  = 380
const HERO_OVERLAP = 40

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
  /** Extra library-management controls, rendered between the overview and the season shelves. Render-prop so it shares this component's own useMediaDetail() result. */
  actions?:        (media: MediaDetailResult) => ReactNode
  /** Admin/details tooling, rendered at the very bottom, after the season shelves. */
  afterShelves?:   ReactNode
}

const metaChip: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
  padding: '3px 9px', borderRadius: 10,
  border: '1px solid var(--hds-line-s)', color: 'var(--hds-txt-3)',
}

export function MediaDetailHero({ id, content_type, discoverResult, onBack, actions, afterShelves }: MediaDetailHeroProps) {
  useNavBack(onBack) // Escape/Backspace closes the detail view, same contract as PlayerPage
  const media = useMediaDetail({ id, content_type, discoverResult })
  const {
    show, movie, loading, detail, contentType,
    posterUrl, backdropUrl, title, year, overview, genres, rating,
    seasonsWithEpisodes, languages, videoInfo, folderName, fileName,
    setFocusedEpisode,
  } = media

  const { scrollRef, collapsed } = useScrollCollapse(HERO_HEIGHT - HERO_OVERLAP)

  const srcColor = discoverResult?.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)'

  const posterBox = (
    <div className="hds-media-detail-poster" style={{
      width: collapsed ? 52 : 170, height: collapsed ? 78 : 255, borderRadius: collapsed ? 6 : 10,
      overflow: 'hidden', flexShrink: 0,
      background: 'var(--hds-bg-3)', boxShadow: '0 8px 32px oklch(0 0 0 / 0.5)',
      transition: 'width .25s ease, height .25s ease, border-radius .25s ease',
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
      <div style={{ display: 'flex', alignItems: 'flex-start', gap: 10, marginBottom: collapsed ? 0 : 10 }}>
        <h2 style={{
          fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700,
          fontSize: collapsed ? 16 : 27,
          color: 'var(--hds-txt)', margin: 0, flex: 1, lineHeight: 1.15,
          textShadow: heroTextShadow, transition: 'font-size .25s ease',
          whiteSpace: collapsed ? 'nowrap' : undefined,
          overflow: collapsed ? 'hidden' : undefined,
          textOverflow: collapsed ? 'ellipsis' : undefined,
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

      {/* Meta chips / genres / overview — collapsed via max-height+opacity
          rather than unmounted outright, so the header reads as shrinking
          rather than popping. */}
      <div style={{
        maxHeight: collapsed ? 0 : 420, opacity: collapsed ? 0 : 1, overflow: 'hidden',
        transition: 'max-height .25s ease, opacity .18s ease',
      }}>
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

      {/* Extra library actions */}
      {actions?.(media)}
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
  // for 10-foot viewing). The poster/title/genres/overview block starts in
  // its usual spot overlapping the backdrop's lower edge, then scrolls up
  // with the page and locks at the top once it gets there, shrinking into a
  // slim header (`collapsed`, from useScrollCollapse) — everything else
  // (secondary metadata, season shelves, fix-match panel) scrolls
  // underneath it with the full remaining viewport instead of competing
  // with a permanently fixed hero.
  return (
    <div style={{ position: 'relative', height: '100%', overflow: 'hidden' }}>
      {/* Pinned above the scroll regardless of how far the content below has moved. */}
      <div style={{ position: 'absolute', top: 18, left: 24, zIndex: 20 }}>
        <BackButton onClick={onBack} overlay />
      </div>

      <div ref={scrollRef} style={{ position: 'relative', height: '100%', overflowY: 'auto' }} className="scrollbar-dark">
        {/* Backdrop — sized for the expanded header, scrolls with the rest
            of the content (it's a normal descendant of this scroll
            container, not fixed/pinned). By the time the sticky header
            below locks at the top it has already scrolled out of view, so
            it needs no collapse handling of its own. */}
        <div style={{
          position: 'absolute', top: 0, left: 0, right: 0, height: HERO_HEIGHT, zIndex: 0,
          background: backdropUrl
            ? `url(${backdropUrl}) center/cover no-repeat`
            : 'linear-gradient(135deg, oklch(0.12 0.04 292) 0%, oklch(0.18 0.06 270) 50%, oklch(0.14 0.03 280) 100%)',
          opacity: loading && !discoverResult ? 0.6 : 1, transition: 'opacity .3s ease',
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

        {/* Spacer — pushes the sticky header down to its expanded starting
            position (overlapping the backdrop's lower edge, same starting
            composition the old fixed-height layout had). Past this point
            the header has nowhere left to go and locks at the top. */}
        <div style={{ height: HERO_HEIGHT - HERO_OVERLAP }} />

        <div
          className={`hds-media-detail-hero-container${collapsed ? ' hds-media-detail-hero-collapsed' : ''}`}
          style={{
            position: 'sticky', top: 0, zIndex: 2,
            padding: collapsed ? '14px 48px' : '0 48px 24px',
            background: collapsed ? 'var(--hds-bg)' : 'transparent',
            borderBottom: `1px solid ${collapsed ? 'var(--hds-line-s)' : 'transparent'}`,
            boxShadow: collapsed ? '0 4px 20px oklch(0 0 0 / 0.35)' : 'none',
            transition: 'padding .25s ease, background .25s ease, border-color .25s ease, box-shadow .25s ease',
          }}
        >
          {loading && !discoverResult ? (
            <DetailSkeleton />
          ) : (
            <div
              className="hds-media-detail-hero-row"
              style={{
                display: 'flex', gap: collapsed ? 18 : 36,
                alignItems: collapsed ? 'center' : 'flex-start', maxWidth: 1200,
                transition: 'gap .25s ease',
              }}
            >
              {posterBox}
              {primaryInfo}
            </div>
          )}
        </div>

        <div style={{ padding: '0 48px 48px' }}>
          {!loading && (
            <div style={{ maxWidth: 1200, paddingLeft: 206 /* align under the info column above, not the poster */ }}>
              <div style={{ maxWidth: 760 }}>{secondaryInfo}</div>
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
