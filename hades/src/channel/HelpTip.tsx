import { useState, useRef, useEffect } from 'react'
import { createPortal } from 'react-dom'
import { observer } from 'mobx-react-lite'
import type { ReactNode } from 'react'
import { helpTipsStore } from '../stores/HelpTipsStore'
import styles from './HelpTip.module.css'

// ─── Sub-components used inside modal body ────────────────────────────────────

export function HelpSection({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className={styles.sectionWrap}>
      <div className={styles.sectionHeading}>
        {title.toUpperCase()}
      </div>
      {children}
    </div>
  )
}

export function GifSlot({ src, label }: { src?: string; label: string }) {
  if (src) return (
    <div className={styles.gifWrap}>
      <img src={src} alt={label} className={styles.gifImg} />
      <div className={styles.gifLabel}>{label}</div>
    </div>
  )
  return (
    <div className={styles.gifPlaceholder}>
      <span className={styles.gifPlaceholderIcon}>▶</span>
      <span className={styles.gifPlaceholderLabel}>{label}</span>
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
    <span className={`${styles.triggerWrap} ${label ? styles.triggerWrapLabeled : styles.triggerWrapUnlabeled}`}>
      {label ? (
        <button
          onClick={e => { e.stopPropagation(); setModal(true) }}
          className={styles.labelBtn}
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
          // backgroundColor is set via .questionBtn (longhand, not `background`
          // shorthand) so the shimmer class's background-image/position/
          // attachment/size survive alongside it.
          className={`${styles.questionBtn} ${hover || modal ? '' : 'hds-shimmer-sweep'}`}
        >?</button>
      )}

      {/* Brief hover tooltip */}
      {hover && !modal && createPortal(
        <div
          className={styles.tooltip}
          style={{
            top:  down ? hPos.bottom : hPos.top,
            left: hPos.left,
            transform: down ? 'translate(-50%, 10px)' : 'translate(-50%, calc(-100% - 10px))',
          }}
        >
          {tip}<span className={styles.tooltipHint}> · click for more</span>
        </div>,
        document.body
      )}

      {/* Click-to-open explanation modal */}
      {modal && createPortal(
        <div
          className={styles.modalBackdrop}
          onClick={() => setModal(false)}
        >
          <div
            className={styles.modalBox}
            onClick={e => e.stopPropagation()}
          >
            <div className={styles.modalHeader}>
              <span className={styles.modalTitle}>{title}</span>
              <button onClick={() => setModal(false)} className={styles.modalCloseBtn}>×</button>
            </div>
            <div className={`${styles.modalBody} scrollbar-dark`}>
              {children}
            </div>
          </div>
        </div>,
        document.body
      )}
    </span>
  )
})
