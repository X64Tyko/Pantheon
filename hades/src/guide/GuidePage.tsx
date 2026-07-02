import { useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { api } from '../api/client'
import type { Channel, EpgProgram } from '../api/types'
import { startPreview, switchPreview, stopPreview } from './previewApi'
import { GuideGrid } from './GuideGrid'
import { GuidePreview } from './GuidePreview'
import { WINDOW_LOOKBACK_MIN, WINDOW_FORWARD_HOURS } from './constants'

const FOCUS_DEBOUNCE_MS  = 300
const HIDDEN_STOP_MS     = 20_000 // grace period before a backgrounded tab's preview is actually torn down

export function GuidePage() {
  const navigate = useNavigate()
  const [channels,     setChannels]     = useState<Channel[]>([])
  const [epgByChannel, setEpgByChannel] = useState<Record<string, EpgProgram[]>>({})
  const [focusedId,    setFocusedId]    = useState<string | null>(null)
  const [manifestUrl,  setManifestUrl]  = useState<string | null>(null)
  const [nowMs,        setNowMs]        = useState(() => Date.now())

  const sessionIdRef  = useRef<string | null>(null)
  const startingRef   = useRef<Promise<string | null> | null>(null) // in-flight startPreview() call, if any
  const genRef        = useRef(0) // bumped to invalidate in-flight/pending work (hidden, unmount)
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

  // Only one startPreview() may ever be in flight at a time — sessionIdRef
  // isn't set until the async call resolves, so without startingRef as a
  // synchronous guard, a second trigger arriving before a slow cold-start
  // resolves (rapid re-focus, or a visibility toggle) would fire a second
  // startPreview() and spawn a second ffmpeg process; only the last one to
  // resolve would ever get tracked, orphaning the other forever.
  //
  // genRef guards the other direction: if we're told to stop (tab hidden)
  // while a start is still in flight, bumping genRef lets the eventual
  // resolution recognize it's stale and self-stop instead of resurrecting a
  // session for a tab nobody's looking at anymore.
  const beginPreview = (channelId: string) => {
    const myGen = genRef.current
    if (sessionIdRef.current) {
      switchPreview(sessionIdRef.current, channelId).catch(() => {})
      return
    }
    if (startingRef.current) {
      startingRef.current.then(sid => {
        if (sid && genRef.current === myGen) switchPreview(sid, channelId).catch(() => {})
      })
      return
    }
    startingRef.current = startPreview(channelId).then(res => {
      if (genRef.current !== myGen) { stopPreview(res.session_id); return null }
      sessionIdRef.current = res.session_id
      setManifestUrl(res.manifest_url)
      return res.session_id
    }).catch(() => null).finally(() => { startingRef.current = null })
  }

  const stopCurrentPreview = () => {
    genRef.current++ // invalidate any in-flight startPreview() so it self-stops on resolve
    if (sessionIdRef.current) {
      stopPreview(sessionIdRef.current)
      sessionIdRef.current = null
      setManifestUrl(null)
    }
  }

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
        if (focusedId && !sessionIdRef.current && !startingRef.current) beginPreview(focusedId)
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
  const nowProgram = focusedId
    ? (epgByChannel[focusedId] ?? []).find(p => p.wall_clock_start_ms <= nowMs && nowMs < p.wall_clock_end_ms) ?? null
    : null

  if (channels.length === 0) return null

  const watchChannel = (channelId: string) => navigate(`/player/channel/${channelId}`)

  return (
    <div>
      <GuidePreview channel={focusedChannel} nowProgram={nowProgram} manifestUrl={manifestUrl} onWatch={() => focusedId && watchChannel(focusedId)} />
      <GuideGrid
        channels={channels}
        epgByChannel={epgByChannel}
        windowStartMs={windowStartMs}
        nowMs={nowMs}
        focusedChannelId={focusedId}
        onFocus={setFocusedId}
        onWatch={watchChannel}
      />
    </div>
  )
}
