import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import type { FillerSelectionMode, BlockType } from '../api/types'
import { ALIGN_OPTS, BLOCK_META, DAY_BITS, DAYS, DELAY_OPTS, EARLY_OPTS, FILLER_SEL_OPTS } from './constants'
import type { ChannelDetailStore } from './store'
import type { BlockDraft } from './types'
import styles from './BulkEditPanel.module.css'

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
    <div className={`${styles.fieldRow} ${enabled ? styles.fieldRowEnabled : styles.fieldRowDisabled}`}>
      <input type="checkbox" checked={enabled} onChange={e => onToggle(e.target.checked)}
        className={styles.fieldCheckbox} />
      <span className={styles.fieldLabel}>{label}</span>
      <div className={`${styles.fieldContent} ${enabled ? styles.fieldContentEnabled : styles.fieldContentDisabled}`}>{children}</div>
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

  const applyDisabled = !nSelected || !anyEnabled || store.bulkSaving

  return (
    <div className={styles.root}>

      {/* Header */}
      <div className={styles.header}>
        <span className={styles.headerTitle}>
          Bulk Edit
        </span>
        <button onClick={() => store.toggleBulkMode()} className={styles.closeBtn}>×</button>
      </div>

      {/* Block selector */}
      <div className={styles.blockSelector}>

        {/* Quick-select row */}
        <div className={styles.quickSelectRow}>
          <button onClick={() => store.selectAllBulk()} className={styles.chipBtn}>All</button>
          <button onClick={() => store.clearBulkSelection()} className={styles.chipBtn}>None</button>
          <div className={styles.chipDivider} />
          {TYPE_ORDER.map(t => {
            const m   = BLOCK_META[t]
            const ids = store.blocks.filter(b => b.block_type === t).map(b => b.block_id)
            const allSel = ids.length > 0 && ids.every(id => store.bulkSelectedIds.includes(id))
            return (
              <button key={t} onClick={() => store.selectBulkByType(t)} className={`${styles.chipBtn} ${allSel ? styles.chipBtnActive : ''}`}>
                {/* dot color comes from BLOCK_META, a small closed per-type
                    lookup — not a design token, same exception as elsewhere
                    in this directory. */}
                <span className={styles.chipDot} style={{ background: m.edge }} />
                {m.name}
              </button>
            )
          })}
        </div>

        {/* Block list */}
        <div className={`${styles.blockList} scrollbar-dark`}>
          {store.blocks.map(block => {
            const m   = BLOCK_META[block.block_type]
            const sel = store.bulkSelectedIds.includes(block.block_id)
            const title = block.content[0]?.title ?? m.name
            return (
              <div key={block.block_id}
                onClick={() => store.toggleBulkBlock(block.block_id)}
                className={`${styles.blockRow} ${sel ? styles.blockRowSelected : styles.blockRowUnselected}`}
              >
                <input type="checkbox" checked={sel} readOnly className={styles.blockRowCheckbox} />
                <span className={styles.blockRowDot} style={{ background: m.edge }} />
                <span className={styles.blockRowTime}>{block.start_time}</span>
                <span className={styles.blockRowDays}>{daysSummary(block.day_mask)}</span>
                <span className={styles.blockRowTitle}>{title}</span>
              </div>
            )
          })}
        </div>

        {/* Count */}
        <div className={styles.selectedCount}>
          {nSelected} of {store.blocks.length} selected
        </div>
      </div>

      {/* Field controls */}
      <div className={`${styles.fieldControls} scrollbar-dark`}>
        <div className={styles.fieldControlsLabel}>APPLY TO SELECTED</div>

        <FieldRow label="ALIGNMENT" enabled={alignEnabled} onToggle={setAlignEnabled}>
          <select value={alignVal} onChange={e => setAlignVal(+e.target.value)} className={styles.inlineSelect}>
            {ALIGN_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
          </select>
        </FieldRow>

        <FieldRow label="LATE START" enabled={lateEnabled} onToggle={setLateEnabled}>
          <select value={lateVal} onChange={e => setLateVal(+e.target.value)} className={styles.inlineSelect}>
            {DELAY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
          </select>
        </FieldRow>

        <FieldRow label="EARLY START" enabled={earlyEnabled} onToggle={setEarlyEnabled}>
          <select value={earlyVal} onChange={e => setEarlyVal(+e.target.value)} className={styles.inlineSelect}>
            {EARLY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
          </select>
        </FieldRow>

        <FieldRow label="START SCOPE" enabled={scopeEnabled} onToggle={setScopeEnabled}>
          <select value={scopeVal} onChange={e => setScopeVal(e.target.value as 'block' | 'episode')} className={styles.inlineSelect}>
            <option value="block">Block</option>
            <option value="episode">Episode</option>
          </select>
        </FieldRow>

        <FieldRow label="FILLER MODE" enabled={fillerSelEnabled} onToggle={setFillerSelEnabled}>
          <select value={fillerSelVal} onChange={e => setFillerSelVal(e.target.value as FillerSelectionMode)} className={styles.inlineSelect}>
            {FILLER_SEL_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
          </select>
        </FieldRow>

        <FieldRow label="BTW PROGRAMS" enabled={interEnabled} onToggle={setInterEnabled}>
          <div className={styles.toggleGroup}>
            {([false, true] as const).map(v => (
              <button key={String(v)} onClick={() => setInterVal(v)}
                className={`${styles.toggleBtn} ${interVal === v ? styles.toggleBtnActive : ''}`}>
                {v ? 'ON' : 'OFF'}
              </button>
            ))}
          </div>
        </FieldRow>

      </div>

      {/* Footer */}
      <div className={styles.footer}>
        <button
          onClick={handleApply}
          disabled={applyDisabled}
          className={`${styles.applyBtn} ${applyDisabled ? styles.applyBtnDisabled : styles.applyBtnEnabled}`}
        >
          {store.bulkSaving ? 'Applying…' : `Apply to ${nSelected} block${nSelected !== 1 ? 's' : ''}`}
        </button>
        {store.bulkErr && (
          <div className={styles.errText}>{store.bulkErr}</div>
        )}
      </div>
    </div>
  )
})
