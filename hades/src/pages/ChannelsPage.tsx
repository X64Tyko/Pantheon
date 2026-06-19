import { observer } from 'mobx-react-lite'
import { useEffect, useRef, useState } from 'react'
import { Link } from 'react-router-dom'
import { channelStore } from '../stores'
import { api } from '../api/client'
import type { Block, Channel, ExportDepth } from '../api/types'
import { DAYS } from '../channel/constants'
import { computeEpg } from '../channel/EpgPreview'
import { todayEpgDay } from '../channel/utils'

function triggerJsonDownload(data: object, filename: string) {
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' })
  const url  = URL.createObjectURL(blob)
  const a    = document.createElement('a')
  a.href = url; a.download = filename; a.click()
  URL.revokeObjectURL(url)
}

export default observer(function ChannelsPage() {
  const store = channelStore
  const [showAdd, setShowAdd]           = useState(false)
  const [form, setForm]                 = useState({ name: '', number: '', timezone: 'UTC' })
  const [importError, setImportError]   = useState<string | null>(null)
  const [importResult, setImportResult] = useState<{ channel_id: string; unresolved: any[] } | null>(null)
  const fileRef = useRef<HTMLInputElement>(null)

  useEffect(() => { store.fetchAll() }, [])

  const add = async () => {
    await store.add({ name: form.name, number: parseInt(form.number), timezone: form.timezone })
    setShowAdd(false)
    setForm({ name: '', number: '', timezone: 'UTC' })
  }

  const handleExport = async (ch: Channel, depth: ExportDepth) => {
    try {
      const data = await api.exportChannel(ch.channel_id, depth)
      triggerJsonDownload(data, `${ch.name.replace(/[^a-z0-9]/gi, '_')}-${depth}.json`)
    } catch (e: any) {
      alert(`Export failed: ${e.message}`)
    }
  }

  const handleImportFile = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0]
    if (!file) return
    e.target.value = ''
    setImportError(null); setImportResult(null)
    try {
      const text = await file.text()
      const data = JSON.parse(text)
      const result = await api.importChannel(data)
      setImportResult(result)
      store.fetchAll()
    } catch (err: any) {
      setImportError(err.message ?? 'Import failed')
    }
  }

  return (
    <div className="space-y-5">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold text-zinc-100">Channels</h1>
        <div className="flex gap-2">
          <input ref={fileRef} type="file" accept=".json" className="hidden" onChange={handleImportFile} />
          <button onClick={() => fileRef.current?.click()} className="btn-secondary">
            Import Channel
          </button>
          <button onClick={() => setShowAdd(v => !v)} className="btn-primary">
            + Add Channel
          </button>
        </div>
      </div>

      {store.error && (
        <div className="text-red-400 text-sm bg-red-950/30 border border-red-900/40 rounded-lg p-3">
          {store.error}
        </div>
      )}

      {importError && (
        <div className="text-red-400 text-sm bg-red-950/30 border border-red-900/40 rounded-lg p-3">
          Import failed: {importError}
        </div>
      )}

      {importResult && (
        <div className="text-sm bg-emerald-950/30 border border-emerald-900/40 rounded-lg p-3 space-y-1">
          <div className="text-emerald-400 font-medium">Channel imported successfully.</div>
          {importResult.unresolved.length > 0 && (
            <div className="text-zinc-400 text-xs space-y-0.5 mt-1">
              <div className="text-zinc-500 font-semibold mb-1">
                {importResult.unresolved.length} item{importResult.unresolved.length !== 1 ? 's' : ''} could not be resolved:
              </div>
              {importResult.unresolved.map((u, i) => (
                <div key={i} className="font-mono">
                  {u.block_name ? `[${u.block_name}] ` : ''}{u.content_type}: {u.title}
                </div>
              ))}
            </div>
          )}
          <button onClick={() => setImportResult(null)} className="text-xs text-zinc-600 hover:text-zinc-400 mt-1">
            Dismiss
          </button>
        </div>
      )}

      {showAdd && (
        <div className="card p-4 space-y-4">
          <h2 className="section-label">New Channel</h2>
          <div className="grid grid-cols-3 gap-3">
            <input
              placeholder="Channel name"
              value={form.name}
              onChange={e => setForm({ ...form, name: e.target.value })}
              className="input"
            />
            <input
              type="number"
              placeholder="Channel number"
              value={form.number}
              onChange={e => setForm({ ...form, number: e.target.value })}
              className="input"
            />
            <input
              placeholder="Timezone  (e.g. America/Chicago)"
              value={form.timezone}
              onChange={e => setForm({ ...form, timezone: e.target.value })}
              className="input"
            />
          </div>
          <div className="flex gap-2">
            <button onClick={add} className="btn-primary">Save</button>
            <button onClick={() => setShowAdd(false)} className="btn-ghost">Cancel</button>
          </div>
        </div>
      )}

      <div className="space-y-2">
        {store.channels.length === 0 && !store.loading && (
          <p className="text-zinc-600 text-sm">No channels configured.</p>
        )}
        {store.channels.map(ch => (
          <div key={ch.channel_id} className="card px-4 pt-3 pb-3">
            <div className="flex items-center justify-between">
              <div className="flex items-center gap-5">
                <span className="text-amber-400 font-mono font-bold w-8 text-right">
                  {ch.number}
                </span>
                <div>
                  <div className="font-medium text-sm text-zinc-100">{ch.name}</div>
                  <div className="text-[10px] text-zinc-600 mt-0.5">{ch.timezone}</div>
                </div>
              </div>
              <div className="flex gap-2 items-center">
                <Link to={`/channels/${ch.channel_id}`} className="btn-secondary">
                  Edit Schedule
                </Link>
                <ExportButton channel={ch} onExport={handleExport} />
                <button onClick={() => store.remove(ch.channel_id)} className="btn-danger">
                  Remove
                </button>
              </div>
            </div>
            <ChannelGuideStrip channelId={ch.channel_id} timezone={ch.timezone} />
          </div>
        ))}
      </div>
    </div>
  )
})

// ── Channel guide strip ───────────────────────────────────────────────────────

function ChannelGuideStrip({ channelId, timezone }: { channelId: string; timezone: string }) {
  const [blocks, setBlocks] = useState<Block[] | null>(null)

  useEffect(() => {
    api.getBlocks(channelId).then(setBlocks).catch(() => setBlocks([]))
  }, [channelId])

  const todayIdx = todayEpgDay(timezone)
  const dayLabel = DAYS[todayIdx][1]

  const segs = blocks ? computeEpg(blocks, todayIdx) : []
  const blockCount = blocks ? blocks.filter(b => (b.day_mask & [2, 4, 8, 16, 32, 64, 1][todayIdx]) !== 0).length : 0

  return (
    <div style={{ marginTop: 10 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 5 }}>
        <span style={{ fontSize: 9, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
          {dayLabel}
        </span>
        {blocks && (
          <span style={{ fontSize: 9, color: 'var(--hds-txt-3)' }}>
            {blockCount === 0 ? 'no blocks' : `${blockCount} block${blockCount !== 1 ? 's' : ''}`}
          </span>
        )}
      </div>
      <div style={{ borderRadius: 6, border: '1px solid var(--hds-line-s)', overflow: 'hidden' }}>
        <div style={{ position: 'relative', height: 44, background: 'oklch(0.13 0.014 286)' }}>
          {!blocks && (
            <div style={{ position: 'absolute', inset: 0, background: 'oklch(0.16 0.014 286)', animation: 'pulse 1.5s ease-in-out infinite' }} />
          )}
          {segs.map((s, i) => (
            <div
              key={i}
              title={[s.time, s.title, s.subtitle].filter(Boolean).join('  ·  ')}
              style={{
                position: 'absolute', top: 0, bottom: 0,
                left: `${s.leftPct}%`, width: `${s.widthPct}%`,
                background: s.bg,
                borderLeft: '1px solid oklch(0.13 0.014 286)',
                padding: '5px 7px',
                overflow: 'hidden',
                opacity: s.faded ? 0.45 : 1,
              }}
            >
              <div style={{ fontSize: 8.5, color: s.faded ? 'var(--hds-txt-3)' : 'oklch(0.78 0.04 286)', letterSpacing: '0.06em', lineHeight: 1 }}>
                {s.time}
              </div>
              <div style={{ fontSize: 10.5, fontWeight: 700, color: s.faded ? 'var(--hds-txt-3)' : 'var(--hds-txt)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', marginTop: 3, lineHeight: 1.2 }}>
                {s.title}
              </div>
            </div>
          ))}
          {blocks && segs.length === 0 && (
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100%', color: 'var(--hds-txt-3)', fontSize: 11 }}>
              No blocks scheduled for today
            </div>
          )}
        </div>
      </div>
    </div>
  )
}

// ── Export button with depth selector ────────────────────────────────────────

function ExportButton({ channel, onExport }: {
  channel:  Channel
  onExport: (ch: Channel, depth: ExportDepth) => void
}) {
  const [depth, setDepth] = useState<ExportDepth>('shallow')
  return (
    <div className="flex">
      <select
        value={depth}
        onChange={e => setDepth(e.target.value as ExportDepth)}
        className="input text-xs rounded-r-none border-r-0 pr-1 pl-2 py-1 h-auto"
        style={{ borderRight: 'none', borderRadius: '6px 0 0 6px' }}
      >
        <option value="shallow">Shallow</option>
        <option value="deep">Deep</option>
      </select>
      <button
        onClick={() => onExport(channel, depth)}
        className="btn-secondary rounded-l-none text-xs px-2"
        style={{ borderRadius: '0 6px 6px 0' }}
      >
        Export
      </button>
    </div>
  )
}
