import { useState } from 'react'
import type { MatchStatus } from './MatchBadge'
import type { LibraryDensity } from '../../api/types'

export interface MediaCardProps {
  id:                string
  title:             string
  year?:             number
  poster_url?:       string
  thumb_url?:        string
  content_type:      'show' | 'movie'
  genres?:           string[]
  rating?:           number
  locked?:           boolean
  match_status?:     MatchStatus
  match_score?:      number
  is_4k?:            boolean
  hdr_type?:         string
  network?:          string
  network_logo_url?: string
  density:           LibraryDensity
  selected?:         boolean
  onClick?:          () => void
}

const RING_COLOR: Record<string, string> = {
  matched:   'var(--hds-match-green)',
  uncertain: 'var(--hds-match-amber)',
  unmatched: 'var(--hds-match-red)',
  unscraped: 'var(--hds-match-grey)',
}

function initials(title: string): string {
  return title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()
}

export function MediaCard(props: MediaCardProps) {
  const {
    id, title, year, poster_url, thumb_url, content_type,
    genres, rating, locked, match_status, match_score,
    is_4k, hdr_type, network, network_logo_url,
    density, selected, onClick,
  } = props

  const [hovered, setHovered] = useState(false)
  const [imgError, setImgError] = useState(false)

  const imgUrl = poster_url ?? thumb_url
  const showImg = imgUrl && !imgError

  const ringColor = match_status ? RING_COLOR[match_status] : null
  const glow = `0 0 0 2px var(--hds-violet), 0 8px 32px oklch(0.55 0.14 292 / 0.3)`
  const ringGlow = ringColor ? `0 0 0 3px ${ringColor}, 0 0 12px ${ringColor}40` : null

  const boxShadow = selected
    ? glow
    : hovered
      ? (density === 'rich' && ringColor ? `${ringGlow}, ${glow}` : glow)
      : (density === 'rich' && ringColor ? ringGlow ?? 'none' : 'none')

  const cardStyle: React.CSSProperties = {
    borderRadius: 10,
    overflow: 'hidden',
    cursor: 'pointer',
    transform: hovered ? 'scale(1.03)' : 'scale(1)',
    boxShadow: boxShadow ?? undefined,
    transition: 'transform 0.15s cubic-bezier(0.2,0,0.2,1), box-shadow 0.15s',
    position: 'relative',
    width: density === 'minimal' ? 160 : undefined,
    flexShrink: density === 'minimal' ? 0 : undefined,
  }

  const posterStyle: React.CSSProperties = {
    aspectRatio: '2/3',
    width: '100%',
    background: showImg ? undefined : `
      linear-gradient(135deg,
        oklch(0.18 0.03 287) 0%,
        oklch(0.13 0.02 285) 100%
      )
    `,
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
    position: 'relative',
    overflow: 'hidden',
  }

  const poster = (
    <div style={posterStyle}>
      {showImg ? (
        <img
          src={imgUrl}
          alt={title}
          onError={() => setImgError(true)}
          style={{ width: '100%', height: '100%', objectFit: 'cover', display: 'block' }}
        />
      ) : (
        <span style={{
          fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700,
          fontSize: 28, color: 'var(--hds-violet)', opacity: 0.5,
          letterSpacing: '0.05em',
        }}>
          {initials(title)}
        </span>
      )}

      {/* Rich: bottom gradient bar with rating + HDR badges */}
      {density === 'rich' && (
        <>
          <div style={{
            position: 'absolute', bottom: 0, left: 0, right: 0,
            background: 'linear-gradient(to top, oklch(0 0 0 / 0.85) 0%, transparent 100%)',
            padding: '20px 8px 8px',
            display: 'flex', alignItems: 'flex-end', justifyContent: 'space-between',
          }}>
            {rating != null && (
              <span style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
                color: 'var(--hds-gold)', fontWeight: 600,
              }}>★ {rating.toFixed(1)}</span>
            )}
            <div style={{ display: 'flex', gap: 4, marginLeft: 'auto' }}>
              {is_4k && (
                <span style={{
                  fontSize: 9, fontFamily: "'JetBrains Mono', monospace",
                  padding: '1px 5px', borderRadius: 4,
                  background: 'oklch(0.55 0.14 292 / 0.8)',
                  color: 'oklch(1 0 0)', fontWeight: 700, letterSpacing: '0.06em',
                }}>4K</span>
              )}
              {hdr_type && (
                <span style={{
                  fontSize: 9, fontFamily: "'JetBrains Mono', monospace",
                  padding: '1px 5px', borderRadius: 4,
                  background: hdr_type === 'DV'
                    ? 'oklch(0.83 0.13 84 / 0.8)'
                    : 'oklch(0.42 0.08 285 / 0.8)',
                  color: 'oklch(1 0 0)', fontWeight: 700, letterSpacing: '0.04em',
                }}>{hdr_type}</span>
              )}
            </div>
          </div>

          {/* Lock indicator top-left */}
          {locked && (
            <div style={{
              position: 'absolute', top: 8, left: 8,
              background: 'oklch(0 0 0 / 0.6)', borderRadius: 4, padding: '2px 5px',
              fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
              color: 'var(--hds-violet)', letterSpacing: '0.06em',
            }}>🔒</div>
          )}

          {/* Match score chip (bottom overlay, next to rating) */}
          {match_status && match_score != null && (
            <div style={{
              position: 'absolute', bottom: 8, left: 8,
              background: 'oklch(0 0 0 / 0.65)', borderRadius: 4, padding: '2px 6px',
              fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
              color: RING_COLOR[match_status] ?? 'var(--hds-txt-3)',
            }}>{Math.round(match_score * 100)}%</div>
          )}

          {/* Network logo top-right */}
          {(network_logo_url || network) && (
            <div style={{
              position: 'absolute', top: 8, right: 8,
              background: 'oklch(0 0 0 / 0.6)', borderRadius: 6, padding: 4,
            }}>
              {network_logo_url ? (
                <img src={network_logo_url} alt={network}
                  style={{ width: 24, height: 24, objectFit: 'contain' }} />
              ) : (
                <span style={{
                  fontSize: 8, fontFamily: "'JetBrains Mono', monospace",
                  color: 'oklch(0.8 0.01 285)', letterSpacing: '0.06em',
                  display: 'block', maxWidth: 40, textAlign: 'center', lineHeight: 1.2,
                }}>{network}</span>
              )}
            </div>
          )}
        </>
      )}

      {/* Minimal: hover title overlay */}
      {density === 'minimal' && hovered && (
        <div style={{
          position: 'absolute', bottom: 0, left: 0, right: 0,
          background: 'linear-gradient(to top, oklch(0 0 0 / 0.9) 0%, transparent 100%)',
          padding: '24px 10px 10px',
        }}>
          <div style={{
            fontFamily: "'Chakra Petch', sans-serif", fontSize: 11,
            fontWeight: 600, color: 'oklch(1 0 0)',
            overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
          }}>{title}</div>
        </div>
      )}
    </div>
  )

  return (
    <div
      data-media-id={id}
      style={cardStyle}
      onClick={onClick}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
    >
      {poster}

      {density === 'standard' && (
        <div style={{ padding: '8px 2px 2px' }}>
          <div style={{
            fontFamily: "'Chakra Petch', sans-serif", fontSize: 12, fontWeight: 600,
            color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
          }}>{title}</div>
          <div style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
            color: 'var(--hds-txt-3)', marginTop: 3,
          }}>
            {year}{year && ' · '}{content_type}
          </div>
        </div>
      )}

      {density === 'rich' && (
        <div style={{ padding: '8px 4px 4px' }}>
          <div style={{ display: 'flex', alignItems: 'baseline', gap: 6, marginBottom: 5 }}>
            <div style={{
              fontFamily: "'Chakra Petch', sans-serif", fontSize: 12, fontWeight: 600,
              color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', flex: 1,
            }}>{title}</div>
            {year && (
              <span style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
                color: 'var(--hds-txt-3)', flexShrink: 0,
              }}>{year}</span>
            )}
          </div>
          {genres && genres.length > 0 && (
            <div style={{ display: 'flex', gap: 4, flexWrap: 'wrap' }}>
              {genres.slice(0, 3).map(g => (
                <span key={g} style={{
                  fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
                  padding: '2px 6px', borderRadius: 10,
                  border: '1px solid var(--hds-line-s)',
                  color: 'var(--hds-txt-3)',
                }}>{g}</span>
              ))}
            </div>
          )}
        </div>
      )}
    </div>
  )
}
