import { observer } from 'mobx-react-lite'
import { useEffect, useRef, useState } from 'react'
import { mediaUrl } from '../api/client'
import { BLOCK_META, DAYS, GUTTER_W, DAY_MIN_W } from './constants'
import DayColumn from './DayColumn'
import { LibraryBrowser } from './LibraryBrowser'
import { TimeslotEditor } from './TimeslotEditor'
import { FillerEntryRow } from './FillerPanel'
import { DropZone } from './sections'
import type { AddContentParams } from './BrowserTiles'
import type { ChannelDetailStore } from './store'

type Tab = 'content' | 'filler' | 'bumpers'

const TAB_LABELS: Record<Tab, string> = {
  content: 'Content',
  filler:  'Filler',
  bumpers: 'Bumpers',
}

const BUMPER_SLOTS = [
  { key: 'intro',         label: 'Intro',         hint: 'Plays before each episode' },
  { key: 'outro',         label: 'Outro',         hint: 'Plays after each episode' },
  { key: 'interstitial',  label: 'Interstitial',  hint: 'Plays between episodes' },
] as const



// ─── Compact week grid ────────────────────────────────────────────────────────

const COMPACT_GRID_H = 188

const CompactWeekGrid = observer(function CompactWeekGrid({ channelId, store, collapsed, onToggle }: {
  channelId: string
  store:     ChannelDetailStore
  collapsed: boolean
  onToggle:  () => void
}) {
  const scrollRef = useRef<HTMLDivElement>(null)
  const pph       = store.pxPerHour
  const gridH     = 24 * pph

  // Scroll to ≈1 hr before the active block's start time when grid opens.
  useEffect(() => {
    if (collapsed || !scrollRef.current) return
    const t = store.draft.start_time
    if (!t) return
    const [hh, mm] = t.split(':').map(Number)
    scrollRef.current.scrollTop = Math.max(0, (hh + mm / 60 - 1.5) * pph)
  }, [collapsed])

  return (
    <div style={{ flexShrink: 0, borderBottom: '1px solid var(--hds-line-s)', background: 'var(--hds-bg)' }}>
      {/* Toggle header */}
      <div
        onClick={onToggle}
        style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '5px 14px', cursor: 'pointer', userSelect: 'none' }}
      >
        <span style={{ fontSize: 8, color: 'var(--hds-txt-3)', letterSpacing: '0.22em', fontFamily: "'JetBrains Mono', monospace" }}>SCHEDULE</span>
        <span style={{ flex: 1 }} />
        <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', transition: 'transform .15s', display: 'inline-block', transform: collapsed ? 'none' : 'rotate(90deg)' }}>▶</span>
      </div>

      {!collapsed && (
        <div ref={scrollRef} style={{ height: COMPACT_GRID_H, overflow: 'auto' }} className="scrollbar-dark">
          <div style={{ minWidth: GUTTER_W + DAY_MIN_W * 7 }}>
            {/* Day header */}
            <div style={{ display: 'flex', position: 'sticky', top: 0, zIndex: 25, borderBottom: '1px solid var(--hds-line-s)', background: 'var(--hds-bg)' }}>
              <div style={{ width: GUTTER_W, flexShrink: 0 }} />
              {DAYS.map(([, long]) => (
                <div key={long} style={{ flex: `1 0 ${DAY_MIN_W}px`, textAlign: 'center', padding: '6px 0', fontSize: 9, letterSpacing: '0.2em', color: 'var(--hds-txt-2)', borderLeft: '1px solid var(--hds-line-s)' }}>
                  {long}
                </div>
              ))}
            </div>

            {/* Grid body */}
            <div style={{ display: 'flex' }}>
              <div style={{ width: GUTTER_W, flexShrink: 0, position: 'relative', height: gridH }}>
                {Array.from({ length: 25 }, (_, h) => (
                  <div key={h} style={{ position: 'absolute', top: h * pph, right: 7, transform: 'translateY(-50%)', fontSize: 9, color: 'var(--hds-txt-3)', letterSpacing: '0.03em' }}>
                    {String(h).padStart(2, '0')}:00
                  </div>
                ))}
              </div>
              {DAYS.map(([, long], di) => (
                <DayColumn key={long} dayIdx={di} blocks={store.blocks} pph={pph} selectedId={store.selectedId} store={store} channelId={channelId} />
              ))}
            </div>
          </div>
        </div>
      )}
    </div>
  )
})

// ─── Main component ───────────────────────────────────────────────────────────

export const BlockEditMain = observer(function BlockEditMain({ channelId, store }: {
  channelId: string
  store:     ChannelDetailStore
}) {
  const tab          = store.activeBlockTab
  const isTimeslot   = store.draft.block_type === 'timeslot'
  const selectedSlot = store.selectedBumperSlot

  const [gridCollapsed, setGridCollapsed] = useState(() =>
    localStorage.getItem('hds-block-grid-collapsed') === '1'
  )
  const toggleGrid = () => setGridCollapsed(c => {
    const next = !c
    localStorage.setItem('hds-block-grid-collapsed', next ? '1' : '0')
    return next
  })

  // Initialise the browser search when the editor opens.
  useEffect(() => {
    if (store.pickerShows.length === 0 && store.pickerMovies.length === 0) {
      store.openPicker()
    }
  }, [])

  // onAdd handler varies by active tab and selected bumper slot.
  const onAdd = (params: AddContentParams) => {
    if (tab === 'filler') {
      store.addBlockFiller(channelId, {
        content_type: params.content_type,
        content_id:   params.content_id,
        title:        params.title ?? params.content_id,
        advancement:  'sequential',
        weight:       1,
        season_filter: params.season_filter ?? undefined,
      })
    } else if (tab === 'bumpers' && selectedSlot) {
      store.setBumperSlot(selectedSlot, params.content_type, params.content_id)
    } else {
      store.addContent(channelId, params)
    }
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', flex: 1, minWidth: 0, minHeight: 0 }}>

      {/* ── Tab bar ── */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 2, padding: '10px 16px 0', flexShrink: 0, borderBottom: '1px solid var(--hds-line-s)' }}>
        {(['content', 'filler', 'bumpers'] as Tab[]).map(t => {
          const active = tab === t
          return (
            <button
              key={t}
              onClick={() => store.setActiveBlockTab(t)}
              style={{
                padding: '7px 16px 9px',
                border: 'none',
                borderBottom: active ? '2px solid var(--hds-violet)' : '2px solid transparent',
                background: 'transparent',
                color: active ? 'var(--hds-txt)' : 'var(--hds-txt-3)',
                fontFamily: "'JetBrains Mono', monospace",
                fontSize: 11, fontWeight: active ? 600 : 400,
                letterSpacing: '0.1em',
                cursor: 'pointer',
                transition: 'color .12s, border-color .12s',
                marginBottom: -1,
              }}
            >
              {TAB_LABELS[t]}
            </button>
          )
        })}
      </div>

      {/* ── Split panel ── */}
      <div style={{ display: 'flex', flex: 1, minHeight: 0 }}>

        {/* Left: compact week grid (collapsible) + library browser */}
        <div style={{ flex: 1, minWidth: 0, borderRight: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
          <CompactWeekGrid channelId={channelId} store={store} collapsed={gridCollapsed} onToggle={toggleGrid} />
          <LibraryBrowser channelId={channelId} store={store} onAdd={onAdd} />
        </div>

        {/* Right: tab-specific list (full height) */}
        <div style={{ width: 300, flexShrink: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
          {tab === 'content' && (
            isTimeslot && store.editing
              ? <SlotList store={store} />
              : <ContentList channelId={channelId} store={store} />
          )}
          {tab === 'filler'  && <FillerList channelId={channelId} store={store} />}
          {tab === 'bumpers' && <BumperList store={store} />}
        </div>

      </div>
    </div>
  )
})

// ─── Content list ─────────────────────────────────────────────────────────────

const ContentList = observer(function ContentList({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const items = store.draftContent
  const [draggingId, setDraggingId] = useState<number | null>(null)
  const [overPos,    setOverPos]    = useState<{ id: number; half: 'top' | 'bottom' } | null>(null)

  const startReorder = (e: React.DragEvent, id: number) => {
    setDraggingId(id)
    e.dataTransfer.effectAllowed = 'move'
    e.dataTransfer.setData('text/plain', `reorder:${id}`)
    // Don't set store.dragContent — this drag is internal to the list
    e.stopPropagation()
  }

  const onItemDragOver = (e: React.DragEvent, id: number) => {
    if (draggingId === null || draggingId === id) return
    e.preventDefault()
    const rect = (e.currentTarget as HTMLDivElement).getBoundingClientRect()
    setOverPos({ id, half: e.clientY < rect.top + rect.height / 2 ? 'top' : 'bottom' })
  }

  const onItemDrop = (e: React.DragEvent, id: number) => {
    e.preventDefault()
    if (draggingId !== null && draggingId !== id) {
      store.reorderContent(draggingId, id, overPos?.half ?? 'bottom')
    }
    setDraggingId(null)
    setOverPos(null)
  }

  const onDragEnd = () => { setDraggingId(null); setOverPos(null) }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      <div style={{ padding: '10px 12px 6px', flexShrink: 0, display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <span style={{ fontSize: 9, letterSpacing: '0.2em', color: 'var(--hds-txt-3)' }}>CONTENT</span>
        {items.length > 0 && <span style={{ fontSize: 10, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{items.length}</span>}
      </div>
      <div style={{ flex: 1, overflow: 'auto', padding: '4px 10px 16px' }} className="scrollbar-dark">
        {items.length === 0 ? (
          <div style={{ padding: '32px 0', textAlign: 'center', color: 'var(--hds-txt-3)', fontSize: 11 }}>
            Add content from the browser
          </div>
        ) : items.map(item => {
          const dot      = BLOCK_META[item.content_type === 'movie' ? 'movie' : 'episode'].edge
          const selected = store.selectedContentItemId === item.id
          const thumbUrl = item.content_type === 'show'  ? mediaUrl(`/api/shows/${item.content_id}/thumb`)
                         : item.content_type === 'movie' ? mediaUrl(`/api/movies/${item.content_id}/thumb`)
                         : null
          const isDragging = draggingId === item.id
          const over       = overPos?.id === item.id ? overPos.half : null
          return (
            <div
              key={item.id}
              draggable
              onDragStart={e => startReorder(e, item.id)}
              onDragOver={e => onItemDragOver(e, item.id)}
              onDrop={e => onItemDrop(e, item.id)}
              onDragEnd={onDragEnd}
              onDragLeave={() => setOverPos(null)}
              onClick={() => { store.selectedContentItemId = selected ? null : item.id }}
              style={{
                display: 'flex', alignItems: 'center', gap: 9, padding: '7px 9px', marginBottom: 4,
                borderRadius: 8, cursor: 'grab',
                border: `1px solid ${selected ? 'var(--hds-violet)' : 'var(--hds-line-s)'}`,
                background: selected ? 'oklch(0.55 0.14 292 / 0.1)' : 'oklch(0.19 0.018 288 / 0.45)',
                opacity: isDragging ? 0.35 : 1,
                boxShadow: over === 'top'    ? 'inset 0 2px 0 var(--hds-violet)'
                         : over === 'bottom' ? 'inset 0 -2px 0 var(--hds-violet)'
                         : 'none',
                transition: 'border-color .1s, background .1s, opacity .1s',
              }}
            >
              {/* Drag handle */}
              <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', flexShrink: 0, cursor: 'grab', lineHeight: 1, marginRight: -2 }}>⠿</span>
              {thumbUrl && (
                <img src={thumbUrl} loading="lazy"
                  style={{ width: 28, height: 40, objectFit: 'cover', borderRadius: 3, flexShrink: 0, opacity: 0, transition: 'opacity .2s' }}
                  onLoad={e  => { (e.target as HTMLImageElement).style.opacity = '1' }}
                  onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
              )}
              <span style={{ width: 6, height: 6, borderRadius: 1, background: dot, flexShrink: 0 }} />
              <span style={{ flex: 1, fontSize: 11.5, fontWeight: 500, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                {item.title || item.content_id}
              </span>
              <button
                onClick={e => { e.stopPropagation(); store.removeContent(channelId, item.id) }}
                style={{ background: 'none', border: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 14, lineHeight: 1, padding: '0 2px', flexShrink: 0 }}
              >×</button>
            </div>
          )
        })}
      </div>
    </div>
  )
})

// ─── Slot list (timeslot blocks) ──────────────────────────────────────────────

const SlotList = observer(function SlotList({ store }: { store: ChannelDetailStore }) {
  const droppable = store.dragContent &&
    (store.dragContent.content_type === 'show' || store.dragContent.content_type === 'movie')

  const handleNewSlotDrop = () => {
    const c = store.dragContent
    if (!c || (c.content_type !== 'show' && c.content_type !== 'movie')) return
    store.addDraftSlot()
    const newSlot = store.draftSlots[store.draftSlots.length - 1]
    store.addDraftQueueEntry(newSlot.slot_id, { content_type: c.content_type, content_id: c.content_id, title: c.title })
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      <div style={{ padding: '10px 12px 6px', flexShrink: 0, display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <span style={{ fontSize: 9, letterSpacing: '0.2em', color: 'var(--hds-txt-3)' }}>SLOTS</span>
        {store.draftSlots.length > 0 && <span style={{ fontSize: 10, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{store.draftSlots.length}</span>}
      </div>
      <div style={{ flex: 1, overflow: 'auto', padding: '4px 10px 16px' }} className="scrollbar-dark">
        <TimeslotEditor block={store.editing!} store={store} />
        {droppable && (
          <div style={{ marginTop: 8 }}>
            <DropZone label="DROP SHOW / MOVIE TO CREATE SLOT" onDrop={handleNewSlotDrop} />
          </div>
        )}
      </div>
    </div>
  )
})

// ─── Filler list ──────────────────────────────────────────────────────────────

const FillerList = observer(function FillerList({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const entries = store.draftFillerEntries
  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      <div style={{ padding: '10px 12px 6px', flexShrink: 0, display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <span style={{ fontSize: 9, letterSpacing: '0.2em', color: 'var(--hds-txt-3)' }}>FILLER LISTS</span>
        {entries.length > 0 && <span style={{ fontSize: 10, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{entries.length}</span>}
      </div>
      <div style={{ flex: 1, overflow: 'auto', padding: '4px 10px 16px' }} className="scrollbar-dark">
        {entries.length === 0 ? (
          <div style={{ padding: '32px 0', textAlign: 'center', color: 'var(--hds-txt-3)', fontSize: 11 }}>
            Add filler from the browser
          </div>
        ) : entries.map(entry => {
          const selected = store.selectedFillerItemId === entry.id
          return (
            <div
              key={entry.id}
              onClick={() => { store.selectedFillerItemId = selected ? null : entry.id }}
              style={{
                marginBottom: 4, borderRadius: 8, cursor: 'pointer', overflow: 'hidden',
                border: `1px solid ${selected ? 'var(--hds-violet)' : 'var(--hds-line-s)'}`,
                background: selected ? 'oklch(0.55 0.14 292 / 0.1)' : 'transparent',
                transition: 'border-color .1s, background .1s',
              }}
            >
              <FillerEntryRow
                entry={entry}
                showWeight
                onAdvancement={adv => store.updateBlockFiller(entry.id, { advancement: adv })}
                onWeight={w   => store.updateBlockFiller(entry.id, { weight: w })}
                onRemove={() => store.removeBlockFiller(entry.id)}
              />
            </div>
          )
        })}
      </div>
    </div>
  )
})

// ─── Bumper slot list ─────────────────────────────────────────────────────────

const BumperList = observer(function BumperList({ store }: { store: ChannelDetailStore }) {
  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      <div style={{ padding: '10px 12px 6px', flexShrink: 0 }}>
        <span style={{ fontSize: 9, letterSpacing: '0.2em', color: 'var(--hds-txt-3)' }}>BUMPER SLOTS</span>
      </div>
      <div style={{ flex: 1, overflow: 'auto', padding: '4px 10px 16px' }} className="scrollbar-dark">
        <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', lineHeight: 1.55, marginBottom: 14 }}>
          Select a slot, then click content in the browser to assign it.
        </div>
        {BUMPER_SLOTS.map(({ key, label, hint }) => {
          const selected    = store.selectedBumperSlot === key
          const contentId   = store.getBumperSlotContentId(key)
          const contentType = store.getBumperSlotContentType(key)
          const assigned    = contentId !== ''
          return (
            <div
              key={key}
              onClick={() => { store.selectedBumperSlot = selected ? null : key }}
              style={{
                display: 'flex', flexDirection: 'column', gap: 5,
                padding: '10px 12px', marginBottom: 8, borderRadius: 9, cursor: 'pointer',
                border: `1px solid ${selected ? 'var(--hds-violet)' : assigned ? 'var(--hds-line)' : 'var(--hds-line-s)'}`,
                background: selected ? 'oklch(0.55 0.14 292 / 0.1)' : 'oklch(0.19 0.018 288 / 0.35)',
                transition: 'border-color .1s, background .1s',
              }}
            >
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 8 }}>
                <span style={{ fontSize: 10, letterSpacing: '0.12em', color: selected ? 'var(--hds-violet)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace" }}>
                  {label.toUpperCase()}
                </span>
                {selected && (
                  <span style={{ fontSize: 9, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", letterSpacing: '0.06em' }}>
                    ACTIVE
                  </span>
                )}
                {assigned && !selected && (
                  <button
                    onClick={e => { e.stopPropagation(); store.clearBumperSlot(key) }}
                    style={{ background: 'none', border: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13, lineHeight: 1, padding: '0 2px' }}
                  >×</button>
                )}
              </div>
              {assigned ? (
                <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                  <span style={{ fontSize: 9.5, padding: '1px 5px', borderRadius: 3, background: 'var(--hds-bg)', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{contentType}</span>
                  <span style={{ fontSize: 11, color: 'var(--hds-txt-2)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{contentId}</span>
                </div>
              ) : (
                <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontStyle: 'italic' }}>{hint}</span>
              )}
            </div>
          )
        })}
      </div>
    </div>
  )
})
