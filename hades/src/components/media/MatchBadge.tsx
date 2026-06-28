export type MatchStatus = 'matched' | 'unmatched' | 'uncertain' | 'unscraped'

interface MatchBadgeProps {
  status: MatchStatus
  score?: number
  size?:  'sm' | 'md'
}

const COLORS: Record<MatchStatus, string> = {
  matched:   'var(--hds-match-green)',
  uncertain: 'var(--hds-match-amber)',
  unmatched: 'var(--hds-match-red)',
  unscraped: 'var(--hds-match-grey)',
}

const LABELS: Record<MatchStatus, string> = {
  matched:   'Matched',
  uncertain: 'Uncertain',
  unmatched: 'Unmatched',
  unscraped: 'Not scraped',
}

export function MatchBadge({ status, score, size = 'md' }: MatchBadgeProps) {
  const color = COLORS[status]

  if (size === 'sm') {
    return (
      <span style={{
        display: 'inline-block', width: 8, height: 8, borderRadius: '50%',
        background: color, boxShadow: `0 0 6px ${color}`,
        flexShrink: 0,
      }} />
    )
  }

  return (
    <span style={{
      display: 'inline-flex', alignItems: 'center', gap: 6,
      padding: '3px 8px', borderRadius: 20,
      background: `color-mix(in oklch, ${color} 15%, transparent)`,
      border: `1px solid color-mix(in oklch, ${color} 40%, transparent)`,
    }}>
      <span style={{
        width: 7, height: 7, borderRadius: '50%', flexShrink: 0,
        background: color, boxShadow: `0 0 5px ${color}`,
      }} />
      <span style={{
        fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
        color, letterSpacing: '0.08em',
      }}>
        {LABELS[status]}{score != null && status === 'uncertain' ? ` ${Math.round(score * 100)}%` : ''}
      </span>
    </span>
  )
}
