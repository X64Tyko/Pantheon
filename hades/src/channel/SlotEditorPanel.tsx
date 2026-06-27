import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import { api } from '../api/client'
import { inputStyle } from './styles'
import type { TimeslotSlot, TimeslotQueueEntry, Show, Movie } from '../api/types'
import type { ChannelDetailStore } from './store'

// ─── helpers ─────────────────────────────────────────────────────────────────

const ss: React.CSSProperties = { ...inputStyle, fontSize: 11, padding: '4px 8px' }

function lbl(txt: string): React.CSSProperties {
  return { fontSize: 9, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 3, display: 'block' }
}
function Row({ children }: { children: React.ReactNode }) {
  return <div style={{ display: 'flex', alignItems: 'flex-end', gap: 8, marginBottom: 8 }}>{children}</div>
}
function Col({ label, children, width }: { label: string; children: React.ReactNode; width?: number }) {
  return (
    <div style={{ flex: width ? `0 0 ${width}px` : 1, minWidth: 0 }}>
      <span style={lbl(label)}>{label}</span>
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
function SectionLabel({ children }: { children: React.ReactNode }) {
  return (
    <div style={{ fontSize: 9, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 8 }}>
      {children}
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

function QueueRow({ entry, slotId, store }: {
  entry:  TimeslotQueueEntry
  slotId: string
  store:  ChannelDetailStore
}) {
  const [open, setOpen] = useState(false)
  return (
    <div style={{ borderRadius: 7, border: '1px solid var(--hds-line-s)', marginBottom: 5, overflow: 'hidden' }}>
      <div
        style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '8px 10px', background: 'oklch(0.18 0.015 285 / 0.7)', cursor: 'pointer' }}
        onClick={() => setOpen(o => !o)}
      >
        <span style={{ fontSize: 8, color: 'var(--hds-txt-3)', transform: open ? 'rotate(90deg)' : 'none', display: 'inline-block', transition: 'transform .12s', flexShrink: 0 }}>▶</span>
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
          onClick={e => { e.stopPropagation(); store.removeDraftQueueEntry(slotId, entry.entry_id) }}
          style={{ background: 'none', border: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 14, lineHeight: 1, flexShrink: 0, padding: 0 }}
        >×</button>
      </div>
      {open && (
        <div style={{ padding: '10px 12px 12px', borderTop: '1px solid var(--hds-line-s)' }}>
          <Row>
            <Col label="PREMIERE DATE">
              <input
                type="date" value={entry.premiere_date} style={ss}
                onChange={e => store.patchDraftQueueEntry(slotId, entry.entry_id, { premiere_date: e.target.value })}
              />
            </Col>
            <Col label="IF NOT YET PREMIERED">
              <select
                value={entry.pre_premiere_behavior} style={ss}
                onChange={e => store.patchDraftQueueEntry(slotId, entry.entry_id, { pre_premiere_behavior: e.target.value as TimeslotQueueEntry['pre_premiere_behavior'] })}
              >
                <option value="replay_previous">Replay previous</option>
                <option value="filler">Filler</option>
                <option value="skip">Skip slot</option>
              </select>
            </Col>
          </Row>
        </div>
      )}
    </div>
  )
}

// ─── Content search ───────────────────────────────────────────────────────────

function ContentSearch({ slotId, store }: { slotId: string; store: ChannelDetailStore }) {
  const [query,   setQuery]   = useState('')
  const [type,    setType]    = useState<'show' | 'movie'>('show')
  const [results, setResults] = useState<(Show | Movie)[]>([])
  const [loading, setLoading] = useState(false)

  const search = async () => {
    if (!query.trim()) return
    setLoading(true)
    try {
      const path = type === 'show'
        ? `/shows?q=${encodeURIComponent(query)}&limit=20`
        : `/movies?q=${encodeURIComponent(query)}&limit=20`
      const r = await api.get<{ items: (Show | Movie)[] }>(path)
      setResults(r.items ?? [])
    } finally {
      setLoading(false)
    }
  }

  const add = (item: Show | Movie) => {
    const id = 'show_id' in item ? item.show_id : item.movie_id
    store.addDraftQueueEntry(slotId, { content_type: type, content_id: id, title: item.title })
  }

  return (
    <div>
      <Row>
        <Col label="TYPE" width={80}>
          <select value={type} onChange={e => setType(e.target.value as 'show' | 'movie')} style={ss}>
            <option value="show">Show</option>
            <option value="movie">Movie</option>
          </select>
        </Col>
        <Col label="SEARCH">
          <input
            value={query}
            onChange={e => setQuery(e.target.value)}
            onKeyDown={e => { if (e.key === 'Enter') search() }}
            placeholder="Title…"
            style={ss}
          />
        </Col>
        <div style={{ paddingBottom: 1 }}>
          <button
            onClick={search} disabled={loading}
            style={{ ...ss, cursor: 'pointer', background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line)', borderRadius: 6 }}
          >
            {loading ? '…' : 'Search'}
          </button>
        </div>
      </Row>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 3 }}>
        {results.map(item => {
          const id = 'show_id' in item ? item.show_id : item.movie_id
          return (
            <div
              key={id}
              style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '7px 10px', cursor: 'pointer', borderRadius: 6, border: '1px solid var(--hds-line-s)', background: 'oklch(0.18 0.015 285 / 0.5)', transition: 'border-color .1s' }}
              onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line)'}
              onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line-s)'}
              onClick={() => add(item)}
            >
              <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt)', flex: 1 }}>{item.title}</span>
              <span style={{ fontSize: 8.5, color: 'var(--hds-gold)', border: '1px solid currentColor', borderRadius: 4, padding: '1px 5px', flexShrink: 0 }}>+ Add</span>
            </div>
          )
        })}
      </div>
    </div>
  )
}

// ─── SlotEditorPanel ─────────────────────────────────────────────────────────

export const SlotEditorPanel = observer(function SlotEditorPanel({ store }: {
  store: ChannelDetailStore
}) {
  const slotId = store.editingSlotId!
  const slot   = store.draftSlots.find(s => s.slot_id === slotId)
  const blockId = store.editing?.block_id ?? ''

  if (!slot) return null

  const label = slotLabel(slot)
  const patch  = (field: string, value: string | number) =>
    store.patchDraftSlot(slotId, { [field]: value } as Partial<TimeslotSlot>)

  const handleCursorReset = () =>
    store.resetDraftSlotCursor(blockId, slotId).catch(console.error)

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
        {label}
        <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', marginLeft: 10 }}>{slot.slot_duration_mins} min</span>
      </div>

      {/* Settings */}
      <Card>
        <SectionLabel>SETTINGS</SectionLabel>
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

      {/* Queue */}
      <Card>
        <div style={{ display: 'flex', alignItems: 'center', marginBottom: 8 }}>
          <SectionLabel>CONTENT QUEUE</SectionLabel>
          <div style={{ flex: 1 }} />
          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
              cursor {slot.queue_pos}/{slot.queue.length > 0 ? slot.queue.length - 1 : 0} · ep {slot.episode_pos}
            </span>
            <button
              onClick={handleCursorReset}
              style={{ fontSize: 9, padding: '2px 7px', borderRadius: 4, border: '1px solid var(--hds-line)', background: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace" }}
            >
              Reset
            </button>
          </div>
        </div>
        {slot.queue.length === 0 && (
          <div style={{ padding: '8px 0', fontSize: 10.5, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace', letterSpacing: '0.05em" }}>
            No content — add shows or movies below.
          </div>
        )}
        {slot.queue.map(e => (
          <QueueRow key={e.entry_id} entry={e} slotId={slotId} store={store} />
        ))}
      </Card>

      {/* Add content */}
      <div style={{ borderRadius: 9, border: '1px solid var(--hds-line-s)', overflow: 'hidden' }}>
        <div style={{ padding: '10px 14px', background: 'oklch(0.19 0.015 285 / 0.5)' }}>
          <SectionLabel>ADD CONTENT</SectionLabel>
          <ContentSearch slotId={slotId} store={store} />
        </div>
      </div>
    </div>
  )
})
