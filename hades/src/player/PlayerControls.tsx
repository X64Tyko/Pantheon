import { useEffect, useState, type RefObject } from 'react'
import { useFocusable } from '../nav/useFocusable'

const SEEK_STEP_MS = 10_000 // scrub-bar left/right nudge while D-pad focused

// Bundles just the pieces PlayerControls needs to render/drive the Cast
// button and (once connected) route transport controls to the receiver
// instead of the local <video> — PlayerPage owns the actual useCastSession()
// call and the loadMedia() trigger, since those need session data (manifest
// URL, title, live/VOD) that lives there, not here.
export interface CastBridge {
  available:  boolean // SDK usable — hide the button entirely otherwise (e.g. no App ID configured)
  connected:  boolean
  deviceName: string | null
  paused:     boolean
  togglePlay: () => void
  endSession: () => void
  onRequestCast: () => void
}

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
  // While hidden (auto-idle), the first D-pad/keyboard press should only
  // reveal the controls again, not also move focus — standard TV playback
  // UX. onActivity resets the idle timer on every nav input, visible or not.
  controlsVisible: boolean
  onActivity:      () => void
  cast?:           CastBridge
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
  onOpenTracks, onOpenSettings, showSettings, controlsVisible, onActivity, cast,
}: PlayerControlsProps) {
  const [playingLocal, setPlayingLocal] = useState(true)
  const [volume,  setVolume]  = useState(1)
  const [muted,   setMuted]   = useState(false)
  const [scrubMs, setScrubMs] = useState<number | null>(null) // non-null while dragging

  const isCasting = cast?.connected ?? false
  const playing = isCasting ? !cast!.paused : playingLocal

  useEffect(() => {
    const video = videoRef.current
    if (!video) return
    const onPlay  = () => setPlayingLocal(true)
    const onPause = () => setPlayingLocal(false)
    video.addEventListener('play',  onPlay)
    video.addEventListener('pause', onPause)
    return () => {
      video.removeEventListener('play',  onPlay)
      video.removeEventListener('pause', onPause)
    }
  }, [videoRef])

  const togglePlay = () => {
    if (isCasting) { cast!.togglePlay(); return }
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

  // Every arrow press resets the idle timer (visible or not); while hidden,
  // that same first press only reveals the controls (returns false — the
  // library's "prevent default nav" signal) rather than also moving focus
  // onto a control nobody could see a moment ago.
  const guardArrow = (handler?: (direction: string) => boolean) => (direction: string) => {
    const wasHidden = !controlsVisible
    onActivity()
    if (wasHidden) return false
    return handler ? handler(direction) : true
  }

  // Each control is a plain focusable leaf — the library's own nearest-
  // neighbor grid resolution chains them left-right/up-down correctly on its
  // own (buttons are laid out in a row; the scrub bar sits directly above).
  // onEnterPress mirrors each button's existing onClick — same handler, both
  // input paths converge on identical behavior.
  const back        = useFocusable<object, HTMLButtonElement>({ focusKey: 'player-back',       onEnterPress: onBack,             onArrowPress: guardArrow() })
  // forceFocus: /player/* routes render outside <Layout> (no sidebar, so its
  // own forceFocus fallback never mounts here) — without its own fallback, a
  // direct/refreshed load straight into the player would hit the same
  // "nothing focused yet, every arrow press no-ops" dead end.
  const play        = useFocusable<object, HTMLButtonElement>({ focusKey: 'player-play',       onEnterPress: togglePlay,         onArrowPress: guardArrow(), forceFocus: true })
  const mute         = useFocusable<object, HTMLButtonElement>({ focusKey: 'player-mute',       onEnterPress: toggleMute,         onArrowPress: guardArrow() })
  const tracks         = useFocusable<object, HTMLButtonElement>({ focusKey: 'player-tracks',     onEnterPress: onOpenTracks,       onArrowPress: guardArrow() })
  const settings         = useFocusable<object, HTMLButtonElement>({ focusKey: 'player-settings',   onEnterPress: onOpenSettings,     onArrowPress: guardArrow(), focusable: showSettings })
  const fullscreen         = useFocusable<object, HTMLButtonElement>({ focusKey: 'player-fullscreen', onEnterPress: toggleFullscreen,   onArrowPress: guardArrow() })
  const castBtn  = useFocusable<object, HTMLButtonElement>({ focusKey: 'player-cast', onEnterPress: () => cast?.onRequestCast(), onArrowPress: guardArrow(), focusable: !!cast?.available && !isCasting })
  const stopCast = useFocusable<object, HTMLButtonElement>({ focusKey: 'player-stop-cast', onEnterPress: () => cast?.endSession(), onArrowPress: guardArrow(), focusable: isCasting })
  const scrub = useFocusable<object, HTMLDivElement>({
    focusKey: 'player-scrub',
    focusable: !isLive && durationMs > 0,
    // Prevent default nav (return false) on left/right — nudge position
    // instead of moving focus to the next control, the same escape hatch a
    // Guide program block doesn't need but a scrub bar does.
    onArrowPress: guardArrow(direction => {
      if (direction !== 'left' && direction !== 'right') return true
      const delta = direction === 'right' ? SEEK_STEP_MS : -SEEK_STEP_MS
      onSeek(Math.min(durationMs, Math.max(0, currentMs + delta)))
      return false
    }),
  })

  return (
    <div style={wrapStyle}>
      {/* Top bar */}
      <div style={topBarStyle}>
        <button ref={back.ref} data-tv-focused={back.focused} onClick={onBack} style={iconBtnStyle} aria-label="Back">
          <BackIcon />
        </button>
        <span style={titleStyle}>{title}</span>
      </div>

      {/* Bottom bar */}
      <div style={bottomBarStyle}>
        {!isLive && (
          <div
            ref={scrub.ref} data-tv-focused={scrub.focused}
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
          <button ref={play.ref} data-tv-focused={play.focused} onClick={togglePlay} style={iconBtnStyle} aria-label={playing ? 'Pause' : 'Play'}>
            {playing ? <PauseIcon /> : <PlayIcon />}
          </button>

          {!isCasting && (
            <>
              <button ref={mute.ref} data-tv-focused={mute.focused} onClick={toggleMute} style={iconBtnStyle} aria-label={muted ? 'Unmute' : 'Mute'}>
                {muted || volume === 0 ? <MuteIcon /> : <VolumeIcon />}
              </button>
              <input
                type="range" min={0} max={1} step={0.05}
                value={muted ? 0 : volume}
                onChange={e => changeVolume(Number(e.target.value))}
                style={volumeSliderStyle}
              />
            </>
          )}

          {isCasting ? (
            <span style={castingLabelStyle}>Casting to {cast!.deviceName ?? 'device'}</span>
          ) : isLive ? (
            <span style={liveBadgeStyle}>● LIVE</span>
          ) : (
            <span style={timeStyle}>{fmtTime(displayMs)} / {fmtTime(durationMs)}</span>
          )}

          <div style={{ flex: 1 }} />

          {isCasting ? (
            <button ref={stopCast.ref} data-tv-focused={stopCast.focused} onClick={() => cast?.endSession()} style={iconBtnStyle} aria-label="Stop casting">
              <CastConnectedIcon />
            </button>
          ) : (
            <>
              <button ref={tracks.ref} data-tv-focused={tracks.focused} onClick={onOpenTracks} style={iconBtnStyle} aria-label="Audio & Subtitles">
                <TracksIcon />
              </button>
              {showSettings && (
                <button ref={settings.ref} data-tv-focused={settings.focused} onClick={onOpenSettings} style={iconBtnStyle} aria-label="Playback info">
                  <SettingsIcon />
                </button>
              )}
              {cast?.available && (
                <button ref={castBtn.ref} data-tv-focused={castBtn.focused} onClick={() => cast.onRequestCast()} style={iconBtnStyle} aria-label="Cast">
                  <CastIcon />
                </button>
              )}
              <button ref={fullscreen.ref} data-tv-focused={fullscreen.focused} onClick={toggleFullscreen} style={iconBtnStyle} aria-label="Fullscreen">
                <FullscreenIcon />
              </button>
            </>
          )}
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

const castingLabelStyle: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 12, fontWeight: 600,
  color: 'var(--hds-violet)', letterSpacing: '0.02em', marginLeft: 4,
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
function CastIcon() {
  return (
    <svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.4">
      <rect x="2" y="2.5" width="14" height="10" rx="1.2" />
      <path d="M2 15.5a4 4 0 0 0-2-3.5M2 12.5a1.5 1.5 0 0 1 1.5 1.5" fill="none" strokeLinecap="round" />
      <circle cx="2.3" cy="15.5" r="0.9" fill="currentColor" stroke="none" />
    </svg>
  )
}
function CastConnectedIcon() {
  return (
    <svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.4">
      <rect x="2" y="2.5" width="14" height="10" rx="1.2" fill="var(--hds-violet)" fillOpacity="0.25" />
      <path d="M2 15.5a4 4 0 0 0-2-3.5M2 12.5a1.5 1.5 0 0 1 1.5 1.5" fill="none" strokeLinecap="round" />
      <circle cx="2.3" cy="15.5" r="0.9" fill="currentColor" stroke="none" />
    </svg>
  )
}
