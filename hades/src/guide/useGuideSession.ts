import { useEffect, useRef, useState } from 'react'
import { api } from '../api/client'
import type { Channel, EpgProgram } from '../api/types'
import { startPreview, switchPreview, stopPreview } from './previewApi'
import {PreviewSessionController} from './previewSessionController'
import { WINDOW_LOOKBACK_MIN, WINDOW_FORWARD_HOURS } from './constants'

const FOCUS_DEBOUNCE_MS  = 300
const HIDDEN_STOP_MS     = 20_000 // grace period before a backgrounded tab's preview is actually torn down

// Whatever's airing on this channel right this instant, if anything — same
// live boundary rule as GuidePreview's own computePreviewTiming (start
// inclusive, end exclusive). Exported standalone (not just inlined in the
// hook) so it's directly unit-testable without a React renderer.
export function findLiveProgram(programs: EpgProgram[], nowMs: number): EpgProgram | null {
    return programs.find(p => p.wall_clock_start_ms <= nowMs && nowMs < p.wall_clock_end_ms) ?? null
}

// Channel/EPG data + live-preview session lifecycle, shared by the desktop
// GuidePage and TvGuideSection — extracted so both shells drive the same
// preview-session state machine instead of forking it.
export function useGuideSession() {
  const [channels,     setChannels]     = useState<Channel[]>([])
  const [epgByChannel, setEpgByChannel] = useState<Record<string, EpgProgram[]>>({})
  const [focusedId,    setFocusedId]    = useState<string | null>(null)
    // Which specific EpgProgram cell is focused, independent of focusedId (the
    // channel) — null means "nothing specific," i.e. show that channel's own
    // live/now program. Only ever drives the preview hero's TEXT; the live
    // video always follows focusedId alone (see GuidePage.tsx) — you can't
    // actually preview a future program, only read about it.
    const [focusedProgram, setFocusedProgram] = useState<EpgProgram | null>(null)
  const [manifestUrl,  setManifestUrl]  = useState<string | null>(null)
  const [nowMs,        setNowMs]        = useState(() => Date.now())

    // Start/switch/stop orchestration lives in PreviewSessionController (see
    // its own class comment) — held in a ref since it's plain, framework-free
    // state that must survive across renders without itself triggering any.
    const controllerRef = useRef<PreviewSessionController | null>(null)
    if (!controllerRef.current) {
        controllerRef.current = new PreviewSessionController(
            {startPreview, switchPreview, stopPreview},
            setManifestUrl,
        )
    }
  const debounceRef   = useRef<ReturnType<typeof setTimeout> | null>(null)
  const hiddenStopRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const windowStartMs = useRef(Date.now() - WINDOW_LOOKBACK_MIN * 60_000).current

  useEffect(() => {
    const fromSec  = Math.floor(windowStartMs / 1000)
    const hours    = WINDOW_LOOKBACK_MIN / 60 + WINDOW_FORWARD_HOURS

    api.getChannels().then(chs => {
      setChannels(chs)
      // Preview only starts once the user actually hovers/focuses a column
      // (see the focusedId effect below) — leave it unset here rather than
      // defaulting to channels[0], which used to spin up a live encode
      // session on every homepage load with no user interaction at all.
      Promise.all(chs.map(ch => api.getChannelEpg(ch.channel_id, hours, fromSec).catch(() => [])))
        .then(results => {
          const byChannel: Record<string, EpgProgram[]> = {}
          chs.forEach((ch, i) => { byChannel[ch.channel_id] = results[i] })
          setEpgByChannel(byChannel)
        })
    })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  useEffect(() => {
    const tick = setInterval(() => setNowMs(Date.now()), 30_000)
    return () => clearInterval(tick)
  }, [])

    // Thin wrappers so the rest of this hook doesn't need to know the
    // controller exists as a ref — see PreviewSessionController's own comment
    // for the actual start/switch/queue/self-stop state machine.
    const beginPreview = (channelId: string) => controllerRef.current!.begin(channelId)
    const stopCurrentPreview = () => controllerRef.current!.stop()

  useEffect(() => {
    if (!focusedId) return
    if (debounceRef.current) clearTimeout(debounceRef.current)

    debounceRef.current = setTimeout(() => {
      if (document.hidden) return // visibility effect below owns hidden-tab state
      beginPreview(focusedId)
    }, FOCUS_DEBOUNCE_MS)

    return () => { if (debounceRef.current) clearTimeout(debounceRef.current) }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [focusedId])

  // A backgrounded tab still runs its JS (hls.js keeps polling), so the
  // session never looks idle to Hephaestus's own reaper and would hold a
  // scarce hardware encoder slot forever without this. But a bare
  // stop-on-hidden/start-on-visible churns a brand new ffmpeg process on
  // every quick alt-tab (e.g. switching to a terminal and back) — so the
  // stop is debounced with a grace period, cancelled if visibility returns
  // first, and only actually tears the session down once the tab has been
  // hidden for a real stretch of time.
  useEffect(() => {
    const onVisibility = () => {
      if (document.hidden) {
        if (debounceRef.current) clearTimeout(debounceRef.current)
        if (hiddenStopRef.current) clearTimeout(hiddenStopRef.current)
        hiddenStopRef.current = setTimeout(stopCurrentPreview, HIDDEN_STOP_MS)
      } else {
        if (hiddenStopRef.current) { clearTimeout(hiddenStopRef.current); hiddenStopRef.current = null }
        // Session (or an in-flight start for it) is still alive from before
        // we were hidden — nothing to do. Only re-start if it was actually
        // torn down (grace period elapsed while we were away).
          if (focusedId && !controllerRef.current!.hasSession() && !controllerRef.current!.isStarting()) beginPreview(focusedId)
      }
    }
    document.addEventListener('visibilitychange', onVisibility)
    return () => document.removeEventListener('visibilitychange', onVisibility)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [focusedId])

  useEffect(() => () => {
    if (hiddenStopRef.current) clearTimeout(hiddenStopRef.current)
    stopCurrentPreview()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const focusedChannel = channels.find(c => c.channel_id === focusedId) ?? null
    const nowProgram = focusedId ? findLiveProgram(epgByChannel[focusedId] ?? [], nowMs) : null

    // Header focus (or a plain channel switch) — "nothing specific," fall back
    // to that channel's own live program in the hero.
    const selectChannel = (channelId: string) => {
        setFocusedId(channelId)
        setFocusedProgram(null)
    }
    // A specific program cell (now OR future) was focused/hovered — still
    // switches the live preview to that channel (same as selectChannel), but
    // also pins the hero's text to this exact program rather than whatever's
    // live right now.
    const selectProgram = (channelId: string, program: EpgProgram) => {
        setFocusedId(channelId)
        setFocusedProgram(program)
    }

  return {
    channels, epgByChannel, windowStartMs, nowMs,
      focusedId, focusedChannel, focusedProgram, nowProgram,
      selectChannel, selectProgram,
    manifestUrl,
  }
}
