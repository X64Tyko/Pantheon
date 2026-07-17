import { useEffect, useRef } from 'react'
import type { Channel, EpgProgram } from '../api/types'
import { VideoPlayer } from '../player/VideoPlayer'
import styles from './GuidePreview.module.css'

interface GuidePreviewProps {
  channel:      Channel | null
  nowProgram:   EpgProgram | null
  manifestUrl:  string | null
  onWatch:      () => void
}

export function GuidePreview({ channel, nowProgram, manifestUrl, onWatch }: GuidePreviewProps) {
  const videoRef = useRef<HTMLVideoElement>(null)

  useEffect(() => {
    if (videoRef.current) videoRef.current.muted = true
  }, [manifestUrl])

  const label = nowProgram && nowProgram.item_type === 'episode' && nowProgram.season != null && nowProgram.episode_num != null
    ? `S${String(nowProgram.season).padStart(2, '0')}E${String(nowProgram.episode_num).padStart(2, '0')}`
    : undefined

  return (
    <div className={`hds-guide-preview-row ${styles.row}`}>
      <div className={styles.videoWrap}>
        {manifestUrl && (
          <VideoPlayer
            videoRef={videoRef}
            manifestUrl={manifestUrl}
            subtitleUrl={null}
            isLive
            autoPlay
            onTimeUpdate={() => {}}
            onEnded={() => {}}
            onError={() => {}}
          />
        )}
      </div>
      <div className={styles.infoCol}>
        <div className={styles.title}>
          {nowProgram?.item_type === 'episode' ? (nowProgram.show_title ?? nowProgram.title) : nowProgram?.title ?? 'No program info'}
        </div>
        {nowProgram && (
          <div className={styles.subtitle}>
            {label && <>{label} — </>}{nowProgram.item_type === 'episode' ? nowProgram.title : ''}
          </div>
        )}
        {nowProgram?.overview && (
          <p className={styles.overview}>{nowProgram.overview}</p>
        )}
        {channel && (
          <div className={styles.channelLine}>
            Ch {channel.number} · {channel.name}
          </div>
        )}
        <button onClick={onWatch} className={styles.watchBtn}>
          <svg width="11" height="11" viewBox="0 0 14 14" fill="currentColor"><path d="M3 1.5v11l9-5.5-9-5.5z" /></svg>
          Watch
        </button>
      </div>
    </div>
  )
}
