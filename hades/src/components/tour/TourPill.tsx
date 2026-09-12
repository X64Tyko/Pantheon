import { observer } from 'mobx-react-lite'
import { useLocation, useNavigate } from 'react-router-dom'
import { tourStore, TOUR_STEPS } from '../../stores/TourStore'
import styles from './TourPill.module.css'

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
    <div className={styles.pill}>
      <span className={styles.stepCount}>
        Setup {stepIndex + 1}/{TOUR_STEPS.length}
      </span>
      <span className={styles.stepTitle}>{step.title}</span>
      <button
        onClick={() => navigate(step.route)}
        className={styles.goBtn}
      >Go →</button>
      <button
        onClick={() => tourStore.skipStep(stepIndex)}
        title="Skip this step"
        className={styles.skipBtn}
      >Skip</button>
      <button
        onClick={() => tourStore.dismiss()}
        title="End setup tour"
        className={styles.closeBtn}
      >✕</button>
    </div>
  )
})
