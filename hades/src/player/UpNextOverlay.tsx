import { useEffect, useState } from 'react'
import { mediaUrl } from '../api/client'
import type { NextEpisode } from '../api/types'

const COUNTDOWN_SECS = 10

interface UpNextOverlayProps {
  nextEpisode: NextEpisode
  onPlayNow:   () => void
  // Omit for the live-channel case: there's no user-controlled advance for a
  // linear channel, just an informational "coming up" card, so no
  // countdown/button and no dismiss (it clears on its own once the schedule
  // moves on — see PlayerPage's channel-PiP effect).
  onDismiss?:  () => void
}

export function UpNextOverlay({ nextEpisode, onPlayNow, onDismiss }: UpNextOverlayProps) {
  const passive = !onDismiss
  const [secsLeft, setSecsLeft] = useState(COUNTDOWN_SECS)

  useEffect(() => {
    if (passive) return
    if (secsLeft <= 0) { onPlayNow(); return }
    const t = setTimeout(() => setSecsLeft(s => s - 1), 1000)
    return () => clearTimeout(t)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [secsLeft, passive])

  return (
    <div style={cardStyle}>
      {!passive && (
        <button onClick={onDismiss} style={dismissBtnStyle} aria-label="Dismiss">✕</button>
      )}
      <div style={rowStyle}>
        {nextEpisode.thumb && (
          <img
            src={mediaUrl(`/api/episodes/${nextEpisode.episode_id}/thumb`)}
            alt=""
            style={thumbStyle}
          />
        )}
        <div style={{ display: 'flex', flexDirection: 'column', gap: 4, minWidth: 0 }}>
          <div style={labelStyle}>{passive ? 'Coming Up' : 'Up Next'}</div>
          <div style={epNumStyle}>S{nextEpisode.season} · E{nextEpisode.episode}</div>
          <div style={titleStyle}>{nextEpisode.title}</div>
        </div>
      </div>
      {!passive && (
        <button onClick={onPlayNow} style={playBtnStyle}>
          Play Now {secsLeft > 0 ? `(${secsLeft})` : ''}
        </button>
      )}
    </div>
  )
}

const cardStyle: React.CSSProperties = {
  position: 'absolute', right: 28, bottom: 110, zIndex: 60,
  width: 340, padding: 16, borderRadius: 10,
  background: 'var(--hds-bg-2, rgba(20,20,24,0.92))',
  border: '1px solid var(--hds-line, rgba(255,255,255,0.12))',
  boxShadow: '0 8px 28px rgba(0,0,0,0.5)',
  fontFamily: "'JetBrains Mono', monospace", color: 'var(--hds-txt, #eee)',
}

const rowStyle: React.CSSProperties = { display: 'flex', gap: 12, alignItems: 'center' }

const thumbStyle: React.CSSProperties = {
  width: 100, height: 56, objectFit: 'cover', borderRadius: 6, flexShrink: 0,
  background: 'var(--hds-bg-3, #222)',
}

const labelStyle: React.CSSProperties = {
  fontSize: 10, letterSpacing: '0.12em', textTransform: 'uppercase',
  color: 'var(--hds-gold, #cba135)',
}

const epNumStyle: React.CSSProperties = { fontSize: 11, color: 'var(--hds-txt-2, #999)' }

const titleStyle: React.CSSProperties = {
  fontSize: 13, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
}

const dismissBtnStyle: React.CSSProperties = {
  position: 'absolute', top: 6, right: 8, background: 'transparent', border: 'none',
  color: 'var(--hds-txt-2, #999)', cursor: 'pointer', fontSize: 13, padding: 4,
}

const playBtnStyle: React.CSSProperties = {
  marginTop: 12, width: '100%', padding: '8px 0', borderRadius: 6, cursor: 'pointer',
  border: '1px solid var(--hds-line, rgba(255,255,255,0.12))',
  background: 'var(--hds-gold, #cba135)', color: '#111', fontWeight: 600,
  fontFamily: "'JetBrains Mono', monospace", fontSize: 12,
}
