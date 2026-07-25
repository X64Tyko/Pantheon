import type { VodTracks } from './playbackApi'
import styles from './SettingsMenu.module.css'

interface SettingsMenuProps {
  onClose:    () => void
    directStream: boolean | null
  tracks:     VodTracks | null
}

export function SettingsMenu({onClose, directStream, tracks}: SettingsMenuProps) {
  const video = tracks?.video[0]
  return (
    <div className={styles.overlay} onClick={onClose}>
      <div className={styles.panel} onClick={e => e.stopPropagation()}>
        <div className={styles.sectionTitle}>Playback</div>
        <Row label="Mode">
          <span className={directStream ? styles.modeDirectStream : styles.modeTranscoding}>
            {directStream === null ? '—' : directStream ? 'Direct Stream' : 'Transcoding'}
          </span>
        </Row>
        {video && (
          <>
            <Row label="Video">{video.codec.toUpperCase()} · {video.width}×{video.height}</Row>
          </>
        )}
      </div>
    </div>
  )
}

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className={styles.row}>
      <span className={styles.rowLabel}>{label}</span>
      <span className={styles.rowValue}>{children}</span>
    </div>
  )
}
