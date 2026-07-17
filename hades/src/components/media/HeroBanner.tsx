import { goldBtnStyle, ghostBtnStyle } from '../../channel/styles'
import type { MediaHeroItem } from '../../api/types'
import styles from './HeroBanner.module.css'

interface HeroBannerProps {
  item:           MediaHeroItem
  onViewDetail:   () => void
  onAddToChannel: () => void
}

export function HeroBanner({ item, onViewDetail, onAddToChannel }: HeroBannerProps) {
  const { title, year, overview, backdrop_url, genres, rating } = item

  // Backdrop is a genuinely dynamic runtime value (an arbitrary image URL, or
  // a fallback gradient when there's none) — kept as a targeted inline style,
  // same pattern as TvHome's/MediaDetailHero's hero backdrops.
  const bgStyle = backdrop_url
    ? { backgroundImage: `url(${backdrop_url})`, backgroundPosition: 'center', backgroundSize: 'cover', backgroundRepeat: 'no-repeat' }
    : { background: 'linear-gradient(135deg, var(--hds-backdrop-fallback-1) 0%, var(--hds-backdrop-fallback-2) 50%, var(--hds-backdrop-fallback-3) 100%)' }

  return (
    <div className={styles.root} style={bgStyle}>
      <div className={styles.scrim} />

      <div className={styles.textBlock}>
        {genres && genres.length > 0 && (
          <div className={styles.genreRow}>
            {genres.slice(0, 4).map(g => (
              <span key={g} className={styles.genreChip}>{g}</span>
            ))}
          </div>
        )}

        <h1 className={styles.title}>{title}</h1>

        <div className={styles.metaRow}>
          {year && <span>{year}</span>}
          {rating != null && <span className={styles.rating}>★ {rating.toFixed(1)}</span>}
        </div>

        {overview && (
          <p className={styles.overview}>{overview}</p>
        )}

        <div className={styles.actionsRow}>
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
