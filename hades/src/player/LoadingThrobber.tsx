interface LoadingThrobberProps {
  label?:   string
  percent?: number // omit when there's nothing real to measure yet
}

export function LoadingThrobber({ label, percent }: LoadingThrobberProps) {
  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 12 }}>
      <div style={{ position: 'relative', width: 48, height: 48 }}>
        <svg className="animate-spin" width="48" height="48" viewBox="0 0 48 48" style={{ display: 'block' }}>
          <circle cx="24" cy="24" r="20" fill="none" stroke="var(--hds-line)" strokeWidth="3" />
          <circle
            cx="24" cy="24" r="20" fill="none" stroke="var(--hds-violet)" strokeWidth="3"
            strokeDasharray={2 * Math.PI * 20} strokeDashoffset={2 * Math.PI * 20 * 0.75}
            strokeLinecap="round"
          />
        </svg>
        {percent != null && (
          <div style={{
            position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-2)',
          }}>{Math.round(percent)}%</div>
        )}
      </div>
      {label && <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 12, color: 'var(--hds-txt-2)' }}>{label}</div>}
    </div>
  )
}
