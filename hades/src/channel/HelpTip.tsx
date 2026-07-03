import { useState, useRef, useEffect } from 'react'
import { createPortal } from 'react-dom'
import { observer } from 'mobx-react-lite'
import type { ReactNode } from 'react'
import { helpTipsStore } from '../stores/HelpTipsStore'

// ─── Sub-components used inside modal body ────────────────────────────────────

export function HelpSection({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div style={{ marginBottom: 20 }}>
      <div style={{
        fontSize: 9.5, letterSpacing: '0.2em', color: 'var(--hds-txt-3)',
        fontFamily: "'JetBrains Mono', monospace",
        paddingBottom: 6, marginBottom: 8,
        borderBottom: '1px solid var(--hds-line-s)',
      }}>
        {title.toUpperCase()}
      </div>
      {children}
    </div>
  )
}

export function GifSlot({ src, label }: { src?: string; label: string }) {
  if (src) return (
    <div style={{ marginTop: 12 }}>
      <img src={src} alt={label} style={{ width: '100%', borderRadius: 7, border: '1px solid var(--hds-line-s)', display: 'block' }} />
      <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 5, fontStyle: 'italic' }}>{label}</div>
    </div>
  )
  return (
    <div style={{
      marginTop: 12,
      border: '1px dashed oklch(0.38 0.05 286)',
      borderRadius: 7, padding: '12px 14px',
      background: 'oklch(0.15 0.012 286)',
      display: 'flex', alignItems: 'center', gap: 10,
    }}>
      <span style={{ fontSize: 15, color: 'var(--hds-txt-3)', flexShrink: 0 }}>▶</span>
      <span style={{ fontSize: 10.5, color: 'var(--hds-txt-3)', fontStyle: 'italic', lineHeight: 1.4 }}>{label}</span>
    </div>
  )
}

// ─── Main component ───────────────────────────────────────────────────────────

export const HelpTip = observer(function HelpTip({ title, tip, children, down, label }: {
  title:    string
  // Only used by the default small "?" trigger's hover preview — the full
  // button variant (label) opens straight to the modal, no preview needed.
  tip?:     string
  children: ReactNode
  down?:    boolean
  // When set, renders a full labeled button (e.g. "Setup Guide") instead of
  // the small "?" circle used elsewhere (EditorForm's block-type/late-start
  // tips) — same modal underneath either way.
  label?:   string
}) {
  const [hover, setHover] = useState(false)
  const [modal, setModal] = useState(false)
  const [hPos, setHPos]   = useState({ top: 0, bottom: 0, left: 0 })
  const btnRef = useRef<HTMLButtonElement>(null)

  useEffect(() => {
    if (!modal) return
    const h = (e: KeyboardEvent) => { if (e.key === 'Escape') setModal(false) }
    document.addEventListener('keydown', h)
    return () => document.removeEventListener('keydown', h)
  }, [modal])

  const onEnter = () => {
    if (btnRef.current) {
      const r = btnRef.current.getBoundingClientRect()
      setHPos({ top: r.top, bottom: r.bottom, left: r.left + r.width / 2 })
    }
    setHover(true)
  }

  // Settings → Help & Tips toggle — removes every help affordance app-wide
  // (both this and the label-button variant), not just the trigger, so a
  // modal that was already open can't be left dangling once this flips off.
  // Placed after all hooks above — an early return before them would violate
  // the Rules of Hooks the moment the toggle's state ever changed.
  if (!helpTipsStore.enabled) return null

  return (
    <span style={{ display: 'inline-flex', alignItems: 'center', marginLeft: label ? 0 : 5 }}>
      {label ? (
        <button
          onClick={e => { e.stopPropagation(); setModal(true) }}
          style={{
            padding: '5px 12px', borderRadius: 6, cursor: 'pointer',
            border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
            color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
          }}
        >{label}</button>
      ) : (
        <button
          ref={btnRef}
          onMouseEnter={onEnter}
          onMouseLeave={() => setHover(false)}
          onClick={e => { e.stopPropagation(); setHover(false); setModal(true) }}
          // Paused once the user's actually noticed it (hovering or already
          // reading the modal) — the pulse's whole job is discoverability,
          // not competing for attention with someone already using it.
          className={hover || modal ? undefined : 'hds-shimmer-sweep'}
          style={{
            display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
            width: 14, height: 14, borderRadius: '50%',
            border: '1px solid var(--hds-line)',
            // backgroundColor (longhand), not `background` — the shorthand
            // would reset background-image/position/attachment/size back to
            // initial, wiping out the shimmer the CSS class above applies.
            backgroundColor: 'var(--hds-bg-3)',
            color: 'var(--hds-txt-3)', fontSize: 9, fontFamily: 'sans-serif',
            fontWeight: 700, cursor: 'pointer', lineHeight: 1, flexShrink: 0,
            padding: 0, transition: '.1s',
          }}
        >?</button>
      )}

      {/* Brief hover tooltip */}
      {hover && !modal && createPortal(
        <div style={{
          position: 'fixed',
          top:  down ? hPos.bottom : hPos.top,
          left: hPos.left,
          transform: down ? 'translate(-50%, 10px)' : 'translate(-50%, calc(-100% - 10px))',
          zIndex: 9999,
          background: 'oklch(0.2 0.018 286)',
          border: '1px solid var(--hds-line)',
          borderRadius: 6, padding: '5px 10px',
          fontSize: 10.5, color: 'var(--hds-txt-2)',
          boxShadow: '0 6px 16px -4px oklch(0 0 0 / 0.5)',
          pointerEvents: 'none', whiteSpace: 'nowrap',
          fontFamily: "'JetBrains Mono', monospace",
        }}>
          {tip}<span style={{ color: 'var(--hds-txt-3)', fontSize: 9.5 }}> · click for more</span>
        </div>,
        document.body
      )}

      {/* Click-to-open explanation modal */}
      {modal && createPortal(
        <div
          style={{ position: 'fixed', inset: 0, zIndex: 10000, display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'oklch(0.08 0.015 286 / 0.85)', backdropFilter: 'blur(2px)' }}
          onClick={() => setModal(false)}
        >
          <div
            style={{ width: 'min(96vw, 540px)', maxHeight: '82vh', display: 'flex', flexDirection: 'column', background: 'var(--hds-bg-2)', borderRadius: 14, border: '1px solid var(--hds-line)', boxShadow: '0 32px 80px -16px oklch(0 0 0 / 0.7)', overflow: 'hidden', fontFamily: "'JetBrains Mono', monospace" }}
            onClick={e => e.stopPropagation()}
          >
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '16px 20px 13px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
              <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 15, letterSpacing: '0.04em' }}>{title}</span>
              <button onClick={() => setModal(false)} style={{ width: 28, height: 28, border: 'none', borderRadius: 7, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 16, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>×</button>
            </div>
            <div style={{ flex: 1, overflow: 'auto', padding: '18px 20px 24px', fontSize: 12, color: 'var(--hds-txt-2)', lineHeight: 1.75 }} className="scrollbar-dark">
              {children}
            </div>
          </div>
        </div>,
        document.body
      )}
    </span>
  )
})
