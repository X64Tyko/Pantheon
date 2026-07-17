import { observer } from 'mobx-react-lite'
import type { CSSProperties } from 'react'
import type { Block } from '../api/types'
import { BLOCK_META, DAY_BITS, DAY_MIN_W } from './constants'
import { endOf, t2m } from './utils'
import type { ChannelDetailStore } from './store'
import styles from './DayColumn.module.css'

const DayColumn = observer(function DayColumn({ dayIdx, blocks, pph, selectedId, store, channelId }: {
  dayIdx:     number
  blocks:     Block[]
  pph:        number
  selectedId: string | null
  store:      ChannelDetailStore
  channelId:  string
}) {
  const bit       = DAY_BITS[dayIdx]
  const colBlocks = blocks.filter(b => (b.day_mask & bit) !== 0)
  const colMax    = Math.max(0, ...colBlocks.map(b => b.priority))
  const gridH     = 24 * pph
  const lineBg    = `repeating-linear-gradient(to bottom, transparent 0px, transparent ${pph - 1}px, var(--hds-line-s) ${pph - 1}px, var(--hds-line-s) ${pph}px)`

  return (
    <div style={{ flex: `1 0 ${DAY_MIN_W}px`, position: 'relative', height: gridH, borderLeft: '1px solid var(--hds-line-s)', background: lineBg }}>
      {colBlocks.slice().sort((a, b) => a.priority - b.priority).map(block => {
        const m       = BLOCK_META[block.block_type]
        const start   = t2m(block.start_time)
        const end     = endOf(block)
        const limitM  = block.end_time ? 'end' : block.program_count > 0 ? 'programs' : 'fill'
        const top     = Math.round((start / 60) * pph)
        const height  = Math.max(Math.round(((end - start) / 60) * pph), 30)
        const left    = Math.min((block.priority - 1) * 11, 33)
        const depth   = colMax > 1 ? colMax - block.priority : 0
        const isTop        = block.priority === colMax
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
          zIndex: block.priority * 2 + (sel ? 40 : 0),
          background: m.bg,
          borderRadius: limitM === 'programs' ? '7px 7px 0 0' : 7,
          border: `1px solid ${m.border}`,
          borderLeft: `3px solid ${m.edge}`,
          borderBottom: limitM === 'programs' ? `2px dashed ${m.edge}` : `1px solid ${m.border}`,
          boxShadow: shadow,
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
