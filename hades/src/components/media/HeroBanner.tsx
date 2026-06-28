import { goldBtnStyle, ghostBtnStyle } from '../../channel/styles'
import type { MediaHeroItem } from '../../api/types'

interface HeroBannerProps {
  item:           MediaHeroItem
  onViewDetail:   () => void
  onAddToChannel: () => void
}

export function HeroBanner({ item, onViewDetail, onAddToChannel }: HeroBannerProps) {
  const { title, year, overview, backdrop_url, genres, rating } = item

  const bg = backdrop_url
    ? `url(${backdrop_url}) center/cover no-repeat`
    : 'linear-gradient(135deg, oklch(0.12 0.04 292) 0%, oklch(0.18 0.06 270) 50%, oklch(0.14 0.03 280) 100%)'

  return (
    <div style={{ position: 'relative', height: '60vh', minHeight: 380, background: bg, overflow: 'hidden', flexShrink: 0 }}>
      <div style={{
        position: 'absolute', inset: 0,
        background: `
          linear-gradient(to right, oklch(0 0 0 / 0.85) 0%, oklch(0 0 0 / 0.4) 60%, transparent 100%),
          linear-gradient(to top, var(--hds-bg) 0%, transparent 28%)
        `,
      }} />

      <div style={{ position: 'absolute', bottom: 0, left: 0, padding: '0 64px 48px', maxWidth: 640 }}>
        {genres && genres.length > 0 && (
          <div style={{ display: 'flex', gap: 8, marginBottom: 16, flexWrap: 'wrap' }}>
            {genres.slice(0, 4).map(g => (
              <span key={g} style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
                padding: '3px 10px', borderRadius: 12,
                background: 'var(--hds-glass)', border: '1px solid var(--hds-glass-border)',
                color: 'var(--hds-txt-2)', letterSpacing: '0.06em',
              }}>{g}</span>
            ))}
          </div>
        )}

        <h1 style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 36, fontWeight: 700,
          color: 'oklch(1 0 0)', margin: 0, lineHeight: 1.1, letterSpacing: '-0.02em',
        }}>{title}</h1>

        <div style={{
          display: 'flex', alignItems: 'center', gap: 16, marginTop: 10, marginBottom: 14,
          fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-2)',
        }}>
          {year && <span>{year}</span>}
          {rating != null && <span style={{ color: 'var(--hds-gold)' }}>★ {rating.toFixed(1)}</span>}
        </div>

        {overview && (
          <p style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 12, lineHeight: 1.6,
            color: 'oklch(0.75 0.01 285)', margin: '0 0 24px',
            display: '-webkit-box', WebkitLineClamp: 2, WebkitBoxOrient: 'vertical', overflow: 'hidden',
          }}>{overview}</p>
        )}

        <div style={{ display: 'flex', gap: 12 }}>
          <button className="hds-btn-gold" style={goldBtnStyle} onClick={onViewDetail}>
            View Details
          </button>
          <button className="hds-btn-ghost" style={ghostBtnStyle} onClick={onAddToChannel}>
            + Add to Channel
          </button>
        </div>
      </div>
    </div>
  )
}
