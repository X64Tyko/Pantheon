import styles from './LoadingThrobber.module.css'

interface LoadingThrobberProps {
  label?:   string
  percent?: number // omit when there's nothing real to measure yet
}

export function LoadingThrobber({ label, percent }: LoadingThrobberProps) {
  return (
    <div className={styles.wrap}>
      <div className={styles.spinnerWrap}>
        <svg className={`animate-spin ${styles.spinnerSvg}`} width="48" height="48" viewBox="0 0 48 48">
          <circle cx="24" cy="24" r="20" fill="none" stroke="var(--hds-line)" strokeWidth="3" />
          <circle
            cx="24" cy="24" r="20" fill="none" stroke="var(--hds-violet)" strokeWidth="3"
            strokeDasharray={2 * Math.PI * 20} strokeDashoffset={2 * Math.PI * 20 * 0.75}
            strokeLinecap="round"
          />
        </svg>
        {percent != null && (
          <div className={styles.percentLabel}>{Math.round(percent)}%</div>
        )}
      </div>
      {label && <div className={styles.label}>{label}</div>}
    </div>
  )
}
