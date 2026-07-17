import { useState, useRef } from 'react'
import type { ReactNode } from 'react'
import styles from './sections.module.css'

export function AccordionSection({ title, badge, open, onToggle, children, forceOpen }: {
  title:      string
  badge?:     ReactNode
  open:       boolean
  onToggle:   () => void
  children:   ReactNode
  forceOpen?: boolean
}) {
  if (forceOpen) {
    return (
      <div className={styles.cardOuter}>
        <div className={styles.cardHeader}>
          <span className={styles.cardDot} />
          <span className={styles.cardTitle}>{title}</span>
          {badge}
        </div>
        <div className={styles.cardBody}>
          {children}
        </div>
      </div>
    )
  }
  return (
    <div className={styles.accordionOuter}>
      <button
        onClick={onToggle}
        className={styles.accordionToggle}
      >
        <span className={styles.accordionChevron} style={{ transform: open ? 'rotate(90deg)' : 'none' }}>▶</span>
        <span className={styles.accordionTitle}>{title}</span>
        {badge}
      </button>
      {open && (
        <div className={styles.accordionBody}>
          {children}
        </div>
      )}
    </div>
  )
}

export function CardSection({ title, summary, children }: {
  title:    string
  summary?: ReactNode
  children: ReactNode
}) {
  return (
    <div className={styles.cardOuter}>
      <div className={styles.cardHeader}>
        <span className={styles.cardDot} />
        <span className={styles.cardTitle}>{title}</span>
        {summary && <span className={styles.cardSummary}>{summary}</span>}
      </div>
      <div className={styles.cardBody}>
        {children}
      </div>
    </div>
  )
}

export function LauncherRow({ icon, title, summary, onClick }: {
  icon:    ReactNode
  title:   string
  summary: string
  onClick: () => void
}) {
  const [hovered, setHovered] = useState(false)
  return (
    <div
      onClick={onClick}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      className={styles.launcherRow}
      style={{
        border: `1px solid ${hovered ? 'var(--hds-line)' : 'var(--hds-line-s)'}`,
        background: hovered ? 'oklch(0.24 0.025 290 / 0.5)' : 'oklch(0.19 0.018 288 / 0.45)',
      }}
    >
      <span className={styles.launcherIcon}>{icon}</span>
      <div className={styles.launcherTextWrap}>
        <div className={styles.launcherTitle}>{title}</div>
        <div className={styles.launcherSummary}>{summary}</div>
      </div>
      <span className={styles.launcherChevron}>›</span>
    </div>
  )
}

export function DropZone({ label, onDrop }: { label: string; onDrop: () => void }) {
  const [over, setOver] = useState(false)
  const counter = useRef(0)
  return (
    <div
      onDragEnter={e => { e.preventDefault(); counter.current++; setOver(true) }}
      onDragLeave={() => { counter.current--; if (counter.current === 0) setOver(false) }}
      onDragOver={e => e.preventDefault()}
      onDrop={e => { e.preventDefault(); counter.current = 0; setOver(false); onDrop() }}
      className={styles.dropZone}
      style={{
        border: `1.5px dashed ${over ? 'var(--hds-violet)' : 'oklch(0.55 0.14 292 / 0.35)'}`,
        background: over ? 'oklch(0.55 0.14 292 / 0.1)' : 'transparent',
        color: over ? 'var(--hds-violet)' : 'var(--hds-txt-3)',
      }}
    >
      {label}
    </div>
  )
}
