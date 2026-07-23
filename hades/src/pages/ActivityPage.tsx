import { observer } from 'mobx-react-lite'
import { useEffect, useRef, useState } from 'react'
import { api } from '../api/client'
import { sourceStore, systemStore, statusStore, metricsStore } from '../stores'
import type { LogEntry } from '../stores'
import type { LibraryWithSource } from '../api/types'
import { NowPlayingPanel } from '../components/activity/NowPlayingPanel'
import { DeviceConnectionsPanel } from '../components/activity/DeviceConnectionsPanel'
import { Sparkline } from '../components/activity/Sparkline'
import { OperationMetricsPanel } from '../components/activity/OperationMetricsPanel'

function tagColor(line: string): string {
  if (/^\[error\]/i.test(line)) return 'text-red-400'
  if (/\[sync/.test(line))      return 'text-amber-400'
  if (/\[writeback/.test(line)) return 'text-amber-400'
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
  // Splits the live status/gauge cards ("Monitor") from operator/diagnostic
  // tooling ("Debugging": per-operation timing stats + the raw log stream) —
  // keeps either tab from growing unboundedly tall as more cards get added.
  const [tab, setTab] = useState<'monitor' | 'debugging'>('monitor')

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

  useEffect(() => {
    const timer = setInterval(() => metricsStore.poll(), 1000)
    return () => clearInterval(timer)
  }, [])

  // Only the latest poll decides whether to draw the GPU row at all — not
  // every historical entry, so a single transient query hiccup upstream
  // (see GpuMetrics.h's "nullopt on failure" convention) doesn't flicker
  // the section in and out. Absent (no hw-accel resolved to a real GPU
  // backend) is "draw nothing," not a zeroed-out card — real feedback: "it
  // should change between nvidia, AMD or not drawn at all."
  const latestGpu = metricsStore.history[metricsStore.history.length - 1]?.hephaestus.gpu

  const syncAll = async () => {
    try { await api.syncAll() } catch {}
  }

  const [confirmHardSyncAll, setConfirmHardSyncAll] = useState(false)
  const syncAllHard = async () => {
    setConfirmHardSyncAll(false)
    try { await api.syncAllHard() } catch {}
  }

  // Writeback All — pushes every match-confirmed show/movie's metadata to
  // its linked source(s), optionally scoped to one library and/or one
  // source. Fires-and-returns like syncAll; progress/errors show up on the
  // log stream below (tagged [writeback]), not in this response.
  const [allLibraries,       setAllLibraries]       = useState<LibraryWithSource[]>([])
  const [writebackLibraryId, setWritebackLibraryId] = useState('')
  const [writebackSourceId,  setWritebackSourceId]  = useState('')
  const [confirmWritebackAll, setConfirmWritebackAll] = useState(false)
  const [writebackError,      setWritebackError]      = useState<string | null>(null)
  useEffect(() => { api.getAllLibraries().then(setAllLibraries).catch(() => {}) }, [])
  const writebackAll = async () => {
    setConfirmWritebackAll(false)
    setWritebackError(null)
    try {
      await api.writebackAll({
        library_id: writebackLibraryId || undefined,
        source_id:  writebackSourceId  || undefined,
      })
      statusStore.markWritebackStarted()
    } catch (e) {
      // Most likely a 409 (one's already running) — real progress/failures
      // for a run that did start are on the [writeback]-tagged log stream.
      setWritebackError(e instanceof Error ? e.message : 'Failed to start writeback.')
    }
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
    <div className="flex flex-col h-full p-6 gap-5 overflow-hidden">

      {/* Header */}
      <div className="flex items-center justify-between shrink-0">
        <h1 className="text-xl font-semibold text-zinc-100">Activity</h1>
        <div className="flex items-center gap-2">
          {confirmHardSyncAll ? (
            <span className="flex items-center gap-1.5 text-xs">
              <span className="text-orange-400">
                Re-resolves every item on every source from scratch, like a first-ever sync.
                Also drops all manual cross-source links. Continue?
              </span>
              <button
                onClick={syncAllHard}
                className="px-2 py-1 rounded bg-orange-900/60 border border-orange-700/50 text-orange-200 hover:bg-orange-800/60 transition-colors"
              >Yes</button>
              <button
                onClick={() => setConfirmHardSyncAll(false)}
                className="px-2 py-1 rounded bg-zinc-800 border border-zinc-700/50 text-zinc-400 hover:bg-zinc-700 transition-colors"
              >No</button>
            </span>
          ) : (
            <button
              onClick={() => setConfirmHardSyncAll(true)}
              disabled={statusStore.anyRunning}
              title="Forces a full recheck of every source, as if each were being synced for the first time"
              className="flex items-center gap-2 px-3 py-1.5 bg-orange-900/30 hover:bg-orange-800/40
                         disabled:opacity-40 text-orange-300 text-sm rounded border border-orange-800/30
                         font-medium transition-colors"
            >
              Hard Sync All
            </button>
          )}
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
      </div>

      {/* Tabs */}
      <div className="flex items-center gap-4 shrink-0 border-b border-zinc-800/60">
        {(['monitor', 'debugging'] as const).map(t => (
          <button
            key={t}
            onClick={() => setTab(t)}
            className={`pb-2 px-1 text-sm font-medium capitalize border-b-2 -mb-px transition-colors ${
              tab === t
                ? 'text-amber-400 border-amber-400'
                : 'text-zinc-500 border-transparent hover:text-zinc-300'
            }`}
          >
            {t}
          </button>
        ))}
      </div>

      {tab === 'monitor' && (
      <div className="flex flex-col gap-5 overflow-y-auto scrollbar-dark flex-1 min-h-0">

      {/* Sync status */}
      <div className="rounded-lg border border-violet-900/50 bg-zinc-900 p-4 shrink-0">
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
            {sourceStore.sources.map(src => {
              // Pulses only for the source whose content-ingestion phase is
              // actually running right now — undefined (idle, or a
              // not-per-source phase like scraper matching/chapter sync)
              // means no single source's dot should claim to be "the" active one.
              const isActive = statusStore.currentSourceId === src.source_id
              return (
                <div key={src.source_id}
                     className="flex items-center gap-2 px-3 py-2 rounded
                                bg-zinc-950/60 border border-zinc-800/60 text-xs">
                  <span className={`w-1.5 h-1.5 rounded-full ${
                    isActive ? 'bg-violet-400 animate-pulse' : 'bg-zinc-600'
                  }`} />
                  <span className="text-zinc-300 font-medium">{src.display_name}</span>
                  <span className="text-zinc-600 ml-auto uppercase">{src.source_type}</span>
                </div>
              )
            })}
          </div>
        )}
      </div>

      {/* Writeback status + trigger */}
      <div className="rounded-lg border border-violet-900/50 bg-zinc-900 p-4 shrink-0">
        <h2 className="section-label mb-3">Writeback</h2>
        <div className="flex items-center gap-2.5 mb-3">
          {statusStore.writingBack ? (
            <>
              <Spinner className="text-amber-400" />
              <span className="text-sm text-amber-300">Writeback in progress… (see log below, tagged [writeback])</span>
            </>
          ) : (
            <>
              <span className="w-2 h-2 rounded-full bg-zinc-600" />
              <span className="text-sm text-zinc-500">Idle</span>
            </>
          )}
        </div>

        <div className="flex flex-wrap items-center gap-2">
          <select
            value={writebackLibraryId}
            onChange={e => setWritebackLibraryId(e.target.value)}
            disabled={statusStore.writingBack}
            className="px-2 py-1.5 rounded bg-zinc-950/60 border border-zinc-800/60 text-xs text-zinc-300 disabled:opacity-40"
          >
            <option value="">All libraries</option>
            {allLibraries.map(l => (
              <option key={l.library_id} value={l.library_id}>{l.display_name} ({l.source_name})</option>
            ))}
          </select>
          <select
            value={writebackSourceId}
            onChange={e => setWritebackSourceId(e.target.value)}
            disabled={statusStore.writingBack}
            className="px-2 py-1.5 rounded bg-zinc-950/60 border border-zinc-800/60 text-xs text-zinc-300 disabled:opacity-40"
          >
            <option value="">All sources</option>
            {sourceStore.sources.map(s => (
              <option key={s.source_id} value={s.source_id}>{s.display_name}</option>
            ))}
          </select>

          {confirmWritebackAll ? (
            <span className="flex items-center gap-1.5 text-xs">
              <span className="text-orange-400">
                Pushes confirmed metadata to {writebackSourceId ? 'the selected source' : 'every linked source'} for
                {writebackLibraryId ? ' the selected library' : ' every match-confirmed item'}. Continue?
              </span>
              <button
                onClick={writebackAll}
                className="px-2 py-1 rounded bg-orange-900/60 border border-orange-700/50 text-orange-200 hover:bg-orange-800/60 transition-colors"
              >Yes</button>
              <button
                onClick={() => setConfirmWritebackAll(false)}
                className="px-2 py-1 rounded bg-zinc-800 border border-zinc-700/50 text-zinc-400 hover:bg-zinc-700 transition-colors"
              >No</button>
            </span>
          ) : (
            <button
              onClick={() => setConfirmWritebackAll(true)}
              disabled={statusStore.anyBusy}
              title="Pushes confirmed metadata (title, genres, cast, artwork, etc.) to every linked Plex/Jellyfin source"
              className="flex items-center gap-2 px-3 py-1.5 bg-amber-500 hover:bg-amber-400
                         disabled:bg-amber-500/40 text-black text-sm rounded font-medium transition-colors"
            >
              {statusStore.writingBack
                ? <><Spinner className="text-black/70" /> Writing back…</>
                : '▶ Writeback All'}
            </button>
          )}
        </div>
        {writebackError && <p className="text-xs text-red-400 mt-2">{writebackError}</p>}
      </div>

      <NowPlayingPanel />
      <DeviceConnectionsPanel />

      {/* System Resources */}
      <div className="rounded-lg border border-violet-900/50 bg-zinc-900 p-4 shrink-0">
        <h2 className="section-label mb-4">System Resources</h2>
        <div className="flex flex-wrap gap-8 justify-between">
            <div className="flex gap-8">
                <Sparkline 
                    label="System CPU" 
                    unit="%"
                    data={metricsStore.history.map(h => h.system.cpu_usage)} 
                    max={100}
                    color="#EAB308" 
                />
                <Sparkline 
                    label="RAM Usage" 
                    unit=" GB"
                    data={metricsStore.history.map(h => (h.system.ram_total - h.system.ram_free) / 1024 / 1024 / 1024)} 
                    max={metricsStore.history[0]?.system.ram_total / 1024 / 1024 / 1024}
                    color="#A855F7" 
                />
            </div>
            <div className="flex flex-wrap gap-6">
                <Sparkline 
                    label="Kairos CPU" 
                    unit="%"
                    width={120}
                    data={metricsStore.history.map(h => h.kairos.cpu_usage)} 
                    color="#10B981" 
                />
                <Sparkline 
                    label="Hermes CPU" 
                    unit="%"
                    width={120}
                    data={metricsStore.history.map(h => h.hermes.cpu_usage)} 
                    color="#3B82F6" 
                />
                <Sparkline
                    label="Hephaestus CPU"
                    unit="%"
                    width={120}
                    data={metricsStore.history.map(h => h.hephaestus.cpu_usage)}
                    color="#F43F5E"
                />
            </div>
        </div>

        <div className="flex flex-wrap gap-6 pt-4 mt-4 border-t border-zinc-800/60">
            <Sparkline
                label="Kairos RAM"
                unit=" MB"
                width={120}
                data={metricsStore.history.map(h => h.kairos.ram_bytes / 1024 / 1024)}
                color="#34D399"
            />
            <Sparkline
                label="Hermes RAM"
                unit=" MB"
                width={120}
                data={metricsStore.history.map(h => h.hermes.ram_bytes / 1024 / 1024)}
                color="#60A5FA"
            />
            <Sparkline
                label="Hephaestus RAM"
                unit=" MB"
                width={120}
                data={metricsStore.history.map(h => h.hephaestus.ram_bytes / 1024 / 1024)}
                color="#FB7185"
            />
        </div>

        {latestGpu && (
          <div className="flex flex-wrap items-end gap-6 pt-4 mt-4 border-t border-zinc-800/60">
            <Sparkline
                label={latestGpu.backend === 'nvidia' ? 'NVIDIA GPU' : 'AMD GPU'}
                unit="%"
                width={120}
                data={metricsStore.history.map(h => h.hephaestus.gpu?.gpu_util_pct ?? 0)}
                max={100}
                color="#22D3EE"
            />
            {/* Separate encode/decode engine load — nvidia-smi only,
                see GpuMetrics.h's own comment on why AMD has no
                equivalent query. */}
            {latestGpu.backend === 'nvidia' && (
              <>
                <Sparkline
                    label="NVENC"
                    unit="%"
                    width={120}
                    data={metricsStore.history.map(h => h.hephaestus.gpu?.encoder_util_pct ?? 0)}
                    max={100}
                    color="#F59E0B"
                />
                <Sparkline
                    label="NVDEC"
                    unit="%"
                    width={120}
                    data={metricsStore.history.map(h => h.hephaestus.gpu?.decoder_util_pct ?? 0)}
                    max={100}
                    color="#84CC16"
                />
              </>
            )}
            <div className="flex flex-col gap-1 text-xs">
              <span className="text-white/60 uppercase tracking-wider font-medium">
                {latestGpu.name || (latestGpu.backend === 'nvidia' ? 'NVIDIA GPU' : 'AMD GPU')}
              </span>
              <span className="text-zinc-400">
                {(latestGpu.mem_used_mb / 1024).toFixed(1)} / {(latestGpu.mem_total_mb / 1024).toFixed(1)} GB VRAM
              </span>
              {latestGpu.temp_c != null && (
                <span className="text-zinc-400">{latestGpu.temp_c.toFixed(0)}°C</span>
              )}
            </div>
          </div>
        )}
      </div>

      </div>
      )}

      {tab === 'debugging' && (
      <div className="flex flex-col gap-5 flex-1 min-h-0">

      <OperationMetricsPanel />

      {/* Log viewer fills whatever's left after Hot Zones above. */}
      <div className="rounded-lg border border-violet-900/50 bg-zinc-900 overflow-hidden
                      flex-1 min-h-0 flex flex-col">
        <div className="flex items-center justify-between px-4 py-2.5 border-b border-zinc-800/80 shrink-0">
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
          className="overflow-y-auto p-3 font-mono text-xs space-y-0.5 scrollbar-dark flex-1 min-h-0"
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
      )}
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
