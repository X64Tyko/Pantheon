import type { VodTracks } from './playbackApi'
import styles from './TrackMenu.module.css'

interface TrackMenuProps {
  onClose:          () => void
  isLive:           boolean
  tracks:           VodTracks | null
  currentAudio:     number
  currentSubtitle:  number
  onSelectAudio:    (index: number) => void
  onSelectSubtitle: (index: number) => void // -1 = off
  liveAudioLang?:    string
  liveSubtitleLang?: string
}

const langLabel = (lang: string) => lang ? lang.toUpperCase() : 'Unknown'

export function TrackMenu({
  onClose, isLive, tracks, currentAudio, currentSubtitle,
  onSelectAudio, onSelectSubtitle, liveAudioLang, liveSubtitleLang,
}: TrackMenuProps) {
  return (
    <div className={styles.overlay} onClick={onClose}>
      <div className={styles.panel} onClick={e => e.stopPropagation()}>
        <div className={styles.sectionTitle}>Audio</div>
        {isLive ? (
          <div className={styles.liveRow}>{liveAudioLang ? langLabel(liveAudioLang) : 'Default'}</div>
        ) : (
          <div className={styles.optionList}>
            {(tracks?.audio ?? []).map(t => (
              <button
                key={t.index}
                onClick={() => onSelectAudio(t.index)}
                className={`${styles.option} ${t.index === currentAudio ? styles.optionActive : ''}`}
              >
                {t.title || langLabel(t.language)}
                {t.channels > 0 && <span className={styles.metaSpan}>{t.channels === 1 ? 'mono' : t.channels === 2 ? 'stereo' : `${t.channels}ch`}</span>}
              </button>
            ))}
            {(tracks?.audio ?? []).length === 0 && <div className={styles.emptyRow}>No audio tracks found</div>}
          </div>
        )}

        <div className={`${styles.sectionTitle} ${styles.sectionTitleSpaced}`}>Subtitles</div>
        {isLive ? (
          <div className={styles.liveRow}>{liveSubtitleLang ? langLabel(liveSubtitleLang) : 'Off'}</div>
        ) : (
          <div className={styles.optionList}>
            {/* -1 is the only "off" value — external tracks are negative too (<= -2), so this can't be `currentSubtitle < 0`. */}
            <button onClick={() => onSelectSubtitle(-1)} className={`${styles.option} ${currentSubtitle === -1 ? styles.optionActive : ''}`}>Off</button>
            {(tracks?.subtitles ?? []).map(t => {
              const selectable = t.extractable || t.burn_in
              return (
                <button
                  key={t.index}
                  onClick={() => selectable && onSelectSubtitle(t.index)}
                  disabled={!selectable}
                  className={`${styles.option} ${t.index === currentSubtitle ? styles.optionActive : ''} ${selectable ? '' : styles.optionDisabled}`}
                  title={!selectable ? 'This subtitle format is not supported for streaming' : t.burn_in ? 'Burned into the video — switching tracks will restart playback' : undefined}
                >
                  {t.title || langLabel(t.language)}
                  {t.burn_in && <span className={styles.metaSpan}>burned-in</span>}
                  {t.source === 'external' && <span className={styles.metaSpan}>external</span>}
                </button>
              )
            })}
          </div>
        )}
      </div>
    </div>
  )
}
