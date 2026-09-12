import { observer } from 'mobx-react-lite'
import { useEffect, useRef, useState } from 'react'
import type { CSSProperties } from 'react'
import type { Block } from '../api/types'
import { BLOCK_META, DAY_BITS, DAY_MIN_W } from './constants'
import { endOf, t2m } from './utils'
import type { ChannelDetailStore } from './store'
import styles from './DayColumn.module.css'

const CREATE_SNAP_MIN = 15

const DayColumn = observer(function DayColumn({ dayIdx, blocks, pph, selectedId, store, channelId, enableCreate }: {
  dayIdx:     number
  blocks:     Block[]
  pph:        number
  selectedId: string | null
  store:      ChannelDetailStore
  channelId:  string
  enableCreate?: boolean
}) {
  const bit       = DAY_BITS[dayIdx]
  const colBlocks = blocks.filter(b => (b.day_mask & bit) !== 0)
  const colMax    = Math.max(0, ...colBlocks.map(b => b.priority))

    // Same-priority overlap has no defined resolution (priority is what
    // decides which of two overlapping blocks wins — see the depth/filter
    // stacking below); two same-priority blocks covering the same moment on
    // the same day is always a config mistake, not something the scheduler can
    // meaningfully arbitrate. Different-priority overlap is the normal/
    // supported preemption case and is deliberately left alone here.
    const conflictIds = (() => {
        const ids = new Set<string>()
        for (let i = 0; i < colBlocks.length; i++) {
            const a = colBlocks[i]
            const aStart = t2m(a.start_time), aEnd = endOf(a)
            for (let j = i + 1; j < colBlocks.length; j++) {
                const b = colBlocks[j]
                if (a.priority !== b.priority) continue
                const bStart = t2m(b.start_time), bEnd = endOf(b)
                if (aStart < bEnd && bStart < aEnd) {
                    ids.add(a.block_id);
                    ids.add(b.block_id)
                }
            }
        }
        return ids
    })()
  const gridH     = 24 * pph
  const lineBg    = `repeating-linear-gradient(to bottom, transparent 0px, transparent ${pph - 1}px, var(--hds-line-s) ${pph - 1}px, var(--hds-line-s) ${pph}px)`

  // ── Click-and-drag to create a new block over a time range ────────────────
  const colRef       = useRef<HTMLDivElement>(null)
  const draggingRef  = useRef(false)
  const [dragRange, setDragRange] = useState<{ start: number; end: number } | null>(null)

  const minsFromY = (clientY: number) => {
    const rect     = colRef.current?.getBoundingClientRect()
    if (!rect) return 0
    const raw      = ((clientY - rect.top) / pph) * 60
    const snapped  = Math.round(raw / CREATE_SNAP_MIN) * CREATE_SNAP_MIN
    return Math.max(0, Math.min(1440, snapped))
  }

  useEffect(() => {
    if (!enableCreate) return
    const onMove = (e: MouseEvent) => {
      if (!draggingRef.current) return
      setDragRange(r => r ? { start: r.start, end: minsFromY(e.clientY) } : r)
    }
    const onUp = (e: MouseEvent) => {
      if (!draggingRef.current) return
      draggingRef.current = false
      setDragRange(r => {
        if (r) {
          const lo = Math.min(r.start, r.end)
          const hi = Math.max(r.start, r.end)
          if (hi - lo >= CREATE_SNAP_MIN) store.openNewAt(dayIdx, lo, hi)
        }
        return null
      })
    }
    window.addEventListener('mousemove', onMove)
    window.addEventListener('mouseup', onUp)
    return () => { window.removeEventListener('mousemove', onMove); window.removeEventListener('mouseup', onUp) }
  }, [enableCreate, pph, dayIdx])

  const onColumnMouseDown = (e: React.MouseEvent) => {
    if (!enableCreate || e.target !== e.currentTarget) return
    const start = minsFromY(e.clientY)
    draggingRef.current = true
    setDragRange({ start, end: start })
  }

  return (
    <div
      ref={colRef}
      onMouseDown={onColumnMouseDown}
      style={{ flex: `1 0 ${DAY_MIN_W}px`, position: 'relative', height: gridH, borderLeft: '1px solid var(--hds-line-s)', background: lineBg, cursor: enableCreate ? 'crosshair' : undefined }}
    >
      {dragRange && (() => {
        const lo = Math.min(dragRange.start, dragRange.end)
        const hi = Math.max(dragRange.start, dragRange.end)
        return (
          <div style={{
            position: 'absolute', left: 2, right: 2,
            top: Math.round((lo / 60) * pph), height: Math.max(Math.round(((hi - lo) / 60) * pph), 3),
            background: 'oklch(0.7 0.13 287 / 0.28)',
            border: '2px dashed var(--hds-violet)',
            borderRadius: 6,
            pointerEvents: 'none',
            zIndex: 100,
          }} />
        )
      })()}
      {colBlocks.slice().sort((a, b) => a.priority - b.priority).map(block => {
        const m       = BLOCK_META[block.block_type]
        const start   = t2m(block.start_time)
        const end     = endOf(block)
        const limitM  = block.end_time ? 'end' : block.program_count > 0 ? 'programs' : 'fill'
        const top     = Math.round((start / 60) * pph)
        const height  = Math.max(Math.round(((end - start) / 60) * pph), 30)
        const left    = Math.min((block.priority - 1) * 11, 33)
        const depth   = colMax > 1 ? colMax - block.priority : 0
          const conflict = conflictIds.has(block.block_id)
          // A same-priority conflict needs to actually be seen to get fixed —
          // the normal priority stacking (dimmed/buried behind whichever block
          // in this column has the highest priority) would otherwise hide
          // exactly the blocks this warning exists for, including from each
          // other when they tie for colMax themselves.
          const isTop = block.priority === colMax || conflict
        const sel          = selectedId === block.block_id
        const bulkSelected = store.bulkMode && store.bulkSelectedIds.includes(block.block_id)
        const filter  = isTop ? 'none' : `brightness(${(0.82 - depth * 0.1).toFixed(2)}) saturate(0.85)`
        const shadow  = block.priority > 1 ? '0 8px 22px -8px rgba(0,0,0,0.65)' : 'none'
        const outline = bulkSelected ? '2px solid var(--hds-violet)' : sel ? '2px solid var(--hds-gold)' : 'none'
        // Selected (not bulk-selected) blocks additionally get a shimmering
        // gold ring layered on top of that static outline (hds-shimmer-ring
        // className below) — the outline above stays exactly as it was.
        const shimmerGold = sel && !bulkSelected
        const firstName = block.name || block.content[0]?.title || m.name
        const showMeta  = height > 62
        const metaLabel = m.name.toUpperCase() + ' · P' + block.priority + (limitM === 'programs' ? ` · ${block.program_count}×` : '') + (limitM === 'programs' ? ' · flex' : '')

        const boxStyle: CSSProperties = {
          position: 'absolute', left, right: 5,
          top, height,
            // Conflict wins over the normal priority-based stacking (see isTop
            // above) — a fixed value well above any realistic priority*2+40
            // ceiling, so a conflicting block always sits above every other
            // block in the column, not just the ones it directly overlaps.
            zIndex: (conflict ? 1000 : block.priority * 2) + (sel ? 40 : 0),
          background: m.bg,
          borderRadius: limitM === 'programs' ? '7px 7px 0 0' : 7,
            border: conflict ? '1px solid var(--hds-danger-border)' : `1px solid ${m.border}`,
            borderLeft: `3px solid ${conflict ? 'var(--hds-danger-text)' : m.edge}`,
            borderBottom: limitM === 'programs' ? `2px dashed ${m.edge}` : conflict ? '1px solid var(--hds-danger-border)' : `1px solid ${m.border}`,
            boxShadow: conflict ? '0 0 0 1px var(--hds-danger-text)' : shadow,
          padding: '7px 9px',
          overflow: 'hidden',
          cursor: 'pointer',
          outline,
          outlineOffset: 1,
          filter,
          transition: 'filter .12s',
        }

        return (
          <div
            key={block.block_id}
            style={boxStyle}
            // boxStyle's own position: absolute already establishes the
            // positioning context hds-shimmer-ring's ::after needs (any
            // non-static position works, not just relative).
            className={shimmerGold ? 'hds-shimmer-ring hds-shimmer-ring--gold' : undefined}
            title={conflict ? `Overlaps another priority ${block.priority} block on this day — same-priority overlap has no defined winner. Change one block's priority or time.` : undefined}
            onClick={() => store.bulkMode ? store.toggleBulkBlock(block.block_id) : store.select(block.block_id)}
            onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.filter = 'brightness(1.15) saturate(1)'}
            onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.filter = filter}
          >
            <div className={styles.headerRow}>
              <span className={styles.timeGroup}>
                <span className={styles.startTime}>
                  {block.start_time}
                </span>
                {block.late_start_mins > 0 && (
                  <span className={styles.lateStartBadge}>
                    ↧{block.late_start_mins}m
                  </span>
                )}
                  {conflict && (
                      <span className={styles.lateStartBadge}
                            style={{color: 'var(--hds-danger-text)', background: 'var(--hds-danger-border)'}}>
                    ⚠ overlap
                  </span>
                )}
              </span>
              {!store.bulkMode && (
                <button
                  onClick={e => { e.stopPropagation(); store.select(block.block_id); store.duplicate(channelId) }}
                  title="Duplicate"
                  className={styles.duplicateBtn}
                >⧉</button>
              )}
            </div>
            <div className={styles.firstName}>
              {firstName}
            </div>
            {showMeta && (
              <div className={styles.metaRow} style={{ color: m.edge }}>
                <span className={styles.metaDot} style={{ background: m.edge }} />
                {metaLabel}
              </div>
            )}
          </div>
        )
      })}
    </div>
  )
})

export default DayColumn
