import { useState } from 'react'
import { useFocusable } from '../nav/useFocusable'

export interface TvMediaCardProps {
  id:           string
  title:        string
  year?:        number
  thumb_url?:   string
  rating?:      number
  content_type: 'show' | 'movie'
  onClick:      () => void
}

// Deliberately simpler than MediaCard (no density levels, no match badges —
// those are library-management concerns that don't belong on a 10-foot
// browse surface) and bigger, for legibility at TV viewing distance.
export function TvMediaCard({ title, year, thumb_url, rating, content_type, onClick }: TvMediaCardProps) {
  const [hovered, setHovered] = useState(false)
  const [imgErr,  setImgErr]  = useState(false)
  const showImg = thumb_url && !imgErr

  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `tv-media-card-${content_type}-${title}`,
    onEnterPress: onClick,
  })
  const active = hovered || focused

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      style={{
        borderRadius: 12, overflow: 'hidden', cursor: 'pointer',
        boxShadow: active ? '0 0 0 3px var(--hds-violet), 0 8px 32px oklch(0.55 0.14 292 / 0.35)' : 'none',
        transform: active ? 'scale(1.045)' : 'scale(1)',
        transition: 'transform .15s cubic-bezier(0.2,0,0.2,1), box-shadow .15s',
      }}
    >
      <div style={{
        aspectRatio: '2/3', width: '100%', position: 'relative',
        background: 'linear-gradient(135deg, oklch(0.18 0.03 287), oklch(0.13 0.02 285))',
        display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
      }}>
        {showImg ? (
          <img
            src={thumb_url} alt={title} onError={() => setImgErr(true)}
            style={{ width: '100%', height: '100%', objectFit: 'cover' }}
          />
        ) : (
          <span style={{
            fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700,
            fontSize: 32, color: 'var(--hds-violet)', opacity: 0.4,
          }}>
            {title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
        {rating != null && (
          <div style={{
            position: 'absolute', top: 10, right: 10,
            background: 'oklch(0 0 0 / 0.6)', borderRadius: 6, padding: '3px 8px',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 13, color: 'var(--hds-gold)',
          }}>★ {rating.toFixed(1)}</div>
        )}
      </div>
      <div style={{ padding: '12px 6px 4px' }}>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 16, fontWeight: 600,
          color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }}>{title}</div>
        <div style={{
          fontFamily: "'JetBrains Mono', monospace", fontSize: 13, color: 'var(--hds-txt-3)', marginTop: 3,
        }}>
          {year}{year && ' · '}{content_type}
        </div>
      </div>
    </div>
  )
}
