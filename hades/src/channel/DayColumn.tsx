import { observer } from 'mobx-react-lite'
import type { CSSProperties } from 'react'
import type { Block } from '../api/types'
import { BLOCK_META, DAY_BITS, DAY_MIN_W } from './constants'
import { endOf, t2m } from './utils'
import type { ChannelDetailStore } from './store'

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
        const isTop   = block.priority === colMax
        const sel     = selectedId === block.block_id
        const filter  = isTop ? 'none' : `brightness(${(0.82 - depth * 0.1).toFixed(2)}) saturate(0.85)`
        const shadow  = block.priority > 1 ? '0 8px 22px -8px rgba(0,0,0,0.65)' : 'none'
        const outline = sel ? '2px solid var(--hds-gold)' : 'none'
        const firstName = block.content[0]?.title ?? m.name
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
            onClick={() => store.select(block.block_id)}
            onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.filter = 'brightness(1.15) saturate(1)'}
            onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.filter = filter}
          >
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 6 }}>
              <span style={{ display: 'flex', alignItems: 'center', gap: 5, minWidth: 0 }}>
                <span style={{ fontWeight: 700, fontSize: 11, color: 'var(--hds-txt)', letterSpacing: '0.03em', whiteSpace: 'nowrap' }}>
                  {block.start_time}
                </span>
                {block.late_start_mins > 0 && (
                  <span style={{ fontSize: 8.5, padding: '1px 4px', borderRadius: 4, background: 'oklch(0.98 0 0 / 0.14)', color: 'var(--hds-txt)', whiteSpace: 'nowrap' }}>
                    ↧{block.late_start_mins}m
                  </span>
                )}
              </span>
              <button
                onClick={e => { e.stopPropagation(); store.select(block.block_id); store.duplicate(channelId) }}
                title="Duplicate"
                style={{ width: 18, height: 18, border: 'none', borderRadius: 5, background: 'oklch(0.98 0 0 / 0.1)', color: 'var(--hds-txt)', cursor: 'pointer', fontSize: 10, lineHeight: 1, padding: 0, opacity: 0.65, flexShrink: 0 }}
              >⧉</button>
            </div>
            <div style={{ fontSize: 12, fontWeight: 600, color: 'var(--hds-txt)', marginTop: 3, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
              {firstName}
            </div>
            {showMeta && (
              <div style={{ display: 'inline-flex', alignItems: 'center', gap: 5, marginTop: 6, fontSize: 9.5, letterSpacing: '0.08em', color: m.edge }}>
                <span style={{ width: 6, height: 6, borderRadius: 2, background: m.edge, flexShrink: 0 }} />
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
