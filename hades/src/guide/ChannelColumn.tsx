import { useState } from 'react'
import type { Channel, EpgProgram } from '../api/types'
import { channelLogoUrl } from '../api/client'
import { PX_PER_MIN } from './constants'
import { useFocusable } from '../nav/useFocusable'
import styles from './ChannelColumn.module.css'

interface ChannelColumnProps {
  channel:       Channel
  programs:      EpgProgram[]
  windowStartMs: number
  windowMs:      number
  nowMs:         number
  focused:       boolean
  onFocus:       () => void // hover/keyboard focus — switches the live preview
  onWatch:       () => void // click/select — starts full playback
}

function programNodeId(channelId: string, p: EpgProgram): string {
  return `guide-prog-${channelId}-${p.item_type}-${p.item_id}-${p.wall_clock_start_ms}`
}

export function ChannelColumn({ channel, programs, windowStartMs, windowMs, nowMs, focused, onFocus, onWatch }: ChannelColumnProps) {
  const [logoErr, setLogoErr] = useState(false)

  const { ref: headerRef, focused: headerNavFocused } = useFocusable<object, HTMLDivElement>({
    focusKey: `guide-col-header-${channel.channel_id}`,
    onEnterPress: onWatch,
    onFocus,
  })

  return (
    <div
      className={styles.column}
      onMouseEnter={onFocus}
      onClick={onWatch}
    >
      <div
        ref={headerRef}
        data-tv-focused={headerNavFocused}
        className={`${styles.header} ${focused ? styles.headerFocused : ''}`}>
        <span className={`${styles.channelNumber} ${focused ? styles.channelNumberFocused : ''}`}>
          {channel.number}
        </span>
        {channel.logo_path && !logoErr ? (
          <img
            src={channelLogoUrl(channel.channel_id)} alt={channel.name} onError={() => setLogoErr(true)}
            className={styles.channelLogo}
          />
        ) : (
          <span className={styles.channelNameFallback}>{channel.name}</span>
        )}
      </div>

      <div className={styles.programsWrap} style={{ height: windowMs / 60000 * PX_PER_MIN }}>
        {programs.map(p => (
          <ProgramBlock
            key={`${p.item_type}:${p.item_id}:${p.wall_clock_start_ms}`}
            program={p} windowStartMs={windowStartMs} nowMs={nowMs}
            channelId={channel.channel_id}
            onFocus={onFocus} onWatch={onWatch}
          />
        ))}
      </div>
    </div>
  )
}

function ProgramBlock({ program, windowStartMs, nowMs, channelId, onFocus, onWatch }: {
  program: EpgProgram; windowStartMs: number; nowMs: number
  channelId: string; onFocus: () => void; onWatch: () => void
}) {
  const top    = (program.wall_clock_start_ms - windowStartMs) / 60000 * PX_PER_MIN
  const height = Math.max(18, (program.wall_clock_end_ms - program.wall_clock_start_ms) / 60000 * PX_PER_MIN)

  const isPast   = program.wall_clock_end_ms   <= nowMs
  const isFuture = program.wall_clock_start_ms >  nowMs
  const isNow    = !isPast && !isFuture

  const label = program.item_type === 'episode' && program.season != null && program.episode_num != null
    ? `${program.show_title ?? program.title} · S${String(program.season).padStart(2, '0')}E${String(program.episode_num).padStart(2, '0')}`
    : program.title

  // Left/right/up/down all resolve via the library's own nearest-neighbor
  // grid algorithm — blocks are already time-aligned across columns (same
  // windowStartMs/PX_PER_MIN) and stacked vertically within a column.
  const { ref, focused: navFocused } = useFocusable<object, HTMLDivElement>({
    focusKey: programNodeId(channelId, program),
    onEnterPress: onWatch,
    onFocus,
  })

  return (
    <div
      ref={ref}
      data-tv-focused={navFocused}
      title={label}
      className={`${styles.programBlock} ${isNow ? 'hds-guide-now' : isPast ? styles.programBlockPast : styles.programBlockFuture}`}
      style={{ top, left: 2, right: 2, height: height - 2 }}
    >
      <div className={`${styles.programLabel} ${isNow ? styles.programLabelNow : styles.programLabelDefault}`}>{label}</div>
    </div>
  )
}
