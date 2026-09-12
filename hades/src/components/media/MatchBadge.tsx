import styles from './MatchBadge.module.css'

export type MatchStatus = 'matched' | 'unmatched' | 'uncertain' | 'unscraped'

interface MatchBadgeProps {
  status: MatchStatus
  score?: number
  size?:  'sm' | 'md'
}

const LABELS: Record<MatchStatus, string> = {
  matched:   'Matched',
  uncertain: 'Uncertain',
  unmatched: 'Unmatched',
  unscraped: 'Not scraped',
}

export function MatchBadge({ status, score, size = 'md' }: MatchBadgeProps) {
  if (size === 'sm') {
    return <span className={`${styles.dot} ${styles[status]}`} />
  }

  return (
    <span className={`${styles.badge} ${styles[status]}`}>
      <span className={styles.badgeDot} />
      <span className={styles.badgeLabel}>
        {LABELS[status]}{score != null && status === 'uncertain' ? ` ${Math.round(score * 100)}%` : ''}
      </span>
    </span>
  )
}
