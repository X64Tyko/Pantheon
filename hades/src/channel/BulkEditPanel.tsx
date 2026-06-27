import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import type { FillerSelectionMode, BlockType } from '../api/types'
import { ALIGN_OPTS, BLOCK_META, DAY_BITS, DAYS, DELAY_OPTS, EARLY_OPTS, FILLER_SEL_OPTS } from './constants'
import { inputStyle } from './styles'
import type { ChannelDetailStore } from './store'
import type { BlockDraft } from './types'

// ─── Helpers ──────────────────────────────────────────────────────────────────

function daysSummary(mask: number) {
  if (mask === 127) return 'Daily'
  if (mask === 62)  return 'Mo–Fr'
  if (mask === 65)  return 'Sa–Su'
  return DAYS.filter((_, i) => (mask & DAY_BITS[i]) !== 0).map(([s]) => s).join(' ')
}

// ─── Field row ────────────────────────────────────────────────────────────────

function FieldRow({ label, enabled, onToggle, children }: {
  label:    string
  enabled:  boolean
  onToggle: (v: boolean) => void
  children: React.ReactNode
}) {
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 9, opacity: enabled ? 1 : 0.45, transition: 'opacity .12s' }}>
      <input type="checkbox" checked={enabled} onChange={e => onToggle(e.target.checked)}
        style={{ width: 13, height: 13, cursor: 'pointer', accentColor: 'var(--hds-violet)', flexShrink: 0 }} />
      <span style={{ fontSize: 9.5, letterSpacing: '0.14em', color: 'var(--hds-txt-3)', width: 82, flexShrink: 0 }}>{label}</span>
      <div style={{ flex: 1, pointerEvents: enabled ? 'auto' : 'none' }}>{children}</div>
    </div>
  )
}

// ─── BulkEditPanel ────────────────────────────────────────────────────────────

const TYPE_ORDER: BlockType[] = ['episode', 'movie', 'timeslot', 'filler']

export const BulkEditPanel = observer(function BulkEditPanel({ channelId, store }: {
  channelId: string
  store:     ChannelDetailStore
}) {
  const [alignEnabled,       setAlignEnabled]       = useState(false)
  const [alignVal,           setAlignVal]           = useState(0)
  const [lateEnabled,        setLateEnabled]        = useState(false)
  const [lateVal,            setLateVal]            = useState(5)
  const [earlyEnabled,       setEarlyEnabled]       = useState(false)
  const [earlyVal,           setEarlyVal]           = useState(15)
  const [scopeEnabled,       setScopeEnabled]       = useState(false)
  const [scopeVal,           setScopeVal]           = useState<'block' | 'episode'>('block')
  const [fillerSelEnabled,   setFillerSelEnabled]   = useState(false)
  const [fillerSelVal,       setFillerSelVal]       = useState<FillerSelectionMode>('round_robin')
  const [interEnabled,       setInterEnabled]       = useState(false)
  const [interVal,           setInterVal]           = useState(false)
  const nSelected = store.bulkSelectedIds.length
  const anyEnabled = alignEnabled || lateEnabled || earlyEnabled || scopeEnabled ||
                     fillerSelEnabled || interEnabled

  const handleApply = () => {
    const patch: Partial<BlockDraft> = {}
    if (alignEnabled)     patch.align_to_mins     = alignVal
    if (lateEnabled)      patch.late_start_mins   = lateVal
    if (earlyEnabled)     patch.early_start_secs  = earlyVal
    if (scopeEnabled)     patch.start_scope       = scopeVal
    if (fillerSelEnabled) patch.filler_selection  = fillerSelVal
    if (interEnabled)     patch.inter_filler      = interVal
    store.applyBulk(channelId, patch)
  }

  const inlineSelect: React.CSSProperties = { ...inputStyle, fontSize: 11, padding: '5px 8px' }
  const chipBtn = (active: boolean): React.CSSProperties => ({
    padding: '3px 8px', border: `1px solid ${active ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
    borderRadius: 5, background: active ? 'oklch(0.38 0.09 287 / 0.3)' : 'transparent',
    color: active ? 'var(--hds-txt)' : 'var(--hds-txt-3)',
    fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer',
  })

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>

      {/* Header */}
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '18px 20px 14px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 15, letterSpacing: '0.04em' }}>
          Bulk Edit
        </span>
        <button onClick={() => store.toggleBulkMode()}
          style={{ width: 28, height: 28, border: 'none', borderRadius: 7, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 15 }}>×</button>
      </div>

      {/* Block selector */}
      <div style={{ flexShrink: 0, borderBottom: '1px solid var(--hds-line-s)' }}>

        {/* Quick-select row */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 5, padding: '9px 12px 8px', flexWrap: 'wrap' }}>
          <button onClick={() => store.selectAllBulk()} style={chipBtn(false)}>All</button>
          <button onClick={() => store.clearBulkSelection()} style={chipBtn(false)}>None</button>
          <div style={{ width: 1, height: 14, background: 'var(--hds-line-s)', margin: '0 2px' }} />
          {TYPE_ORDER.map(t => {
            const m   = BLOCK_META[t]
            const ids = store.blocks.filter(b => b.block_type === t).map(b => b.block_id)
            const allSel = ids.length > 0 && ids.every(id => store.bulkSelectedIds.includes(id))
            return (
              <button key={t} onClick={() => store.selectBulkByType(t)} style={chipBtn(allSel)}>
                <span style={{ display: 'inline-block', width: 6, height: 6, borderRadius: 2, background: m.edge, verticalAlign: 'middle', marginRight: 4 }} />
                {m.name}
              </button>
            )
          })}
        </div>

        {/* Block list */}
        <div style={{ maxHeight: 210, overflowY: 'auto' }} className="scrollbar-dark">
          {store.blocks.map(block => {
            const m   = BLOCK_META[block.block_type]
            const sel = store.bulkSelectedIds.includes(block.block_id)
            const title = block.content[0]?.title ?? m.name
            return (
              <div key={block.block_id}
                onClick={() => store.toggleBulkBlock(block.block_id)}
                style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '6px 12px', cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)', background: sel ? 'oklch(0.24 0.025 287 / 0.6)' : 'transparent', transition: 'background .1s' }}
                onMouseEnter={e => { if (!sel) (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)' }}
                onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = sel ? 'oklch(0.24 0.025 287 / 0.6)' : 'transparent' }}
              >
                <input type="checkbox" checked={sel} readOnly
                  style={{ pointerEvents: 'none', accentColor: 'var(--hds-violet)', width: 12, height: 12, flexShrink: 0 }} />
                <span style={{ width: 5, height: 5, borderRadius: 2, background: m.edge, flexShrink: 0 }} />
                <span style={{ fontSize: 10.5, color: 'var(--hds-txt-3)', flexShrink: 0, minWidth: 38 }}>{block.start_time}</span>
                <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', flexShrink: 0, minWidth: 34 }}>{daysSummary(block.day_mask)}</span>
                <span style={{ fontSize: 11, color: 'var(--hds-txt)', flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{title}</span>
              </div>
            )
          })}
        </div>

        {/* Count */}
        <div style={{ padding: '6px 12px', fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.06em' }}>
          {nSelected} of {store.blocks.length} selected
        </div>
      </div>

      {/* Field controls */}
      <div style={{ flex: 1, minHeight: 0, overflowY: 'auto', padding: '14px 16px 8px' }} className="scrollbar-dark">
        <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 12 }}>APPLY TO SELECTED</div>

        <FieldRow label="ALIGNMENT" enabled={alignEnabled} onToggle={setAlignEnabled}>
          <select value={alignVal} onChange={e => setAlignVal(+e.target.value)} style={inlineSelect}>
            {ALIGN_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
          </select>
        </FieldRow>

        <FieldRow label="LATE START" enabled={lateEnabled} onToggle={setLateEnabled}>
          <select value={lateVal} onChange={e => setLateVal(+e.target.value)} style={inlineSelect}>
            {DELAY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
          </select>
        </FieldRow>

        <FieldRow label="EARLY START" enabled={earlyEnabled} onToggle={setEarlyEnabled}>
          <select value={earlyVal} onChange={e => setEarlyVal(+e.target.value)} style={inlineSelect}>
            {EARLY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
          </select>
        </FieldRow>

        <FieldRow label="START SCOPE" enabled={scopeEnabled} onToggle={setScopeEnabled}>
          <select value={scopeVal} onChange={e => setScopeVal(e.target.value as 'block' | 'episode')} style={inlineSelect}>
            <option value="block">Block</option>
            <option value="episode">Episode</option>
          </select>
        </FieldRow>

        <FieldRow label="FILLER MODE" enabled={fillerSelEnabled} onToggle={setFillerSelEnabled}>
          <select value={fillerSelVal} onChange={e => setFillerSelVal(e.target.value as FillerSelectionMode)} style={inlineSelect}>
            {FILLER_SEL_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
          </select>
        </FieldRow>

        <FieldRow label="BTW PROGRAMS" enabled={interEnabled} onToggle={setInterEnabled}>
          <div style={{ display: 'flex', border: '1px solid var(--hds-line)', borderRadius: 6, overflow: 'hidden', width: 'fit-content' }}>
            {([false, true] as const).map(v => (
              <button key={String(v)} onClick={() => setInterVal(v)}
                style={{ padding: '4px 12px', border: 'none', background: interVal === v ? 'var(--hds-violet)' : 'var(--hds-bg-3)', color: interVal === v ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer', fontWeight: 600 }}>
                {v ? 'ON' : 'OFF'}
              </button>
            ))}
          </div>
        </FieldRow>

      </div>

      {/* Footer */}
      <div style={{ borderTop: '1px solid var(--hds-line-s)', padding: '14px 16px', flexShrink: 0 }}>
        <button
          onClick={handleApply}
          disabled={!nSelected || !anyEnabled || store.bulkSaving}
          style={{ width: '100%', padding: '10px 0', border: 'none', borderRadius: 9, background: (!nSelected || !anyEnabled || store.bulkSaving) ? 'var(--hds-bg-3)' : 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))', color: (!nSelected || !anyEnabled || store.bulkSaving) ? 'var(--hds-txt-3)' : 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 13, cursor: (!nSelected || !anyEnabled || store.bulkSaving) ? 'default' : 'pointer', transition: '.12s' }}
        >
          {store.bulkSaving ? 'Applying…' : `Apply to ${nSelected} block${nSelected !== 1 ? 's' : ''}`}
        </button>
        {store.bulkErr && (
          <div style={{ marginTop: 8, fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{store.bulkErr}</div>
        )}
      </div>
    </div>
  )
})
