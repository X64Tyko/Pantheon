import type { RokuDevice } from '../api/types'
import styles from './RokuDeviceMenu.module.css'

interface RokuDeviceMenuProps {
  onClose:  () => void
  devices:  RokuDevice[]
  onSelect: (deviceId: string) => void
}

// Only ever shown when there's more than one paired device — PlayerControls
// calls onRequestCast directly against the single device otherwise, same as
// Chromecast's requestSession() skips its own native picker for one device.
export function RokuDeviceMenu({ onClose, devices, onSelect }: RokuDeviceMenuProps) {
  return (
    <div className={styles.overlay} onClick={onClose}>
      <div className={styles.panel} onClick={e => e.stopPropagation()}>
        <div className={styles.sectionTitle}>Play on Roku</div>
        <div className={styles.optionList}>
          {devices.map(d => (
            <button key={d.id} onClick={() => { onSelect(d.id); onClose() }} className={styles.option}>
              {d.name}
            </button>
          ))}
        </div>
      </div>
    </div>
  )
}
