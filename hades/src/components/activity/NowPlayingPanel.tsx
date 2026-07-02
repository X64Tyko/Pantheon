import { useEffect, useRef, useState } from 'react'
import { getActivitySessions, getSessionLogs, type ActivitySession } from '../../player/playbackApi'

const SESSIONS_POLL_MS = 5_000
const LOGS_POLL_MS     = 3_000

function elapsed(startedAtMs: number): string {
  if (!startedAtMs) return ''
  const secs = Math.max(0, Math.floor((Date.now() - startedAtMs) / 1000))
  const h = Math.floor(secs / 3600)
  const m = Math.floor((secs % 3600) / 60)
  const s = secs % 60
  return h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m ${s}s` : `${s}s`
}

function hwLabel(session: ActivitySession): string {
  const enc = session.hw_accel        !== 'none' ? session.hw_accel        : null
  const dec = session.decode_hw_accel !== 'none' ? session.decode_hw_accel : null
  if (!enc && !dec) return 'software'
  if (enc === dec)  return `${enc} (hw)`
  return `enc:${enc ?? 'sw'} dec:${dec ?? 'sw'}`
}

export function NowPlayingPanel() {
  const [sessions, setSessions] = useState<ActivitySession[]>([])
  const [selected,  setSelected]  = useState<ActivitySession | null>(null)
  const [logs,      setLogs]      = useState<string[]>([])
  const [logsError, setLogsError] = useState<string | null>(null)
  const logRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    let cancelled = false
    const poll = () => {
      getActivitySessions()
        .then(list => { if (!cancelled) setSessions(list) })
        .catch(() => {})
    }
    poll()
    const interval = setInterval(poll, SESSIONS_POLL_MS)
    return () => { cancelled = true; clearInterval(interval) }
  }, [])

  // If the selected session drops out of the active list (stopped/reaped),
  // clear the selection rather than keep polling logs for a dead session.
  useEffect(() => {
    if (selected && !sessions.some(s => s.id === selected.id)) setSelected(null)
  }, [sessions, selected])

  useEffect(() => {
    if (!selected) { setLogs([]); return }
    let cancelled = false
    const poll = () => {
      getSessionLogs(selected.id)
        .then(lines => { if (!cancelled) { setLogs(lines); setLogsError(null) } })
        .catch(e => { if (!cancelled) setLogsError(e instanceof Error ? e.message : 'failed to load logs') })
    }
    poll()
    const interval = setInterval(poll, LOGS_POLL_MS)
    return () => { cancelled = true; clearInterval(interval) }
  }, [selected])

  useEffect(() => {
    const el = logRef.current
    if (el) el.scrollTop = el.scrollHeight
  }, [logs])

  return (
    <div className="rounded-lg border border-violet-900/50 bg-zinc-900 p-4" style={{ flexShrink: 0 }}>
      <h2 className="section-label mb-3">Now Playing</h2>

      {sessions.length === 0 ? (
        <p className="text-xs text-zinc-600">Nothing is currently streaming.</p>
      ) : (
        <div className="flex flex-col gap-2">
          {sessions.map(s => (
            <button
              key={s.id}
              onClick={() => setSelected(cur => cur?.id === s.id ? null : s)}
              className={`text-left flex items-center gap-3 px-3 py-2 rounded border text-xs transition-colors
                          ${selected?.id === s.id
                            ? 'bg-violet-950/40 border-violet-700/60'
                            : 'bg-zinc-950/60 border-zinc-800/60 hover:border-zinc-700'}`}
            >
              <span className={`w-1.5 h-1.5 rounded-full shrink-0 ${s.kind === 'channel' ? 'bg-emerald-400' : 'bg-sky-400'}`} />
              <span className="text-zinc-300 font-medium truncate" style={{ maxWidth: 260 }}>
                {s.title || s.file_path || s.id}
              </span>
              <span className="text-zinc-600 uppercase shrink-0">{s.kind}</span>
              <span className="text-zinc-500 shrink-0">{hwLabel(s)}</span>
              <span className="text-zinc-600 ml-auto shrink-0">{elapsed(s.started_at_ms)}</span>
            </button>
          ))}
        </div>
      )}

      {selected && (
        <div className="mt-3 rounded border border-zinc-800/80 bg-zinc-950/60 p-3">
          <div className="grid grid-cols-2 gap-x-4 gap-y-1 text-xs mb-3">
            <Stat label="ID"            value={selected.id} />
            <Stat label="Kind"          value={selected.kind} />
            <Stat label="Encode"        value={selected.hw_accel} />
            <Stat label="Decode"        value={selected.decode_hw_accel} />
            <Stat label="Started"       value={elapsed(selected.started_at_ms) + ' ago'} />
            {selected.direct_play !== undefined && (
              <Stat label="Direct play" value={selected.direct_play ? 'yes' : 'no'} />
            )}
            <Stat label="File" value={selected.file_path} wide />
          </div>

          <div className="text-[10px] text-zinc-600 mb-1 uppercase tracking-wide">
            Recent log lines
            {' · '}enable Verbose Transcode Logging in Settings for full ffmpeg detail
          </div>
          <div
            ref={logRef}
            className="overflow-y-auto font-mono text-[11px] text-zinc-400 space-y-0.5 scrollbar-dark"
            style={{ maxHeight: 220 }}
          >
            {logsError ? (
              <span className="text-red-400">{logsError}</span>
            ) : logs.length === 0 ? (
              <span className="text-zinc-700">No matching log lines yet.</span>
            ) : (
              logs.map((line, i) => <div key={i} className="break-all leading-4">{line}</div>)
            )}
          </div>
        </div>
      )}
    </div>
  )
}

function Stat({ label, value, wide }: { label: string; value: string; wide?: boolean }) {
  return (
    <div className={wide ? 'col-span-2 truncate' : 'truncate'}>
      <span className="text-zinc-600">{label}: </span>
      <span className="text-zinc-300">{value || '—'}</span>
    </div>
  )
}
