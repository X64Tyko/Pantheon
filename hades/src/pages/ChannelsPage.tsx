import { observer } from 'mobx-react-lite'
import { useEffect, useRef, useState } from 'react'
import { Link } from 'react-router-dom'
import { channelStore } from '../stores'
import { api } from '../api/client'
import type { Block, Channel, ChannelExport, ExportDepth, ImportPreviewResult } from '../api/types'
import { BLOCK_META, DAYS } from '../channel/constants'
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
  const [confirmRemove, setConfirmRemove] = useState<string | null>(null)
  const [importError, setImportError]   = useState<string | null>(null)
  const [importResult, setImportResult] = useState<{ channel_id: string; unresolved: any[] } | null>(null)
  const [importPending, setImportPending] = useState<{
    data: ChannelExport
    name: string
    number: string
    timezone: string
  } | null>(null)
  const [importPreview, setImportPreview] = useState<ImportPreviewResult | null>(null)
  const [importing, setImporting] = useState(false)
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
    setImportError(null); setImportResult(null); setImportPending(null); setImportPreview(null)
    try {
      const text = await file.text()
      const data = JSON.parse(text) as ChannelExport
      if (!data.kairos_export || !data.channel) throw new Error('Not a valid Kairos channel export.')
      setImportPending({
        data,
        name:     data.channel.name,
        number:   String(data.channel.number),
        timezone: data.channel.timezone,
      })
      // Kick off resolution preview in background — don't block UI
      api.previewImport(data).then(setImportPreview).catch(() => {})
    } catch (err: any) {
      setImportError(err.message ?? 'Could not parse file')
    }
  }

  const confirmImport = async () => {
    if (!importPending) return
    setImporting(true); setImportError(null)
    try {
      const payload: ChannelExport = {
        ...importPending.data,
        channel: {
          ...importPending.data.channel,
          name:     importPending.name,
          number:   parseInt(importPending.number) || importPending.data.channel.number,
          timezone: importPending.timezone,
        },
      }
      const result = await api.importChannel(payload)
      setImportResult(result)
      setImportPending(null)
      setImportPreview(null)
      store.fetchAll()
    } catch (err: any) {
      setImportError(err.message ?? 'Import failed')
    } finally {
      setImporting(false)
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
          {importError}
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

      {importPending && (
        <ImportPreview
          data={importPending.data}
          name={importPending.name}
          number={importPending.number}
          timezone={importPending.timezone}
          preview={importPreview}
          importing={importing}
          onChange={(k, v) => setImportPending(p => p ? { ...p, [k]: v } : p)}
          onConfirm={confirmImport}
          onCancel={() => { setImportPending(null); setImportPreview(null); setImportError(null) }}
        />
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
                {confirmRemove === ch.channel_id ? (
                  <span className="flex items-center gap-1.5 text-xs">
                    <span className="text-red-400">Delete channel?</span>
                    <button
                      onClick={() => { store.remove(ch.channel_id); setConfirmRemove(null) }}
                      className="px-2 py-0.5 rounded bg-red-900/60 border border-red-700/50 text-red-300 hover:bg-red-800/60 transition-colors"
                    >Yes</button>
                    <button
                      onClick={() => setConfirmRemove(null)}
                      className="px-2 py-0.5 rounded bg-zinc-800 border border-zinc-700/50 text-zinc-400 hover:bg-zinc-700 transition-colors"
                    >No</button>
                  </span>
                ) : (
                  <button onClick={() => setConfirmRemove(ch.channel_id)} className="btn-danger">
                    Remove
                  </button>
                )}
              </div>
            </div>
            <ChannelGuideStrip channelId={ch.channel_id} timezone={ch.timezone} />
          </div>
        ))}
      </div>
    </div>
  )
})

// ── Import preview ────────────────────────────────────────────────────────────

const DAY_BITS = [2, 4, 8, 16, 32, 64, 1] // Mon…Sun matching DAYS order

function formatDayMask(mask: number): string {
  const labels = DAYS.map(([short], i) => (mask & DAY_BITS[i] ? short : null)).filter(Boolean)
  if (labels.length === 7) return 'Daily'
  if (labels.length === 0) return '—'
  return labels.join(' ')
}

function ImportPreview({ data, name, number, timezone, preview, importing, onChange, onConfirm, onCancel }: {
  data:      ChannelExport
  name:      string
  number:    string
  timezone:  string
  preview:   ImportPreviewResult | null
  importing: boolean
  onChange:  (k: 'name' | 'number' | 'timezone', v: string) => void
  onConfirm: () => void
  onCancel:  () => void
}) {
  const [expanded,  setExpanded]  = useState<Record<number, boolean>>({})
  const [arrStatus, setArrStatus] = useState<Record<string, 'idle'|'adding'|'ok'|'err'>>({})
  const [arrMsg,    setArrMsg]    = useState<Record<string, string>>({})
  const toggle = (i: number) => setExpanded(e => ({ ...e, [i]: !e[i] }))

  const addToArr = async (item: ImportPreviewResult['blocks'][0]['content'][0], key: string) => {
    setArrStatus(s => ({ ...s, [key]: 'adding' }))
    try {
      const r = await api.arrAdd({
        type:    item.content_type === 'movie' ? 'movie' : 'show',
        title:   item.title,
        ...(item.tvdb_id ? { tvdb_id: item.tvdb_id } : {}),
        ...(item.tmdb_id ? { tmdb_id: item.tmdb_id } : {}),
        ...(item.imdb_id ? { imdb_id: item.imdb_id } : {}),
      })
      setArrStatus(s => ({ ...s, [key]: 'ok' }))
      setArrMsg(m => ({ ...m, [key]: r.message }))
    } catch (e: any) {
      setArrStatus(s => ({ ...s, [key]: 'err' }))
      setArrMsg(m => ({ ...m, [key]: e?.message ?? 'Failed' }))
    }
  }

  return (
    <div className="card p-5 space-y-5">
      {/* Header */}
      <div className="flex items-center justify-between">
        <h2 className="section-label">Import Preview</h2>
        <div className="flex items-center gap-3">
          {preview && preview.unresolved_count > 0 && (
            <span className="text-[10px] font-mono text-amber-500">
              {preview.unresolved_count} unresolved
            </span>
          )}
          {preview && preview.unresolved_count === 0 && (
            <span className="text-[10px] font-mono text-emerald-500">all resolved</span>
          )}
          {!preview && (
            <span className="text-[10px] font-mono text-zinc-600">resolving…</span>
          )}
          <span className="text-[10px] font-mono text-zinc-600 uppercase">
            {data.depth} export · {data.blocks.length} block{data.blocks.length !== 1 ? 's' : ''}
          </span>
        </div>
      </div>

      {/* Editable fields */}
      <div className="grid grid-cols-3 gap-3">
        <div className="space-y-1">
          <div className="text-[10px] text-zinc-500 uppercase tracking-widest">Channel Name</div>
          <input
            value={name}
            onChange={e => onChange('name', e.target.value)}
            className="input w-full"
            placeholder="Channel name"
          />
        </div>
        <div className="space-y-1">
          <div className="text-[10px] text-zinc-500 uppercase tracking-widest">Channel #</div>
          <input
            type="number"
            value={number}
            onChange={e => onChange('number', e.target.value)}
            className="input w-full"
            placeholder="Number"
          />
        </div>
        <div className="space-y-1">
          <div className="text-[10px] text-zinc-500 uppercase tracking-widest">Timezone</div>
          <input
            value={timezone}
            onChange={e => onChange('timezone', e.target.value)}
            className="input w-full"
            placeholder="e.g. America/Denver"
          />
        </div>
      </div>

      {/* Block list */}
      <div className="space-y-2">
        <div className="text-[10px] text-zinc-500 uppercase tracking-widest mb-1">Blocks</div>
        {data.blocks.length === 0 && (
          <p className="text-zinc-600 text-sm">No blocks in this export.</p>
        )}
        {data.blocks.map((b, i) => {
          const meta     = BLOCK_META[b.block_type] ?? BLOCK_META.episode
          const isOpen   = !!expanded[i]
          const cCount   = b.content.length
          const fCount   = b.filler_entries.length
          return (
            <div key={i} className="rounded-lg border border-zinc-800 overflow-hidden">
              <button
                onClick={() => toggle(i)}
                className="w-full flex items-center gap-3 px-3 py-2.5 text-left hover:bg-zinc-800/40 transition-colors"
              >
                <span className="w-2 h-2 rounded-sm flex-shrink-0" style={{ background: meta.edge }} />
                <span className="font-medium text-sm text-zinc-100 flex-1 truncate">{b.name}</span>
                <span className="text-[10px] font-mono text-zinc-500 flex-shrink-0">{formatDayMask(b.day_mask)}</span>
                <span className="text-[10px] font-mono text-zinc-500 flex-shrink-0 w-12 text-right">{b.start_time}</span>
                <span className="text-[10px] text-zinc-600 flex-shrink-0 w-24 text-right">
                  {cCount} content{fCount > 0 ? ` · ${fCount} filler` : ''}
                </span>
                <span className="text-zinc-600 text-xs">{isOpen ? '▲' : '▼'}</span>
              </button>

              {isOpen && (
                <div className="border-t border-zinc-800 px-3 py-2.5 space-y-3 bg-zinc-900/40">
                  {/* Block meta */}
                  <div className="flex flex-wrap gap-x-4 gap-y-0.5 text-[10px] font-mono text-zinc-500">
                    <span>type: {b.block_type}</span>
                    <span>adv: {b.advancement}</span>
                    {b.program_count > 0 && <span>programs: {b.program_count}</span>}
                    {b.end_time && <span>end: {b.end_time}</span>}
                  </div>

                  {/* Content */}
                  {cCount > 0 && (
                    <div>
                      <div className="text-[9px] text-zinc-600 uppercase tracking-widest mb-1">Content</div>
                      <div className="space-y-1">
                        {b.content.map((c, j) => {
                          const pItem    = preview?.blocks[i]?.content[j]
                          const resolved = pItem?.resolved
                          const key      = `${i}-${j}`
                          const status   = arrStatus[key] ?? 'idle'
                          const isShow   = c.content_type === 'show'
                          const isMovie  = c.content_type === 'movie'
                          const canArr   = (isShow || isMovie) && resolved === false
                          return (
                            <div key={j} className="flex items-center gap-2 text-xs">
                              {/* Resolution dot */}
                              {preview ? (
                                <span className={`w-1.5 h-1.5 rounded-full flex-shrink-0 ${resolved ? 'bg-emerald-500' : 'bg-amber-500'}`} />
                              ) : (
                                <span className="w-1.5 h-1.5 rounded-full flex-shrink-0 bg-zinc-700" />
                              )}
                              <span className="text-[9px] font-mono text-zinc-600 w-14 flex-shrink-0">{c.content_type}</span>
                              <span className="text-zinc-300 truncate flex-1">{c.title}</span>
                              {c.season_filter != null && (
                                <span className="text-[9px] font-mono text-zinc-600 flex-shrink-0">S{String(c.season_filter).padStart(2,'0')}</span>
                              )}
                              {canArr && status === 'idle' && (
                                <button
                                  onClick={() => addToArr(pItem!, key)}
                                  className="text-[9px] font-mono text-zinc-500 hover:text-amber-400 flex-shrink-0 border border-zinc-700 hover:border-amber-700 rounded px-1.5 py-0.5 transition-colors"
                                >
                                  + {isShow ? 'Sonarr' : 'Radarr'}
                                </button>
                              )}
                              {status === 'adding' && (
                                <span className="text-[9px] font-mono text-zinc-500 flex-shrink-0">adding…</span>
                              )}
                              {status === 'ok' && (
                                <span className="text-[9px] font-mono text-emerald-500 flex-shrink-0" title={arrMsg[key]}>added</span>
                              )}
                              {status === 'err' && (
                                <span className="text-[9px] font-mono text-red-500 flex-shrink-0 cursor-help" title={arrMsg[key]}>failed</span>
                              )}
                            </div>
                          )
                        })}
                      </div>
                    </div>
                  )}

                  {/* Filler */}
                  {fCount > 0 && (
                    <div>
                      <div className="text-[9px] text-zinc-600 uppercase tracking-widest mb-1">Filler</div>
                      <div className="space-y-0.5">
                        {b.filler_entries.map((f, j) => (
                          <div key={j} className="text-xs text-zinc-400 truncate">{f.title}</div>
                        ))}
                      </div>
                    </div>
                  )}
                </div>
              )}
            </div>
          )
        })}
      </div>

      {/* Actions */}
      <div className="flex gap-2 pt-1">
        <button onClick={onConfirm} disabled={importing} className="btn-primary">
          {importing ? 'Importing…' : 'Import Channel'}
        </button>
        <button onClick={onCancel} className="btn-ghost">Cancel</button>
      </div>
    </div>
  )
}

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
