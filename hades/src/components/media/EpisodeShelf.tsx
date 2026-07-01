import { useRef, useState } from 'react'
import { mediaUrl } from '../../api/client'
import type { Episode } from '../../api/types'

interface EpisodeShelfProps {
  seasonNumber: number
  seasonName?:  string
  episodes:     Episode[]
}

export function EpisodeShelf({ seasonNumber, seasonName, episodes }: EpisodeShelfProps) {
  const scrollRef = useRef<HTMLDivElement>(null)
  const [showArrows, setShowArrows] = useState(false)
  const scroll = (d: 'left' | 'right') =>
    scrollRef.current?.scrollBy({ left: d === 'right' ? 300 : -300, behavior: 'smooth' })

  const title = seasonName || (seasonNumber === 0 ? 'Specials' : `Season ${seasonNumber}`)

  return (
    <div
      style={{ position: 'relative', marginBottom: 20 }}
      onMouseEnter={() => setShowArrows(true)}
      onMouseLeave={() => setShowArrows(false)}
    >
      <div style={{ display: 'flex', alignItems: 'baseline', gap: 8, marginBottom: 10 }}>
        <span style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 13, fontWeight: 600,
          color: 'var(--hds-txt)', letterSpacing: '0.02em',
        }}>{title}</span>
        <span style={{
          fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)',
        }}>{episodes.length} episode{episodes.length === 1 ? '' : 's'}</span>
      </div>

      {showArrows && episodes.length > 3 && <EpArrow side="left" onClick={() => scroll('left')} />}
      <div ref={scrollRef} style={{
        display: 'flex', gap: 10, overflowX: 'auto', overflowY: 'hidden',
        paddingBottom: 4, scrollbarWidth: 'none',
      }}>
        {episodes.map(ep => <EpisodeTile key={ep.episode_id} episode={ep} />)}
      </div>
      {showArrows && episodes.length > 3 && <EpArrow side="right" onClick={() => scroll('right')} />}
    </div>
  )
}

function EpisodeTile({ episode }: { episode: Episode }) {
  const [hovered, setHovered] = useState(false)
  const [imgErr,  setImgErr]  = useState(false)
  const showImg = episode.thumb && !imgErr
  const code = `S${String(episode.season).padStart(2, '0')}E${String(episode.episode).padStart(2, '0')}`

  return (
    <div
      title={episode.overview || episode.title}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      style={{
        flexShrink: 0, width: 220, borderRadius: 8, overflow: 'hidden',
        border: `1px solid ${hovered ? 'var(--hds-line-s)' : 'var(--hds-line)'}`,
        background: 'var(--hds-bg-2)', transition: 'border-color .12s, transform .12s',
        transform: hovered ? 'translateY(-2px)' : 'none',
      }}
    >
      <div style={{
        aspectRatio: '16/9', width: '100%', position: 'relative', overflow: 'hidden',
        background: 'linear-gradient(135deg, oklch(0.18 0.03 287), oklch(0.13 0.02 285))',
      }}>
        {showImg ? (
          <img
            src={mediaUrl(`/api/episodes/${episode.episode_id}/thumb`)}
            alt={episode.title}
            onError={() => setImgErr(true)}
            style={{ width: '100%', height: '100%', objectFit: 'cover' }}
          />
        ) : (
          <span style={{
            position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)',
          }}>{code}</span>
        )}
        <span style={{
          position: 'absolute', top: 6, left: 6,
          background: 'oklch(0 0 0 / 0.6)', borderRadius: 4, padding: '2px 6px',
          fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'oklch(0.9 0.01 285)',
          letterSpacing: '0.04em',
        }}>{code}</span>
        {episode.duration_ms > 0 && (
          <span style={{
            position: 'absolute', bottom: 6, right: 6,
            background: 'oklch(0 0 0 / 0.6)', borderRadius: 4, padding: '2px 6px',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'oklch(0.9 0.01 285)',
          }}>{Math.round(episode.duration_ms / 60000)}m</span>
        )}
      </div>
      <div style={{ padding: '7px 9px 9px' }}>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, fontWeight: 600,
          color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }}>{episode.title || code}</div>
        {episode.air_date && (
          <div style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)', marginTop: 2,
          }}>{episode.air_date}</div>
        )}
      </div>
    </div>
  )
}

function EpArrow({ side, onClick }: { side: 'left' | 'right'; onClick: () => void }) {
  return (
    <button onClick={onClick} style={{
      position: 'absolute', top: 66, transform: 'translateY(-50%)',
      left: side === 'left' ? -6 : undefined, right: side === 'right' ? -6 : undefined,
      zIndex: 2, width: 28, height: 28, borderRadius: '50%', cursor: 'pointer',
      background: 'var(--hds-glass)', backdropFilter: 'blur(8px)',
      border: '1px solid var(--hds-glass-border)', color: 'var(--hds-txt)', fontSize: 16,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
    }}>
      {side === 'left' ? '‹' : '›'}
    </button>
  )
}
