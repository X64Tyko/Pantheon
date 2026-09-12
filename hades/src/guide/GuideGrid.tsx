import {useEffect, useRef, type CSSProperties} from 'react'
import type { Channel, EpgProgram } from '../api/types'
import {ChannelHeader, ChannelColumn} from './ChannelColumn'
import { PX_PER_MIN, WINDOW_LOOKBACK_MIN, WINDOW_FORWARD_HOURS } from './constants'
import styles from './GuideGrid.module.css'

const THIRTY_MIN_MS = 30 * 60_000

interface GuideGridProps {
  channels:         Channel[]
  epgByChannel:     Record<string, EpgProgram[]>
  windowStartMs:    number
  nowMs:            number
  focusedChannelId: string | null
    onFocusChannel: (channelId: string) => void
    onFocusProgram: (channelId: string, program: EpgProgram) => void
  onWatch:          (channelId: string) => void
}

export function GuideGrid({
                              channels,
                              epgByChannel,
                              windowStartMs,
                              nowMs,
                              focusedChannelId,
                              onFocusChannel,
                              onFocusProgram,
                              onWatch
                          }: GuideGridProps) {
    // Header row and body scroll independently in the DOM (see the class
    // comment on why — sharing one scroll axis with the body is exactly what
    // let program blocks render underneath the header before), synced
    // horizontally by hand: the classic "two scrollers, one drives the other"
    // technique, no library needed for it.
    const headerRef = useRef<HTMLDivElement>(null)
    const bodyRef = useRef<HTMLDivElement>(null)
  const scrolledToNow = useRef(false)
  const windowMs = (WINDOW_LOOKBACK_MIN * 60_000) + (WINDOW_FORWARD_HOURS * 3_600_000)

  useEffect(() => {
      if (scrolledToNow.current || !bodyRef.current) return
    scrolledToNow.current = true
    const nowOffsetPx = (nowMs - windowStartMs) / 60000 * PX_PER_MIN
      // No header-clearance math needed anymore — the header lives outside
      // this scroll region entirely now, so it can never cover whatever lands
      // at the top. This is just a little breathing room above "now."
      bodyRef.current.scrollTop = Math.max(0, nowOffsetPx - 40)
  }, [nowMs, windowStartMs])

    const syncHeaderScroll = () => {
        if (headerRef.current && bodyRef.current) headerRef.current.scrollLeft = bodyRef.current.scrollLeft
    }

    // Background time-grid alignment (see GuideGrid.module.css's
    // .scrollContainer) — lines every 30min, but windowStartMs itself is
    // "now minus a lookback," essentially never sitting exactly on a real
    // half-hour boundary. This is how far *into* the current half-hour
    // windowStartMs already is, negated so the repeating pattern's first line
    // lands on the true :00/:30 mark instead of an arbitrary offset from y=0.
    const gridOffsetPx = -((windowStartMs % THIRTY_MIN_MS) / 60000 * PX_PER_MIN)

  return (
      <div className={styles.root}>
          <div ref={headerRef} className={styles.headerRow}>
              <div className={styles.headerRowInner}>
                  {channels.map(ch => (
                      <ChannelHeader
                          key={ch.channel_id}
                          channel={ch}
                          focused={ch.channel_id === focusedChannelId}
                          onFocus={() => onFocusChannel(ch.channel_id)}
                          onWatch={() => onWatch(ch.channel_id)}
                      />
                  ))}
              </div>
          </div>
          <div
              ref={bodyRef}
              className={styles.scrollContainer}
              onScroll={syncHeaderScroll}
              style={{'--guide-grid-offset': `${gridOffsetPx}px`} as CSSProperties}
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
                          onFocus={program => onFocusProgram(ch.channel_id, program)}
                          onWatch={() => onWatch(ch.channel_id)}
                      />
                  ))}
              </div>
      </div>
    </div>
  )
}
