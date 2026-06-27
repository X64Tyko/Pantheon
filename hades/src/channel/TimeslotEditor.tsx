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

function SlotRow({ slot, store }: { slot: TimeslotSlot; store: ChannelDetailStore }) {
  const head = slot.queue[slot.queue_pos]

  return (
    <div
      onClick={() => store.setEditingSlot(slot.slot_id)}
      style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '9px 12px', marginBottom: 5, borderRadius: 8, border: '1px solid var(--hds-line-s)', background: 'oklch(0.19 0.018 288 / 0.45)', cursor: 'pointer', transition: 'border-color .1s, background .1s' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line)'; (e.currentTarget as HTMLDivElement).style.background = 'oklch(0.24 0.025 290 / 0.5)' }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line-s)'; (e.currentTarget as HTMLDivElement).style.background = 'oklch(0.19 0.018 288 / 0.45)' }}
    >
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

      {slots.map(slot => <SlotRow key={slot.slot_id} slot={slot} store={store} />)}

      <button
        onClick={() => store.addDraftSlot()}
        style={{ width: '100%', padding: '9px 0', marginTop: 4, border: '1px dashed var(--hds-violet)', borderRadius: 9, background: 'oklch(0.38 0.09 287 / 0.08)', color: 'var(--hds-violet)', cursor: 'pointer', fontFamily: "'Chakra Petch', sans-serif", fontSize: 12, letterSpacing: '0.08em', transition: '.12s' }}
      >
        + Add Slot
      </button>
    </div>
  )
})
