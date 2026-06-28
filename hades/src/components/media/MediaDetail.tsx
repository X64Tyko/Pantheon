import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import type { ShowDetail, MovieDetail } from '../../api/types'
import { MatchBadge } from './MatchBadge'
import { goldBtnStyle } from '../../channel/styles'

interface MediaDetailProps {
  id:           string
  content_type: 'show' | 'movie'
  onClose:      () => void
}

const panel: React.CSSProperties = {
  width: 400, flexShrink: 0, height: '100%', overflow: 'hidden',
  display: 'flex', flexDirection: 'column',
  background: 'var(--hds-bg-2)', borderLeft: '1px solid var(--hds-line)',
}

const metaChip: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
  padding: '2px 8px', borderRadius: 10,
  border: '1px solid var(--hds-line-s)', color: 'var(--hds-txt-3)',
}

export function MediaDetail({ id, content_type, onClose }: MediaDetailProps) {
  const [show,    setShow]    = useState<ShowDetail | null>(null)
  const [movie,   setMovie]   = useState<MovieDetail | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    setLoading(true)
    setShow(null)
    setMovie(null)
    if (content_type === 'show') {
      api.getShow(id).then(d => setShow(d)).finally(() => setLoading(false))
    } else {
      api.getMovie(id).then(d => setMovie(d)).finally(() => setLoading(false))
    }
  }, [id, content_type])

  const detail    = show ?? movie
  const posterUrl = detail?.thumb ? `${detail.source_base_url}${detail.thumb}` : undefined
  const backdropUrl = detail?.art ? `${detail.source_base_url}${detail.art}` : undefined

  return (
    <div style={panel} className="hds-in">
      {/* Backdrop */}
      <div style={{ position: 'relative', height: 200, flexShrink: 0 }}>
        <div style={{
          position: 'absolute', inset: 0,
          background: backdropUrl
            ? `url(${backdropUrl}) center/cover no-repeat`
            : 'linear-gradient(135deg, oklch(0.12 0.04 292), oklch(0.16 0.03 280))',
        }} />
        <div style={{
          position: 'absolute', inset: 0,
          background: 'linear-gradient(to top, var(--hds-bg-2) 0%, transparent 55%)',
        }} />
        <button onClick={onClose} style={{
          position: 'absolute', top: 12, right: 12,
          width: 28, height: 28, borderRadius: '50%', border: 'none', cursor: 'pointer',
          background: 'oklch(0 0 0 / 0.5)', color: 'oklch(0.8 0.01 285)', fontSize: 18, lineHeight: '28px',
        }}>×</button>
        {posterUrl && (
          <img src={posterUrl} alt="" style={{
            position: 'absolute', bottom: -32, left: 20,
            width: 60, height: 90, objectFit: 'cover', borderRadius: 6,
            boxShadow: '0 4px 20px oklch(0 0 0 / 0.5)',
          }} />
        )}
      </div>

      {/* Body */}
      <div style={{
        flex: 1, overflowY: 'auto',
        padding: posterUrl ? '44px 20px 24px' : '16px 20px 24px',
      }}>
        {loading && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
            {[180, 100, 80, 260, 200].map((w, i) => (
              <div key={i} className="hds-skeleton" style={{ height: 14, borderRadius: 4, width: w }} />
            ))}
          </div>
        )}

        {detail && (
          <>
            <div style={{ display: 'flex', alignItems: 'flex-start', gap: 8, marginBottom: 8 }}>
              <h2 style={{
                fontFamily: "'Chakra Petch', sans-serif", fontSize: 18, fontWeight: 700,
                color: 'var(--hds-txt)', margin: 0, flex: 1, lineHeight: 1.2,
              }}>{detail.title}</h2>
              {detail.locked && (
                <span style={{
                  fontFamily: "'JetBrains Mono', monospace", fontSize: 9, padding: '2px 7px',
                  borderRadius: 8, background: 'oklch(0.55 0.14 292 / 0.2)',
                  border: '1px solid oklch(0.7 0.13 287 / 0.4)', color: 'var(--hds-violet)',
                  flexShrink: 0, marginTop: 3,
                }}>LOCKED</span>
              )}
            </div>

            <div style={{ display: 'flex', gap: 10, marginBottom: 12, flexWrap: 'wrap' }}>
              {detail.year && <span style={metaChip}>{detail.year}</span>}
              {detail.content_rating && <span style={metaChip}>{detail.content_rating}</span>}
              {detail.audience_rating != null && (
                <span style={{ ...metaChip, color: 'var(--hds-gold)', borderColor: 'oklch(0.83 0.13 84 / 0.4)' }}>
                  ★ {detail.audience_rating.toFixed(1)}
                </span>
              )}
            </div>

            {detail.genres.length > 0 && (
              <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap', marginBottom: 14 }}>
                {detail.genres.map(g => <span key={g} style={metaChip}>{g}</span>)}
              </div>
            )}

            {detail.overview && (
              <p style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 11, lineHeight: 1.7,
                color: 'var(--hds-txt-2)', margin: '0 0 18px',
              }}>{detail.overview}</p>
            )}

            {/* Match section */}
            <div style={{
              padding: '12px 14px', borderRadius: 8,
              background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)', marginBottom: 14,
            }}>
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
                <MatchBadge status="unscraped" size="md" />
                <button disabled style={{
                  fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
                  padding: '3px 10px', borderRadius: 6, cursor: 'not-allowed',
                  border: '1px solid var(--hds-line)', background: 'transparent',
                  color: 'var(--hds-txt-3)', opacity: 0.5,
                }}>Review Match</button>
              </div>
            </div>

            <button disabled style={{
              ...goldBtnStyle, width: '100%', opacity: 0.4, cursor: 'not-allowed', marginBottom: 18,
              boxSizing: 'border-box',
            }}>
              Push to Sources
            </button>

            {/* Show: seasons */}
            {show && show.seasons.length > 0 && (
              <div>
                <div style={{
                  fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
                  color: 'var(--hds-txt-3)', letterSpacing: '0.08em', marginBottom: 10,
                }}>SEASONS</div>
                <div style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
                  {show.seasons.map(s => (
                    <div key={s.number} style={{
                      padding: '8px 12px', borderRadius: 6,
                      border: '1px solid var(--hds-line-s)', background: 'var(--hds-bg)',
                      display: 'flex', justifyContent: 'space-between',
                    }}>
                      <span style={{
                        fontFamily: "'Chakra Petch', sans-serif", fontSize: 11,
                        color: 'var(--hds-txt-2)', fontWeight: 500,
                      }}>{s.name || `Season ${s.number}`}</span>
                      <span style={{
                        fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)',
                      }}>S{String(s.number).padStart(2, '0')}</span>
                    </div>
                  ))}
                </div>
              </div>
            )}

            {/* Movie: credits */}
            {movie && (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                {movie.director && (
                  <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                    <span style={{ color: 'var(--hds-txt-2)' }}>Director</span> · {movie.director}
                  </div>
                )}
                {movie.studio && (
                  <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                    <span style={{ color: 'var(--hds-txt-2)' }}>Studio</span> · {movie.studio}
                  </div>
                )}
                {movie.duration_ms > 0 && (
                  <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                    <span style={{ color: 'var(--hds-txt-2)' }}>Runtime</span> · {Math.round(movie.duration_ms / 60000)} min
                  </div>
                )}
              </div>
            )}
          </>
        )}
      </div>
    </div>
  )
}
