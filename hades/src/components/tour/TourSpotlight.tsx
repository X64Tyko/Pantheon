import { observer } from 'mobx-react-lite'
import { useEffect, useState } from 'react'
import { tourStore, type TourStep } from '../../stores/TourStore'
import styles from './TourSpotlight.module.css'

interface Rect { top: number; left: number; width: number; height: number }

// Mounted conditionally by whichever page currently owns the active step
// (SourcesPage/SettingsPage/ChannelsPage each check tourStore.currentStep's
// route before rendering this). Finds the real button via its data-tour
// attribute and highlights it in place, rather than duplicating the button
// itself in some separate wizard UI.
export const TourSpotlight = observer(function TourSpotlight({ step }: { step: TourStep }) {
  const [rect, setRect] = useState<Rect | null>(null)

  useEffect(() => {
    // Without this, a target below the fold (e.g. Settings' scraper section)
    // never gets scrolled to — the cutout ends up positioned off-screen while
    // the dimmed backdrop still covers the entire visible page, which just
    // looks like the whole page is shadowed for no reason. Only on mount/step
    // change, not from measure() itself, or the scroll listener below would
    // fight this on every frame of its own smooth-scroll animation.
    document.querySelector(`[data-tour="${step.targetAttr}"]`)
      ?.scrollIntoView({ behavior: 'smooth', block: 'center' })

    const measure = () => {
      const el = document.querySelector(`[data-tour="${step.targetAttr}"]`)
      if (!el) { setRect(null); return }
      const r = el.getBoundingClientRect()
      setRect({ top: r.top, left: r.left, width: r.width, height: r.height })
    }
    measure()
    // Re-measure a beat later too — the target can still be reflowing (a
    // list rendering its first row) right as this effect first runs.
    const t = setTimeout(measure, 50)
    window.addEventListener('resize', measure)
    // capture: true — Layout's own scrollable content div is what actually
    // scrolls, and a 'scroll' event doesn't bubble, but a capturing listener
    // on window still sees it on the way down to that element.
    window.addEventListener('scroll', measure, true)
    return () => {
      clearTimeout(t)
      window.removeEventListener('resize', measure)
      window.removeEventListener('scroll', measure, true)
    }
  }, [step.targetAttr])

  const stepIndex = tourStore.currentStepIndex
  if (stepIndex == null || !rect) return null

  const pad = 6
  const spot = {
    top: rect.top - pad, left: rect.left - pad,
    width: rect.width + pad * 2, height: rect.height + pad * 2,
  }
  const tooltipWidth = 320
  const tooltipBelow = spot.top + spot.height + 140 < window.innerHeight

  return (
    <>
      {/* Single-element spotlight: a transparent box sized exactly to the
          target, whose oversized box-shadow doubles as the dimmed backdrop
          everywhere else — no SVG mask needed for a plain rectangular cutout. */}
      <div className={styles.spotlight} style={{
        top: spot.top, left: spot.left, width: spot.width, height: spot.height,
      }} />
      <div className={styles.tooltip} style={{
        top:    tooltipBelow ? spot.top + spot.height + 12 : undefined,
        bottom: tooltipBelow ? undefined : Math.max(12, window.innerHeight - spot.top + 12),
        left: Math.min(Math.max(spot.left, 16), window.innerWidth - tooltipWidth - 16),
        width: tooltipWidth,
      }}>
        <div className={styles.tooltipTitle}>
          {step.title}
        </div>
        <div className={styles.tooltipBody}>
          {step.body}
        </div>
        <div className={styles.tooltipActions}>
          <button
            onClick={() => tourStore.skipStep(stepIndex)}
            className={styles.skipBtn}
          >Skip this step</button>
          <button
            onClick={() => tourStore.dismiss()}
            className={styles.endBtn}
          >End tour</button>
        </div>
      </div>
    </>
  )
})
