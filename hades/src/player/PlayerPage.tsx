import { useEffect, useRef, useState, useCallback, useMemo } from 'react'
import { useNavigate, useParams, useSearchParams } from 'react-router-dom'
import { api, mediaUrl, channelLogoUrl } from '../api/client'
import type { Channel, ChannelNow, Chapter, NextEpisode, WatchTogetherSession } from '../api/types'
import { nextInQueue, type QueueItem } from './playQueue'
import { usePlaybackSession, type PlaybackTarget } from './usePlaybackSession'
import { VideoPlayer } from './VideoPlayer'
import { PlayerControls } from './PlayerControls'
import { TrackMenu } from './TrackMenu'
import { SettingsMenu } from './SettingsMenu'
import { RokuDeviceMenu } from './RokuDeviceMenu'
import { LoadingThrobber } from './LoadingThrobber'
import { UpNextOverlay } from './UpNextOverlay'
import { useNavBack } from '../nav/back'
import { useCastSession } from '../cast/useCastSession'
import { useRokuSession } from '../cast-roku/useRokuSession'
import type { CastMediaArgs } from '../cast/castMedia'
import { useAuth } from '../auth/AuthContext'
import { subscribeWatchTogether, postWatchTogetherCommand, postWatchTogetherHeartbeat, type WatchTogetherEvent } from './watchTogetherApi'
import styles from './PlayerPage.module.css'

const TARGET_BUFFER_SECS = 6 // matches the HLS segment length — "fully buffered" for throbber purposes

interface PlayerPageProps {
  kind: 'movie' | 'episode' | 'channel'
}

const PROGRESS_PING_MS = 15_000
const CONTROLS_IDLE_MS = 3_000
// Up Next trigger when there's no credits/outro chapter data for this item
// yet (most libraries, until Kairos's detector has covered them) — last 30s
// of the episode, Netflix-style.
const UP_NEXT_FALLBACK_WINDOW_MS = 30_000
// How close currentMs must be to session.durationMs for a native `ended`
// event to be trusted as real completion — see handleVideoEnded.
const NATURAL_END_TOLERANCE_MS = 5_000
// Watch Together: host heartbeat cadence — matches Hermes' own `sync` tick
// interval (WatchTogetherManager::tickSync, main.cpp), so a follower's
// authoritative position is never more than one tick stale.
const WT_HEARTBEAT_MS = 4_000
// Watch Together: how far a follower's local position must drift from a
// periodic `sync` tick's authoritative one before snapping to it — small
// clock/decode jitter shouldn't cause constant micro-seeking, but a stalled
// rebuffer or a fresh join should correct promptly.
const WT_SYNC_DRIFT_THRESHOLD_MS = 1_500
// Live channels: how often to re-poll "what's on now" — a live schedule has
// no scrubber to derive position from, so this also re-derives PiP state.
const CHANNEL_NOW_POLL_MS = 7_000

function isCreditsType(t: Chapter['chapter_type']) { return t === 'credits' || t === 'outro' }

// Only an episode-kind queue item has enough fields to drive the Up Next
// preview card (season/episode number) — a movie-typed next item still
// drives auto-advance (see handleAdvanceToNext/onEnded below), just without
// a preview or countdown, same as a channel schedule just cutting over.
function queueItemToNextEpisode(item: QueueItem): NextEpisode | null {
  if (item.kind !== 'episode') return null
  return {
    episode_id: item.id, season: item.season ?? 0, episode: item.episode ?? 0,
    title: item.title, duration_ms: item.duration_ms ?? 0, overview: '', air_date: '', thumb: item.thumb ?? '',
  }
}

export function PlayerPage({ kind }: PlayerPageProps) {
  const { id, channelId } = useParams<{ id: string; channelId: string }>()
  const [searchParams] = useSearchParams()
  const navigate = useNavigate()

  const targetId = (kind === 'channel' ? channelId : id) ?? ''
  const initialPositionMs = Number(searchParams.get('t') ?? 0) || 0
  // Only ever populated by the Cast receiver's LOAD interceptor
  // (CastReceiverProvider.tsx), carrying the sender's selected tracks into
  // this fresh session — see usePlaybackSession's initialAudioTrack param.
  const initialAudioTrack = searchParams.has('audio') ? Number(searchParams.get('audio')) : -1
  const initialSubtitleTrack = searchParams.has('subtitle') ? Number(searchParams.get('subtitle')) : -1

  // Playlist / shuffle-play queue (see playQueue.ts) — takes priority over
  // the default same-show "next episode" continuation below whenever one is
  // present, since a queue can walk across shows/movies (playlists) or a
  // deliberately randomized order (shuffle-play) that the default logic
  // knows nothing about.
  const queueToken = searchParams.get('queue')
  const queueNextItem = useMemo(
    () => (queueToken && kind !== 'channel') ? nextInQueue(queueToken, kind, targetId) : null,
    [queueToken, kind, targetId],
  )

  // Watch Together — present whenever this playback session was opened via
  // the Home shelf's "join"/"host" action (see LibraryDetailActions.tsx and
  // HomePage.tsx's WatchTogetherShelf). Kairos's own session detail (fetched
  // below) is what actually decides host vs. follower — the query param
  // alone only says "this is a watch-together session," never who's hosting.
  const wtSessionId = searchParams.get('wt')
  const { user } = useAuth()
  const [wtSession, setWtSession] = useState<WatchTogetherSession | null>(null)
  const isWtHost     = !!(wtSessionId && wtSession && user && wtSession.host_user_id === user.user_id)
  const isWtFollower = !!(wtSessionId && wtSession && !isWtHost)
  // Mirrors currentMsRef/isRemoteActiveRef above — the leave/close-on-unmount
  // effect below only ever runs once (it must fire exactly once, on real
  // unmount, not on every host/follower transition), so it needs to read
  // whatever isWtHost resolves to LATER through a ref, not the value closed
  // over at its own initial run (which is always false — wtSession hasn't
  // loaded yet on first render).
  const isWtHostRef = useRef(false)
  useEffect(() => { isWtHostRef.current = isWtHost }, [isWtHost])

  const target: PlaybackTarget = kind === 'channel'
    ? { kind: 'channel', id: targetId }
    : { kind, id: targetId }

  const session = usePlaybackSession(target, initialPositionMs, initialAudioTrack, initialSubtitleTrack)
  const videoRef = useRef<HTMLVideoElement>(null)

  const [currentMs,   setCurrentMs]   = useState(initialPositionMs)
  const [playerError, setPlayerError] = useState<string | null>(null)
  const [menu,         setMenu]        = useState<'tracks' | 'settings' | 'roku-devices' | null>(null)
  const [controlsVisible, setControlsVisible] = useState(true)
  const [liveChannel, setLiveChannel] = useState<Channel | null>(null)
  const [buffering, setBuffering] = useState(false)
  const [bufferPercent, setBufferPercent] = useState(0)

  // Series continuation — skip intro, credits/up-next detection, played-marking.
  const [chapters, setChapters] = useState<Chapter[]>([])
  const [nextEpisode, setNextEpisode] = useState<NextEpisode | null>(null)
  const [upNextDismissed, setUpNextDismissed] = useState(false)
  const skipCleanupPingRef = useRef(false)
  // Mirrors currentMs for the ping effect below, which intentionally doesn't
  // depend on currentMs (see that effect's own comment) — reading through a
  // ref keeps every ping's position fresh without restarting the interval
  // (and re-arming the exit-flush) on every timeupdate tick.
  const currentMsRef = useRef(initialPositionMs)
  // Mirrors isRemoteActive for the same reason — read fresh inside the ping
  // interval without restarting it on every cast/Roku connect/disconnect.
  const isRemoteActiveRef = useRef(false)

  // Live-channel credits PiP — driven purely by the currently-airing item's
  // own chapter data + wall-clock elapsed position, not a separate schedule
  // concept (see plan: "credits == pip, post_credits == restore, otherwise
  // normal").
  const [channelNow, setChannelNow] = useState<ChannelNow | null>(null)
  const [channelChapters, setChannelChapters] = useState<Chapter[]>([])

  const idleTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const castSession = useCastSession()
  const rokuSession = useRokuSession()
  const isCasting = castSession.connected
  const isCastingRoku = rokuSession.connected
  // Either remote means "playback is actually happening on that device, not
  // in this tab" — governs whether the local VideoPlayer/backdrop and
  // buffering indicator render, same as isCasting did on its own before Roku.
  const isRemoteActive = isCasting || isCastingRoku

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

  // Poll "what's on now" for the credits PiP. On item_id change, fetch that
  // item's chapters — the currently-airing item's own credits/post_credits
  // chapters drive the PiP, not anything about the filler/bumper that
  // eventually follows (that's just whatever /now reports once the schedule
  // naturally moves on, no separate fetch needed for it).
  useEffect(() => {
    if (kind !== 'channel') { setChannelNow(null); setChannelChapters([]); return }
    let cancelled = false
    let lastItemId = ''

    const tick = () => {
      api.getChannelNow(targetId).then(now => {
        if (cancelled) return
        setChannelNow(now)
        if (now.item_id === lastItemId) return
        lastItemId = now.item_id
        const fetchChapters = now.item_type === 'episode' ? api.getEpisodeChapters(now.item_id)
                             : now.item_type === 'movie'   ? api.getMovieChapters(now.item_id)
                             : Promise.resolve<Chapter[]>([])
        fetchChapters.catch(() => []).then(ch => { if (!cancelled) setChannelChapters(ch) })
      }).catch(() => {})
    }

    tick()
    const interval = setInterval(tick, CHANNEL_NOW_POLL_MS)
    return () => { cancelled = true; clearInterval(interval) }
  }, [kind, targetId])

  // Chapters + next-episode info for series continuation. PlayerPage doesn't
  // remount when advancing episode-to-episode (same route, just a new :id),
  // so targetId changing is also what resets currentMs/upNextDismissed below.
  // A queue-driven movie leg still runs this (queueNextItem can exist while
  // kind==='movie') purely to resolve what's next — chapters are never
  // fetched for movies, unchanged from before queues existed.
  useEffect(() => {
    const relevant = kind === 'episode' || (kind === 'movie' && !!queueNextItem)
    if (!relevant) { setChapters([]); setNextEpisode(null); return }
    let cancelled = false
    setChapters([])
    setNextEpisode(null)
    const chaptersPromise = kind === 'episode' ? api.getEpisodeChapters(targetId).catch(() => []) : Promise.resolve<Chapter[]>([])
    const nextPromise = queueToken
      ? Promise.resolve(queueNextItem ? queueItemToNextEpisode(queueNextItem) : null)
      : kind === 'episode' ? api.getNextEpisode(targetId).catch(() => null) : Promise.resolve<NextEpisode | null>(null)
    Promise.all([chaptersPromise, nextPromise]).then(([ch, next]) => {
      if (cancelled) return
      setChapters(ch)
      setNextEpisode(next)
    })
    return () => { cancelled = true }
  }, [kind, targetId, queueToken, queueNextItem])

  useEffect(() => {
    setCurrentMs(initialPositionMs)
    currentMsRef.current = initialPositionMs
    setUpNextDismissed(false)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [targetId])

  useEffect(() => { currentMsRef.current = currentMs }, [currentMs])
  useEffect(() => { isRemoteActiveRef.current = isRemoteActive }, [isRemoteActive])

  // Periodic + on-unmount watch-progress pings (VOD only). Reads currentMs
  // through a ref (kept fresh by the effect above) rather than depending on
  // currentMs directly — this interval must survive for the whole playback
  // session rather than restarting on every timeupdate tick, so each ping
  // still needs to see the *latest* position at the moment it fires. Skipped
  // once by handleAdvanceToNext/onEnded, which already send their own
  // definitive completed=true write for the item being left — without this
  // guard, this effect's cleanup would fire right after and could overwrite
  // that back to completed=false with a lower, pre-completion position.
  // Also suppressed entirely while casting/Roku-mirroring: the receiver
  // device (Chromecast) or the Roku app itself is the one actually playing
  // and reports its own real position — this tab's currentMs doesn't track
  // it (no local video element is mounted, see isRemoteActive above), so
  // pinging here would just overwrite the receiver's correct value with a
  // stale one every 15s. handleStopCast/handleStopRoku send an explicit
  // flush using the live remote position at the moment of disconnect.
  useEffect(() => {
    if (kind === 'channel') return
    const interval = setInterval(() => {
      if (!isRemoteActiveRef.current && currentMsRef.current > 0 && session.durationMs > 0)
        api.putWatchProgress(kind, targetId, { position_ms: Math.round(currentMsRef.current), duration_ms: Math.round(session.durationMs), device_type: 'web', direct_play: session.directPlay ?? undefined }).catch(() => {})
    }, PROGRESS_PING_MS)
    return () => {
      clearInterval(interval)
      if (skipCleanupPingRef.current) { skipCleanupPingRef.current = false; return }
      if (!isRemoteActiveRef.current && currentMsRef.current > 0 && session.durationMs > 0)
        api.putWatchProgress(kind, targetId, { position_ms: Math.round(currentMsRef.current), duration_ms: Math.round(session.durationMs), device_type: 'web', direct_play: session.directPlay ?? undefined }).catch(() => {})
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [kind, targetId, session.durationMs])

  // ── Watch Together ───────────────────────────────────────────────────────────

  // Session identity/host — Kairos-owned (see WatchTogetherService.cpp); this
  // is the one round trip that decides host vs. follower for everything below.
  useEffect(() => {
    if (!wtSessionId) { setWtSession(null); return }
    api.getWatchTogether(wtSessionId).then(setWtSession).catch(() => setWtSession(null))
  }, [wtSessionId])

  // Host heartbeat — cheap/frequent, updates Hermes' extrapolation baseline
  // between explicit seek/pause/play commands (see WatchTogetherSession.h).
  // Reads through the same currentMsRef the watch-progress ping above uses,
  // for the same reason (survive the whole session without restarting the
  // interval on every timeupdate tick).
  useEffect(() => {
    if (!wtSessionId || !isWtHost || kind === 'channel') return
    const interval = setInterval(() => {
      const video = videoRef.current
      postWatchTogetherHeartbeat(wtSessionId, currentMsRef.current, video?.paused ?? true)
    }, WT_HEARTBEAT_MS)
    return () => clearInterval(interval)
  }, [wtSessionId, isWtHost, kind])

  // Host explicit play/pause commands — separate from PlayerControls' own
  // internal play/pause listener (which only drives its own local button
  // state), attached directly to the native <video> element so a keyboard
  // shortcut/D-pad toggle is covered too, not just PlayerControls' button.
  // Never double-fires as an "echo": only the host ever posts commands, and
  // the host never applies an incoming remote command to its own element
  // (that's the follower-only branch below), so there's no risk of a remote
  // pause bouncing back out as a second local command.
  useEffect(() => {
    if (!wtSessionId || !isWtHost) return
    const video = videoRef.current
    if (!video) return
    const onPlay  = () => postWatchTogetherCommand(wtSessionId, { type: 'play' })
    const onPause = () => postWatchTogetherCommand(wtSessionId, { type: 'pause' })
    video.addEventListener('play',  onPlay)
    video.addEventListener('pause', onPause)
    return () => {
      video.removeEventListener('play',  onPlay)
      video.removeEventListener('pause', onPause)
    }
  }, [wtSessionId, isWtHost, session.manifestUrl])

  // Follower: apply every event exactly the same way regardless of type —
  // paused-state always syncs (idempotent, safe even though explicit
  // pause/play already covers most of it — this also catches a fresh join
  // or a missed event), position only snaps immediately for an explicit
  // seek/pause/play; a plain `sync` tick only corrects once drift exceeds
  // WT_SYNC_DRIFT_THRESHOLD_MS so ordinary clock/decode jitter doesn't cause
  // constant micro-seeking.
  const applyWtEvent = useCallback((event: WatchTogetherEvent) => {
    const video = videoRef.current
    if (!video) return
    if (typeof event.position_ms === 'number') {
      const targetSec = event.position_ms / 1000
      if (event.type !== 'sync' || Math.abs(targetSec - video.currentTime) * 1000 > WT_SYNC_DRIFT_THRESHOLD_MS) {
        video.currentTime = targetSec
        setCurrentMs(event.position_ms)
      }
    }
    if (event.paused === true && !video.paused) video.pause()
    else if (event.paused === false && video.paused) video.play().catch(() => {})
  }, [])

  useEffect(() => {
    if (!wtSessionId || !isWtFollower) return
    const es = subscribeWatchTogether(wtSessionId, applyWtEvent)
    return () => es.close()
  }, [wtSessionId, isWtFollower, applyWtEvent])

  // Leave/close on the way out — best-effort, mirrors stopVodPlayback's own
  // fire-and-forget teardown. The host ending it closes for everyone
  // (Kairos's own host-only /close); a follower leaving only drops their own
  // membership row. Either way this is also just a courtesy — Hermes' own
  // heartbeat-gap reaper (Phase D) is what actually guarantees an abandoned
  // session doesn't linger if this never fires (tab closed, not navigated).
  useEffect(() => {
    if (!wtSessionId) return
    return () => {
      if (isWtHostRef.current) api.closeWatchTogether(wtSessionId).catch(() => {})
      else                     api.leaveWatchTogether(wtSessionId).catch(() => {})
    }
  }, [wtSessionId])

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

  // Shared by both senders — Chromecast's LOAD needs the absolute
  // manifestUrl (the receiver fetches media itself); Roku's load command
  // (buildRokuLoadCommand, cast-roku/rokuMedia.ts) only reads .route/.currentMs
  // off the same object and resolves its own manifest server-side.
  const buildCastArgs = useCallback((): CastMediaArgs => ({
    // Safe to assert: every call site is only reachable from inside the JSX
    // block gated on `session.manifestUrl` truthy (the Cast/Roku buttons and
    // the device-picker menu only render there).
    manifestUrl: session.manifestUrl!,
    isLive:      session.isLive,
    currentMs,
    metadata: {
      title:    title,
      imageUrl: kind === 'movie' ? mediaUrl(`/api/movies/${targetId}/thumb`)
              : kind === 'channel' ? channelLogoUrl(targetId)
              : undefined, // episode: no cheap thumb URL without an extra fetch — v1 scope
    },
    route: { contentType: kind, contentId: targetId },
    audioTrack:    session.audioTrack,
    subtitleTrack: session.subtitleTrack,
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }), [session.manifestUrl, session.isLive, currentMs, title, kind, targetId, session.audioTrack, session.subtitleTrack])

  // Fires once per connect, not on every manifestUrl change — casting mid-
  // track-switch isn't supported in v1 (see castMedia.ts), so there's no
  // "reload the receiver" path to wire up here, only the initial handoff.
  useEffect(() => {
    if (!isCasting || !session.manifestUrl) return
    castSession.load(buildCastArgs()).catch(err => setPlayerError(err?.message ?? 'Failed to start casting'))
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [isCasting])

  const requestRoku = () => {
    if (rokuSession.devices.length === 1) {
      rokuSession.load(buildCastArgs(), rokuSession.devices[0].id)
        .catch(err => setPlayerError(err?.message ?? 'Failed to start casting to Roku'))
    } else if (rokuSession.devices.length > 1) {
      setMenu('roku-devices')
    }
  }

  // Explicit flush at the moment the user disconnects from this tab, using
  // the remote device's own live-reported position — the periodic ping is
  // suppressed throughout the whole session (see the ping effect above), so
  // without this nothing here writes a final position for whatever the
  // remote device was actually showing before endSession() tears it down.
  const handleStopCast = useCallback(() => {
    if (kind !== 'channel' && castSession.currentMs > 0 && castSession.durationMs > 0)
      api.putWatchProgress(kind, targetId, {
        position_ms: Math.round(castSession.currentMs), duration_ms: Math.round(castSession.durationMs), device_type: 'cast',
      }).catch(() => {})
    castSession.endSession()
  }, [kind, targetId, castSession])

  const handleStopRoku = useCallback(() => {
    if (kind !== 'channel' && rokuSession.currentMs > 0 && rokuSession.durationMs > 0)
      api.putWatchProgress(kind, targetId, {
        position_ms: Math.round(rokuSession.currentMs), duration_ms: Math.round(rokuSession.durationMs), device_type: 'roku',
      }).catch(() => {})
    rokuSession.endSession()
  }, [kind, targetId, rokuSession])

  // A plain seek stays on the same persistent VOD session/manifest now
  // (Hephaestus's sliding-window engine transparently regenerates whatever
  // segment range is needed — see VodSession::restartAt) — no more "seek
  // outside the buffered range = tear down this session and start a new
  // one." hls.js/the browser just requests whatever segment covers the
  // target position from the one manifest that already describes the whole
  // file; letting the request happen and briefly buffer is exactly the same
  // experience a normal on-demand video seek already has everywhere else.
  const handleSeek = (ms: number) => {
    // A follower's own scrub is pointless — the next sync tick (at most
    // WT_HEARTBEAT_MS away) just corrects it back to the host's actual
    // position anyway. No-op rather than letting it briefly "work" and then
    // visibly snap back a moment later.
    if (isWtFollower) return
    if (isCasting) { castSession.seek(ms); setCurrentMs(ms); return }
    if (isCastingRoku) { rokuSession.seek(ms); setCurrentMs(ms); return }
    const video = videoRef.current
    if (!video) return
    video.currentTime = ms / 1000
    setCurrentMs(ms)
    if (wtSessionId && isWtHost) postWatchTogetherCommand(wtSessionId, { type: 'seek', position_ms: Math.round(ms) })
  }

  // Plex-style sticky per-show preference — fire-and-forget, episodes only
  // (movies have no show to carry a preference across). Kairos resolves
  // episode_id -> show_id itself, so this never needs show_id client-side.
  const saveTrackPreference = (b: { audio_lang?: string; subtitle_lang?: string }) => {
    if (kind === 'episode') api.setEpisodeTrackPreference(targetId, b).catch(() => {})
  }

  // No session.reload() — audio has been its own independent VodEncodeStream
  // server-side since the video/audio split landed, so this is a pure
  // client-side selection against the master manifest's AUDIO group;
  // VideoPlayer picks it up via its audioTrack prop and switches hls.js
  // directly, without touching video or restarting the session at all.
  const handleSelectAudio = (index: number) => {
    session.selectAudioTrack(index)
    saveTrackPreference({ audio_lang: session.tracks?.audio.find(t => t.index === index)?.language ?? '' })
  }
  // Burn-in (bitmap PGS/DVD/DVB) tracks are never in the master manifest's
  // SUBTITLES group — HLS has no way to express "select a different video,"
  // which is what a burn-in pick actually is (VodSession composites it into
  // the frame itself). They only ever reach this component via the wire
  // tracks.subtitles[].burn_in flag, so route them through session.reload()
  // (a real session-level reattach) instead of the plain client-side toggle
  // below, which has nothing to attach a burn-in selection to and would
  // otherwise silently drop it.
  const handleSelectSubtitle = (index: number) => {
    const track = session.tracks?.subtitles.find(t => t.index === index)
    if (track?.burn_in) {
      session.reload({ positionMs: currentMs, subtitleTrack: index })
    } else {
      // No session.reload() — subtitles are fully decoupled from the main
      // encoder now (VodSession's on-demand /subtitles/{n} pipe), so this is
      // a pure client-side selection against the master manifest's
      // SUBTITLES group; VideoPlayer picks it up via its subtitleTrack prop
      // and switches hls.js directly, without touching video/audio at all.
      session.selectSubtitleTrack(index)
    }
    saveTrackPreference({ subtitle_lang: track?.language ?? '' })
  }

  // Not shown while casting/Roku-mirroring — the sender tab's own navigate()
  // wouldn't affect what's actually playing on the receiver (queuing a new
  // title mid-cast isn't supported yet, see useCastSession), so acting on
  // either affordance here wouldn't do what it visually promises.
  const activeChapter = !isRemoteActive
    ? chapters.find(c => currentMs >= c.start_ms && currentMs < (c.end_ms || Infinity))
    : undefined

  const showSkipIntro = activeChapter?.chapter_type === 'intro'

  const hasCreditsChapterData = chapters.some(c => isCreditsType(c.chapter_type))
  // If this episode has a post-credits scene, don't trigger during the
  // 'credits' chapter itself (that would cover/spoil the scene) — wait until
  // the post_credits chapter is actually reached, same as Netflix holding
  // off Up Next for a Marvel-style stinger.
  const hasPostCreditsChapter = chapters.some(c => c.chapter_type === 'post_credits')
  const inCreditsChapter = !!activeChapter && (
    activeChapter.chapter_type === 'post_credits' ||
    (isCreditsType(activeChapter.chapter_type) && !hasPostCreditsChapter)
  )
  const nearEnd = session.durationMs > 0 && currentMs > 0 &&
    (session.durationMs - currentMs) < UP_NEXT_FALLBACK_WINDOW_MS
  const showUpNext = !isRemoteActive && !!nextEpisode && !upNextDismissed &&
    (inCreditsChapter || (!hasCreditsChapterData && nearEnd))

  // Marks the item being left as played (regardless of its actual position —
  // the whole point of skipping/auto-advancing is doing this before
  // naturally reaching the 95% threshold) and moves on. queueNextItem (any
  // kind) takes priority over nextEpisode (only ever populated for a
  // same-show episode-to-episode continuation, or a queue's episode-typed
  // next item — see the effect above), since a queue is the more specific
  // "what's actually next" source whenever one is active.
  const handleAdvanceToNext = useCallback(() => {
    const next = queueNextItem ?? (nextEpisode ? { kind: 'episode' as const, id: nextEpisode.episode_id } : null)
    if (!next || kind === 'channel') return
    skipCleanupPingRef.current = true
    // session.durationMs > 0 guard — same as handleNaturalEnd's. Without it,
    // a stray/premature native `ended` event (observed when a stream fails
    // to actually start: a failed load's retry cycle flipping manifestUrl
    // can fire `ended` on the outgoing <video> element before session ever
    // got a real duration) would write position_ms:0, duration_ms:0,
    // completed:true — wiping whatever real progress was already saved for
    // this item, exactly like a genuine 100%-watched write would look.
    if (session.durationMs > 0) {
      api.putWatchProgress(kind, targetId, {
        position_ms: Math.round(session.durationMs), duration_ms: Math.round(session.durationMs), completed: true,
        device_type: 'web', direct_play: session.directPlay ?? undefined,
      }).catch(() => {})
    }
    // replace, not push: this is a continuation of the same viewing session,
    // not a new navigation the viewer chose — pushing would leave the
    // just-finished item's route on the history stack, so Back after an
    // auto-advance stepped into it instead of leaving the player entirely.
    const qs = queueToken ? `?queue=${queueToken}` : ''
    navigate(`/player/${next.kind}/${next.id}${qs}`, { replace: true })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [queueNextItem, nextEpisode, kind, targetId, session.durationMs, navigate, queueToken])

  // Movie, or last episode of a series: nothing to auto-advance into, so
  // (unlike handleAdvanceToNext) explicitly mark this item completed rather
  // than trusting the exit-ping's 95%-threshold heuristic to land in time.
  const handleNaturalEnd = useCallback(() => {
    skipCleanupPingRef.current = true
    if (kind !== 'channel' && session.durationMs > 0) {
      api.putWatchProgress(kind, targetId, {
        position_ms: Math.round(session.durationMs), duration_ms: Math.round(session.durationMs), completed: true,
        device_type: 'web', direct_play: session.directPlay ?? undefined,
      }).catch(() => {})
    }
    navigate(-1)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [kind, targetId, session.durationMs, navigate])

  // The native <video> `ended` event has fired away from the file's actual
  // end — observed after a seek lands right on the edge of the currently
  // buffered range, which the browser can read as "no more data" before
  // Hephaestus has actually finished serving what's beyond it. The seek
  // itself succeeded and there's real content left; this isn't completion,
  // so don't advance/mark watched (that would wrongly flip a mid-watch item
  // to finished) — just nudge the same <video> element to keep playing from
  // where it already is.
  const handleVideoEnded = useCallback(() => {
    const nearRealEnd = kind === 'channel' || (
      session.durationMs > 0 && (session.durationMs - currentMsRef.current) < NATURAL_END_TOLERANCE_MS
    )
    if (!nearRealEnd) {
      console.warn('[player] native ended fired away from real end (at', currentMsRef.current, 'of', session.durationMs, 'ms) — treating as spurious, resuming playback')
      videoRef.current?.play().catch(() => {})
      return
    }
    if ((queueNextItem || nextEpisode) && !upNextDismissed) handleAdvanceToNext()
    else handleNaturalEnd()
  }, [kind, session.durationMs, queueNextItem, nextEpisode, upNextDismissed, handleAdvanceToNext, handleNaturalEnd])

  // Re-evaluated on every render (piggybacking on the live video's own
  // onTimeUpdate-driven re-renders for freshness — a live channel has no
  // scrubber/duration to key an effect off of), purely from the currently-
  // airing item's chapters + wall-clock elapsed position.
  const channelElapsedMs = channelNow ? Date.now() - channelNow.wall_clock_start_ms : 0
  const channelActiveChapter = channelChapters.find(
    c => channelElapsedMs >= c.start_ms && channelElapsedMs < (c.end_ms || Infinity))
  const channelPip = kind === 'channel' && !isRemoteActive &&
    !!channelActiveChapter && isCreditsType(channelActiveChapter.chapter_type)

  return (
    <div
      className={styles.page}
      onMouseMove={resetIdleTimer}
      onClick={resetIdleTimer}
    >
      {session.loading && (
        <div className={styles.overlay}>
          <LoadingThrobber label="Starting playback…" />
        </div>
      )}

      {(session.error || playerError) && !session.loading && (
        <div className={styles.overlay}>
          <div className={styles.errorText}>
            {session.error ?? playerError}
          </div>
          <button onClick={() => navigate(-1)} className={styles.backBtn}>Go back</button>
        </div>
      )}

      {!session.loading && !session.error && session.manifestUrl && (
        <>
          {isRemoteActive ? (
            // The local hls.js instance only exists inside VideoPlayer — not
            // rendering it while casting tears it down via its own unmount
            // cleanup (hls?.destroy()), same as navigating away normally.
            <div className={styles.castingBackdrop} />
          ) : (
            <>
              {channelPip && (
                <div className={styles.pipBackdrop}>
                  <img src={channelLogoUrl(targetId)} alt="" className={styles.pipBackdropLogo} />
                </div>
              )}
              <div className={channelPip ? styles.pipVideoContainer : styles.fullVideoContainer}>
                <VideoPlayer
                  videoRef={videoRef}
                  manifestUrl={session.manifestUrl}
                  subtitleTrack={session.subtitleTrack}
                  subtitleLanguage={session.tracks?.subtitles.find(t => t.index === session.subtitleTrack)?.language ?? null}
                  audioTrack={session.audioTrack}
                  isLive={session.isLive}
                  startPositionSec={session.startPositionMs / 1000}
                  // The manifest's timeline is the file's real absolute
                  // position now (see usePlaybackSession's startPositionMs
                  // comment) — ms already is the true position, no offset
                  // to add back anymore.
                  onTimeUpdate={(ms) => setCurrentMs(ms)}
                  // Dismissing Up Next means "don't continue into the next
                  // episode" — without the upNextDismissed check here, simply
                  // letting playback run to the true end of the file would
                  // silently override that and auto-advance anyway. A
                  // dismissed-and-finished episode still just completes
                  // normally (handleNaturalEnd), same as a movie or series
                  // finale; the next episode stays reachable from Continue
                  // Watching (see Kairos's up_next synthesis) instead of
                  // auto-playing. handleVideoEnded itself filters out native
                  // `ended` events that fire away from the real end (see its
                  // own comment) before any of this runs.
                  onEnded={handleVideoEnded}
                  onError={setPlayerError}
                />
              </div>
            </>
          )}
          {buffering && !isRemoteActive && (
            <div className={`${styles.overlay} ${styles.overlayNoPointer}`}>
              <LoadingThrobber percent={bufferPercent} />
            </div>
          )}
          {showSkipIntro && (
            <button onClick={() => handleSeek(activeChapter!.end_ms)} className={styles.skipIntroBtn}>
              Skip Intro
            </button>
          )}
          {showUpNext && nextEpisode && (
            <UpNextOverlay
              nextEpisode={nextEpisode}
              onPlayNow={handleAdvanceToNext}
              onDismiss={() => setUpNextDismissed(true)}
            />
          )}
          <div className={`${styles.controlsFade} ${controlsVisible || menu ? styles.controlsVisible : styles.controlsHidden}`}>
            <PlayerControls
              videoRef={videoRef}
              title={title}
              isLive={session.isLive}
              currentMs={isCasting ? castSession.currentMs : isCastingRoku ? rokuSession.currentMs : currentMs}
              durationMs={isCasting ? castSession.durationMs : isCastingRoku ? rokuSession.durationMs : session.durationMs}
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
                volumeLevel: castSession.volumeLevel,
                muted:       castSession.muted,
                togglePlay: castSession.togglePlay,
                setVolumeLevel: castSession.setVolumeLevel,
                toggleMuted:    castSession.toggleMuted,
                endSession: handleStopCast,
                onRequestCast: () => cast.framework.CastContext.getInstance().requestSession().catch(err => {
                  // 'cancel' is the normal "closed the device picker" case —
                  // anything else was previously silent here with zero
                  // feedback, which looks identical to the button doing
                  // nothing at all.
                  if (err !== 'cancel') console.error('Cast requestSession() failed:', err)
                }),
              }}
              roku={{
                available:  rokuSession.available,
                connected:  rokuSession.connected,
                deviceName: rokuSession.deviceName,
                paused:     rokuSession.paused,
                volumeLevel: rokuSession.volumeLevel,
                muted:       rokuSession.muted,
                togglePlay: rokuSession.togglePlay,
                setVolumeLevel: rokuSession.setVolumeLevel,
                toggleMuted:    rokuSession.toggleMuted,
                endSession: handleStopRoku,
                onRequestCast: requestRoku,
              }}
            />
            {menu === 'roku-devices' && (
              <RokuDeviceMenu
                onClose={() => setMenu(null)}
                devices={rokuSession.devices}
                onSelect={deviceId => rokuSession.load(buildCastArgs(), deviceId)
                  .catch(err => setPlayerError(err?.message ?? 'Failed to start casting to Roku'))}
              />
            )}
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
