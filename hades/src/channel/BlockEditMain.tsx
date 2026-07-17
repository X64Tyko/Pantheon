import { observer } from 'mobx-react-lite'
import { useEffect, useRef, useState } from 'react'
import { mediaUrl } from '../api/client'
import { DAYS } from './constants'
import DayColumn from './DayColumn'
import { LibraryBrowser } from './LibraryBrowser'
import { TimeslotEditor } from './TimeslotEditor'
import { FillerEntryRow } from './FillerPanel'
import { DropZone } from './sections'
import type { AddContentParams } from './BrowserTiles'
import type { ChannelDetailStore } from './store'
import styles from './BlockEditMain.module.css'

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
// COMPACT_GRID_H (188), GUTTER_W (58), DAY_MIN_W (94) are baked directly into
// BlockEditMain.module.css's .weekGridScroll/.weekGridInner/.dayHeaderGutter/
// .dayHeaderCell/.hourGutter — they're fixed constants, not per-render values.

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
    <div className={styles.weekGridRoot}>
      {/* Toggle header */}
      <div onClick={onToggle} className={styles.weekGridHeader}>
        <span className={styles.weekGridLabel}>SCHEDULE</span>
        <span className={styles.weekGridSpacer} />
        <span className={`${styles.weekGridChevron} ${collapsed ? styles.weekGridChevronCollapsed : ''}`}>▶</span>
      </div>

      {!collapsed && (
        <div ref={scrollRef} className={`${styles.weekGridScroll} scrollbar-dark`}>
          <div className={styles.weekGridInner}>
            {/* Day header */}
            <div className={styles.dayHeaderRow}>
              <div className={styles.dayHeaderGutter} />
              {DAYS.map(([, long]) => (
                <div key={long} className={styles.dayHeaderCell}>
                  {long}
                </div>
              ))}
            </div>

            {/* Grid body */}
            <div className={styles.gridBody}>
              <div className={styles.hourGutter} style={{ height: gridH }}>
                {Array.from({ length: 25 }, (_, h) => (
                  <div key={h} className={styles.hourLabel} style={{ top: h * pph }}>
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
    <div className={styles.root}>

      {/* ── Tab bar ── */}
      <div className={styles.tabBar}>
        {(['content', 'filler', 'bumpers'] as Tab[]).map(t => {
          const active = tab === t
          return (
            <button
              key={t}
              onClick={() => store.setActiveBlockTab(t)}
              className={`${styles.tabButton} ${active ? styles.tabButtonActive : ''}`}
            >
              {TAB_LABELS[t]}
            </button>
          )
        })}
      </div>

      {/* ── Split panel ── */}
      <div className={styles.splitPanel}>

        {/* Left: compact week grid (collapsible) + library browser */}
        <div className={styles.leftPanel}>
          <CompactWeekGrid channelId={channelId} store={store} collapsed={gridCollapsed} onToggle={toggleGrid} />
          <LibraryBrowser channelId={channelId} store={store} onAdd={onAdd} />
        </div>

        {/* Right: tab-specific list (full height) */}
        <div className={styles.rightPanel}>
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
    <div className={styles.listRoot}>
      <div className={styles.listHeader}>
        <span className={styles.listHeaderLabel}>CONTENT</span>
        {items.length > 0 && <span className={styles.listHeaderCount}>{items.length}</span>}
      </div>
      <div className={`${styles.listScroll} scrollbar-dark`}>
        {items.length === 0 ? (
          <div className={styles.emptyState}>
            Add content from the browser
          </div>
        ) : items.map(item => {
          const isMovie  = item.content_type === 'movie'
          const selected = store.selectedContentItemId === item.id
          const thumbUrl = item.content_type === 'show'  ? mediaUrl(`/api/shows/${item.content_id}/thumb`)
                         : item.content_type === 'movie' ? mediaUrl(`/api/movies/${item.content_id}/thumb`)
                         : null
          const isDragging = draggingId === item.id
          const over       = overPos?.id === item.id ? overPos.half : null
          const rowClass = [
            styles.itemRow,
            selected ? styles.itemRowSelected : '',
            isDragging ? styles.itemRowDragging : '',
            over === 'top' ? styles.itemRowOverTop : over === 'bottom' ? styles.itemRowOverBottom : '',
          ].filter(Boolean).join(' ')
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
              className={rowClass}
            >
              {/* Drag handle */}
              <span className={styles.dragHandle}>⠿</span>
              {thumbUrl && (
                <img src={thumbUrl} loading="lazy"
                  className={styles.itemThumb}
                  onLoad={e  => { (e.target as HTMLImageElement).style.opacity = '1' }}
                  onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
              )}
              <span className={`${styles.dot} ${isMovie ? styles.dotMovie : styles.dotEpisode}`} />
              <span className={styles.itemTitle}>
                {item.title || item.content_id}
              </span>
              <button
                onClick={e => { e.stopPropagation(); store.removeContent(channelId, item.id) }}
                className={styles.removeBtn}
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
    <div className={styles.listRoot}>
      <div className={styles.listHeader}>
        <span className={styles.listHeaderLabel}>SLOTS</span>
        {store.draftSlots.length > 0 && <span className={styles.listHeaderCount}>{store.draftSlots.length}</span>}
      </div>
      <div className={`${styles.listScroll} scrollbar-dark`}>
        <TimeslotEditor block={store.editing!} store={store} />
        {droppable && (
          <div className={styles.dropZoneWrap}>
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
    <div className={styles.listRoot}>
      <div className={styles.listHeader}>
        <span className={styles.listHeaderLabel}>FILLER LISTS</span>
        {entries.length > 0 && <span className={styles.listHeaderCount}>{entries.length}</span>}
      </div>
      <div className={`${styles.listScroll} scrollbar-dark`}>
        {entries.length === 0 ? (
          <div className={styles.emptyState}>
            Add filler from the browser
          </div>
        ) : entries.map(entry => {
          const selected = store.selectedFillerItemId === entry.id
          return (
            <div
              key={entry.id}
              onClick={() => { store.selectedFillerItemId = selected ? null : entry.id }}
              className={`${styles.fillerItemRow} ${selected ? styles.fillerItemRowSelected : ''}`}
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
    <div className={styles.listRoot}>
      <div className={styles.listHeader}>
        <span className={styles.listHeaderLabel}>BUMPER SLOTS</span>
      </div>
      <div className={`${styles.listScroll} scrollbar-dark`}>
        <div className={styles.bumperHint}>
          Select a slot, then click content in the browser to assign it.
        </div>
        {BUMPER_SLOTS.map(({ key, label, hint }) => {
          const selected    = store.selectedBumperSlot === key
          const contentId   = store.getBumperSlotContentId(key)
          const contentType = store.getBumperSlotContentType(key)
          const assigned    = contentId !== ''
          const rowClass = [
            styles.bumperSlotRow,
            selected ? styles.bumperSlotRowSelected : assigned ? styles.bumperSlotRowAssigned : '',
          ].filter(Boolean).join(' ')
          return (
            <div
              key={key}
              onClick={() => { store.selectedBumperSlot = selected ? null : key }}
              className={rowClass}
            >
              <div className={styles.bumperSlotTop}>
                <span className={`${styles.bumperSlotLabel} ${selected ? styles.bumperSlotLabelSelected : ''}`}>
                  {label.toUpperCase()}
                </span>
                {selected && (
                  <span className={styles.bumperActiveTag}>
                    ACTIVE
                  </span>
                )}
                {assigned && !selected && (
                  <button
                    onClick={e => { e.stopPropagation(); store.clearBumperSlot(key) }}
                    className={styles.bumperClearBtn}
                  >×</button>
                )}
              </div>
              {assigned ? (
                <div className={styles.bumperAssignedRow}>
                  <span className={styles.bumperTypeTag}>{contentType}</span>
                  <span className={styles.bumperContentId}>{contentId}</span>
                </div>
              ) : (
                <span className={styles.bumperHintText}>{hint}</span>
              )}
            </div>
          )
        })}
      </div>
    </div>
  )
})
