import type { ReactNode } from 'react'
import styles from './SectionLabel.module.css'

export function SectionLabel({ children, variant }: { children: ReactNode; variant?: 'compact' | 'tight' }) {
  const variantClass = variant === 'compact' ? styles.compact : variant === 'tight' ? styles.tight : ''
  return <div className={`${styles.label} ${variantClass}`}>{children}</div>
}
