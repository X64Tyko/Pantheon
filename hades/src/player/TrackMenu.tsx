import type { VodTracks } from './playbackApi'

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
    <div style={overlayStyle} onClick={onClose}>
      <div style={panelStyle} onClick={e => e.stopPropagation()}>
        <div style={sectionTitleStyle}>Audio</div>
        {isLive ? (
          <div style={liveRowStyle}>{liveAudioLang ? langLabel(liveAudioLang) : 'Default'}</div>
        ) : (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
            {(tracks?.audio ?? []).map(t => (
              <button
                key={t.index}
                onClick={() => onSelectAudio(t.index)}
                style={optionStyle(t.index === currentAudio)}
              >
                {t.title || langLabel(t.language)}
                {t.channels > 0 && <span style={metaSpanStyle}>{t.channels === 1 ? 'mono' : t.channels === 2 ? 'stereo' : `${t.channels}ch`}</span>}
              </button>
            ))}
            {(tracks?.audio ?? []).length === 0 && <div style={emptyStyle}>No audio tracks found</div>}
          </div>
        )}

        <div style={{ ...sectionTitleStyle, marginTop: 16 }}>Subtitles</div>
        {isLive ? (
          <div style={liveRowStyle}>{liveSubtitleLang ? langLabel(liveSubtitleLang) : 'Off'}</div>
        ) : (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
            <button onClick={() => onSelectSubtitle(-1)} style={optionStyle(currentSubtitle < 0)}>Off</button>
            {(tracks?.subtitles ?? []).map(t => (
              <button
                key={t.index}
                onClick={() => t.extractable && onSelectSubtitle(t.index)}
                disabled={!t.extractable}
                style={{ ...optionStyle(t.index === currentSubtitle), ...(t.extractable ? {} : { opacity: 0.4, cursor: 'not-allowed' }) }}
                title={t.extractable ? undefined : 'This subtitle format is not supported for streaming'}
              >
                {t.title || langLabel(t.language)}
              </button>
            ))}
          </div>
        )}
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

const liveRowStyle: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 12, color: 'var(--hds-txt-2)', padding: '4px 0',
}

const emptyStyle: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)', padding: '4px 0',
}

const metaSpanStyle: React.CSSProperties = {
  marginLeft: 8, fontSize: 9, color: 'var(--hds-txt-3)',
}

function optionStyle(active: boolean): React.CSSProperties {
  return {
    textAlign: 'left', padding: '7px 10px', borderRadius: 6, cursor: 'pointer',
    fontFamily: "'JetBrains Mono', monospace", fontSize: 12,
    border: active ? '1px solid var(--hds-violet)' : '1px solid transparent',
    background: active ? 'oklch(0.55 0.14 292 / 0.18)' : 'transparent',
    color: active ? 'var(--hds-violet)' : 'var(--hds-txt-2)',
  }
}
