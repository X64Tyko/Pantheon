import type { VodTracks } from './playbackApi'

interface SettingsMenuProps {
  onClose:    () => void
  directPlay: boolean | null
  tracks:     VodTracks | null
}

export function SettingsMenu({ onClose, directPlay, tracks }: SettingsMenuProps) {
  const video = tracks?.video[0]
  return (
    <div style={overlayStyle} onClick={onClose}>
      <div style={panelStyle} onClick={e => e.stopPropagation()}>
        <div style={sectionTitleStyle}>Playback</div>
        <Row label="Mode">
          <span style={{ color: directPlay ? 'var(--hds-match-green, oklch(0.72 0.18 145))' : 'var(--hds-gold)' }}>
            {directPlay === null ? '—' : directPlay ? 'Direct Play' : 'Transcoding'}
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
    <div style={{ display: 'flex', justifyContent: 'space-between', gap: 12, padding: '5px 0', fontFamily: "'JetBrains Mono', monospace", fontSize: 11 }}>
      <span style={{ color: 'var(--hds-txt-3)' }}>{label}</span>
      <span style={{ color: 'var(--hds-txt)' }}>{children}</span>
    </div>
  )
}

const overlayStyle: React.CSSProperties = {
  position: 'absolute', inset: 0, zIndex: 20,
  display: 'flex', alignItems: 'flex-end', justifyContent: 'flex-end',
  padding: '0 24px 88px 0',
}

const panelStyle: React.CSSProperties = {
  width: 240,
  background: 'var(--hds-glass)', backdropFilter: 'blur(14px)',
  border: '1px solid var(--hds-glass-border)', borderRadius: 10,
  padding: '14px 16px', boxShadow: '0 12px 40px oklch(0 0 0 / 0.5)',
}

const sectionTitleStyle: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 10, letterSpacing: '0.1em',
  color: 'var(--hds-txt-3)', marginBottom: 4, textTransform: 'uppercase',
}
