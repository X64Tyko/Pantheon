import { useEffect, useState } from 'react'
import { setFocus } from '@noriginmedia/norigin-spatial-navigation'
import { useFocusable } from '../nav/useFocusable'
import { mediaUrl } from '../api/client'
import type { NextEpisode } from '../api/types'
import styles from './UpNextOverlay.module.css'

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
      className={styles.card}
      onClick={passive ? undefined : onPlayNow}
      role={passive ? undefined : 'button'}
      tabIndex={passive ? undefined : 0}
    >
      <div className={styles.row}>
        {nextEpisode.thumb && (
          <img
            src={mediaUrl(`/api/episodes/${nextEpisode.episode_id}/thumb`)}
            alt=""
            className={styles.thumb}
          />
        )}
        <div className={styles.textCol}>
          <div className={styles.label}>{passive ? 'Coming Up' : 'Up Next'}</div>
          <div className={styles.epNum}>S{nextEpisode.season} · E{nextEpisode.episode}</div>
          <div className={styles.epTitle}>{nextEpisode.title}</div>
        </div>
      </div>
      {!passive && (
        <>
          <div className={styles.countdownText}>Playing Next In: {secsLeft}s</div>
          <button
            ref={cancel.ref} data-tv-focused={cancel.focused}
            onClick={e => { e.stopPropagation(); onDismiss!() }}
            className={styles.cancelBtn}
          >
            Cancel
          </button>
          <div className={styles.progressTrack}>
            <div
              className={`hds-upnext-fill ${styles.progressFill}`}
              style={{ animationDuration: `${COUNTDOWN_SECS}s` }}
            />
          </div>
        </>
      )}
    </div>
  )
}
