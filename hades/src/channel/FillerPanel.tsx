import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import type { FillerEntryAdvancement } from '../api/types'
import { BLOCK_META, FILLER_ADV_OPTS } from './constants'
import { filterInputStyle } from './styles'
import type { ChannelDetailStore } from './store'

// ─── Filler entry row ─────────────────────────────────────────────────────────

export function FillerEntryRow({ entry, showWeight, onAdvancement, onWeight, onRemove }: {
  entry:         { id: number; filler_list_id: string; title?: string; advancement: FillerEntryAdvancement; weight: number }
  showWeight:    boolean
  onAdvancement: (a: FillerEntryAdvancement) => void
  onWeight:      (w: number) => void
  onRemove:      () => void
}) {
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '8px 10px', background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)', borderRadius: 8 }}>
      <span style={{ width: 7, height: 7, borderRadius: 2, background: BLOCK_META.filler.edge, flexShrink: 0 }} />
      <span style={{ flex: 1, fontSize: 12, fontWeight: 500, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{entry.title || entry.filler_list_id}</span>
      <select
        value={entry.advancement}
        onChange={e => onAdvancement(e.target.value as FillerEntryAdvancement)}
        style={{ ...filterInputStyle, width: 92 }}
      >
        {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
      </select>
      {showWeight && (
        <input
          type="number" min={1} value={entry.weight}
          onChange={e => onWeight(Math.max(1, +e.target.value || 1))}
          style={{ ...filterInputStyle, width: 44, textAlign: 'center' }}
          title="Weight"
        />
      )}
      <button onClick={onRemove} style={{ width: 22, height: 22, border: 'none', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13, flexShrink: 0 }}>×</button>
    </div>
  )
}

// ─── Filler add panel ─────────────────────────────────────────────────────────

export const FillerAddPanel = observer(function FillerAddPanel({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const [listId, setListId] = useState('')
  const [advancement, setAdvancement] = useState<FillerEntryAdvancement>('sequential')
  const [weight, setWeight] = useState(1)
  const showWeight = store.draft.filler_selection === 'weighted'

  return (
    <div style={{ marginTop: 10, padding: '11px 12px', background: 'oklch(0.16 0.016 286)', border: '1px solid var(--hds-line)', borderRadius: 9 }}>
      <div style={{ display: 'flex', gap: 7, alignItems: 'center', flexWrap: 'wrap' }}>
        <select value={listId} onChange={e => setListId(e.target.value)} style={{ ...filterInputStyle, flex: '1 1 140px' }}>
          <option value="">Select filler list…</option>
          {store.allFillerLists.map(f => <option key={f.filler_list_id} value={f.filler_list_id}>{f.title}</option>)}
        </select>
        <select value={advancement} onChange={e => setAdvancement(e.target.value as FillerEntryAdvancement)} style={{ ...filterInputStyle, width: 96 }}>
          {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
        </select>
        {showWeight && (
          <input type="number" min={1} value={weight} onChange={e => setWeight(Math.max(1, +e.target.value || 1))} style={{ ...filterInputStyle, width: 48 }} title="Weight" placeholder="Wt" />
        )}
        <button
          onClick={() => { if (listId) { store.addBlockFiller(channelId, { filler_list_id: listId, advancement, weight }).then(() => setListId('')) } }}
          disabled={!listId || store.fillerSaving}
          style={{ padding: '5px 12px', border: 'none', borderRadius: 6, background: 'var(--hds-violet)', color: 'oklch(0.15 0.02 286)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700, cursor: (listId && !store.fillerSaving) ? 'pointer' : 'default', opacity: (listId && !store.fillerSaving) ? 1 : 0.4 }}
        >
          {store.fillerSaving ? '…' : 'Add'}
        </button>
      </div>
    </div>
  )
})
