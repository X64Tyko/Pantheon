import type { RokuDevice } from '../api/types'

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
    <div style={overlayStyle} onClick={onClose}>
      <div style={panelStyle} onClick={e => e.stopPropagation()}>
        <div style={sectionTitleStyle}>Play on Roku</div>
        <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
          {devices.map(d => (
            <button key={d.id} onClick={() => { onSelect(d.id); onClose() }} style={optionStyle}>
              {d.name}
            </button>
          ))}
        </div>
      </div>
    </div>
  )
}

const overlayStyle: React.CSSProperties = {
  position: 'absolute', inset: 0, zIndex: 20,
  display: 'flex', alignItems: 'flex-end', justifyContent: 'flex-end',
  padding: '0 24px 88px 0',
}

const panelStyle: React.CSSProperties = {
  width: 280, maxHeight: '50vh', overflowY: 'auto',
  background: 'var(--hds-glass)', backdropFilter: 'blur(14px)',
  border: '1px solid var(--hds-glass-border)', borderRadius: 10,
  padding: '14px 16px', boxShadow: '0 12px 40px oklch(0 0 0 / 0.5)',
}

const sectionTitleStyle: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 10, letterSpacing: '0.1em',
  color: 'var(--hds-txt-3)', marginBottom: 6, textTransform: 'uppercase',
}

const optionStyle: React.CSSProperties = {
  textAlign: 'left', padding: '7px 10px', borderRadius: 6, cursor: 'pointer',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 12,
  border: '1px solid transparent', background: 'transparent', color: 'var(--hds-txt-2)',
}
