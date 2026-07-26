import { useEffect, useRef } from 'react'
import type { Channel, EpgProgram } from '../api/types'
import { VideoPlayer } from '../player/VideoPlayer'
import {goldBtnStyle} from '../channel/styles'
import styles from './GuidePreview.module.css'

interface GuidePreviewProps {
    channel: Channel | null
    // The program whose TEXT should be shown — either whatever's live right
    // now (nothing specific focused) or a specific cell (now or future) the
    // viewer focused/hovered. Independent of manifestUrl/the video itself,
    // which always tracks `channel` alone — see useGuideSession's own comment
    // on why (you can't actually preview a future program).
    previewProgram: EpgProgram | null
    nowMs: number
    manifestUrl: string | null
    onWatch: () => void
}

// Exported for direct unit testing (momus/hades/guide/GuidePreview.test.ts) —
// both are pure and easy to regress (e.g. fmtCountdown's 60-minute rollover).
export function fmtClock(ms: number): string {
    return new Date(ms).toLocaleTimeString('en-US', {hour: 'numeric', minute: '2-digit'})
}

export function fmtCountdown(ms: number): string {
    const mins = Math.round(ms / 60000)
    if (mins < 60) return `${mins}m`
    const h = Math.floor(mins / 60), m = mins % 60
    return m > 0 ? `${h}h ${m}m` : `${h}h`
}

export interface PreviewTiming {
    isLive: boolean
    // Only set when NOT live and the program is genuinely in the future —
    // you can't show a countdown for something already over.
    startsInMs: number | null
}

// A program is "live" iff wall_clock_start_ms <= nowMs < wall_clock_end_ms
// (start inclusive, end exclusive — a program ending exactly at nowMs is NOT
// live). Extracted from the component body so this boundary logic is
// directly unit-testable without rendering.
export function computePreviewTiming(program: EpgProgram | null, nowMs: number): PreviewTiming {
    const isLive = !!program && program.wall_clock_start_ms <= nowMs && nowMs < program.wall_clock_end_ms
    const startsInMs = program && !isLive && program.wall_clock_start_ms > nowMs
        ? program.wall_clock_start_ms - nowMs
        : null
    return {isLive, startsInMs}
}

export function GuidePreview({channel, previewProgram, nowMs, manifestUrl, onWatch}: GuidePreviewProps) {
  const videoRef = useRef<HTMLVideoElement>(null)

  useEffect(() => {
    if (videoRef.current) videoRef.current.muted = true
  }, [manifestUrl])

    // "we can remove it if we don't like it" — kept as its own small, isolated
    // bit of markup on purpose, easy to delete wholesale later.
    const {startsInMs} = computePreviewTiming(previewProgram, nowMs)

    const label = previewProgram && previewProgram.item_type === 'episode' && previewProgram.season != null && previewProgram.episode_num != null
        ? `S${String(previewProgram.season).padStart(2, '0')}E${String(previewProgram.episode_num).padStart(2, '0')}`
    : undefined

  return (
      <div className={styles.root}>
          <div className={styles.videoLayer}>
        {manifestUrl && (
            // key={channel.channel_id}: PreviewSession.h's switchChannel()
            // deliberately reuses the exact same manifest_url for the
            // session's whole life (never issues a new one on switch), so
            // VideoPlayer's own [manifestUrl]-keyed load effect never re-fires
            // after the first channel — hls.js was left to notice on its own
            // that the server deleted and recreated the segments underneath
            // it, which it doesn't reliably do. Forcing a full remount on
            // channel change (the same "channel changed" signal the header's
            // own focus highlight already reacts to) sidesteps that instead of
            // trying to make hls.js self-recover from a live media-sequence
            // reset.
          <VideoPlayer
              key={channel?.channel_id}
            videoRef={videoRef}
            manifestUrl={manifestUrl}
            isLive
            autoPlay
            onTimeUpdate={() => {}}
            onEnded={() => {}}
            onError={() => {}}
          />
        )}
      </div>
          <div className={styles.scrim}/>

          <div className={styles.textBlock}>
        {channel && (
          <div className={styles.channelLine}>
              <span className={styles.channelName}>Ch {channel.number} · {channel.name}</span>
              {channel.content_tag && <span className={styles.ratingChip}>{channel.content_tag}</span>}
              <span className={styles.clock}>{fmtClock(nowMs)}</span>
          </div>
        )}

              <h1 className={styles.title}>
                  {previewProgram?.item_type === 'episode' ? (previewProgram.show_title ?? previewProgram.title) : previewProgram?.title ?? 'No program info'}
              </h1>

              <div className={styles.metaRow}>
                  {label && <span>{label}</span>}
                  {previewProgram?.item_type === 'episode' && <span>{previewProgram.title}</span>}
                  {startsInMs != null && <span className={styles.countdown}>Starts in {fmtCountdown(startsInMs)}</span>}
              </div>

              {previewProgram?.overview && (
                  <p className={styles.overview}>{previewProgram.overview}</p>
              )}

              <button onClick={onWatch} style={goldBtnStyle} className={styles.watchBtn}>
                  <svg width="13" height="13" viewBox="0 0 14 14" fill="currentColor">
                      <path d="M3 1.5v11l9-5.5-9-5.5z"/>
                  </svg>
          Watch
        </button>
      </div>
    </div>
  )
}
