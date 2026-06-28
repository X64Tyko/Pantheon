import { useRef, useState } from 'react'
import { MediaCard } from './MediaCard'
import type { MediaCardProps } from './MediaCard'
import type { LibraryDensity } from '../../api/types'

export type ShelfItem = Omit<MediaCardProps, 'density' | 'selected' | 'onClick'>

interface MediaShelfProps {
  title:       string
  items:       ShelfItem[]
  density?:    Exclude<LibraryDensity, 'rich'>
  onItemClick: (id: string, type: 'show' | 'movie') => void
  onViewAll?:  () => void
}

const CARD_WIDTH: Record<string, number> = { minimal: 120, standard: 160 }

export function MediaShelf({ title, items, density = 'standard', onItemClick, onViewAll }: MediaShelfProps) {
  const scrollRef = useRef<HTMLDivElement>(null)
  const [showArrows, setShowArrows] = useState(false)

  function scroll(dir: 'left' | 'right') {
    scrollRef.current?.scrollBy({ left: dir === 'right' ? 320 : -320, behavior: 'smooth' })
  }

  const cardWidth = CARD_WIDTH[density] ?? 160

  return (
    <div
      style={{ position: 'relative', padding: '0 0 8px' }}
      onMouseEnter={() => setShowArrows(true)}
      onMouseLeave={() => setShowArrows(false)}
    >
      <div style={{
        display: 'flex', alignItems: 'baseline', justifyContent: 'space-between',
        padding: '0 24px', marginBottom: 16,
      }}>
        <span style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 14, fontWeight: 600,
          color: 'var(--hds-txt)', letterSpacing: '0.03em',
        }}>{title}</span>
        {onViewAll && (
          <button onClick={onViewAll} style={{
            background: 'none', border: 'none', cursor: 'pointer',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
            color: 'var(--hds-txt-3)', letterSpacing: '0.06em',
          }}>View All →</button>
        )}
      </div>

      {showArrows && <ArrowBtn side="left" onClick={() => scroll('left')} />}

      <div ref={scrollRef} style={{
        display: 'flex', gap: 12, overflowX: 'auto', padding: '4px 24px',
        scrollbarWidth: 'none',
      }}>
        {items.map(item => (
          <div key={item.id} style={{ flexShrink: 0, width: cardWidth }}>
            <MediaCard
              {...item}
              density={density}
              selected={false}
              onClick={() => onItemClick(item.id, item.content_type)}
            />
          </div>
        ))}
      </div>

      {showArrows && <ArrowBtn side="right" onClick={() => scroll('right')} />}
    </div>
  )
}

function ArrowBtn({ side, onClick }: { side: 'left' | 'right'; onClick: () => void }) {
  return (
    <button onClick={onClick} style={{
      position: 'absolute', top: '50%', transform: 'translateY(-50%)',
      left: side === 'left' ? 4 : undefined,
      right: side === 'right' ? 4 : undefined,
      zIndex: 2, width: 36, height: 36, borderRadius: '50%', cursor: 'pointer',
      background: 'var(--hds-glass)', backdropFilter: 'blur(8px)',
      border: '1px solid var(--hds-glass-border)',
      color: 'var(--hds-txt)', fontSize: 22,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
    }}>
      {side === 'left' ? '‹' : '›'}
    </button>
  )
}
