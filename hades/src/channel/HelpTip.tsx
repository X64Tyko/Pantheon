import { useState, useRef } from 'react'
import { createPortal } from 'react-dom'
import type { ReactNode } from 'react'

export function HelpTip({ children, down }: { children: ReactNode; down?: boolean }) {
  const [open, setOpen] = useState(false)
  const [pos, setPos]   = useState({ top: 0, bottom: 0, left: 0 })
  const btnRef = useRef<HTMLButtonElement>(null)

  const show = () => {
    if (btnRef.current) {
      const r = btnRef.current.getBoundingClientRect()
      setPos({ top: r.top, bottom: r.bottom, left: r.left + r.width / 2 })
    }
    setOpen(true)
  }

  return (
    <span style={{ display: 'inline-flex', alignItems: 'center', marginLeft: 5 }}>
      <button
        ref={btnRef}
        onMouseEnter={show}
        onMouseLeave={() => setOpen(false)}
        style={{
          display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
          width: 14, height: 14, borderRadius: '50%',
          border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
          color: 'var(--hds-txt-3)', fontSize: 9, fontFamily: 'sans-serif',
          fontWeight: 700, cursor: 'default', lineHeight: 1, flexShrink: 0,
          padding: 0,
        }}
      >?</button>
      {open && createPortal(
        <div style={{
          position: 'fixed',
          top:  down ? pos.bottom : pos.top,
          left: pos.left,
          transform: down
            ? 'translate(-50%, 10px)'
            : 'translate(-50%, calc(-100% - 10px))',
          zIndex: 9999,
          width: 280,
          background: 'oklch(0.2 0.018 286)',
          border: '1px solid var(--hds-line)',
          borderRadius: 9,
          padding: '11px 14px',
          fontSize: 11, color: 'var(--hds-txt-2)', lineHeight: 1.65,
          boxShadow: '0 12px 32px -6px oklch(0 0 0 / 0.6)',
          pointerEvents: 'none',
          fontFamily: "'JetBrains Mono', monospace",
          letterSpacing: '0.01em',
        }}>
          {children}
        </div>,
        document.body
      )}
    </span>
  )
}
