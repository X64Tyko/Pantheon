import { useRef, useState } from 'react'
import { MediaCard } from './MediaCard'
import type { MediaCardProps } from './MediaCard'
import type { LibraryDensity } from '../../api/types'
import styles from './MediaShelf.module.css'

export type ShelfItem = Omit<MediaCardProps, 'density' | 'selected' | 'onClick'>

interface MediaShelfProps {
  title:       string
  items:       ShelfItem[]
  density?:    Exclude<LibraryDensity, 'rich'>
  onItemClick: (id: string, type: 'show' | 'movie') => void
  onViewAll?:  () => void
}

export function MediaShelf({ title, items, density = 'standard', onItemClick, onViewAll }: MediaShelfProps) {
  const scrollRef = useRef<HTMLDivElement>(null)
  const [showArrows, setShowArrows] = useState(false)

  function scroll(dir: 'left' | 'right') {
    scrollRef.current?.scrollBy({ left: dir === 'right' ? 320 : -320, behavior: 'smooth' })
  }

  const cardSlotClass = density === 'minimal' ? styles.cardSlotMinimal : styles.cardSlotStandard

  return (
    <div
      className={styles.root}
      onMouseEnter={() => setShowArrows(true)}
      onMouseLeave={() => setShowArrows(false)}
    >
      <div className={styles.headerRow}>
        <span className={styles.title}>{title}</span>
        {onViewAll && (
          <button onClick={onViewAll} className={styles.viewAllButton}>View All →</button>
        )}
      </div>

      {showArrows && <ArrowBtn side="left" onClick={() => scroll('left')} />}

      <div ref={scrollRef} className={styles.scrollRow}>
        {items.map(item => (
          <div key={item.id} className={cardSlotClass}>
            <MediaCard
              {...item}
              density={density}
              selected={false}
              // MediaShelf is never handed episode items today (MediaCard's
              // content_type widened to include 'episode' for the Library
              // grid's own direct rendering — see MediaGrid.tsx) — this
              // narrows back since onItemClick here is show/movie only.
              onClick={() => onItemClick(item.id, item.content_type as 'show' | 'movie')}
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
    <button onClick={onClick} className={`${styles.arrowButton} ${side === 'left' ? styles.arrowLeft : styles.arrowRight}`}>
      {side === 'left' ? '‹' : '›'}
    </button>
  )
}
