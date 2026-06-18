import type { ReactNode } from 'react'

export function SectionLabel({ children }: { children: ReactNode }) {
  return <div style={{ fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt-3)', marginBottom: 9 }}>{children}</div>
}
