import { useEffect, useRef } from 'react'
import type { Channel, EpgProgram } from '../api/types'
import { VideoPlayer } from '../player/VideoPlayer'

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
    <div style={{
      display: 'flex', gap: 20, padding: '16px 20px', marginBottom: 12,
      background: 'var(--hds-bg-2)', border: '1px solid var(--hds-line-s)', borderRadius: 10,
    }}>
      <div style={{ width: 220, aspectRatio: '16/9', borderRadius: 8, overflow: 'hidden', flexShrink: 0, background: '#000' }}>
        {manifestUrl && (
          <VideoPlayer
            videoRef={videoRef}
            manifestUrl={manifestUrl}
            subtitleUrl={null}
            autoPlay
            onTimeUpdate={() => {}}
            onEnded={() => {}}
            onError={() => {}}
          />
        )}
      </div>
      <div style={{ minWidth: 0 }}>
        <div style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 18, fontWeight: 700, color: 'var(--hds-txt)' }}>
          {nowProgram?.item_type === 'episode' ? (nowProgram.show_title ?? nowProgram.title) : nowProgram?.title ?? 'No program info'}
        </div>
        {nowProgram && (
          <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)', marginTop: 4 }}>
            {label && <>{label} — </>}{nowProgram.item_type === 'episode' ? nowProgram.title : ''}
          </div>
        )}
        {nowProgram?.overview && (
          <p style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 11.5, lineHeight: 1.6, color: 'var(--hds-txt-2)',
            margin: '10px 0 0', maxWidth: 560, display: '-webkit-box', WebkitLineClamp: 3, WebkitBoxOrient: 'vertical', overflow: 'hidden',
          }}>{nowProgram.overview}</p>
        )}
        {channel && (
          <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 10 }}>
            Ch {channel.number} · {channel.name}
          </div>
        )}
        <button onClick={onWatch} style={{
          display: 'flex', alignItems: 'center', gap: 7, marginTop: 12, padding: '7px 14px', borderRadius: 7, cursor: 'pointer',
          border: '1px solid var(--hds-violet)', background: 'oklch(0.55 0.14 292 / 0.15)', color: 'var(--hds-violet)',
          fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 600,
        }}>
          <svg width="11" height="11" viewBox="0 0 14 14" fill="currentColor"><path d="M3 1.5v11l9-5.5-9-5.5z" /></svg>
          Watch
        </button>
      </div>
    </div>
  )
}
