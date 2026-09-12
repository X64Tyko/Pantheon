import { observer } from 'mobx-react-lite'
import { DropZone } from './sections'
import { SectionLabel } from './SectionLabel'
import type { TimeslotSlot, TimeslotQueueEntry } from '../api/types'
import type { ChannelDetailStore } from './store'
import styles from './SlotEditorPanel.module.css'
import sharedStyles from './sharedStyles.module.css'

// ─── helpers ─────────────────────────────────────────────────────────────────

function Row({ children }: { children: React.ReactNode }) {
  return <div className={styles.row}>{children}</div>
}
function Col({ label, children, width }: { label: string; children: React.ReactNode; width?: number }) {
  return (
    <div className={styles.col} style={width ? { flex: `0 0 ${width}px` } : undefined}>
      <span className={styles.label}>{label}</span>
      {children}
    </div>
  )
}
function Card({ children }: { children: React.ReactNode }) {
  return (
    <div className={styles.card}>
      <div className={styles.cardInner}>{children}</div>
    </div>
  )
}

function slotLabel(slot: TimeslotSlot) {
  const oHH = String(Math.floor(slot.slot_offset_mins / 60)).padStart(2, '0')
  const oMM = String(slot.slot_offset_mins % 60).padStart(2, '0')
  const end = slot.slot_offset_mins + slot.slot_duration_mins
  const eHH = String(Math.floor(end / 60)).padStart(2, '0')
  const eMM = String(end % 60).padStart(2, '0')
  return `+${oHH}:${oMM} → +${eHH}:${eMM}`
}

// ─── Queue entry row ──────────────────────────────────────────────────────────

const QueueRow = observer(function QueueRow({ entry, slotId, store }: {
  entry:  TimeslotQueueEntry
  slotId: string
  store:  ChannelDetailStore
}) {
  return (
    <div className={styles.queueRow}>

      {/* Title row */}
      <div className={styles.queueTitleRow}>
        <span className={styles.queueTitle}>
          {entry.title || entry.content_id}
        </span>
        <span className={styles.queueType}>{entry.content_type}</span>
        {entry.premiere_date && (
          <span className={styles.queuePremiereDate}>
            {entry.premiere_date}
          </span>
        )}
        <button
          onClick={() => store.removeDraftQueueEntry(slotId, entry.entry_id)}
          className={styles.queueRemoveBtn}
        >×</button>
      </div>

      {/* Premiere date — always visible */}
      <div className={styles.queueDetailRow}>
        <Col label="PREMIERE DATE">
          <input
            type="date"
            value={entry.premiere_date}
            className={sharedStyles.filterInput}
            onChange={e => store.patchDraftQueueEntry(slotId, entry.entry_id, { premiere_date: e.target.value })}
          />
        </Col>
        <Col label="IF NOT YET PREMIERED">
          <select
            value={entry.pre_premiere_behavior}
            className={sharedStyles.filterInput}
            onChange={e => store.patchDraftQueueEntry(slotId, entry.entry_id, { pre_premiere_behavior: e.target.value as TimeslotQueueEntry['pre_premiere_behavior'] })}
          >
            <option value="replay_previous">Replay previous</option>
            <option value="filler">Filler</option>
            <option value="skip">Skip slot</option>
          </select>
        </Col>
      </div>
    </div>
  )
})

// ─── SlotEditorPanel ─────────────────────────────────────────────────────────

export const SlotEditorPanel = observer(function SlotEditorPanel({ store }: {
  store: ChannelDetailStore
}) {
  const slotId  = store.editingSlotId!
  const slot    = store.draftSlots.find(s => s.slot_id === slotId)
  const blockId = store.editing?.block_id ?? ''

  if (!slot) return null

  const patch = (field: string, value: string | number) =>
    store.patchDraftSlot(slotId, { [field]: value } as Partial<TimeslotSlot>)

  const droppable = store.dragContent &&
    (store.dragContent.content_type === 'show' || store.dragContent.content_type === 'movie')

  const handleQueueDrop = () => {
    const c = store.dragContent
    if (!c || (c.content_type !== 'show' && c.content_type !== 'movie')) return
    store.addDraftQueueEntry(slotId, { content_type: c.content_type, content_id: c.content_id, title: c.title })
  }

  return (
    <div className={`${styles.root} scrollbar-dark`}>

      {/* Back nav */}
      <button
        onClick={() => store.setEditingSlot(null)}
        className={styles.backBtn}
      >
        ‹ SLOTS
      </button>

      {/* Slot identity */}
      <div className={styles.slotIdentity}>
        {slotLabel(slot)}
        <span className={styles.slotDuration}>{slot.slot_duration_mins} min</span>
      </div>

      {/* Settings */}
      <Card>
        <SectionLabel variant="compact">SETTINGS</SectionLabel>
        <Row>
          <Col label="DURATION (MIN)">
            <input type="number" min={1} value={slot.slot_duration_mins} className={sharedStyles.filterInput}
              onChange={e => patch('slot_duration_mins', +e.target.value)} />
          </Col>
          <Col label="OVERFLOW">
            <select value={slot.overflow} onChange={e => patch('overflow', e.target.value)} className={sharedStyles.filterInput}>
              <option value="cutoff">Cutoff</option>
              <option value="finish">Finish episode</option>
            </select>
          </Col>
        </Row>
        <Row>
          <Col label="LATE START (MIN)">
            <input type="number" min={0} value={slot.late_start_mins} className={sharedStyles.filterInput}
              onChange={e => patch('late_start_mins', +e.target.value)} />
          </Col>
          <Col label="EARLY START (SEC)">
            <input type="number" min={0} value={slot.early_start_secs} className={sharedStyles.filterInput}
              onChange={e => patch('early_start_secs', +e.target.value)} />
          </Col>
          <Col label="ALIGN">
            <select value={slot.align_to_mins} onChange={e => patch('align_to_mins', +e.target.value)} className={sharedStyles.filterInput}>
              <option value={0}>None</option>
              <option value={15}>:00/:15/:30/:45</option>
              <option value={30}>:00/:30</option>
              <option value={60}>Top of hour</option>
            </select>
          </Col>
          <Col label="SCOPE" width={88}>
            <select value={slot.start_scope} onChange={e => patch('start_scope', e.target.value)} className={sharedStyles.filterInput}>
              <option value="block">Block</option>
              <option value="episode">Episode</option>
            </select>
          </Col>
        </Row>
      </Card>

      {/* Content queue */}
      <Card>
        <div className={styles.queueHeaderRow}>
          <SectionLabel variant="compact">CONTENT QUEUE</SectionLabel>
          <div className={styles.queueHeaderSpacer} />
          <span className={styles.queueCursor}>
            cursor {slot.queue_pos}/{Math.max(0, slot.queue.length - 1)} · ep {slot.episode_pos}
          </span>
          <button
            onClick={() => store.resetDraftSlotCursor(blockId, slotId).catch(console.error)}
            className={styles.resetBtn}
          >
            Reset
          </button>
        </div>

        {slot.queue.length === 0 && !droppable && (
          <div className={styles.emptyQueueHint}>
            Drag a show or movie from the browser on the left to add it to this slot's queue.
          </div>
        )}

        {slot.queue.map(e => <QueueRow key={e.entry_id} entry={e} slotId={slotId} store={store} />)}

        {droppable && (
          <div className={slot.queue.length > 0 ? styles.dropZoneWrap : styles.dropZoneWrapNoGap}>
            <DropZone label="DROP SHOW / MOVIE TO ADD TO QUEUE" onDrop={handleQueueDrop} />
          </div>
        )}
      </Card>
    </div>
  )
})
