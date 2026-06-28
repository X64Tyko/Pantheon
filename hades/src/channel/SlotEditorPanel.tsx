import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import { inputStyle } from './styles'
import { DropZone } from './sections'
import { SectionLabel } from './SectionLabel'
import type { TimeslotSlot, TimeslotQueueEntry } from '../api/types'
import type { ChannelDetailStore } from './store'

// ─── helpers ─────────────────────────────────────────────────────────────────

const ss: React.CSSProperties = { ...inputStyle, fontSize: 11, padding: '4px 8px' }

function lbl(): React.CSSProperties {
  return { fontSize: 9, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 3, display: 'block' }
}
function Row({ children }: { children: React.ReactNode }) {
  return <div style={{ display: 'flex', alignItems: 'flex-end', gap: 8, marginBottom: 8 }}>{children}</div>
}
function Col({ label, children, width }: { label: string; children: React.ReactNode; width?: number }) {
  return (
    <div style={{ flex: width ? `0 0 ${width}px` : 1, minWidth: 0 }}>
      <span style={lbl()}>{label}</span>
      {children}
    </div>
  )
}
function Card({ children }: { children: React.ReactNode }) {
  return (
    <div style={{ borderRadius: 9, border: '1px solid var(--hds-line-s)', marginBottom: 10, overflow: 'hidden' }}>
      <div style={{ padding: '12px 14px 14px' }}>{children}</div>
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
    <div style={{ borderRadius: 7, border: '1px solid var(--hds-line-s)', marginBottom: 6, overflow: 'hidden' }}>

      {/* Title row */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '7px 10px', background: 'oklch(0.18 0.015 285 / 0.7)' }}>
        <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt)', flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
          {entry.title || entry.content_id}
        </span>
        <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', flexShrink: 0 }}>{entry.content_type}</span>
        {entry.premiere_date && (
          <span style={{ fontSize: 9, color: 'oklch(0.78 0.12 68)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>
            {entry.premiere_date}
          </span>
        )}
        <button
          onClick={() => store.removeDraftQueueEntry(slotId, entry.entry_id)}
          style={{ background: 'none', border: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 14, lineHeight: 1, flexShrink: 0, padding: 0 }}
        >×</button>
      </div>

      {/* Premiere date — always visible */}
      <div style={{ display: 'flex', gap: 8, padding: '8px 10px 10px', borderTop: '1px solid var(--hds-line-s)', background: 'oklch(0.165 0.012 284 / 0.5)' }}>
        <Col label="PREMIERE DATE">
          <input
            type="date"
            value={entry.premiere_date}
            style={ss}
            onChange={e => store.patchDraftQueueEntry(slotId, entry.entry_id, { premiere_date: e.target.value })}
          />
        </Col>
        <Col label="IF NOT YET PREMIERED">
          <select
            value={entry.pre_premiere_behavior}
            style={ss}
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
    <div style={{ flex: 1, minHeight: 0, overflow: 'auto', padding: '12px 12px 20px' }} className="scrollbar-dark">

      {/* Back nav */}
      <button
        onClick={() => store.setEditingSlot(null)}
        style={{ background: 'none', border: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 10, fontFamily: "'JetBrains Mono', monospace", marginBottom: 10, display: 'flex', alignItems: 'center', gap: 5, padding: 0, letterSpacing: '0.12em' }}
      >
        ‹ SLOTS
      </button>

      {/* Slot identity */}
      <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 13, color: 'var(--hds-txt)', marginBottom: 14, letterSpacing: '0.05em' }}>
        {slotLabel(slot)}
        <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', marginLeft: 10 }}>{slot.slot_duration_mins} min</span>
      </div>

      {/* Settings */}
      <Card>
        <SectionLabel style={{ fontSize: 9, letterSpacing: '0.18em', marginBottom: 8 }}>SETTINGS</SectionLabel>
        <Row>
          <Col label="DURATION (MIN)">
            <input type="number" min={1} value={slot.slot_duration_mins} style={ss}
              onChange={e => patch('slot_duration_mins', +e.target.value)} />
          </Col>
          <Col label="OVERFLOW">
            <select value={slot.overflow} onChange={e => patch('overflow', e.target.value)} style={ss}>
              <option value="cutoff">Cutoff</option>
              <option value="finish">Finish episode</option>
            </select>
          </Col>
        </Row>
        <Row>
          <Col label="LATE START (MIN)">
            <input type="number" min={0} value={slot.late_start_mins} style={ss}
              onChange={e => patch('late_start_mins', +e.target.value)} />
          </Col>
          <Col label="EARLY START (SEC)">
            <input type="number" min={0} value={slot.early_start_secs} style={ss}
              onChange={e => patch('early_start_secs', +e.target.value)} />
          </Col>
          <Col label="ALIGN">
            <select value={slot.align_to_mins} onChange={e => patch('align_to_mins', +e.target.value)} style={ss}>
              <option value={0}>None</option>
              <option value={15}>:00/:15/:30/:45</option>
              <option value={30}>:00/:30</option>
              <option value={60}>Top of hour</option>
            </select>
          </Col>
          <Col label="SCOPE" width={88}>
            <select value={slot.start_scope} onChange={e => patch('start_scope', e.target.value)} style={ss}>
              <option value="block">Block</option>
              <option value="episode">Episode</option>
            </select>
          </Col>
        </Row>
      </Card>

      {/* Content queue */}
      <Card>
        <div style={{ display: 'flex', alignItems: 'center', marginBottom: 8 }}>
          <SectionLabel style={{ fontSize: 9, letterSpacing: '0.18em', marginBottom: 8 }}>CONTENT QUEUE</SectionLabel>
          <div style={{ flex: 1 }} />
          <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", marginRight: 8 }}>
            cursor {slot.queue_pos}/{Math.max(0, slot.queue.length - 1)} · ep {slot.episode_pos}
          </span>
          <button
            onClick={() => store.resetDraftSlotCursor(blockId, slotId).catch(console.error)}
            style={{ fontSize: 9, padding: '2px 7px', borderRadius: 4, border: '1px solid var(--hds-line)', background: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace" }}
          >
            Reset
          </button>
        </div>

        {slot.queue.length === 0 && !droppable && (
          <div style={{ padding: '4px 0 6px', fontSize: 10.5, color: 'var(--hds-txt-3)', lineHeight: 1.55 }}>
            Drag a show or movie from the browser on the left to add it to this slot's queue.
          </div>
        )}

        {slot.queue.map(e => <QueueRow key={e.entry_id} entry={e} slotId={slotId} store={store} />)}

        {droppable && (
          <div style={{ marginTop: slot.queue.length > 0 ? 6 : 0 }}>
            <DropZone label="DROP SHOW / MOVIE TO ADD TO QUEUE" onDrop={handleQueueDrop} />
          </div>
        )}
      </Card>
    </div>
  )
})
