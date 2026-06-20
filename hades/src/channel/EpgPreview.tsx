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

// Returns 'YYYY-MM-DD' for a timestamp in the given timezone.
function localDateStr(ms: number, tz: string): string {
  try {
    const p = new Intl.DateTimeFormat('en-US', {
      timeZone: tz || 'UTC', year: 'numeric', month: '2-digit', day: '2-digit',
    }).formatToParts(new Date(ms))
    const yr = p.find(x => x.type === 'year')?.value  ?? '2000'
    const mo = p.find(x => x.type === 'month')?.value ?? '01'
    const dy = p.find(x => x.type === 'day')?.value   ?? '01'
    return `${yr}-${mo.padStart(2,'0')}-${dy.padStart(2,'0')}`
  } catch {
    return new Date(ms).toISOString().slice(0, 10)
  }
}

// Merge consecutive filler items into a single visual segment.
function mergeFiller(items: EpgProgram[]): EpgProgram[] {
  const out: EpgProgram[] = []
  for (const item of items) {
    const last = out[out.length - 1]
    if (item.item_type === 'filler' && last?.item_type === 'filler') {
      out[out.length - 1] = { ...last, wall_clock_end_ms: item.wall_clock_end_ms }
    } else {
      out.push(item)
    }
  }
  return out
}

function fmtLocal(ms: number, tz: string): string {
  try {
    return new Intl.DateTimeFormat('en-US', {
      timeZone: tz, year: 'numeric', month: '2-digit', day: '2-digit',
      hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
    }).format(new Date(ms))
  } catch {
    return new Date(ms).toISOString()
  }
}

function exportEpg(items: EpgProgram[], tz: string) {
  const rows = items.map(item => ({
    start_ms:    item.wall_clock_start_ms,
    end_ms:      item.wall_clock_end_ms,
    start_local: fmtLocal(item.wall_clock_start_ms, tz),
    end_local:   fmtLocal(item.wall_clock_end_ms,   tz),
    timezone:    tz,
    block_id:    item.block_id,
    item_type:   item.item_type,
    status:      item.status,
    title:       item.title,
    show_title:  item.show_title,
    season:      item.season,
    episode_num: item.episode_num,
  }))
  const blob = new Blob([JSON.stringify(rows, null, 2)], { type: 'application/json' })
  const url  = URL.createObjectURL(blob)
  const a    = document.createElement('a')
  a.href     = url
  a.download = `epg-export-${new Date().toISOString().slice(0, 16).replace('T', '_')}.json`
  a.click()
  URL.revokeObjectURL(url)
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
  const [epgWeek, setEpgWeek] = useState<0 | 1>(0)
  const zoomIdx    = EPG_ZOOM_LEVELS.indexOf(zoom as typeof EPG_ZOOM_LEVELS[number])
  const canZoomIn  = zoomIdx < EPG_ZOOM_LEVELS.length - 1
  const canZoomOut = zoomIdx > 0

  // Group unique dates from the data into weeks (up to 2).
  const allDates = [...new Set(epgItems.map(i => localDateStr(i.wall_clock_start_ms, tz)))].sort()
  const week0Dates = allDates.slice(0, 7)
  const week1Dates = allDates.slice(7, 14)
  const hasWeek2   = week1Dates.length > 0
  const weekDates  = (epgWeek === 1 && hasWeek2) ? week1Dates : week0Dates

  // Find the specific date in the selected week matching the current day-of-week tab.
  // Use UTC noon to avoid DST ambiguity when mapping dates to DOW.
  const targetDOW_js = [1, 2, 3, 4, 5, 6, 0][epgDay] // JS DOW: 0=Sun 1=Mon…6=Sat
  const targetDate   = weekDates.find(d =>
    getDayInTZ(Date.parse(d + 'T12:00:00Z'), tz) === targetDOW_js
  ) ?? null

  const dayItems  = targetDate
    ? mergeFiller(epgItems.filter(i => localDateStr(i.wall_clock_start_ms, tz) === targetDate))
    : []
  const hasEpg    = dayItems.length > 0
  const hasCached = hasEpg && dayItems.some(i => i.status === 'aired' || i.status === 'scheduled')

  const segs: EpgSeg[] = hasEpg
    ? dayItems.flatMap(item => {
        let startMins = msToTzMins(item.wall_clock_start_ms, tz)
        let endMins   = msToTzMins(item.wall_clock_end_ms,   tz)
        // Items that cross midnight have endMins < startMins; cap them at the day boundary.
        // The `< 60` guard was too narrow — it silently dropped items ending after 1 AM.
        if (endMins <= startMins) endMins = 1440
        endMins = Math.min(endMins, 1440)
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

  const dateLabel  = targetDate ? ` · ${targetDate.slice(5).replace('-', '/')}` : ''
  const statusText = epgLoading
    ? 'loading…'
    : hasEpg
      ? `${hasCached ? 'live' : 'simulated'} · ${dayItems.length} items${dateLabel}`
      : `block coverage · no scheduled programs${dateLabel}`

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
        <button onClick={() => exportEpg(epgItems, tz)} disabled={epgItems.length === 0} title="Export EPG data as JSON" style={{ ...btnBase, opacity: epgItems.length === 0 ? 0.35 : 1, cursor: epgItems.length === 0 ? 'default' : 'pointer' }}>↓ JSON</button>

        {hasWeek2 && (
          <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 8, padding: 3, flexShrink: 0 }}>
            {(['Wk 1', 'Wk 2'] as const).map((label, wi) => (
              <button key={wi} onClick={() => setEpgWeek(wi as 0 | 1)} style={{ padding: '5px 8px', border: 'none', borderRadius: 6, background: epgWeek === wi ? 'oklch(0.38 0.09 287)' : 'transparent', color: epgWeek === wi ? 'var(--hds-txt)' : 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, letterSpacing: '0.06em', cursor: 'pointer' }}>
                {label}
              </button>
            ))}
          </div>
        )}

        <div style={{ display: 'flex', gap: 3, background: 'var(--hds-bg-3)', borderRadius: 8, padding: 3, flexShrink: 0 }}>
          {DAYS.map(([short], i) => {
            const active  = epgDay === i
            const dowJs   = [1, 2, 3, 4, 5, 6, 0][i]
            const hasData = weekDates.some(d => getDayInTZ(Date.parse(d + 'T12:00:00Z'), tz) === dowJs)
            return (
              <button key={short} onClick={() => onDay(i)} disabled={!hasData} style={{ padding: '5px 9px', border: 'none', borderRadius: 6, background: active ? 'var(--hds-gold)' : 'transparent', color: active ? 'oklch(0.2 0.04 70)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, letterSpacing: '0.1em', cursor: hasData ? 'pointer' : 'default', opacity: hasData ? 1 : 0.3 }}>
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
