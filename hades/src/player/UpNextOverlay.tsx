import { useEffect, useState } from 'react'
import { setFocus } from '@noriginmedia/norigin-spatial-navigation'
import { useFocusable } from '../nav/useFocusable'
import { mediaUrl } from '../api/client'
import type { NextEpisode } from '../api/types'

const COUNTDOWN_SECS = 10
const CANCEL_FOCUS_KEY = 'player-upnext-cancel'

interface UpNextOverlayProps {
  nextEpisode: NextEpisode
  onPlayNow:   () => void
  // Omit for the live-channel case: there's no user-controlled advance for a
  // linear channel, just an informational "coming up" card, so no
  // countdown/Cancel and no dismiss (it clears on its own once the schedule
  // moves on — see PlayerPage's channel-PiP effect).
  onDismiss?:  () => void
}

export function UpNextOverlay({ nextEpisode, onPlayNow, onDismiss }: UpNextOverlayProps) {
  const passive = !onDismiss
  const [secsLeft, setSecsLeft] = useState(COUNTDOWN_SECS)

  const cancel = useFocusable<object, HTMLButtonElement>({
    focusKey: CANCEL_FOCUS_KEY,
    focusable: !passive,
    onEnterPress: () => onDismiss?.(),
  })

  // Defaults focus to Cancel the moment the overlay appears, so a remote's
  // OK button stops auto-play by default rather than requiring an explicit
  // arrow-nav over from PlayerControls first.
  useEffect(() => {
    if (passive) return
    setFocus(CANCEL_FOCUS_KEY)
  }, [passive])

  useEffect(() => {
    if (passive) return
    if (secsLeft <= 0) { onPlayNow(); return }
    const t = setTimeout(() => setSecsLeft(s => s - 1), 1000)
    return () => clearTimeout(t)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [secsLeft, passive])

  return (
    <div
      style={cardStyle}
      onClick={passive ? undefined : onPlayNow}
      role={passive ? undefined : 'button'}
      tabIndex={passive ? undefined : 0}
    >
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
        <>
          <div style={countdownTextStyle}>Playing Next In: {secsLeft}s</div>
          <button
            ref={cancel.ref} data-tv-focused={cancel.focused}
            onClick={e => { e.stopPropagation(); onDismiss!() }}
            style={cancelBtnStyle}
          >
            Cancel
          </button>
          <div style={progressTrackStyle}>
            <div
              className="hds-upnext-fill"
              style={{ ...progressFillStyle, animationDuration: `${COUNTDOWN_SECS}s` }}
            />
          </div>
        </>
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

const countdownTextStyle: React.CSSProperties = {
  marginTop: 14, fontSize: 11, color: 'var(--hds-txt-2, #999)',
}

const cancelBtnStyle: React.CSSProperties = {
  marginTop: 8, width: '100%', padding: '8px 0', borderRadius: 6, cursor: 'pointer',
  border: '1px solid var(--hds-line, rgba(255,255,255,0.12))',
  background: 'transparent', color: 'var(--hds-txt, #eee)', fontWeight: 600,
  fontFamily: "'JetBrains Mono', monospace", fontSize: 12,
}

// The bar drains over the same countdown that drives auto-play — visually,
// "how much time is left to hit Cancel" (see hds-upnext-drain in index.css).
const progressTrackStyle: React.CSSProperties = {
  marginTop: 10, height: 3, borderRadius: 2, overflow: 'hidden',
  background: 'var(--hds-bg-3, rgba(255,255,255,0.1))',
}

const progressFillStyle: React.CSSProperties = {
  height: '100%', width: '100%', background: 'var(--hds-gold, #cba135)',
  animationName: 'hds-upnext-drain', animationTimingFunction: 'linear', animationFillMode: 'forwards',
}
