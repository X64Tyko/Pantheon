import { type ReactNode, useEffect, useState } from 'react'
import { api, mediaUrl } from '../../api/client'
import type { Episode, MediaLanguages, ScraperSearchResult, ShowDetail, MovieDetail } from '../../api/types'
import { EpisodeShelf } from './EpisodeShelf'
import { LanguageChips } from './LanguageChips'

interface MediaDetailHeroProps {
  id?:             string
  content_type?:   'show' | 'movie'
  discoverResult?: ScraperSearchResult
  onBack:          () => void
  /** Extra library-management controls, rendered between the overview and the season shelves. */
  actions?:        ReactNode
  /** Admin/details tooling, rendered at the very bottom, after the season shelves. */
  afterShelves?:   ReactNode
  /** Home page already renders its own rotating backdrop above this component. */
  showBackdrop?:   boolean
}

const metaChip: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
  padding: '3px 9px', borderRadius: 10,
  border: '1px solid var(--hds-line-s)', color: 'var(--hds-txt-3)',
}

function showThumbUrl(id: string) { return mediaUrl(`/api/shows/${id}/thumb`) }
function showArtUrl(id: string)   { return mediaUrl(`/api/shows/${id}/art`) }
function movieThumbUrl(id: string) { return mediaUrl(`/api/movies/${id}/thumb`) }
function movieArtUrl(id: string)   { return mediaUrl(`/api/movies/${id}/art`) }

export function MediaDetailHero({ id, content_type, discoverResult, onBack, actions, afterShelves, showBackdrop = true }: MediaDetailHeroProps) {
  const [show,      setShow]      = useState<ShowDetail | null>(null)
  const [movie,     setMovie]     = useState<MovieDetail | null>(null)
  const [loading,   setLoading]   = useState(!discoverResult)
  const [episodes,  setEpisodes]  = useState<Episode[]>([])
  const [languages, setLanguages] = useState<MediaLanguages | null>(null)

  useEffect(() => {
    if (discoverResult) return
    if (!id || !content_type) return
    setLoading(true)
    setShow(null); setMovie(null); setEpisodes([]); setLanguages(null)

    if (content_type === 'show') {
      api.getShow(id).then(setShow).finally(() => setLoading(false))
      api.getEpisodes(id).then(setEpisodes).catch(() => {})
      api.getShowLanguages(id).then(setLanguages).catch(() => {})
    } else {
      api.getMovie(id).then(setMovie).finally(() => setLoading(false))
      api.getMovieLanguages(id).then(setLanguages).catch(() => {})
    }
  }, [id, content_type, discoverResult])

  const detail = discoverResult ? null : (show ?? movie)
  const contentType: 'show' | 'movie' = discoverResult?.content_type ?? content_type ?? 'show'

  const posterUrl = discoverResult?.poster_url
    ?? (id && detail?.thumb ? (contentType === 'show' ? showThumbUrl(id) : movieThumbUrl(id)) : undefined)
  const backdropUrl = discoverResult?.poster_url
    ?? (id && detail?.art ? (contentType === 'show' ? showArtUrl(id) : movieArtUrl(id)) : undefined)

  const title    = discoverResult?.title    ?? detail?.title    ?? ''
  const year     = discoverResult?.year     ?? detail?.year
  const overview = discoverResult?.overview ?? detail?.overview ?? ''
  const genres   = detail?.genres ?? []
  const rating   = detail?.audience_rating
  const srcColor = discoverResult?.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)'

  const seasonsWithEpisodes = show
    ? show.seasons
        .map(s => ({ ...s, episodes: episodes.filter(e => e.season === s.number).sort((a, b) => a.episode - b.episode) }))
        .filter(s => s.episodes.length > 0)
    : []

  return (
    <div>
      {showBackdrop && (
        <div style={{
          position: 'relative', height: '42vh', minHeight: 300, flexShrink: 0,
          background: backdropUrl
            ? `url(${backdropUrl}) center/cover no-repeat`
            : 'linear-gradient(135deg, oklch(0.12 0.04 292) 0%, oklch(0.18 0.06 270) 50%, oklch(0.14 0.03 280) 100%)',
          marginBottom: -60, paddingBottom: 60,
          opacity: loading && !discoverResult ? 0.6 : 1, transition: 'opacity .3s ease',
        }}>
          <div style={{
            position: 'absolute', inset: 0,
            background: 'linear-gradient(to right, oklch(0 0 0 / 0.75) 0%, oklch(0 0 0 / 0.3) 55%, transparent 100%)',
          }} />
          <div style={{
            position: 'absolute', inset: 0,
            background: 'linear-gradient(to top, var(--hds-bg) 0%, transparent 46%)',
            paddingBottom: 60, marginBottom: -60,
          }} />
          <BackButton onClick={onBack} overlay />
        </div>
      )}

      <div style={{ position: 'relative', zIndex: 2, padding: '0 48px 48px' }}>
        {!showBackdrop && <BackButton onClick={onBack} />}

        {loading && !discoverResult ? (
          <DetailSkeleton />
        ) : (
          <div style={{ display: 'flex', gap: 36, alignItems: 'flex-start', maxWidth: 980, paddingTop: showBackdrop ? 20 : 0 }}>
            {/* Poster */}
            <div style={{
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

            {/* Info */}
            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ display: 'flex', alignItems: 'flex-start', gap: 10, marginBottom: 10 }}>
                <h2 style={{
                  fontFamily: "'Chakra Petch', sans-serif", fontSize: 27, fontWeight: 700,
                  color: 'var(--hds-txt)', margin: 0, flex: 1, lineHeight: 1.15,
                }}>{title}</h2>
                {detail?.locked && (
                  <span style={{
                    fontFamily: "'JetBrains Mono', monospace", fontSize: 9, padding: '2px 8px',
                    borderRadius: 8, background: 'oklch(0.55 0.14 292 / 0.2)',
                    border: '1px solid oklch(0.7 0.13 287 / 0.4)', color: 'var(--hds-violet)',
                    flexShrink: 0, marginTop: 5,
                  }}>LOCKED</span>
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

              {/* Overview */}
              {overview && (
                <p style={{
                  fontFamily: "'JetBrains Mono', monospace", fontSize: 11.5, lineHeight: 1.75,
                  color: 'var(--hds-txt-2)', margin: '0 0 16px', maxWidth: 760,
                }}>{overview}</p>
              )}

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
                </div>
              )}
              {show?.studio && <MetaRow label="Studio">{show.studio}</MetaRow>}

              {/* Extra library actions — between description and season shelves */}
              {actions}
            </div>
          </div>
        )}

        {/* Season shelves — intentionally full-width (not capped like the info column) so each shelf shows more than a handful of tiles before scrolling */}
        {seasonsWithEpisodes.length > 0 && (
          <div style={{ marginTop: 32 }}>
            {seasonsWithEpisodes.map(s => (
              <EpisodeShelf key={s.number} seasonNumber={s.number} seasonName={s.name} episodes={s.episodes} />
            ))}
          </div>
        )}

        {afterShelves}
      </div>
    </div>
  )
}

function BackButton({ onClick, overlay }: { onClick: () => void; overlay?: boolean }) {
  return (
    <button
      onClick={onClick}
      style={{
        display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 11, letterSpacing: '0.06em',
        transition: 'color .12s, background .12s',
        ...(overlay
          ? {
              position: 'absolute', top: 18, left: 24, zIndex: 3,
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
