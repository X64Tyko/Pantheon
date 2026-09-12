import { useState } from 'react'
import { useFocusable } from '../nav/useFocusable'
import styles from './TvMediaCard.module.css'

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
export function TvMediaCard({ id, title, year, thumb_url, rating, content_type, onClick }: TvMediaCardProps) {
  const [hovered, setHovered] = useState(false)
  const [imgErr,  setImgErr]  = useState(false)
  const showImg = thumb_url && !imgErr

  // Keyed on id, not title — two different items can share a title (a show
  // and movie of the same name, or two same-titled movies), and TvLibrary
  // needs to reproduce this exact key before the card exists, to remember
  // it and restore focus on the way back from Detail (libraryFocusMemory.ts).
  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `tv-media-card-${content_type}-${id}`,
    onEnterPress: onClick,
  })
  const active = hovered || focused

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      className={`${styles.card} ${active ? styles.cardActive : ''}`}
    >
      <div className={styles.posterWrap}>
        {showImg ? (
          <img
            src={thumb_url} alt={title} onError={() => setImgErr(true)}
            className={styles.posterImg}
          />
        ) : (
          <span className={styles.monogram}>
            {title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
        {rating != null && (
          <div className={styles.ratingBadge}>★ {rating.toFixed(1)}</div>
        )}
      </div>
      <div className={styles.infoBlock}>
        <div className={styles.title}>{title}</div>
        <div className={styles.meta}>
          {year}{year && ' · '}{content_type}
        </div>
      </div>
    </div>
  )
}
