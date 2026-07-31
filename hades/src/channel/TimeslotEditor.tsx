import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import {runInAction} from 'mobx'
import type { Block, TimeslotSlot } from '../api/types'
import type { ChannelDetailStore } from './store'
import styles from './TimeslotEditor.module.css'

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
      className={styles.slotRow}
      style={{
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
      <span className={styles.dragHandle}>⠿</span>
      <span className={styles.slotTime}>
        {slotLabel(slot)}
      </span>
      <span className={`${styles.slotTitle} ${head ? styles.slotTitleFilled : styles.slotTitleEmpty}`}>
        {head ? (head.title || head.content_id) : 'empty'}
      </span>
      {head && (
        <span className={styles.episodePos}>
          ep {slot.episode_pos}
        </span>
      )}
      <span className={styles.chevron}>›</span>
      <button
        onClick={e => { e.stopPropagation(); store.removeDraftSlot(slot.slot_id) }}
        className={styles.removeBtn}
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

    const dragEntry = () => {
        const c = store.dragContent
        return c && (c.content_type === 'show' || c.content_type === 'movie')
            ? {content_type: c.content_type, content_id: c.content_id, title: c.title}
            : null
    }

  const onRowDragOver = (e: React.DragEvent, id: string) => {
      // Reordering an existing slot takes priority; otherwise accept an
      // external show/movie dragged in from the library browser.
      if (draggingId !== null) {
          if (draggingId === id) return
      } else if (!dragEntry()) {
          return
      }
    e.preventDefault()
    const rect = (e.currentTarget as HTMLDivElement).getBoundingClientRect()
    setOverPos({ id, half: e.clientY < rect.top + rect.height / 2 ? 'top' : 'bottom' })
  }

  const onRowDrop = (e: React.DragEvent, id: string) => {
    e.preventDefault()
      if (draggingId !== null) {
          if (draggingId !== id) store.reorderSlots(draggingId, id, overPos?.half ?? 'bottom')
      } else {
          const entry = dragEntry()
          if (entry) {
              const idx = slots.findIndex(s => s.slot_id === id)
              if (idx !== -1) {
                  store.insertDraftSlotAt((overPos?.half ?? 'bottom') === 'top' ? idx : idx + 1, entry)
              }
              runInAction(() => {
                  store.dragContent = null
              })
          }
    }
    setDraggingId(null)
    setOverPos(null)
  }

  const onDragEnd = () => { setDraggingId(null); setOverPos(null) }

  return (
    <div>
      {/* Convert-from-content banner — only shown when there are no slots yet */}
      {slots.length === 0 && convertible.length > 0 && (
        <div className={styles.convertBanner}>
          <div className={styles.convertBannerTitle}>
            {convertible.length} content {convertible.length === 1 ? 'entry' : 'entries'} ready to import
          </div>
          <div className={styles.convertBannerBody}>
            Each show or movie becomes a 30-min slot, stacked sequentially. Adjust timing per slot afterward.
          </div>
          <button
            onClick={() => store.convertContentToSlots()}
            className={styles.convertBtn}
          >
            Import as Slots
          </button>
        </div>
      )}

      <div className={styles.slotsHeading}>
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
        className={styles.addSlotBtn}
      >
        + Add Slot
      </button>
    </div>
  )
})
