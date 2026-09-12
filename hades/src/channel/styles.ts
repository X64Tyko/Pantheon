import type { CSSProperties } from 'react'

// NOTE: these CSSProperties objects are kept for backward compatibility with
// existing `style={xStyle}` / `style={{...xStyle, ...}}` consumers outside
// this pass's scope (pages/*, components/media/*) — their values below are
// tokenized (var(--hds-*)) but the export shape is unchanged so nothing that
// already imports these breaks. New/converted call sites should prefer the
// real CSS classes in sharedStyles.module.css instead of importing these.

export const inputStyle: CSSProperties = {
  width: '100%', padding: '9px 10px',
  border: '1px solid var(--hds-line)', borderRadius: 'var(--hds-radius-input)',
  background: 'var(--hds-bg-3)', color: 'var(--hds-txt)',
  fontFamily: 'var(--hds-font-mono-brand)', fontSize: 'var(--hds-font-size-tv-meta)',
  boxSizing: 'border-box',
}

export const filterInputStyle: CSSProperties = {
  padding: '5px 7px',
  border: '1px solid var(--hds-line)', borderRadius: 'var(--hds-radius-sm)',
  background: 'var(--hds-bg-3)', color: 'var(--hds-txt)',
  fontFamily: 'var(--hds-font-mono-brand)', fontSize: 'var(--hds-font-size-tv-small-title)',
  boxSizing: 'border-box',
}

export const goldBtnStyle: CSSProperties = {
  padding: '10px 18px', border: 'none', borderRadius: 'var(--hds-radius-btn)',
  background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))',
  color: 'var(--hds-txt-on-gold)', fontFamily: 'var(--hds-font-display)',
  fontWeight: 700, fontSize: 13, cursor: 'pointer',
  boxShadow: 'var(--hds-gold-btn-shadow)',
}

export const ghostBtnStyle: CSSProperties = {
  display: 'flex', alignItems: 'center', gap: 'var(--hds-space-2)',
  padding: '10px 14px', border: '1px solid var(--hds-line)', borderRadius: 'var(--hds-radius-btn)',
  background: 'transparent', color: 'var(--hds-txt-2)',
  fontFamily: 'var(--hds-font-mono-brand)', fontSize: 'var(--hds-font-size-tv-label)', cursor: 'pointer',
}

export const dangerBtnStyle: CSSProperties = {
  padding: '10px 16px', border: '1px solid var(--hds-danger-border)', borderRadius: 'var(--hds-radius-btn)',
  background: 'transparent', color: 'var(--hds-danger-text)',
  fontFamily: 'var(--hds-font-mono-brand)', fontSize: 'var(--hds-font-size-tv-label)', cursor: 'pointer',
}

export const seasonBtnStyle: CSSProperties = {
  padding: '2px 6px', borderRadius: 'var(--hds-radius-xs)', border: '1px solid var(--hds-line)',
  background: 'transparent', color: 'var(--hds-txt-2)',
  fontFamily: 'var(--hds-font-mono-brand)', fontSize: 'var(--hds-font-size-tv-caption)', cursor: 'pointer',
}

export const zoomBtnStyle: CSSProperties = {
  width: 28, height: 26, border: 'none', background: 'transparent',
  color: 'var(--hds-txt-2)', borderRadius: 'var(--hds-radius-sm)', cursor: 'pointer', fontSize: 16,
  fontFamily: 'var(--hds-font-mono-brand)',
}

// Backdrop-darkening gradients only guarantee contrast against a flat
// average — a bright patch of art directly behind a glyph can still wash it
// out. A real per-glyph shadow holds up regardless of what's under it.
// Shared by the Home hero (desktop + TV) and MediaDetailHero so title/meta/
// overview text reads the same way everywhere it sits over backdrop art.
export const heroTextShadow = 'var(--hds-text-shadow-hero)'
