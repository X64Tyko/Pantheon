import { useState } from 'react'
import type { Channel, EpgProgram } from '../api/types'
import { channelLogoUrl } from '../api/client'
import { COLUMN_WIDTH, PX_PER_MIN, HEADER_HEIGHT } from './constants'

interface ChannelColumnProps {
  channel:       Channel
  programs:      EpgProgram[]
  windowStartMs: number
  windowMs:      number
  nowMs:         number
  focused:       boolean
  onFocus:       () => void // hover — switches the live preview
  onWatch:       () => void // click — starts full playback
}

export function ChannelColumn({ channel, programs, windowStartMs, windowMs, nowMs, focused, onFocus, onWatch }: ChannelColumnProps) {
  const [logoErr, setLogoErr] = useState(false)

  return (
    <div
      style={{ width: COLUMN_WIDTH, flexShrink: 0, scrollSnapAlign: 'start', borderRight: '1px solid var(--hds-line-s)', cursor: 'pointer' }}
      onMouseEnter={onFocus}
      onClick={onWatch}
    >
      <div style={{
        position: 'sticky', top: 0, zIndex: 2, height: HEADER_HEIGHT,
        display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 4,
        background: focused ? 'var(--hds-bg-3)' : 'var(--hds-bg-2)',
        borderBottom: `1px solid ${focused ? 'var(--hds-violet)' : 'var(--hds-line-s)'}`,
        cursor: 'pointer', transition: 'background .12s, border-color .12s',
      }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 18, fontWeight: 800, color: focused ? 'var(--hds-gold)' : 'var(--hds-txt-2)' }}>
          {channel.number}
        </span>
        {channel.logo_path && !logoErr ? (
          <img
            src={channelLogoUrl(channel.channel_id)} alt={channel.name} onError={() => setLogoErr(true)}
            style={{ height: 20, maxWidth: 120, objectFit: 'contain' }}
          />
        ) : (
          <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>{channel.name}</span>
        )}
      </div>

      <div style={{ position: 'relative', height: windowMs / 60000 * PX_PER_MIN }}>
        {programs.map(p => (
          <ProgramBlock key={`${p.item_type}:${p.item_id}:${p.wall_clock_start_ms}`} program={p} windowStartMs={windowStartMs} nowMs={nowMs} />
        ))}
      </div>
    </div>
  )
}

function ProgramBlock({ program, windowStartMs, nowMs }: { program: EpgProgram; windowStartMs: number; nowMs: number }) {
  const top    = (program.wall_clock_start_ms - windowStartMs) / 60000 * PX_PER_MIN
  const height = Math.max(18, (program.wall_clock_end_ms - program.wall_clock_start_ms) / 60000 * PX_PER_MIN)

  const isPast   = program.wall_clock_end_ms   <= nowMs
  const isFuture = program.wall_clock_start_ms >  nowMs
  const isNow    = !isPast && !isFuture

  const label = program.item_type === 'episode' && program.season != null && program.episode_num != null
    ? `${program.show_title ?? program.title} · S${String(program.season).padStart(2, '0')}E${String(program.episode_num).padStart(2, '0')}`
    : program.title

  return (
    <div
      title={label}
      className={isNow ? 'hds-guide-now' : undefined}
      style={{
        position: 'absolute', top, left: 2, right: 2, height: height - 2,
        borderRadius: 5, padding: '4px 6px', overflow: 'hidden', boxSizing: 'border-box',
        background: isNow ? undefined : isPast ? 'var(--hds-bg-3)' : 'var(--hds-bg-4)',
        opacity: isPast ? 0.55 : 1,
        border: '1px solid var(--hds-line-s)',
      }}
    >
      <div style={{
        fontFamily: "'JetBrains Mono', monospace", fontSize: 10, lineHeight: 1.3,
        color: isNow ? 'oklch(0.98 0.01 285)' : 'var(--hds-txt-2)',
        overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
      }}>{label}</div>
    </div>
  )
}
