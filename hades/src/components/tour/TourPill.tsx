import { observer } from 'mobx-react-lite'
import { useLocation, useNavigate } from 'react-router-dom'
import { tourStore, TOUR_STEPS } from '../../stores/TourStore'

// Cross-route half of the setup tour — TourSpotlight (mounted on the target
// page itself) handles pointing at the actual button once the admin gets
// there. This just tells them which page to go to next; hidden once they're
// already on it so the two don't say the same thing twice.
export const TourPill = observer(function TourPill() {
  const location = useLocation()
  const navigate = useNavigate()

  const stepIndex = tourStore.currentStepIndex
  if (!tourStore.active || stepIndex == null) return null
  const step = TOUR_STEPS[stepIndex]
  if (location.pathname === step.route) return null

  return (
    <div style={{
      position: 'fixed', left: 24, bottom: 24, zIndex: 500,
      display: 'flex', alignItems: 'center', gap: 10,
      padding: '10px 10px 10px 16px', borderRadius: 999,
      background: 'oklch(0.16 0.022 286)',
      border: '1px solid var(--hds-glass-border)',
      boxShadow: '0 8px 32px -4px rgba(0,0,0,0.7)',
      fontFamily: "'JetBrains Mono', monospace", fontSize: 12,
      animation: 'hds-toast-in 0.22s cubic-bezier(0.22,1,0.36,1)',
    }}>
      <span style={{ color: 'var(--hds-txt-3)' }}>
        Setup {stepIndex + 1}/{TOUR_STEPS.length}
      </span>
      <span style={{ color: 'var(--hds-txt)' }}>{step.title}</span>
      <button
        onClick={() => navigate(step.route)}
        style={{
          padding: '6px 14px', border: 'none', borderRadius: 999,
          background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))',
          color: 'oklch(0.2 0.04 70)', fontWeight: 700, fontSize: 11,
          cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace",
        }}
      >Go →</button>
      <button
        onClick={() => tourStore.skipStep(stepIndex)}
        title="Skip this step"
        style={{
          background: 'none', border: 'none', cursor: 'pointer',
          color: 'var(--hds-txt-3)', fontSize: 11, padding: '4px 2px',
          fontFamily: "'JetBrains Mono', monospace",
        }}
      >Skip</button>
      <button
        onClick={() => tourStore.dismiss()}
        title="End setup tour"
        style={{
          background: 'none', border: 'none', cursor: 'pointer',
          color: 'var(--hds-txt-3)', fontSize: 14, lineHeight: 1, padding: '2px 4px',
        }}
      >✕</button>
    </div>
  )
})
