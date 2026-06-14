import { observer } from 'mobx-react-lite'
import { useEffect, useRef } from 'react'
import { api } from '../api/client'
import { sourceStore, systemStore } from '../stores'
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

  // Auto-scroll to bottom when new logs arrive, if already near bottom.
  useEffect(() => {
    const el = logRef.current
    if (el && atBottomRef.current) el.scrollTop = el.scrollHeight
  }, [logs.length])

  const handleScroll = () => {
    const el = logRef.current
    if (!el) return
    atBottomRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 60
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
    <div className="space-y-5">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold text-zinc-100">Activity</h1>
        <button
          onClick={syncAll}
          disabled={systemStore.syncing}
          className="flex items-center gap-2 px-3 py-1.5 bg-amber-500 hover:bg-amber-400
                     disabled:bg-amber-500/40 text-black text-sm rounded font-medium transition-colors"
        >
          {systemStore.syncing
            ? <><Spinner className="text-black/70" /> Syncing…</>
            : '▶ Sync All'}
        </button>
      </div>

      {/* Sync status */}
      <div className="rounded-lg border border-violet-900/50 bg-zinc-900 p-4">
        <h2 className="section-label mb-3">Sync Status</h2>
        <div className="flex items-center gap-2.5 mb-3">
          {systemStore.syncing ? (
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
                  systemStore.syncing ? 'bg-violet-400 animate-pulse' : 'bg-zinc-600'
                }`} />
                <span className="text-zinc-300 font-medium">{src.display_name}</span>
                <span className="text-zinc-600 ml-auto uppercase">{src.source_type}</span>
              </div>
            ))}
          </div>
        )}
      </div>

      {/* Log viewer */}
      <div className="rounded-lg border border-violet-900/50 bg-zinc-900 overflow-hidden">
        <div className="flex items-center justify-between px-4 py-2.5 border-b border-zinc-800/80">
          <h2 className="section-label">Engine Logs</h2>
          <div className="flex items-center gap-4">
            <span className={`flex items-center gap-1.5 text-xs ${liveColors[liveStatus]}`}>
              <span className={`w-1.5 h-1.5 rounded-full ${liveDots[liveStatus]}`} />
              {liveStatus === 'connecting' ? 'Connecting…'
               : liveStatus === 'live'    ? 'Live'
               :                           'Disconnected'}
            </span>
            <button
              onClick={() => { /* logs live in store — can't wipe store, just scroll to bottom */ }}
              className="text-xs text-zinc-600 hover:text-zinc-400 transition-colors"
            >
              ↓ Jump to bottom
            </button>
          </div>
        </div>

        <div
          ref={logRef}
          onScroll={handleScroll}
          className="h-[28rem] overflow-y-auto p-3 font-mono text-xs space-y-0.5 scrollbar-dark"
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
