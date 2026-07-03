import { useEffect, useRef, useState, useCallback } from 'react'
import { useNavigate, useParams, useSearchParams } from 'react-router-dom'
import { api, mediaUrl, channelLogoUrl } from '../api/client'
import type { Channel } from '../api/types'
import { usePlaybackSession, type PlaybackTarget } from './usePlaybackSession'
import { VideoPlayer } from './VideoPlayer'
import { PlayerControls } from './PlayerControls'
import { TrackMenu } from './TrackMenu'
import { SettingsMenu } from './SettingsMenu'
import { LoadingThrobber } from './LoadingThrobber'
import { useNavBack } from '../nav/back'
import { useCastSession } from '../cast/useCastSession'

const TARGET_BUFFER_SECS = 6 // matches the HLS segment length — "fully buffered" for throbber purposes

interface PlayerPageProps {
  kind: 'movie' | 'episode' | 'channel'
}

const PROGRESS_PING_MS = 15_000
const CONTROLS_IDLE_MS = 3_000

export function PlayerPage({ kind }: PlayerPageProps) {
  const { id, channelId } = useParams<{ id: string; channelId: string }>()
  const [searchParams] = useSearchParams()
  const navigate = useNavigate()

  const targetId = (kind === 'channel' ? channelId : id) ?? ''
  const initialPositionMs = Number(searchParams.get('t') ?? 0) || 0

  const target: PlaybackTarget = kind === 'channel'
    ? { kind: 'channel', id: targetId }
    : { kind, id: targetId }

  const session = usePlaybackSession(target, initialPositionMs)
  const videoRef = useRef<HTMLVideoElement>(null)

  const [currentMs,   setCurrentMs]   = useState(initialPositionMs)
  const [playerError, setPlayerError] = useState<string | null>(null)
  const [menu,         setMenu]        = useState<'tracks' | 'settings' | null>(null)
  const [controlsVisible, setControlsVisible] = useState(true)
  const [liveChannel, setLiveChannel] = useState<Channel | null>(null)
  const [buffering, setBuffering] = useState(false)
  const [bufferPercent, setBufferPercent] = useState(0)

  const idleTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const castSession = useCastSession()
  const isCasting = castSession.connected

  useEffect(() => {
    const video = videoRef.current
    if (!video) return
    const onWaiting = () => setBuffering(true)
    const onPlaying = () => setBuffering(false)
    const onProgress = () => {
      if (video.buffered.length === 0) return
      const aheadSecs = video.buffered.end(video.buffered.length - 1) - video.currentTime
      setBufferPercent(Math.min(100, Math.max(0, aheadSecs / TARGET_BUFFER_SECS * 100)))
    }
    video.addEventListener('waiting',  onWaiting)
    video.addEventListener('playing',  onPlaying)
    video.addEventListener('progress', onProgress)
    return () => {
      video.removeEventListener('waiting',  onWaiting)
      video.removeEventListener('playing',  onPlaying)
      video.removeEventListener('progress', onProgress)
    }
  }, [session.manifestUrl])

  // Live channel metadata (for the title bar and the TrackMenu's read-only info).
  useEffect(() => {
    if (kind !== 'channel') return
    api.getChannels().then(chs => setLiveChannel(chs.find(c => c.channel_id === targetId) ?? null)).catch(() => {})
  }, [kind, targetId])

  // Periodic + on-unmount watch-progress pings (VOD only).
  useEffect(() => {
    if (kind === 'channel') return
    const interval = setInterval(() => {
      if (currentMs > 0 && session.durationMs > 0)
        api.putWatchProgress(kind, targetId, { position_ms: Math.round(currentMs), duration_ms: Math.round(session.durationMs) }).catch(() => {})
    }, PROGRESS_PING_MS)
    return () => {
      clearInterval(interval)
      if (currentMs > 0 && session.durationMs > 0)
        api.putWatchProgress(kind, targetId, { position_ms: Math.round(currentMs), duration_ms: Math.round(session.durationMs) }).catch(() => {})
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [kind, targetId, session.durationMs])

  const resetIdleTimer = useCallback(() => {
    setControlsVisible(true)
    if (idleTimer.current) clearTimeout(idleTimer.current)
    idleTimer.current = setTimeout(() => setControlsVisible(false), CONTROLS_IDLE_MS)
  }, [])

  useEffect(() => {
    resetIdleTimer()
    return () => { if (idleTimer.current) clearTimeout(idleTimer.current) }
  }, [resetIdleTimer])

  useNavBack(useCallback(() => navigate(-1), [navigate]))

  const title = kind === 'channel' ? (liveChannel?.name ?? 'Live TV') : session.title

  // Fires once per connect, not on every manifestUrl change — casting mid-
  // track-switch isn't supported in v1 (see castMedia.ts), so there's no
  // "reload the receiver" path to wire up here, only the initial handoff.
  useEffect(() => {
    if (!isCasting || !session.manifestUrl) return
    castSession.load({
      manifestUrl: session.manifestUrl,
      isLive:      session.isLive,
      currentMs,
      metadata: {
        title:    title,
        imageUrl: kind === 'movie' ? mediaUrl(`/api/movies/${targetId}/thumb`)
                : kind === 'channel' ? channelLogoUrl(targetId)
                : undefined, // episode: no cheap thumb URL without an extra fetch — v1 scope
      },
    })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [isCasting])

  const handleSeek = (ms: number) => {
    if (isCasting) { castSession.seek(ms); setCurrentMs(ms); return }
    const video = videoRef.current
    if (!video) return
    const targetSec = ms / 1000
    let withinBuffer = false
    for (let i = 0; i < video.buffered.length; i++) {
      if (targetSec >= video.buffered.start(i) && targetSec <= video.buffered.end(i)) { withinBuffer = true; break }
    }
    if (withinBuffer) {
      video.currentTime = targetSec
      setCurrentMs(ms)
    } else {
      session.reload({ positionMs: ms })
    }
  }

  const handleSelectAudio    = (index: number) => session.reload({ positionMs: currentMs, audioTrack: index })
  const handleSelectSubtitle = (index: number) => session.reload({ positionMs: currentMs, subtitleTrack: index })

  return (
    <div
      style={pageStyle}
      onMouseMove={resetIdleTimer}
      onClick={resetIdleTimer}
    >
      {session.loading && (
        <div style={overlayStyle}>
          <LoadingThrobber label="Starting playback…" />
        </div>
      )}

      {(session.error || playerError) && !session.loading && (
        <div style={overlayStyle}>
          <div style={{ color: 'var(--hds-match-red, oklch(0.62 0.2 22))', marginBottom: 10 }}>
            {session.error ?? playerError}
          </div>
          <button onClick={() => navigate(-1)} style={backBtnStyle}>Go back</button>
        </div>
      )}

      {!session.loading && !session.error && session.manifestUrl && (
        <>
          {isCasting ? (
            // The local hls.js instance only exists inside VideoPlayer — not
            // rendering it while casting tears it down via its own unmount
            // cleanup (hls?.destroy()), same as navigating away normally.
            <div style={castingBackdropStyle} />
          ) : (
            <VideoPlayer
              videoRef={videoRef}
              manifestUrl={session.manifestUrl}
              subtitleUrl={session.subtitleUrl}
              isLive={session.isLive}
              onTimeUpdate={(ms) => setCurrentMs(ms)}
              onEnded={() => navigate(-1)}
              onError={setPlayerError}
            />
          )}
          {buffering && !isCasting && (
            <div style={{ ...overlayStyle, pointerEvents: 'none' }}>
              <LoadingThrobber percent={bufferPercent} />
            </div>
          )}
          <div style={{ opacity: controlsVisible || menu ? 1 : 0, transition: 'opacity .25s', pointerEvents: controlsVisible || menu ? 'auto' : 'none' }}>
            <PlayerControls
              videoRef={videoRef}
              title={title}
              isLive={session.isLive}
              currentMs={isCasting ? castSession.currentMs : currentMs}
              durationMs={isCasting ? castSession.durationMs : session.durationMs}
              onSeek={handleSeek}
              onBack={() => navigate(-1)}
              onOpenTracks={() => setMenu(m => m === 'tracks' ? null : 'tracks')}
              onOpenSettings={() => setMenu(m => m === 'settings' ? null : 'settings')}
              showSettings={!session.isLive}
              controlsVisible={controlsVisible}
              onActivity={resetIdleTimer}
              cast={{
                available:  castSession.available,
                connected:  castSession.connected,
                deviceName: castSession.deviceName,
                paused:     castSession.paused,
                togglePlay: castSession.togglePlay,
                endSession: castSession.endSession,
                onRequestCast: () => cast.framework.CastContext.getInstance().requestSession().catch(() => {}),
              }}
            />
            {menu === 'tracks' && (
              <TrackMenu
                onClose={() => setMenu(null)}
                isLive={session.isLive}
                tracks={session.tracks}
                currentAudio={session.audioTrack}
                currentSubtitle={session.subtitleTrack}
                onSelectAudio={handleSelectAudio}
                onSelectSubtitle={handleSelectSubtitle}
                liveAudioLang={liveChannel?.audio_lang}
                liveSubtitleLang={liveChannel?.subtitle_lang}
              />
            )}
            {menu === 'settings' && (
              <SettingsMenu onClose={() => setMenu(null)} directPlay={session.directPlay} tracks={session.tracks} />
            )}
          </div>
        </>
      )}
    </div>
  )
}

// Plain fixed backdrop — must NOT be a flex/grid container. Flex alignItems
// other than the default 'stretch' stops a video/controls child's 100%
// width+height from actually filling it, so it renders undersized and
// centered instead of edge-to-edge. Centering for loading/error states lives
// in overlayStyle, a separate absolutely-positioned layer, not here.
const pageStyle: React.CSSProperties = {
  position: 'fixed', inset: 0, background: '#000', zIndex: 100,
}

const castingBackdropStyle: React.CSSProperties = {
  width: '100%', height: '100%', background: '#000',
}

const overlayStyle: React.CSSProperties = {
  position: 'absolute', inset: 0, zIndex: 50,
  display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 13, color: 'var(--hds-txt-2)',
}

const backBtnStyle: React.CSSProperties = {
  padding: '8px 18px', borderRadius: 8, cursor: 'pointer',
  border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
  color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 12,
}
