import type { ReactNode } from 'react'

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
      <div style={{ borderRadius: 9, border: '1px solid var(--hds-line)', marginBottom: 8, overflow: 'hidden', background: 'oklch(0.21 0.022 288 / 0.5)' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '11px 13px' }}>
          <span style={{ width: 7, height: 7, borderRadius: 2, background: 'var(--hds-gold)', flexShrink: 0 }} />
          <span style={{ flex: 1, fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace" }}>{title}</span>
          {badge}
        </div>
        <div style={{ padding: '4px 14px 16px', borderTop: '1px solid var(--hds-line-s)' }}>
          {children}
        </div>
      </div>
    )
  }
  return (
    <div style={{ borderRadius: 9, border: '1px solid var(--hds-line-s)', marginBottom: 8, overflow: 'hidden' }}>
      <button
        onClick={onToggle}
        style={{ display: 'flex', alignItems: 'center', gap: 10, width: '100%', padding: '9px 13px', background: 'oklch(0.2 0.018 286 / 0.6)', border: 'none', cursor: 'pointer' }}
      >
        <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', display: 'inline-block', transition: 'transform .15s', transform: open ? 'rotate(90deg)' : 'none' }}>▶</span>
        <span style={{ flex: 1, fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", textAlign: 'left' }}>{title}</span>
        {badge}
      </button>
      {open && (
        <div style={{ padding: '14px 14px 16px', borderTop: '1px solid var(--hds-line-s)' }}>
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
    <div style={{ borderRadius: 9, border: '1px solid var(--hds-line)', marginBottom: 8, overflow: 'hidden', background: 'oklch(0.21 0.022 288 / 0.5)' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '11px 13px' }}>
        <span style={{ width: 7, height: 7, borderRadius: 2, background: 'var(--hds-gold)', flexShrink: 0 }} />
        <span style={{ flex: 1, fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace" }}>{title}</span>
        {summary && <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{summary}</span>}
      </div>
      <div style={{ padding: '4px 14px 16px', borderTop: '1px solid var(--hds-line-s)' }}>
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
  return (
    <div
      onClick={onClick}
      style={{ display: 'flex', alignItems: 'center', gap: 12, padding: '12px 13px', marginBottom: 9, borderRadius: 11, cursor: 'pointer', border: '1px solid var(--hds-line-s)', background: 'oklch(0.19 0.018 288 / 0.45)', transition: 'border-color .12s, background .12s' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line)'; (e.currentTarget as HTMLDivElement).style.background = 'oklch(0.24 0.025 290 / 0.5)' }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line-s)'; (e.currentTarget as HTMLDivElement).style.background = 'oklch(0.19 0.018 288 / 0.45)' }}
    >
      <span style={{ width: 30, height: 30, flexShrink: 0, display: 'flex', alignItems: 'center', justifyContent: 'center', borderRadius: 8, border: '1px solid var(--hds-line-s)', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 16 }}>{icon}</span>
      <div style={{ flex: 1, minWidth: 0 }}>
        <div style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, letterSpacing: '0.18em', color: 'var(--hds-txt)' }}>{title}</div>
        <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", marginTop: 2, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{summary}</div>
      </div>
      <span style={{ fontSize: 13, color: 'var(--hds-violet)', flexShrink: 0 }}>›</span>
    </div>
  )
}
