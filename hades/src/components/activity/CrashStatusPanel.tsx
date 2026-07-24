import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import type { CrashStatus } from '../../api/types'

const POLL_MS = 30_000

const SERVICE_LABELS: Record<keyof CrashStatus, string> = {
  hermes: 'Hermes', kairos: 'Kairos', hephaestus: 'Hephaestus',
}

// Local-only crash markers (see shared/crash/CrashHandler.h) — each service
// writes a plain marker file on an unhandled exception or fatal signal and
// never sends it anywhere; this just reads them back via Hermes's
// aggregating GET /api/activity/crash. Empty string per service means
// nothing recorded since that marker was last overwritten (or ever) — this
// is NOT cleared on read, so a crash stays visible here until the next one
// overwrites it (no acknowledge/clear affordance yet).
export function CrashStatusPanel() {
  const [status, setStatus] = useState<CrashStatus | null>(null)
  const [error,  setError]  = useState<string | null>(null)

  useEffect(() => {
    let cancelled = false
    const poll = () => {
      api.getCrashStatus()
        .then(s => { if (!cancelled) { setStatus(s); setError(null) } })
        .catch(e => { if (!cancelled) setError(e instanceof Error ? e.message : 'failed to load') })
    }
    poll()
    const interval = setInterval(poll, POLL_MS)
    return () => { cancelled = true; clearInterval(interval) }
  }, [])

  const crashed = status
    ? (Object.keys(SERVICE_LABELS) as (keyof CrashStatus)[]).filter(k => status[k])
    : []

  return (
    <div className={`rounded-lg border p-4 shrink-0 ${
      crashed.length > 0 ? 'border-red-900/60 bg-red-950/20' : 'border-violet-900/50 bg-zinc-900'
    }`}>
      <div className="flex items-center justify-between mb-3">
        <h2 className="section-label">Crash Status</h2>
        {status && (
          <span className={`text-xs ${crashed.length > 0 ? 'text-red-400' : 'text-emerald-400'}`}>
            {crashed.length > 0 ? `${crashed.length} service${crashed.length === 1 ? '' : 's'} crashed` : 'All clear'}
          </span>
        )}
      </div>

      {error ? (
        <p className="text-xs text-red-400">{error}</p>
      ) : !status ? (
        <p className="text-xs text-zinc-600">Loading…</p>
      ) : crashed.length === 0 ? (
        <p className="text-xs text-zinc-600">No crashes recorded since each service's marker was last cleared.</p>
      ) : (
        <div className="flex flex-col gap-2">
          {crashed.map(k => (
            <div key={k} className="rounded border border-red-900/60 bg-zinc-950/60 px-3 py-2">
              <div className="text-xs font-medium text-red-300 mb-1">{SERVICE_LABELS[k]}</div>
              <pre className="text-[11px] text-zinc-400 whitespace-pre-wrap break-all font-mono">{status[k]}</pre>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
