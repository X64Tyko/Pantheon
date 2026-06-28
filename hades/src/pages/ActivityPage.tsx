import { observer } from 'mobx-react-lite'
import { useEffect, useRef } from 'react'
import { api } from '../api/client'
import { sourceStore, systemStore, statusStore } from '../stores'
import type { LogEntry } from '../stores'

function tagColor(line: string): string {
  if (/^\[error\]/i.test(line)) return 'text-red-400'
  if (/\[sync/.test(line))      return 'text-amber-400'
  if (/\[plex/.test(line))      return 'text-sky-400'
  if (/\[conf/.test(line))      return 'text-violet-400'
  if (/\[kairos/.test(line))    return 'text-emerald-400'
  if (/error|Error/.test(line)) return 'text-red-400'
  return 'text-zinc-500'
}

function LogLine({ entry }: { entry: LogEntry }) {
  const m  = entry.line.match(/^(\[[^\]]+\])\s?(.*)/)
  const tc = tagColor(entry.line)
  return (
    <div className={`flex gap-2 leading-5 ${entry.isError ? 'bg-red-950/20' : ''}`}>
      <span className="text-zinc-600 shrink-0 select-none">{entry.ts}</span>
      {m ? (
        <>
          <span className={`shrink-0 ${tc}`}>{m[1]}</span>
          <span className="text-zinc-300 break-all">{m[2]}</span>
        </>
      ) : (
        <span className="text-zinc-400 break-all">{entry.line}</span>
      )}
    </div>
  )
}

export default observer(function ActivityPage() {
  const logRef      = useRef<HTMLDivElement>(null)
  const atBottomRef = useRef(true)
  const logs        = systemStore.logs
  const liveStatus  = systemStore.liveStatus

  // Clear error badge while this page is visible.
  useEffect(() => {
    systemStore.clearUnreadErrors()
  }, [])

  // Auto-scroll when new logs arrive. Watch the last entry's id so this fires
  // even when the buffer is full and logs.length stays constant at 1000.
  const lastId = logs.length > 0 ? logs[logs.length - 1].id : null
  useEffect(() => {
    const el = logRef.current
    if (el && atBottomRef.current) el.scrollTop = el.scrollHeight
  }, [lastId])

  const handleScroll = () => {
    const el = logRef.current
    if (!el) return
    atBottomRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 60
  }

  const jumpToBottom = () => {
    const el = logRef.current
    if (!el) return
    atBottomRef.current = true
    el.scrollTop = el.scrollHeight
  }

  useEffect(() => { sourceStore.fetchAll() }, [])

  const syncAll = async () => {
    try { await api.syncAll() } catch {}
  }

  const liveColors = {
    connecting:   'text-amber-400',
    live:         'text-emerald-400',
    disconnected: 'text-red-400',
  }
  const liveDots = {
    connecting:   'bg-amber-400 animate-pulse',
    live:         'bg-emerald-400',
    disconnected: 'bg-red-400',
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%', padding: 24, gap: 20, overflow: 'hidden' }}>

      {/* Header */}
      <div className="flex items-center justify-between" style={{ flexShrink: 0 }}>
        <h1 className="text-xl font-semibold text-zinc-100">Activity</h1>
        <button
          onClick={syncAll}
          disabled={statusStore.anyRunning}
          className="flex items-center gap-2 px-3 py-1.5 bg-amber-500 hover:bg-amber-400
                     disabled:bg-amber-500/40 text-black text-sm rounded font-medium transition-colors"
        >
          {statusStore.anyRunning
            ? <><Spinner className="text-black/70" /> Syncing…</>
            : '▶ Sync All'}
        </button>
      </div>

      {/* Sync status */}
      <div className="rounded-lg border border-violet-900/50 bg-zinc-900 p-4" style={{ flexShrink: 0 }}>
        <h2 className="section-label mb-3">Sync Status</h2>
        <div className="flex items-center gap-2.5 mb-3">
          {statusStore.anyRunning ? (
            <>
              <Spinner className="text-violet-400" />
              <span className="text-sm text-violet-300">Sync in progress…</span>
            </>
          ) : (
            <>
              <span className="w-2 h-2 rounded-full bg-zinc-600" />
              <span className="text-sm text-zinc-500">Idle</span>
            </>
          )}
        </div>

        {sourceStore.sources.length === 0 ? (
          <p className="text-xs text-zinc-600">No sources configured.</p>
        ) : (
          <div className="grid grid-cols-2 gap-2">
            {sourceStore.sources.map(src => (
              <div key={src.source_id}
                   className="flex items-center gap-2 px-3 py-2 rounded
                              bg-zinc-950/60 border border-zinc-800/60 text-xs">
                <span className={`w-1.5 h-1.5 rounded-full ${
                  statusStore.anyRunning ? 'bg-violet-400 animate-pulse' : 'bg-zinc-600'
                }`} />
                <span className="text-zinc-300 font-medium">{src.display_name}</span>
                <span className="text-zinc-600 ml-auto uppercase">{src.source_type}</span>
              </div>
            ))}
          </div>
        )}
      </div>

      {/* Log viewer — fills remaining height */}
      <div className="rounded-lg border border-violet-900/50 bg-zinc-900 overflow-hidden"
           style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column' }}>
        <div className="flex items-center justify-between px-4 py-2.5 border-b border-zinc-800/80" style={{ flexShrink: 0 }}>
          <h2 className="section-label">Engine Logs</h2>
          <div className="flex items-center gap-4">
            <span className={`flex items-center gap-1.5 text-xs ${liveColors[liveStatus]}`}>
              <span className={`w-1.5 h-1.5 rounded-full ${liveDots[liveStatus]}`} />
              {liveStatus === 'connecting' ? 'Connecting…'
               : liveStatus === 'live'    ? 'Live'
               :                           'Disconnected'}
            </span>
            <button
              onClick={jumpToBottom}
              className="text-xs text-zinc-600 hover:text-zinc-400 transition-colors"
            >
              ↓ Jump to bottom
            </button>
          </div>
        </div>

        <div
          ref={logRef}
          onScroll={handleScroll}
          className="overflow-y-auto p-3 font-mono text-xs space-y-0.5 scrollbar-dark"
          style={{ flex: 1, minHeight: 0 }}
        >
          {logs.length === 0 ? (
            <span className="text-zinc-700">
              {liveStatus === 'connecting' ? 'Connecting to log stream…' : 'No log entries yet.'}
            </span>
          ) : (
            logs.map(entry => <LogLine key={entry.id} entry={entry} />)
          )}
        </div>
      </div>
    </div>
  )
})

function Spinner({ className = '' }: { className?: string }) {
  return (
    <svg className={`w-3.5 h-3.5 animate-spin ${className}`} viewBox="0 0 24 24" fill="none">
      <circle className="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="4" />
      <path className="opacity-75" fill="currentColor"
            d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z" />
    </svg>
  )
}
