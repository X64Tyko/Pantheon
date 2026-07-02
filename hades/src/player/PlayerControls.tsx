import { useEffect, useState, type RefObject } from 'react'

interface PlayerControlsProps {
  videoRef:        RefObject<HTMLVideoElement>
  title:           string
  isLive:          boolean
  currentMs:       number
  durationMs:      number
  onSeek:          (ms: number) => void
  onBack:          () => void
  onOpenTracks:    () => void
  onOpenSettings:  () => void
  showSettings:    boolean
}

function fmtTime(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) return '0:00'
  const total = Math.floor(ms / 1000)
  const h = Math.floor(total / 3600)
  const m = Math.floor((total % 3600) / 60)
  const s = total % 60
  const mm = h > 0 ? String(m).padStart(2, '0') : String(m)
  const ss = String(s).padStart(2, '0')
  return h > 0 ? `${h}:${mm}:${ss}` : `${mm}:${ss}`
}

export function PlayerControls({
  videoRef, title, isLive, currentMs, durationMs, onSeek, onBack,
  onOpenTracks, onOpenSettings, showSettings,
}: PlayerControlsProps) {
  const [playing, setPlaying] = useState(true)
  const [volume,  setVolume]  = useState(1)
  const [muted,   setMuted]   = useState(false)
  const [scrubMs, setScrubMs] = useState<number | null>(null) // non-null while dragging

  useEffect(() => {
    const video = videoRef.current
    if (!video) return
    const onPlay  = () => setPlaying(true)
    const onPause = () => setPlaying(false)
    video.addEventListener('play',  onPlay)
    video.addEventListener('pause', onPause)
    return () => {
      video.removeEventListener('play',  onPlay)
      video.removeEventListener('pause', onPause)
    }
  }, [videoRef])

  const togglePlay = () => {
    const video = videoRef.current
    if (!video) return
    if (video.paused) video.play().catch(() => {})
    else video.pause()
  }

  const toggleMute = () => {
    const video = videoRef.current
    if (!video) return
    video.muted = !video.muted
    setMuted(video.muted)
  }

  const changeVolume = (v: number) => {
    const video = videoRef.current
    if (!video) return
    video.volume = v
    video.muted = v === 0
    setVolume(v)
    setMuted(v === 0)
  }

  const toggleFullscreen = () => {
    const el = videoRef.current?.parentElement
    if (!el) return
    if (document.fullscreenElement) document.exitFullscreen()
    else el.requestFullscreen().catch(() => {})
  }

  const displayMs = scrubMs ?? currentMs
  const progress  = durationMs > 0 ? Math.min(1, displayMs / durationMs) : 0

  return (
    <div style={wrapStyle}>
      {/* Top bar */}
      <div style={topBarStyle}>
        <button onClick={onBack} style={iconBtnStyle} aria-label="Back">
          <BackIcon />
        </button>
        <span style={titleStyle}>{title}</span>
      </div>

      {/* Bottom bar */}
      <div style={bottomBarStyle}>
        {!isLive && (
          <div
            style={scrubTrackStyle}
            onClick={e => {
              if (durationMs <= 0) return
              const rect = e.currentTarget.getBoundingClientRect()
              const pct  = Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width))
              onSeek(pct * durationMs)
            }}
          >
            <div style={{ ...scrubFillStyle, width: `${progress * 100}%` }} />
          </div>
        )}

        <div style={controlsRowStyle}>
          <button onClick={togglePlay} style={iconBtnStyle} aria-label={playing ? 'Pause' : 'Play'}>
            {playing ? <PauseIcon /> : <PlayIcon />}
          </button>

          <button onClick={toggleMute} style={iconBtnStyle} aria-label={muted ? 'Unmute' : 'Mute'}>
            {muted || volume === 0 ? <MuteIcon /> : <VolumeIcon />}
          </button>
          <input
            type="range" min={0} max={1} step={0.05}
            value={muted ? 0 : volume}
            onChange={e => changeVolume(Number(e.target.value))}
            style={volumeSliderStyle}
          />

          {isLive ? (
            <span style={liveBadgeStyle}>● LIVE</span>
          ) : (
            <span style={timeStyle}>{fmtTime(displayMs)} / {fmtTime(durationMs)}</span>
          )}

          <div style={{ flex: 1 }} />

          <button onClick={onOpenTracks} style={iconBtnStyle} aria-label="Audio & Subtitles">
            <TracksIcon />
          </button>
          {showSettings && (
            <button onClick={onOpenSettings} style={iconBtnStyle} aria-label="Playback info">
              <SettingsIcon />
            </button>
          )}
          <button onClick={toggleFullscreen} style={iconBtnStyle} aria-label="Fullscreen">
            <FullscreenIcon />
          </button>
        </div>
      </div>
    </div>
  )
}

const wrapStyle: React.CSSProperties = {
  position: 'absolute', inset: 0, zIndex: 10, pointerEvents: 'none',
  display: 'flex', flexDirection: 'column', justifyContent: 'space-between',
}

const topBarStyle: React.CSSProperties = {
  pointerEvents: 'auto', display: 'flex', alignItems: 'center', gap: 12,
  padding: '18px 24px', background: 'linear-gradient(to bottom, oklch(0 0 0 / 0.6), transparent)',
}

const titleStyle: React.CSSProperties = {
  fontFamily: "'Chakra Petch', sans-serif", fontSize: 15, fontWeight: 600, color: '#fff',
}

const bottomBarStyle: React.CSSProperties = {
  pointerEvents: 'auto', padding: '10px 24px 20px',
  background: 'linear-gradient(to top, oklch(0 0 0 / 0.75), transparent)',
  display: 'flex', flexDirection: 'column', gap: 10,
}

const scrubTrackStyle: React.CSSProperties = {
  position: 'relative', height: 5, borderRadius: 3, cursor: 'pointer',
  background: 'oklch(1 0 0 / 0.2)',
}

const scrubFillStyle: React.CSSProperties = {
  position: 'absolute', top: 0, left: 0, bottom: 0, borderRadius: 3,
  background: 'var(--hds-violet)',
}

const controlsRowStyle: React.CSSProperties = {
  display: 'flex', alignItems: 'center', gap: 4,
}

const iconBtnStyle: React.CSSProperties = {
  display: 'flex', alignItems: 'center', justifyContent: 'center',
  width: 36, height: 36, borderRadius: 8, cursor: 'pointer',
  background: 'transparent', border: 'none', color: '#fff',
}

const volumeSliderStyle: React.CSSProperties = {
  width: 70, accentColor: 'var(--hds-violet)', marginRight: 8,
}

const timeStyle: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 12, color: 'oklch(0.85 0.01 285)', marginLeft: 4,
}

const liveBadgeStyle: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 600,
  color: 'var(--hds-match-red, oklch(0.62 0.2 22))', letterSpacing: '0.04em', marginLeft: 4,
}

// ── Icons (inline SVG, no icon library dependency) ────────────────────────────

function PlayIcon() {
  return <svg width="18" height="18" viewBox="0 0 18 18" fill="currentColor"><path d="M4 2.5v13l11-6.5-11-6.5z" /></svg>
}
function PauseIcon() {
  return <svg width="18" height="18" viewBox="0 0 18 18" fill="currentColor"><rect x="4" y="2.5" width="4" height="13" /><rect x="10" y="2.5" width="4" height="13" /></svg>
}
function VolumeIcon() {
  return <svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.4"><path d="M3 6.5v5h3l4 3.5v-12l-4 3.5H3z" /><path d="M12.5 6.2a4 4 0 0 1 0 5.6M14.5 4.2a7 7 0 0 1 0 9.6" /></svg>
}
function MuteIcon() {
  return <svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.4"><path d="M3 6.5v5h3l4 3.5v-12l-4 3.5H3z" /><path d="M12 6.5l4 5M16 6.5l-4 5" /></svg>
}
function TracksIcon() {
  return <svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.4"><rect x="2.5" y="3.5" width="13" height="9" rx="1.5" /><path d="M5.5 15h7" /></svg>
}
function SettingsIcon() {
  return <svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="9" cy="9" r="2.6" /><path d="M9 2.5v2M9 13.5v2M2.5 9h2M13.5 9h2M4.5 4.5l1.4 1.4M12.1 12.1l1.4 1.4M4.5 13.5l1.4-1.4M12.1 5.9l1.4-1.4" /></svg>
}
function FullscreenIcon() {
  return <svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.4"><path d="M3 7V3h4M15 7V3h-4M3 11v4h4M15 11v4h-4" /></svg>
}
function BackIcon() {
  return <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><path d="M10 3L4.5 8l5.5 5" /></svg>
}
