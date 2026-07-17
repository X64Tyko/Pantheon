import { useState } from 'react'
import type { MatchStatus } from './MatchBadge'
import type { LibraryDensity } from '../../api/types'
import { useFocusable } from '../../nav/useFocusable'
import styles from './MediaCard.module.css'

export interface MediaCardProps {
  id:                string
  title:             string
  year?:             number
  // Already fully resolved by the caller (mediaUrl() applied, token and
  // all) — never re-run through mediaUrl() here. It used to be, which
  // silently doubled the ?token= query param into an unparseable
  // "?token=X?token=X" and 401'd every card's image on the Library page.
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
  watched?:          boolean
  view_count?:       number
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

const SCORE_CHIP_CLASS: Record<string, string> = {
  matched:   styles.scoreChipMatched,
  uncertain: styles.scoreChipUncertain,
  unmatched: styles.scoreChipUnmatched,
  unscraped: styles.scoreChipUnscraped,
}

function initials(title: string): string {
  return title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()
}

export function MediaCard(props: MediaCardProps) {
  const {
    id, title, year, poster_url, thumb_url, content_type,
    genres, rating, locked, match_status, match_score,
    is_4k, hdr_type, network, network_logo_url, watched, view_count,
    density, selected, onClick,
  } = props

  const [hoveredState, setHovered] = useState(false)
  const [imgError, setImgError] = useState(false)

  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `media-card-${content_type}-${id}`,
    onEnterPress: onClick,
  })
  const hovered = hoveredState || focused

  const imgUrl = poster_url ?? thumb_url
  const showImg = imgUrl && !imgError

  const ringColor = match_status ? RING_COLOR[match_status] : null
  const glow = `0 0 0 2px var(--hds-violet), 0 8px 32px oklch(0.55 0.14 292 / 0.3)`
  const ringGlow = ringColor ? `0 0 0 3px ${ringColor}, 0 0 12px ${ringColor}40` : null

  // Combinatorial (selected × hovered × density × match-status ring color) —
  // genuinely not reducible to a small closed set of static CSS classes
  // without either a much larger class matrix or CSS custom-property
  // gymnastics that would be harder to follow than the JS ternary already
  // is, so this one stays a targeted inline style.
  const boxShadow = selected
    ? glow
    : hovered
      ? (density === 'rich' && ringColor ? `${ringGlow}, ${glow}` : glow)
      : (density === 'rich' && ringColor ? ringGlow ?? 'none' : 'none')

  const poster = (
    <div className={styles.posterWrap}>
      {showImg ? (
        <img
          src={imgUrl}
          alt={title}
          onError={() => setImgError(true)}
          className={styles.posterImg}
        />
      ) : (
        <span className={styles.monogram}>
          {initials(title)}
        </span>
      )}

      {/* Rich: bottom gradient bar with rating + HDR badges */}
      {density === 'rich' && (
        <>
          <div className={styles.richOverlay}>
            {rating != null && (
              <span className={styles.richRating}>★ {rating.toFixed(1)}</span>
            )}
            <div className={styles.richBadgeRow}>
              {is_4k && (
                <span className={`${styles.formatBadge} ${styles.formatBadge4k}`}>4K</span>
              )}
              {hdr_type && (
                <span className={`${styles.formatBadge} ${hdr_type === 'DV' ? styles.formatBadgeDv : styles.formatBadgeHdr}`}>{hdr_type}</span>
              )}
            </div>
          </div>

          {/* Lock indicator top-left */}
          {locked && (
            <div className={styles.lockBadge}>🔒</div>
          )}

          {/* Match score chip (bottom overlay, next to rating) */}
          {match_status && match_score != null && (
            <div className={`${styles.scoreChip} ${SCORE_CHIP_CLASS[match_status] ?? ''}`}>{Math.round(match_score * 100)}%</div>
          )}

          {/* Network logo / watched badge, top-right */}
          {(network_logo_url || network || watched) && (
            <div className={styles.topRightStack}>
              {(network_logo_url || network) && (
                <div className={styles.networkBadge}>
                  {network_logo_url ? (
                    <img src={network_logo_url} alt={network} className={styles.networkLogo} />
                  ) : (
                    <span className={styles.networkText}>{network}</span>
                  )}
                </div>
              )}
              {watched && (
                <div className={styles.watchedBadge}>✓{view_count && view_count > 1 ? ` ${view_count}` : ''}</div>
              )}
            </div>
          )}
        </>
      )}

      {/* Minimal: hover title overlay */}
      {density === 'minimal' && hovered && (
        <div className={styles.minimalHoverOverlay}>
          <div className={styles.minimalHoverTitle}>{title}</div>
        </div>
      )}
    </div>
  )

  return (
    <div
      ref={ref} data-tv-focused={focused}
      data-media-id={id}
      className={`${styles.card} ${hovered ? styles.cardHovered : ''} ${density === 'minimal' ? styles.cardMinimal : ''}`}
      style={{ boxShadow: boxShadow ?? undefined }}
      onClick={onClick}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
    >
      {poster}

      {density === 'standard' && (
        <div className={styles.standardInfo}>
          <div className={styles.cardTitle}>{title}</div>
          <div className={styles.cardMeta}>
            {year}{year && ' · '}{content_type}
          </div>
        </div>
      )}

      {density === 'rich' && (
        <div className={styles.richInfo}>
          <div className={styles.richTitleRow}>
            <div className={`${styles.cardTitle} ${styles.richTitle}`}>{title}</div>
            {year && (
              <span className={styles.richYear}>{year}</span>
            )}
          </div>
          {genres && genres.length > 0 && (
            <div className={styles.genreRow}>
              {genres.slice(0, 3).map(g => (
                <span key={g} className={styles.genreTag}>{g}</span>
              ))}
            </div>
          )}
        </div>
      )}
    </div>
  )
}
