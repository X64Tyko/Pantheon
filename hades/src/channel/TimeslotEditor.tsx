import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import type { Block, TimeslotSlot } from '../api/types'
import type { ChannelDetailStore } from './store'

function slotLabel(slot: TimeslotSlot) {
  const oHH = String(Math.floor(slot.slot_offset_mins / 60)).padStart(2, '0')
  const oMM = String(slot.slot_offset_mins % 60).padStart(2, '0')
  const end = slot.slot_offset_mins + slot.slot_duration_mins
  const eHH = String(Math.floor(end / 60)).padStart(2, '0')
  const eMM = String(end % 60).padStart(2, '0')
  return `+${oHH}:${oMM} → +${eHH}:${eMM}`
}

function SlotRow({ slot, store, draggingId, overPos, onDragStart, onDragOver, onDrop, onDragEnd, onDragLeave }: {
  slot: TimeslotSlot;
  store: ChannelDetailStore;
  draggingId: string | null;
  overPos: { id: string; half: 'top' | 'bottom' } | null;
  onDragStart: (e: React.DragEvent, id: string) => void;
  onDragOver: (e: React.DragEvent, id: string) => void;
  onDrop: (e: React.DragEvent, id: string) => void;
  onDragEnd: () => void;
  onDragLeave: () => void;
}) {
  const head = slot.queue[slot.queue_pos]
  const isDragging = draggingId === slot.slot_id
  const over = overPos?.id === slot.slot_id ? overPos.half : null

  return (
    <div
      onClick={() => store.setEditingSlot(slot.slot_id)}
      draggable
      onDragStart={e => onDragStart(e, slot.slot_id)}
      onDragOver={e => onDragOver(e, slot.slot_id)}
      onDrop={e => onDrop(e, slot.slot_id)}
      onDragEnd={onDragEnd}
      onDragLeave={onDragLeave}
      style={{
        display: 'flex', alignItems: 'center', gap: 10, padding: '9px 12px', marginBottom: 5,
        borderRadius: 8, border: '1px solid var(--hds-line-s)',
        background: 'oklch(0.19 0.018 288 / 0.45)', cursor: 'grab', transition: 'border-color .1s, background .1s, opacity .1s',
        opacity: isDragging ? 0.35 : 1,
        boxShadow: over === 'top'    ? 'inset 0 2px 0 var(--hds-violet)'
                 : over === 'bottom' ? 'inset 0 -2px 0 var(--hds-violet)'
                 : 'none',
      }}
      onMouseEnter={e => {
        if (isDragging) return;
        (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line)';
        (e.currentTarget as HTMLDivElement).style.background = 'oklch(0.24 0.025 290 / 0.5)';
      }}
      onMouseLeave={e => {
        (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line-s)';
        (e.currentTarget as HTMLDivElement).style.background = 'oklch(0.19 0.018 288 / 0.45)';
      }}
    >
      <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', flexShrink: 0, cursor: 'grab', lineHeight: 1, marginRight: -2 }}>⠿</span>
      <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9.5, color: 'var(--hds-txt-3)', flexShrink: 0 }}>
        {slotLabel(slot)}
      </span>
      <span style={{ flex: 1, fontSize: 11, color: head ? 'var(--hds-txt)' : 'var(--hds-txt-3)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontStyle: head ? 'normal' : 'italic' }}>
        {head ? (head.title || head.content_id) : 'empty'}
      </span>
      {head && (
        <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>
          ep {slot.episode_pos}
        </span>
      )}
      <span style={{ fontSize: 9, color: 'var(--hds-violet)', flexShrink: 0 }}>›</span>
      <button
        onClick={e => { e.stopPropagation(); store.removeDraftSlot(slot.slot_id) }}
        style={{ background: 'none', border: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 14, lineHeight: 1, flexShrink: 0, padding: 0, marginLeft: 2 }}
      >×</button>
    </div>
  )
}

export const TimeslotEditor = observer(function TimeslotEditor({
  block: _block, store,
}: {
  block: Block
  store: ChannelDetailStore
}) {
  const slots       = store.draftSlots
  const convertible = store.draftContent.filter(
    c => c.content_type === 'show' || c.content_type === 'movie',
  )

  const [draggingId, setDraggingId] = useState<string | null>(null)
  const [overPos,    setOverPos]    = useState<{ id: string; half: 'top' | 'bottom' } | null>(null)

  const startReorder = (e: React.DragEvent, id: string) => {
    setDraggingId(id)
    e.dataTransfer.effectAllowed = 'move'
    e.dataTransfer.setData('text/plain', `reorder-slot:${id}`)
    e.stopPropagation()
  }

  const onRowDragOver = (e: React.DragEvent, id: string) => {
    if (draggingId === null || draggingId === id) return
    e.preventDefault()
    const rect = (e.currentTarget as HTMLDivElement).getBoundingClientRect()
    setOverPos({ id, half: e.clientY < rect.top + rect.height / 2 ? 'top' : 'bottom' })
  }

  const onRowDrop = (e: React.DragEvent, id: string) => {
    e.preventDefault()
    if (draggingId !== null && draggingId !== id) {
      store.reorderSlots(draggingId, id, overPos?.half ?? 'bottom')
    }
    setDraggingId(null)
    setOverPos(null)
  }

  const onDragEnd = () => { setDraggingId(null); setOverPos(null) }

  return (
    <div>
      {/* Convert-from-content banner — only shown when there are no slots yet */}
      {slots.length === 0 && convertible.length > 0 && (
        <div style={{ marginBottom: 12, padding: '12px 14px', borderRadius: 9, border: '1px solid oklch(0.55 0.12 290 / 0.5)', background: 'oklch(0.38 0.09 287 / 0.08)' }}>
          <div style={{ fontSize: 10.5, color: 'var(--hds-txt)', marginBottom: 4 }}>
            {convertible.length} content {convertible.length === 1 ? 'entry' : 'entries'} ready to import
          </div>
          <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', lineHeight: 1.55, marginBottom: 10 }}>
            Each show or movie becomes a 30-min slot, stacked sequentially. Adjust timing per slot afterward.
          </div>
          <button
            onClick={() => store.convertContentToSlots()}
            style={{ padding: '6px 14px', borderRadius: 7, border: '1px solid var(--hds-violet)', background: 'oklch(0.38 0.09 287 / 0.15)', color: 'var(--hds-violet)', cursor: 'pointer', fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, letterSpacing: '0.08em' }}
          >
            Import as Slots
          </button>
        </div>
      )}

      <div style={{ fontSize: 9, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 8 }}>
        {slots.length === 0 ? 'No slots defined' : `${slots.length} slot${slots.length !== 1 ? 's' : ''}`}
      </div>

      {slots.map(slot => (
        <SlotRow
          key={slot.slot_id}
          slot={slot}
          store={store}
          draggingId={draggingId}
          overPos={overPos}
          onDragStart={startReorder}
          onDragOver={onRowDragOver}
          onDrop={onRowDrop}
          onDragEnd={onDragEnd}
          onDragLeave={() => setOverPos(null)}
        />
      ))}

      <button
        onClick={() => store.addDraftSlot()}
        style={{ width: '100%', padding: '9px 0', marginTop: 4, border: '1px dashed var(--hds-violet)', borderRadius: 9, background: 'oklch(0.38 0.09 287 / 0.08)', color: 'var(--hds-violet)', cursor: 'pointer', fontFamily: "'Chakra Petch', sans-serif", fontSize: 12, letterSpacing: '0.08em', transition: '.12s' }}
      >
        + Add Slot
      </button>
    </div>
  )
})
