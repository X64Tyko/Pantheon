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
  volumeLevel: number
  muted:       boolean
  togglePlay: () => void
  setVolumeLevel: (level: number) => void
  toggleMuted:    () => void
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
  // Same shape as CastBridge — useRokuSession() (hades/src/cast-roku/) returns
  // an identical interface, so this whole component treats "casting to
  // Chromecast" and "casting to a paired Roku" as two instances of the same
  // concept rather than duplicating every branch below.
  roku?:           CastBridge
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
  onOpenTracks, onOpenSettings, showSettings, controlsVisible, onActivity, cast, roku,
}: PlayerControlsProps) {
  const [playingLocal, setPlayingLocal] = useState(true)
  const [volume,  setVolume]  = useState(1)
  const [muted,   setMuted]   = useState(false)
  const [scrubMs, setScrubMs] = useState<number | null>(null) // non-null while dragging

  // Whichever remote (if either) is actually connected right now — Chromecast
  // takes priority in the unlikely case both ended up connected at once,
  // since that's not a state the UI below tries to prevent, only tolerate.
  const activeRemote = cast?.connected ? cast : roku?.connected ? roku : null
  const isCasting = activeRemote !== null
  const playing = isCasting ? !activeRemote!.paused : playingLocal

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
    if (isCasting) { activeRemote!.togglePlay(); return }
    const video = videoRef.current
    if (!video) return
    if (video.paused) video.play().catch(() => {})
    else video.pause()
  }

  const toggleMute = () => {
    if (isCasting) { activeRemote!.toggleMuted(); return }
    const video = videoRef.current
    if (!video) return
    video.muted = !video.muted
    setMuted(video.muted)
  }

  const changeVolume = (v: number) => {
    if (isCasting) { activeRemote!.setVolumeLevel(v); return }
    const video = videoRef.current
    if (!video) return
    video.volume = v
    video.muted = v === 0
    setVolume(v)
    setMuted(v === 0)
  }

  const displayVolume = isCasting ? activeRemote!.volumeLevel : volume
  const displayMuted  = isCasting ? activeRemote!.muted       : muted

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
  const rokuBtn  = useFocusable<object, HTMLButtonElement>({ focusKey: 'player-roku', onEnterPress: () => roku?.onRequestCast(), onArrowPress: guardArrow(), focusable: !!roku?.available && !isCasting })
  const stopCast = useFocusable<object, HTMLButtonElement>({ focusKey: 'player-stop-cast', onEnterPress: () => activeRemote?.endSession(), onArrowPress: guardArrow(), focusable: isCasting })
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
        <div style={{ flex: 1 }} />
        {isCasting ? (
          <button ref={stopCast.ref} data-tv-focused={stopCast.focused} onClick={() => activeRemote?.endSession()} style={iconBtnStyle} aria-label="Stop casting">
            <CastConnectedIcon />
          </button>
        ) : (
          <>
            {cast?.available && (
              <button ref={castBtn.ref} data-tv-focused={castBtn.focused} onClick={() => cast.onRequestCast()} style={iconBtnStyle} aria-label="Cast">
                <CastIcon />
              </button>
            )}
            {roku?.available && (
              <button ref={rokuBtn.ref} data-tv-focused={rokuBtn.focused} onClick={() => roku.onRequestCast()} style={iconBtnStyle} aria-label="Play on Roku">
                <RokuIcon />
              </button>
            )}
          </>
        )}
      </div>

      {/* Bottom bar */}
      <div style={bottomBarStyle}>
        {!isLive && (
          // Outer element is the actual hit area (ref/focus/click all live
          // here) — much taller than the visible bar so it's actually
          // draggable with a finger, while the inner div keeps the thin
          // visual track. getBoundingClientRect() below still only cares
          // about X-axis width, so the added vertical padding doesn't
          // affect the seek-position math at all.
          <div
            ref={scrub.ref} data-tv-focused={scrub.focused}
            style={scrubHitAreaStyle}
            onClick={e => {
              if (durationMs <= 0) return
              const rect = e.currentTarget.getBoundingClientRect()
              const pct  = Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width))
              onSeek(pct * durationMs)
            }}
          >
            <div style={scrubTrackStyle}>
              <div style={{ ...scrubFillStyle, width: `${progress * 100}%` }} />
            </div>
          </div>
        )}

        <div style={controlsRowStyle}>
          <button ref={play.ref} data-tv-focused={play.focused} onClick={togglePlay} style={iconBtnStyle} aria-label={playing ? 'Pause' : 'Play'}>
            {playing ? <PauseIcon /> : <PlayIcon />}
          </button>

          <button ref={mute.ref} data-tv-focused={mute.focused} onClick={toggleMute} style={iconBtnStyle} aria-label={displayMuted ? 'Unmute' : 'Mute'}>
            {displayMuted || displayVolume === 0 ? <MuteIcon /> : <VolumeIcon />}
          </button>
          <input
            type="range" min={0} max={1} step={0.05}
            value={displayMuted ? 0 : displayVolume}
            onChange={e => changeVolume(Number(e.target.value))}
            style={volumeSliderStyle}
          />

          {isCasting ? (
            <span style={castingLabelStyle}>Casting to {activeRemote!.deviceName ?? 'device'}</span>
          ) : isLive ? (
            <span style={liveBadgeStyle}>● LIVE</span>
          ) : (
            <span style={timeStyle}>{fmtTime(displayMs)} / {fmtTime(durationMs)}</span>
          )}

          <div style={{ flex: 1 }} />

          {!isCasting && (
            <>
              <button ref={tracks.ref} data-tv-focused={tracks.focused} onClick={onOpenTracks} style={iconBtnStyle} aria-label="Audio & Subtitles">
                <TracksIcon />
              </button>
              {showSettings && (
                <button ref={settings.ref} data-tv-focused={settings.focused} onClick={onOpenSettings} style={iconBtnStyle} aria-label="Playback info">
                  <SettingsIcon />
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

// Real hit area — a 5px-tall track alone is very hard to drag precisely
// with a finger, so the clickable/draggable region (see the wrapping div
// above) is much taller than what's actually drawn.
const scrubHitAreaStyle: React.CSSProperties = {
  padding: '14px 0', cursor: 'pointer',
}

const scrubTrackStyle: React.CSSProperties = {
  position: 'relative', height: 5, borderRadius: 3,
  background: 'oklch(1 0 0 / 0.2)',
}

const scrubFillStyle: React.CSSProperties = {
  position: 'absolute', top: 0, left: 0, bottom: 0, borderRadius: 3,
  background: 'var(--hds-violet)',
}

const controlsRowStyle: React.CSSProperties = {
  display: 'flex', alignItems: 'center', gap: 4, flexWrap: 'wrap',
}

const iconBtnStyle: React.CSSProperties = {
  display: 'flex', alignItems: 'center', justifyContent: 'center',
  // 44x44 — below Apple/Google's ~44-48px minimum recommended touch target,
  // 36px was hard to hit precisely with a finger. Fine on desktop too.
  width: 44, height: 44, borderRadius: 8, cursor: 'pointer',
  background: 'transparent', border: 'none', color: '#fff', flexShrink: 0,
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
function RokuIcon() {
  // A plain TV/remote glyph, not Roku's trademarked logo — this is a
  // generic "cast to a TV device" affordance, distinct enough from
  // CastIcon's Chromecast-style glyph to tell the two buttons apart.
  return (
    <svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.4">
      <rect x="2" y="3" width="14" height="9" rx="1.2" />
      <path d="M6.5 15h5" strokeLinecap="round" />
      <path d="M9 12v3" strokeLinecap="round" />
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
