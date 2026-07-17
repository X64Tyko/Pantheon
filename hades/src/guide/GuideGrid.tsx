import { useEffect, useRef } from 'react'
import type { Channel, EpgProgram } from '../api/types'
import { ChannelColumn } from './ChannelColumn'
import { PX_PER_MIN, WINDOW_LOOKBACK_MIN, WINDOW_FORWARD_HOURS } from './constants'
import styles from './GuideGrid.module.css'

interface GuideGridProps {
  channels:         Channel[]
  epgByChannel:     Record<string, EpgProgram[]>
  windowStartMs:    number
  nowMs:            number
  focusedChannelId: string | null
  onFocus:          (channelId: string) => void
  onWatch:          (channelId: string) => void
}

export function GuideGrid({ channels, epgByChannel, windowStartMs, nowMs, focusedChannelId, onFocus, onWatch }: GuideGridProps) {
  const containerRef = useRef<HTMLDivElement>(null)
  const scrolledToNow = useRef(false)
  const windowMs = (WINDOW_LOOKBACK_MIN * 60_000) + (WINDOW_FORWARD_HOURS * 3_600_000)

  useEffect(() => {
    if (scrolledToNow.current || !containerRef.current) return
    scrolledToNow.current = true
    const nowOffsetPx = (nowMs - windowStartMs) / 60000 * PX_PER_MIN
    containerRef.current.scrollTop = Math.max(0, nowOffsetPx - 60)
  }, [nowMs, windowStartMs])

  return (
    <div
      ref={containerRef}
      className={styles.scrollContainer}
    >
      <div className={styles.columnsRow}>
        {channels.map(ch => (
          <ChannelColumn
            key={ch.channel_id}
            channel={ch}
            programs={epgByChannel[ch.channel_id] ?? []}
            windowStartMs={windowStartMs}
            windowMs={windowMs}
            nowMs={nowMs}
            focused={ch.channel_id === focusedChannelId}
            onFocus={() => onFocus(ch.channel_id)}
            onWatch={() => onWatch(ch.channel_id)}
          />
        ))}
      </div>
    </div>
  )
}
