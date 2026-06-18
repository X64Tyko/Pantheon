import type { CSSProperties } from 'react'

export const inputStyle: CSSProperties = {
  width: '100%', padding: '9px 10px',
  border: '1px solid var(--hds-line)', borderRadius: 8,
  background: 'var(--hds-bg-3)', color: 'var(--hds-txt)',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 13,
  boxSizing: 'border-box',
}

export const filterInputStyle: CSSProperties = {
  padding: '5px 7px',
  border: '1px solid var(--hds-line)', borderRadius: 6,
  background: 'var(--hds-bg-3)', color: 'var(--hds-txt)',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
  boxSizing: 'border-box',
}

export const goldBtnStyle: CSSProperties = {
  padding: '10px 18px', border: 'none', borderRadius: 9,
  background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))',
  color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif",
  fontWeight: 700, fontSize: 13, cursor: 'pointer',
  boxShadow: '0 4px 14px -4px oklch(0.83 0.13 84 / 0.4)',
}

export const ghostBtnStyle: CSSProperties = {
  display: 'flex', alignItems: 'center', gap: 6,
  padding: '10px 14px', border: '1px solid var(--hds-line)', borderRadius: 9,
  background: 'transparent', color: 'var(--hds-txt-2)',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 12, cursor: 'pointer',
}

export const dangerBtnStyle: CSSProperties = {
  padding: '10px 16px', border: '1px solid oklch(0.5 0.14 22 / 0.5)', borderRadius: 9,
  background: 'transparent', color: 'oklch(0.72 0.16 22)',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 12, cursor: 'pointer',
}

export const seasonBtnStyle: CSSProperties = {
  padding: '2px 6px', borderRadius: 4, border: '1px solid var(--hds-line)',
  background: 'transparent', color: 'var(--hds-txt-2)',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer',
}

export const zoomBtnStyle: CSSProperties = {
  width: 28, height: 26, border: 'none', background: 'transparent',
  color: 'var(--hds-txt-2)', borderRadius: 6, cursor: 'pointer', fontSize: 16,
  fontFamily: "'JetBrains Mono', monospace",
}

