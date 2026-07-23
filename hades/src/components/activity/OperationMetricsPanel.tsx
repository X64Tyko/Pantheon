import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import type { OperationMetricsResponse, OperationRun } from '../../api/types'

const POLL_MS = 3_000 // discrete completed-job stats, not a continuous gauge — no need for the 1s system-metrics cadence

// Fixed display order/labels rather than however keys happen to arrive —
// mirrors the instrumented call sites in SyncManager.cpp/EPGMaterializer.cpp/
// ScraperManager.cpp (see shared/metrics/OperationMetrics.h). Operations with
// no recorded runs yet are simply skipped, not shown as an empty row.
const OPERATIONS: { name: string; label: string }[] = [
  { name: 'sync.full',              label: 'Full Sync' },
  { name: 'sync.phase.content',            label: '— Content Ingestion' },
  { name: 'sync.phase.orphan_cleanup',     label: '— Orphan Cleanup' },
  { name: 'sync.phase.scraper_match',      label: '— Scraper Match' },
  { name: 'sync.phase.specials',           label: '— Specials Scan' },
  { name: 'sync.phase.media_probe',        label: '— Media Probe' },
  { name: 'sync.phase.smart_playlists',    label: '— Smart Playlists' },
  { name: 'sync.phase.chapters',           label: '— Chapter Sync' },
  { name: 'epg.generate',           label: 'EPG Regenerate' },
  { name: 'scraper.match',          label: 'Scraper Match (manual)' },
  { name: 'chapters.sync_from_files', label: 'Chapter Sync (per source)' },
]

function relativeTime(ms: number): string {
  const secs = Math.max(0, Math.floor((Date.now() - ms) / 1000))
  if (secs < 60) return `${secs}s ago`
  const mins = Math.floor(secs / 60)
  if (mins < 60) return `${mins}m ago`
  const hours = Math.floor(mins / 60)
  if (hours < 24) return `${hours}h ago`
  return `${Math.floor(hours / 24)}d ago`
}

function formatDuration(ms: number): string {
  if (ms < 1000) return `${ms}ms`
  const secs = ms / 1000
  if (secs < 60) return `${secs.toFixed(1)}s`
  const mins = Math.floor(secs / 60)
  const rem = Math.round(secs % 60)
  return `${mins}m ${rem}s`
}

function formatBytes(bytes: number): string {
  return `${(bytes / 1024 / 1024).toFixed(0)} MB`
}

function Row({ label, run }: { label: string; run: OperationRun }) {
  return (
    <tr className="border-t border-zinc-800/60">
      <td className="py-2 pr-4 text-zinc-300 whitespace-nowrap">{label}</td>
      <td className="py-2 pr-4 text-zinc-500 whitespace-nowrap">{relativeTime(run.started_at_ms)}</td>
      <td className="py-2 pr-4 text-white whitespace-nowrap">{formatDuration(run.duration_ms)}</td>
      <td className="py-2 pr-4 text-white whitespace-nowrap">{run.peak_threads}</td>
      <td className="py-2 pr-4 text-white whitespace-nowrap">
        {run.avg_cpu_pct.toFixed(0)}% / {run.max_cpu_pct.toFixed(0)}%
      </td>
      <td className="py-2 text-white whitespace-nowrap">
        {formatBytes(run.avg_ram_bytes)} / {formatBytes(run.max_ram_bytes)}
      </td>
    </tr>
  )
}

export function OperationMetricsPanel() {
  const [data, setData] = useState<OperationMetricsResponse>({})

  useEffect(() => {
    let cancelled = false
    const poll = () => {
      api.getOperationMetrics().then(res => { if (!cancelled) setData(res) }).catch(() => {})
    }
    poll()
    const interval = setInterval(poll, POLL_MS)
    return () => { cancelled = true; clearInterval(interval) }
  }, [])

  const rows = OPERATIONS
    .map(op => ({ ...op, run: data[op.name]?.[0] }))
    .filter((op): op is typeof op & { run: OperationRun } => op.run != null)

  return (
    <div className="rounded-lg border border-violet-900/50 bg-zinc-900 p-4 shrink-0">
      <h2 className="section-label mb-4">Hot Zones</h2>
      {rows.length === 0 ? (
        <p className="text-xs text-white/40">No sync or EPG runs recorded yet this session.</p>
      ) : (
        <div className="overflow-x-auto">
          <table className="w-full text-xs">
            <thead>
              <tr className="text-white/60 uppercase tracking-wider text-left">
                <th className="pb-2 pr-4 font-medium">Operation</th>
                <th className="pb-2 pr-4 font-medium">Last Run</th>
                <th className="pb-2 pr-4 font-medium">Duration</th>
                <th className="pb-2 pr-4 font-medium">Peak Threads</th>
                <th className="pb-2 pr-4 font-medium">CPU avg/max</th>
                <th className="pb-2 font-medium">RAM avg/max</th>
              </tr>
            </thead>
            <tbody>
              {rows.map(op => <Row key={op.name} label={op.label} run={op.run} />)}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}
