import type { CSSProperties, ReactNode } from 'react'

export function SectionLabel({ children, style }: { children: ReactNode; style?: CSSProperties }) {
  return <div style={{ fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt-3)', marginBottom: 9, ...style }}>{children}</div>
}
