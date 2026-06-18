import { Component, useState } from 'react'
import type { ReactNode } from 'react'
import type { Block, EpgProgram } from '../api/types'
import { BLOCK_META, DAY_BITS, DAYS } from './constants'
import { endOf, m2t, t2m } from './utils'

// ─── Types ────────────────────────────────────────────────────────────────────

interface EpgSeg {
  leftPct:   number
  widthPct:  number
  bg:        string
  time:      string
  title:     string
  subtitle?: string
  epLabel?:  string
  blockId?:  string
  faded?:    boolean
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

export function computeEpg(blocks: Block[], dayIdx: number): EpgSeg[] {
  const bit       = DAY_BITS[dayIdx]
  const dayBlocks = blocks.filter(b => (b.day_mask & bit) !== 0)

  function winnerAt(mn: number): Block | null {
    let w: Block | null = null
    for (const b of dayBlocks) {
      const s = t2m(b.start_time), e = endOf(b)
      if (mn >= s && mn < e && (!w || b.priority > w.priority)) w = b
    }
    return w
  }

  const segs: EpgSeg[] = []
  let curId = '∅', segStart = 0
  for (let mn = 0; mn <= 1440; mn++) {
    const w  = mn < 1440 ? winnerAt(mn) : null
    const id = w ? w.block_id : '∅'
    if (mn === 0) { curId = id; segStart = 0; continue }
    if (id !== curId || mn === 1440) {
      const b = dayBlocks.find(x => x.block_id === curId)
      const m = b ? BLOCK_META[b.block_type] : null
      segs.push({
        leftPct:  segStart / 1440 * 100,
        widthPct: (mn - segStart) / 1440 * 100,
        bg:       b ? m!.bg : 'oklch(0.18 0.012 286)',
        time:     m2t(segStart),
        title:    b ? (b.content[0]?.title ?? m!.name) : '— idle —',
        blockId:  b?.block_id,
        faded:    !b,
      })
      curId = id; segStart = mn
    }
  }
  return segs
}

function msToTzMins(ms: number, tz: string): number {
  try {
    const d = new Date(ms)
    if (isNaN(d.getTime())) return 0
    const safeTz = tz || 'UTC'
    const parts = new Intl.DateTimeFormat('en-US', {
      timeZone: safeTz, hour: 'numeric', minute: 'numeric', hour12: false,
    }).formatToParts(d)
    const h = parseInt(parts.find(p => p.type === 'hour')?.value   ?? '0', 10) % 24
    const m = parseInt(parts.find(p => p.type === 'minute')?.value ?? '0', 10)
    return h * 60 + m
  } catch {
    const d = new Date(ms)
    return d.getUTCHours() * 60 + d.getUTCMinutes()
  }
}

function getDayInTZ(ms: number, tz: string): number {
  try {
    const d = new Date(ms)
    if (isNaN(d.getTime())) return d.getUTCDay()
    const safeTz = tz || 'UTC'
    const p = new Intl.DateTimeFormat('en-US', {
      timeZone: safeTz, year: 'numeric', month: '2-digit', day: '2-digit',
    }).formatToParts(d)
    const yr = parseInt(p.find(x => x.type === 'year')?.value  ?? '2000', 10)
    const mo = parseInt(p.find(x => x.type === 'month')?.value ?? '1',    10) - 1
    const dy = parseInt(p.find(x => x.type === 'day')?.value   ?? '1',    10)
    return new Date(yr, mo, dy).getDay()
  } catch {
    return new Date(ms).getUTCDay()
  }
}

// ─── Error boundary ───────────────────────────────────────────────────────────

export class EpgErrorBoundary extends Component<{ children: ReactNode }, { error: string | null }> {
  state = { error: null }
  static getDerivedStateFromError(e: unknown) { return { error: String(e) } }
  render() {
    if (this.state.error) return (
      <div style={{ padding: '12px 24px', fontSize: 11, color: 'oklch(0.65 0.12 22)', borderTop: '1px solid var(--hds-line-s)' }}>
        EPG preview error: {this.state.error}
      </div>
    )
    return this.props.children
  }
}

// ─── Component ────────────────────────────────────────────────────────────────

const EPG_ZOOM_LEVELS = [1, 2, 3, 6] as const

export default function EpgPreview({ blocks, epgItems, epgLoading, epgDay, timezone, onDay, onRefresh, onSelectBlock }: {
  blocks:        Block[]
  epgItems:      EpgProgram[]
  epgLoading:    boolean
  epgDay:        number
  timezone:      string
  onDay:         (d: number) => void
  onRefresh:     () => void
  onSelectBlock: (blockId: string) => void
}) {
  const tz = timezone || 'UTC'

  const [zoom, setZoom] = useState<number>(1)
  const zoomIdx    = EPG_ZOOM_LEVELS.indexOf(zoom as typeof EPG_ZOOM_LEVELS[number])
  const canZoomIn  = zoomIdx < EPG_ZOOM_LEVELS.length - 1
  const canZoomOut = zoomIdx > 0

  const targetDOW = [1, 2, 3, 4, 5, 6, 0][epgDay]
  const dayItems  = epgItems.filter(item => getDayInTZ(item.wall_clock_start_ms, tz) === targetDOW)
  const hasEpg    = dayItems.length > 0
  const hasCached = hasEpg && dayItems.some(i => i.status === 'aired' || i.status === 'scheduled')

  const segs: EpgSeg[] = hasEpg
    ? dayItems.flatMap(item => {
        let startMins = msToTzMins(item.wall_clock_start_ms, tz)
        let endMins   = msToTzMins(item.wall_clock_end_ms,   tz)
        if (endMins <= startMins && endMins < 60) endMins = 1440
        endMins = Math.min(endMins, 1440)
        if (endMins <= startMins) return []
        const isFiller = item.item_type === 'filler'
        const block    = blocks.find(b => b.block_id === item.block_id)
        const meta     = isFiller ? BLOCK_META.filler : (block ? BLOCK_META[block.block_type] : BLOCK_META.episode)
        const isEp     = item.item_type === 'episode' && item.show_title
        const epLabel  = isEp && item.season != null && item.episode_num != null
          ? `S${String(item.season).padStart(2, '0')}E${String(item.episode_num).padStart(2, '0')}`
          : undefined
        return [{
          leftPct:  startMins / 1440 * 100,
          widthPct: Math.max((endMins - startMins) / 1440 * 100, 0.25),
          bg:       meta.bg,
          time:     m2t(startMins),
          title:    isEp ? item.show_title! : isFiller ? 'Filler' : item.title,
          subtitle: isEp ? item.title : undefined,
          epLabel,
          blockId:  item.block_id || undefined,
          faded:    isFiller,
        }]
      })
    : computeEpg(blocks, epgDay)

  const statusText = epgLoading
    ? 'loading…'
    : hasEpg
      ? `${hasCached ? 'live' : 'simulated'} · ${dayItems.length} programs`
      : 'block coverage · no scheduled programs'

  const btnBase: React.CSSProperties = { padding: '3px 7px', border: '1px solid var(--hds-line)', borderRadius: 5, background: 'transparent', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer', color: 'var(--hds-txt-2)' }

  return (
    <div style={{ flexShrink: 0, borderTop: '1px solid var(--hds-line-s)', background: 'oklch(0.17 0.018 286 / 0.7)', padding: '12px 24px 16px' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, marginBottom: 10 }}>
        <span style={{ fontSize: 10, letterSpacing: '0.22em', color: 'var(--hds-txt-2)', whiteSpace: 'nowrap' }}>EPG PREVIEW</span>
        <span style={{ flex: 1, fontSize: 10, color: hasCached ? 'oklch(0.62 0.1 145)' : hasEpg ? 'oklch(0.62 0.1 220)' : 'var(--hds-txt-3)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
          {statusText}
        </span>

        <div style={{ display: 'flex', alignItems: 'center', gap: 2, flexShrink: 0 }}>
          <button onClick={() => canZoomOut && setZoom(EPG_ZOOM_LEVELS[zoomIdx - 1])} disabled={!canZoomOut} title="Zoom out" style={{ ...btnBase, opacity: canZoomOut ? 1 : 0.35, cursor: canZoomOut ? 'pointer' : 'default' }}>−</button>
          <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', width: 24, textAlign: 'center', letterSpacing: '0.05em' }}>{zoom}×</span>
          <button onClick={() => canZoomIn  && setZoom(EPG_ZOOM_LEVELS[zoomIdx + 1])} disabled={!canZoomIn}  title="Zoom in"  style={{ ...btnBase, opacity: canZoomIn  ? 1 : 0.35, cursor: canZoomIn  ? 'pointer' : 'default' }}>+</button>
        </div>

        <button onClick={onRefresh} title="Refresh EPG" style={{ ...btnBase, color: epgLoading ? 'var(--hds-txt-3)' : 'var(--hds-txt-2)', cursor: epgLoading ? 'default' : 'pointer', opacity: epgLoading ? 0.5 : 1 }}>↺</button>
        <div style={{ display: 'flex', gap: 3, background: 'var(--hds-bg-3)', borderRadius: 8, padding: 3, flexShrink: 0 }}>
          {DAYS.map(([short], i) => {
            const active = epgDay === i
            return (
              <button key={short} onClick={() => onDay(i)} style={{ padding: '5px 9px', border: 'none', borderRadius: 6, background: active ? 'var(--hds-gold)' : 'transparent', color: active ? 'oklch(0.2 0.04 70)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, letterSpacing: '0.1em', cursor: 'pointer' }}>
                {short}
              </button>
            )
          })}
        </div>
      </div>

      <div style={{ borderRadius: 8, border: '1px solid var(--hds-line-s)', overflowX: zoom > 1 ? 'auto' : 'hidden', overflowY: 'hidden' }}>
        <div style={{ position: 'relative', height: 68, width: `${zoom * 100}%`, minWidth: '100%', background: 'oklch(0.13 0.014 286)' }}>
          {segs.map((s, i) => {
            const clickable = !!s.blockId && !s.faded
            const tooltip   = [s.time, s.title, s.subtitle, s.epLabel].filter(Boolean).join('  ·  ')
            return (
              <div
                key={i}
                title={tooltip}
                onClick={() => s.blockId && onSelectBlock(s.blockId)}
                style={{
                  position: 'absolute', top: 0, bottom: 0,
                  left: `${s.leftPct}%`, width: `${s.widthPct}%`,
                  background: s.bg,
                  borderLeft: '1px solid oklch(0.13 0.014 286)',
                  padding: '7px 8px',
                  overflow: 'hidden',
                  cursor: clickable ? 'pointer' : 'default',
                  opacity: s.faded ? 0.5 : 1,
                  transition: 'filter .1s',
                }}
                onMouseEnter={e => { if (clickable) (e.currentTarget as HTMLDivElement).style.filter = 'brightness(1.2)' }}
                onMouseLeave={e => { if (clickable) (e.currentTarget as HTMLDivElement).style.filter = '' }}
              >
                <div style={{ fontSize: 9, color: s.faded ? 'var(--hds-txt-3)' : 'oklch(0.78 0.04 286)', letterSpacing: '0.06em', lineHeight: 1 }}>
                  {s.time}
                </div>
                <div style={{ fontSize: 11, fontWeight: 700, color: s.faded ? 'var(--hds-txt-3)' : 'var(--hds-txt)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', marginTop: 3, lineHeight: 1.2 }}>
                  {s.title}
                </div>
                {(s.subtitle || s.epLabel) && (
                  <div style={{ display: 'flex', alignItems: 'center', gap: 5, marginTop: 3, minWidth: 0 }}>
                    {s.epLabel && (
                      <span style={{ flexShrink: 0, fontSize: 8.5, letterSpacing: '0.06em', padding: '1px 4px', borderRadius: 3, background: 'oklch(0.98 0 0 / 0.13)', color: 'oklch(0.82 0.05 286)' }}>
                        {s.epLabel}
                      </span>
                    )}
                    {s.subtitle && (
                      <span style={{ fontSize: 10, color: 'var(--hds-txt-2)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
                        {s.subtitle}
                      </span>
                    )}
                  </div>
                )}
              </div>
            )
          })}
          {segs.length === 0 && !epgLoading && (
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100%', color: 'var(--hds-txt-3)', fontSize: 11 }}>No programs scheduled for this day</div>
          )}
          {epgLoading && (
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100%', color: 'var(--hds-txt-3)', fontSize: 11 }}>Loading…</div>
          )}
        </div>
      </div>
    </div>
  )
}
