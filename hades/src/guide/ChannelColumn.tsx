import { useState } from 'react'
import type { Channel, EpgProgram } from '../api/types'
import { channelLogoUrl } from '../api/client'
import { PX_PER_MIN } from './constants'
import { useFocusable } from '../nav/useFocusable'
import styles from './ChannelColumn.module.css'

// The now-block's progress split — dark above where "now" falls within the
// block, lighter below. Same purple family as --hds-violet, just a wider
// light/dark spread than that single token gives.
const NOW_DARK = 'oklch(0.32 0.10 292)'
const NOW_LIGHT = 'oklch(0.58 0.12 288)'

function programNodeId(channelId: string, p: EpgProgram): string {
  return `guide-prog-${channelId}-${p.item_type}-${p.item_id}-${p.wall_clock_start_ms}`
}

// The channel identity strip — logo/number, horizontally-scrolled-only (see
// GuideGrid.tsx's class comment for why this is a separate component from
// the time-scrolling body below rather than a sticky element sharing its
// scroll axis).
export function ChannelHeader({channel, focused, onFocus, onWatch}: {
    channel: Channel; focused: boolean; onFocus: () => void; onWatch: () => void
}) {
  const [logoErr, setLogoErr] = useState(false)
    const {ref, focused: navFocused} = useFocusable<object, HTMLDivElement>({
    focusKey: `guide-col-header-${channel.channel_id}`,
    onEnterPress: onWatch,
    onFocus,
  })

  return (
    <div
        ref={ref} data-tv-focused={navFocused}
        onMouseEnter={onFocus} onClick={onWatch}
        className={`${styles.header} ${focused ? styles.headerFocused : ''}`}
    >
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
  )
}

interface ChannelColumnProps {
    channel: Channel
    programs: EpgProgram[]
    windowStartMs: number
    windowMs: number
    nowMs: number
    onFocus: (program: EpgProgram) => void // hover/keyboard focus of a specific cell — switches the live preview AND pins the hero's text to this program
    onWatch: () => void // click/select — starts full playback of the channel
}

export function ChannelColumn({
                                  channel,
                                  programs,
                                  windowStartMs,
                                  windowMs,
                                  nowMs,
                                  onFocus,
                                  onWatch
                              }: ChannelColumnProps) {
    return (
        <div
            className={styles.column}
            onClick={onWatch}
        >
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
    channelId: string; onFocus: (program: EpgProgram) => void; onWatch: () => void
}) {
  const top    = (program.wall_clock_start_ms - windowStartMs) / 60000 * PX_PER_MIN
  const height = Math.max(18, (program.wall_clock_end_ms - program.wall_clock_start_ms) / 60000 * PX_PER_MIN)

  const isPast   = program.wall_clock_end_ms   <= nowMs
  const isFuture = program.wall_clock_start_ms >  nowMs
  const isNow    = !isPast && !isFuture

    // How far into this specific program "now" falls, 0-1 — the actual
    // progress signal the old shimmer never had any connection to.
    const nowFraction = isNow
        ? Math.min(1, Math.max(0, (nowMs - program.wall_clock_start_ms) / (program.wall_clock_end_ms - program.wall_clock_start_ms)))
        : 0

  const label = program.item_type === 'episode' && program.season != null && program.episode_num != null
    ? `${program.show_title ?? program.title} · S${String(program.season).padStart(2, '0')}E${String(program.episode_num).padStart(2, '0')}`
    : program.title

  // Left/right/up/down all resolve via the library's own nearest-neighbor
  // grid algorithm — blocks are already time-aligned across columns (same
  // windowStartMs/PX_PER_MIN) and stacked vertically within a column.
  const { ref, focused: navFocused } = useFocusable<object, HTMLDivElement>({
    focusKey: programNodeId(channelId, program),
    onEnterPress: onWatch,
      onFocus: () => onFocus(program),
  })

  return (
    <div
      ref={ref}
      data-tv-focused={navFocused}
      title={label}
      onMouseEnter={() => onFocus(program)}
      className={`${styles.programBlock} ${isNow ? styles.programBlockNow : isPast ? styles.programBlockPast : styles.programBlockFuture}`}
      style={{
          top, left: 2, right: 2, height: height - 2,
          ...(isNow ? {background: `linear-gradient(to bottom, ${NOW_DARK} 0%, ${NOW_DARK} ${nowFraction * 100}%, ${NOW_LIGHT} ${nowFraction * 100}%, ${NOW_LIGHT} 100%)`} : {}),
      }}
    >
        {isNow && (
            <div className={`${styles.nowPulse} hds-guide-now-pulse`} style={{top: `${nowFraction * 100}%`}}/>
        )}
      <div className={`${styles.programLabel} ${isNow ? styles.programLabelNow : styles.programLabelDefault}`}>{label}</div>
    </div>
  )
}
